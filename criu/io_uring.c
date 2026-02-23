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
#include "pstree.h"
#include "rst-malloc.h"
#include "restorer.h"

#include "file-ids.h"

#include "protobuf.h"
#include "images/io-uring.pb-c.h"

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

#undef LOG_PREFIX
#define LOG_PREFIX "io_uring: "

#define MAX_IO_URING_VMAS 8

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

	/*
	 * Kernel 5.15 only outputs SqThread/UserFiles/UserBufs.
	 * Kernel 6.x adds SqMask/CqMask/SqHead/SqTail/CqHead/CqTail/Features.
	 * For fresh ring restore, we only need entries count which we can
	 * derive from VMA sizes if fdinfo doesn't provide it.
	 */
	if (parsed >= 6) {
		*sq_entries = sq_mask + 1;
		*cq_entries = cq_mask + 1;
	} else {
		/* Defaults — will be refined from VMA sizes by caller */
		*sq_entries = 0;
		*cq_entries = 0;
		*sq_head = 0;
		*sq_tail = 0;
		*cq_head = 0;
		*cq_tail = 0;
	}

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

		if (inode != 0 && ino != inode)
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

	/*
	 * On kernel 5.15 all io_uring fds share the same anon_inode,
	 * so we can't filter VMAs by inode. Pass 0 to skip inode filter.
	 */
	if (collect_io_uring_vmas(pid, 0, &vmas, &n_vmas))
		return -1;

	/*
	 * If fdinfo didn't provide sq_entries (kernel 5.15), derive from
	 * the SQE VMA size: entries = vma_size / sizeof(struct io_uring_sqe).
	 * sizeof(io_uring_sqe) is 64 bytes.
	 */
	if (iue.sq_entries == 0 && n_vmas > 0) {
		for (i = 0; i < n_vmas; i++) {
			if (vmas[i].pgoff == 0x10000000ULL) { /* IORING_OFF_SQES */
				iue.sq_entries = vmas[i].size / 64;
				iue.cq_entries = iue.sq_entries * 2;
				break;
			}
		}
		if (iue.sq_entries == 0) {
			pr_err("Can't determine io_uring sq_entries from VMAs\n");
			goto out;
		}
		pr_info("Derived sq_entries=%u from SQE VMA size\n", iue.sq_entries);
	}

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
	int tmp;

	info = container_of(d, struct io_uring_info, d);
	iue = info->iue;

	pr_info("Restoring id %#x sq=%u cq=%u features=%#x flags=%#x vmas=%zu\n",
		iue->id, iue->sq_entries, iue->cq_entries, iue->features,
		iue->setup_flags, iue->n_vmas);

	/*
	 * Do NOT call io_uring_setup() here.
	 *
	 * io_uring_open() runs in the CRIU child process before the restorer
	 * blob takes over. io_uring_setup() creates kernel-side VMAs in the
	 * process address space. The restorer blob's unmap_old_vmas() then
	 * destroys these VMAs while keeping the fd, leaving a ring fd with
	 * no backing memory — causing SIGSEGV when the process resumes.
	 *
	 * Instead, return a placeholder fd (/dev/null). The application's
	 * io_uring library (uvloop) will be fully reinitialized after restore
	 * via the intercept library's SIGUSR2 handler, which creates fresh
	 * io_uring rings with proper VMAs.
	 */
	tmp = open("/dev/null", O_RDWR);
	if (tmp < 0) {
		pr_perror("Can't open /dev/null as io_uring placeholder");
		return -1;
	}

	pr_debug("  placeholder fd=%d for io_uring (will be reinitialized post-restore)\n", tmp);

	*new_fd = tmp;
	return 0;
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

int prepare_io_urings(struct pstree_item *t, struct task_restore_args *ta)
{
	struct fdinfo_list_entry *fle;
	int count = 0;

	ta->io_urings = (struct rst_io_uring *)rst_mem_align_cpos(RM_PRIVATE);
	ta->io_urings_n = 0;

	/* Walk this task's fd list to find io_uring descriptors */
	list_for_each_entry(fle, &rsti(t)->fds, ps_list) {
		struct io_uring_info *info;
		struct rst_io_uring *rio;
		IoUringEntry *iue;
		unsigned int i;

		if (fle->desc->ops->type != FD_TYPES__IO_URING)
			continue;

		info = container_of(fle->desc, struct io_uring_info, d);
		iue = info->iue;

		rio = rst_mem_alloc(sizeof(*rio), RM_PRIVATE);
		if (!rio)
			return -1;

		rio->fd = fle->fe->fd;
		rio->sq_entries = iue->sq_entries;
		rio->cq_entries = iue->cq_entries;
		rio->setup_flags = iue->setup_flags;
		rio->n_vmas = 0;

		for (i = 0; i < iue->n_vmas && i < RST_IO_URING_MAX_VMAS; i++) {
			rio->vmas[i].addr = iue->vmas[i]->addr;
			rio->vmas[i].size = iue->vmas[i]->size;
			rio->vmas[i].pgoff = iue->vmas[i]->pgoff;
			rio->n_vmas++;
		}

		pr_info("Prepared io_uring fd %d sq=%u cq=%u flags=%#x vmas=%u\n",
			rio->fd, rio->sq_entries, rio->cq_entries,
			rio->setup_flags, rio->n_vmas);

		count++;
	}

	ta->io_urings_n = count;
	return 0;
}
