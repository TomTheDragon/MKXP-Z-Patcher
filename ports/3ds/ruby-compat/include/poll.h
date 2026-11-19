/* MKXP-Z 3DS: poll() for pipes (see compat/pipe.c) */
#ifndef MKXPZ_3DS_POLL_H
#define MKXPZ_3DS_POLL_H
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
typedef unsigned int nfds_t;
struct pollfd {
	int fd;
	short events;
	short revents;
};
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
#endif
