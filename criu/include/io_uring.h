#ifndef __CR_IO_URING_H__
#define __CR_IO_URING_H__

#include <sys/types.h>

struct cr_imgset;
struct fd_parms;
struct cr_img;
struct pstree_item;
struct task_restore_args;
extern int is_io_uring_link(char *link);
extern const struct fdtype_ops io_uring_dump_ops;
extern struct collect_image_info io_uring_cinfo;
extern int dump_io_uring_fds(pid_t pid, int *fds, int nr_fds, struct cr_img *fdinfo_img);
extern int prepare_io_urings(struct pstree_item *t, struct task_restore_args *ta);

#endif /* __CR_IO_URING_H__ */
