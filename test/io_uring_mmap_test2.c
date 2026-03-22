#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/io_uring.h>
#include <errno.h>

int main(void)
{
	struct io_uring_params p = {};
	int fd;
	void *addr, *target, *moved;

	fd = syscall(__NR_io_uring_setup, 32, &p);
	if (fd < 0) { perror("io_uring_setup"); return 1; }
	printf("io_uring_setup: fd=%d sq=%u cq=%u\n", fd, p.sq_entries, p.cq_entries);

	/* Allocate a target address */
	target = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("target address: %p\n", target);

	/* mmap io_uring at kernel-chosen address */
	addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQES);
	if (addr == MAP_FAILED) { perror("mmap SQES"); return 1; }
	printf("mmap SQES at: %p\n", addr);

	/* Try mremap to target */
	moved = mremap(addr, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, target);
	printf("mremap to target: %s (addr=%p)\n",
	       moved == MAP_FAILED ? strerror(errno) : "OK", moved);

	/* Alternative: use /proc/self/mem to copy mapping? */
	/* Or: just memcpy the ring data after mmap */

	/* Test: mmap SQ ring, read some data */
	void *sq = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQ_RING);
	if (sq != MAP_FAILED) {
		printf("SQ ring mmap OK at %p, head=%u tail=%u\n",
		       sq, *(unsigned int *)(sq + p.sq_off.head),
		       *(unsigned int *)(sq + p.sq_off.tail));
	}

	close(fd);
	return 0;
}
