#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <liburing.h>

int main(void)
{
	struct io_uring ring;
	int ret, i = 0;

	ret = io_uring_queue_init(32, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
		return 1;
	}

	printf("io_uring ring created (fd=%d, sq=32)\n", ring.ring_fd);
	fflush(stdout);

	while (1) {
		struct io_uring_sqe *sqe;
		struct io_uring_cqe *cqe;

		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "io_uring_get_sqe failed\n");
			return 1;
		}

		io_uring_prep_nop(sqe);
		sqe->user_data = i;

		ret = io_uring_submit(&ring);
		if (ret < 0) {
			fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
			return 1;
		}

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
			return 1;
		}

		printf("tick %d (cqe res=%d, user_data=%llu)\n",
		       i, cqe->res, (unsigned long long)cqe->user_data);
		fflush(stdout);

		io_uring_cqe_seen(&ring, cqe);
		i++;
		sleep(1);
	}

	io_uring_queue_exit(&ring);
	return 0;
}
