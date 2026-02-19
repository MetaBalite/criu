#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

#include "common/compiler.h"
#include "io_uring.h"
#include "fdinfo.h"
#include "imgset.h"
#include "image.h"
#include "util.h"
#include "log.h"
#include "files.h"
#include "bfd.h"

#include "file-ids.h"

#include "protobuf.h"
#include "images/io-uring.pb-c.h"

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

#undef LOG_PREFIX
#define LOG_PREFIX "io_uring: "

#define MAX_IO_URING_VMAS 3

struct io_uring_info {
	IoUringEntry *iue;
	struct file_desc d;
};

int is_io_uring_link(char *link)
{
	return is_anon_link_type(link, "[io_uring]");
}

static int parse_io_uring_fdinfo(pid_t pid, int fd,
				 unsigned int *sq_entries,
				 unsigned int *cq_entries,
				 unsigned int *sq_head,
				 unsigned int *sq_tail,
				 unsigned int *cq_head,
				 unsigned int *cq_tail,
				 unsigned int *features,
				 unsigned int *setup_flags)
{
	struct bfd f;
	char *str;
	unsigned int sq_mask = 0, cq_mask = 0;
	int parsed = 0;

	f.fd = open_proc(pid, "fdinfo/%d", fd);
	if (f.fd < 0)
		return -1;

	if (bfdopenr(&f))
		return -1;

	while (1) {
		str = breadline(&f);
		if (!str)
			break;
		if (IS_ERR(str))
			goto out;

		if (!strncmp(str, "SqMask:", 7)) {
			if (sscanf(str, "SqMask: %x", &sq_mask) == 1)
				parsed++;
		} else if (!strncmp(str, "CqMask:", 7)) {
			if (sscanf(str, "CqMask: %x", &cq_mask) == 1)
				parsed++;
		} else if (!strncmp(str, "SqHead:", 7)) {
			if (sscanf(str, "SqHead: %u", sq_head) == 1)
				parsed++;
		} else if (!strncmp(str, "SqTail:", 7)) {
			if (sscanf(str, "SqTail: %u", sq_tail) == 1)
				parsed++;
		} else if (!strncmp(str, "CqHead:", 7)) {
			if (sscanf(str, "CqHead: %u", cq_head) == 1)
				parsed++;
		} else if (!strncmp(str, "CqTail:", 7)) {
			if (sscanf(str, "CqTail: %u", cq_tail) == 1)
				parsed++;
		} else if (!strncmp(str, "Features:", 9)) {
			if (sscanf(str, "Features: %x", features) == 1)
				parsed++;
		} else if (!strncmp(str, "SqThreadCpu:", 12)) {
			/* We read this to detect SQPOLL but don't store it */
		} else if (!strncmp(str, "Flags:", 6)) {
			if (sscanf(str, "Flags: %x", setup_flags) == 1)
				parsed++;
		}
	}

	if (parsed < 6) {
		pr_err("Failed to parse io_uring fdinfo (got %d/6 fields)\n", parsed);
		goto out;
	}

	*sq_entries = sq_mask + 1;
	*cq_entries = cq_mask + 1;

	bclose(&f);
	return 0;
out:
	bclose(&f);
	return -1;
}

static int collect_io_uring_vmas(pid_t pid, unsigned long inode,
				 IoUringVma **out_vmas, size_t *out_n)
{
	struct bfd f;
	char *str;
	IoUringVma *vmas = NULL;
	size_t n = 0;

	f.fd = open_proc(pid, "maps");
	if (f.fd < 0)
		return -1;

	if (bfdopenr(&f))
		return -1;

	while (1) {
		unsigned long start, end, pgoff, ino;
		int dev_maj, dev_min;
		char perms[5];

		str = breadline(&f);
		if (!str)
			break;
		if (IS_ERR(str))
			goto err;

		if (!strstr(str, "anon_inode:[io_uring]"))
			continue;

		if (sscanf(str, "%lx-%lx %4s %lx %x:%x %lu",
			   &start, &end, perms, &pgoff,
			   &dev_maj, &dev_min, &ino) != 7)
			continue;

		if (ino != inode)
			continue;

		if (n >= MAX_IO_URING_VMAS) {
			pr_err("Too many io_uring VMAs (max %d)\n", MAX_IO_URING_VMAS);
			goto err;
		}

		vmas = xrealloc(vmas, (n + 1) * sizeof(IoUringVma));
		if (!vmas)
			goto err;

		io_uring_vma__init(&vmas[n]);
		vmas[n].addr = start;
		vmas[n].size = end - start;
		vmas[n].pgoff = pgoff;
		n++;

		pr_debug("  vma: %lx-%lx pgoff %lx\n", start, end, pgoff);
	}

	bclose(&f);
	*out_vmas = vmas;
	*out_n = n;
	return 0;
err:
	bclose(&f);
	xfree(vmas);
	return -1;
}

static int dump_one_io_uring_fd(pid_t pid, int fd, struct cr_img *fdinfo_img)
{
	IoUringEntry iue = IO_URING_ENTRY__INIT;
	FdinfoEntry fde = FDINFO_ENTRY__INIT;
	FileEntry fe = FILE_ENTRY__INIT;
	IoUringVma *vmas = NULL;
	IoUringVma **vma_ptrs = NULL;
	size_t n_vmas = 0;
	struct stat st;
	int ret = -1;
	size_t i;
	int path_fd;

	/* Get inode via O_PATH fd (O_RDONLY fails for io_uring anon_inodes) */
	path_fd = open_proc_path(pid, "fd/%d", fd);
	if (path_fd < 0) {
		pr_perror("Can't open /proc/%d/fd/%d", pid, fd);
		return -1;
	}

	if (fstat(path_fd, &st) < 0) {
		pr_perror("Can't fstat /proc/%d/fd/%d", pid, fd);
		close(path_fd);
		return -1;
	}
	close(path_fd);

	if (parse_io_uring_fdinfo(pid, fd, &iue.sq_entries, &iue.cq_entries,
				  &iue.sq_head, &iue.sq_tail,
				  &iue.cq_head, &iue.cq_tail,
				  &iue.features, &iue.setup_flags))
		return -1;

	if (collect_io_uring_vmas(pid, st.st_ino, &vmas, &n_vmas))
		return -1;

	iue.id = make_gen_id((uint32_t)st.st_dev, (uint32_t)st.st_ino, 0);
	iue.flags = 0;
	iue.fown = NULL;
	iue.setup_entries = iue.sq_entries;

	if (n_vmas > 0) {
		vma_ptrs = xmalloc(n_vmas * sizeof(IoUringVma *));
		if (!vma_ptrs)
			goto out;
		for (i = 0; i < n_vmas; i++)
			vma_ptrs[i] = &vmas[i];
	}
	iue.n_vmas = n_vmas;
	iue.vmas = vma_ptrs;

	pr_info("Dumping id %#x fd %d sq=%u cq=%u features=%#x flags=%#x vmas=%zu\n",
		iue.id, fd, iue.sq_entries, iue.cq_entries, iue.features,
		iue.setup_flags, n_vmas);

	/* Register in CRIU's fd tracking (epoll needs this to resolve targets) */
	fde.type = FD_TYPES__IO_URING;
	fde.id = iue.id;
	fde.fd = fd;
	fde.flags = 0;

	{
		struct fd_parms p = FD_PARMS_INIT;
		p.stat = st;
		p.fd = fd;
		p.pid = pid;
		ret = fd_id_generate(pid, &fde, &p);
		if (ret < 0)
			goto out;
	}

	/* fd_id_generate may update fde.id — propagate to file entry */
	iue.id = fde.id;

	/* Write FileEntry to shared files image */
	fe.type = FD_TYPES__IO_URING;
	fe.id = fde.id;
	fe.iou = &iue;

	if (ret == 1) {
		/* New ID — write the file entry */
		ret = pb_write_one(img_from_set(glob_imgset, CR_FD_FILES), &fe, PB_FILE);
		if (ret)
			goto out;
	}

	/* Write FdinfoEntry to per-task fdinfo image */
	ret = pb_write_one(fdinfo_img, &fde, PB_FDINFO);
out:
	xfree(vma_ptrs);
	xfree(vmas);
	return ret;
}

/*
 * Dump io_uring fds directly using /proc/PID paths.
 * io_uring fds cannot be sent via SCM_RIGHTS, so we bypass
 * the parasite fd drain and read everything from proc.
 */
int dump_io_uring_fds(pid_t pid, int *fds, int nr_fds, struct cr_img *fdinfo_img)
{
	int i, ret = 0;

	for (i = 0; i < nr_fds; i++) {
		ret = dump_one_io_uring_fd(pid, fds[i], fdinfo_img);
		if (ret)
			break;
	}

	return ret;
}

const struct fdtype_ops io_uring_dump_ops = {
	.type = FD_TYPES__IO_URING,
	.dump = NULL, /* Not used — io_uring bypasses parasite drain */
};

static int io_uring_open(struct file_desc *d, int *new_fd)
{
	struct io_uring_info *info;
	IoUringEntry *iue;
	struct io_uring_params params;
	int tmp;
	size_t i;

	info = container_of(d, struct io_uring_info, d);
	iue = info->iue;

	pr_info("Restoring id %#x sq=%u cq=%u features=%#x flags=%#x vmas=%zu\n",
		iue->id, iue->sq_entries, iue->cq_entries, iue->features,
		iue->setup_flags, iue->n_vmas);

	memset(&params, 0, sizeof(params));
	params.flags = iue->setup_flags;
	/* Strip SQPOLL — kernel thread can't survive checkpoint */
	params.flags &= ~IORING_SETUP_SQPOLL;
	params.cq_entries = iue->cq_entries;
	if (iue->cq_entries != iue->sq_entries * 2)
		params.flags |= IORING_SETUP_CQSIZE;

	tmp = syscall(__NR_io_uring_setup, iue->setup_entries, &params);
	if (tmp < 0) {
		pr_perror("Can't create io_uring %#x (entries=%u flags=%#x)",
			  iue->id, iue->setup_entries, params.flags);
		return -1;
	}

	pr_debug("  created ring fd=%d sq=%u cq=%u\n",
		 tmp, params.sq_entries, params.cq_entries);

	/* Remap ring memory at original addresses */
	for (i = 0; i < iue->n_vmas; i++) {
		IoUringVma *v = iue->vmas[i];
		void *addr, *kern_addr;

		/*
		 * Try MAP_FIXED first (works on kernel <=6.4).
		 * Kernel 6.5+ rejects MAP_FIXED for io_uring, so fall
		 * back to mmap at kernel address + memcpy to original.
		 */
		munmap((void *)v->addr, v->size);
		addr = mmap((void *)v->addr, v->size,
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED, tmp, v->pgoff);
		if (addr != MAP_FAILED) {
			pr_debug("  mapped vma %#lx (MAP_FIXED)\n",
				 (unsigned long)v->addr);
			continue;
		}

		/*
		 * Fallback: mmap at kernel-chosen address, then copy
		 * fresh ring data to the original address (re-create
		 * anonymous mapping there).
		 */
		kern_addr = mmap(NULL, v->size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, tmp, v->pgoff);
		if (kern_addr == MAP_FAILED) {
			pr_perror("Can't mmap io_uring vma pgoff %#lx",
				  (unsigned long)v->pgoff);
			goto err_close;
		}

		/* Re-create anonymous mapping at original address */
		addr = mmap((void *)v->addr, v->size,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (addr == MAP_FAILED) {
			pr_perror("Can't recreate anon mapping at %#lx",
				  (unsigned long)v->addr);
			munmap(kern_addr, v->size);
			goto err_close;
		}

		/* Copy fresh ring state to original address */
		memcpy(addr, kern_addr, v->size);
		munmap(kern_addr, v->size);
		pr_info("  mapped vma %#lx (fallback memcpy, ring not shared)\n",
			(unsigned long)v->addr);
	}

	if (rst_file_params(tmp, iue->fown, iue->flags)) {
		pr_perror("Can't restore params on io_uring %#x", iue->id);
		goto err_close;
	}

	*new_fd = tmp;
	return 0;

err_close:
	close(tmp);
	return -1;
}

static struct file_desc_ops io_uring_desc_ops = {
	.type = FD_TYPES__IO_URING,
	.open = io_uring_open,
};

static int collect_one_io_uring(void *o, ProtobufCMessage *msg, struct cr_img *i)
{
	struct io_uring_info *info = o;

	info->iue = pb_msg(msg, IoUringEntry);
	return file_desc_add(&info->d, info->iue->id, &io_uring_desc_ops);
}

struct collect_image_info io_uring_cinfo = {
	.fd_type = CR_FD_IO_URING,
	.pb_type = PB_IO_URING,
	.priv_size = sizeof(struct io_uring_info),
	.collect = collect_one_io_uring,
};
