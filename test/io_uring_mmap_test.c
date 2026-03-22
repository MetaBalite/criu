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
	void *addr;

	fd = syscall(__NR_io_uring_setup, 32, &p);
	if (fd < 0) {
		perror("io_uring_setup");
		return 1;
	}
	printf("io_uring_setup: fd=%d sq=%u cq=%u\n", fd, p.sq_entries, p.cq_entries);

	/* Test 1: mmap SQ ring at kernel-chosen address */
	addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQ_RING);
	printf("mmap SQ_RING NULL: %s (addr=%p)\n",
	       addr == MAP_FAILED ? strerror(errno) : "OK", addr);
	if (addr != MAP_FAILED) munmap(addr, 4096);

	/* Test 2: mmap SQEs at kernel-chosen address */
	addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQES);
	printf("mmap SQES NULL: %s (addr=%p)\n",
	       addr == MAP_FAILED ? strerror(errno) : "OK", addr);
	if (addr != MAP_FAILED) munmap(addr, 4096);

	/* Test 3: mmap SQEs at MAP_FIXED with fresh address */
	void *fixed_addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("anonymous page at %p\n", fixed_addr);

	addr = mmap(fixed_addr, 4096, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, fd, IORING_OFF_SQES);
	printf("mmap SQES MAP_FIXED over anon: %s (addr=%p)\n",
	       addr == MAP_FAILED ? strerror(errno) : "OK", addr);

	/* Test 4: munmap first, then MAP_FIXED */
	void *fixed_addr2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munmap(fixed_addr2, 4096);
	addr = mmap(fixed_addr2, 4096, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, fd, IORING_OFF_SQES);
	printf("mmap SQES MAP_FIXED after munmap: %s (addr=%p)\n",
	       addr == MAP_FAILED ? strerror(errno) : "OK", addr);

	close(fd);
	return 0;
}
