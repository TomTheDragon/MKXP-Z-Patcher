/* MKXP-Z 3DS: pipe() and poll() for Ruby's thread scheduling (its timer
 * thread wakes sleeping threads through a "self-pipe" and waits with poll).
 * The 3DS C library has neither: the pipe lives in memory, as a device of
 * devkitPro's C library (so read/write/close work on its descriptors), and
 * poll() waits on a condition variable that every pipe change signals.
 * Other descriptors (files) count as always ready. */
#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <poll.h>

#define PIPE_SIZE 4096

struct pipe {
	char buf[PIPE_SIZE];
	size_t start, len;
	int readers, writers;
};

/* the device's file structure: one end of a pipe */
struct end {
	struct pipe *p;
	int writing;
	int flags; /* O_NONBLOCK */
};

static LightLock lock;
static CondVar changed;
static int device = -1;

static ssize_t pipe_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	struct end *e = fd;
	struct pipe *p = e->p;
	if (e->writing) {
		r->_errno = EBADF;
		return -1;
	}
	LightLock_Lock(&lock);
	while (p->len == 0 && p->writers > 0) {
		if (e->flags & O_NONBLOCK) {
			LightLock_Unlock(&lock);
			r->_errno = EAGAIN;
			return -1;
		}
		CondVar_Wait(&changed, &lock);
	}
	size_t n = len < p->len ? len : p->len;
	for (size_t i = 0; i < n; i++)
		ptr[i] = p->buf[(p->start + i) % PIPE_SIZE];
	p->start = (p->start + n) % PIPE_SIZE;
	p->len -= n;
	CondVar_Broadcast(&changed);
	LightLock_Unlock(&lock);
	return n;
}

static ssize_t pipe_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	struct end *e = fd;
	struct pipe *p = e->p;
	if (!e->writing) {
		r->_errno = EBADF;
		return -1;
	}
	size_t done = 0;
	LightLock_Lock(&lock);
	while (done < len) {
		if (p->readers == 0) {
			LightLock_Unlock(&lock);
			r->_errno = EPIPE;
			return done ? (ssize_t) done : -1;
		}
		if (p->len == PIPE_SIZE) {
			if (e->flags & O_NONBLOCK) {
				LightLock_Unlock(&lock);
				if (done)
					return done;
				r->_errno = EAGAIN;
				return -1;
			}
			CondVar_Wait(&changed, &lock);
			continue;
		}
		p->buf[(p->start + p->len) % PIPE_SIZE] = ptr[done++];
		p->len++;
	}
	CondVar_Broadcast(&changed);
	LightLock_Unlock(&lock);
	return done;
}

static int pipe_close(struct _reent *r, void *fd)
{
	(void) r;
	struct end *e = fd;
	struct pipe *p = e->p;
	LightLock_Lock(&lock);
	if (e->writing)
		p->writers--;
	else
		p->readers--;
	int unused = p->readers == 0 && p->writers == 0;
	CondVar_Broadcast(&changed);
	LightLock_Unlock(&lock);
	if (unused)
		free(p);
	return 0;
}

static const devoptab_t pipe_device = {
	.name = "mkxpzpipe",
	.structSize = sizeof(struct end),
	.close_r = pipe_close,
	.write_r = pipe_write,
	.read_r = pipe_read,
};

static struct end *end_of(int fd)
{
	if (device < 0 || fd < 0)
		return NULL;
	__handle *h = __get_handle(fd);
	if (!h || (int) h->device != device)
		return NULL;
	return h->fileStruct;
}

static int open_end(struct pipe *p, int writing)
{
	int fd = __alloc_handle(device);
	if (fd < 0)
		return -1;
	struct end *e = __get_handle(fd)->fileStruct;
	e->p = p;
	e->writing = writing;
	e->flags = 0;
	return fd;
}

int pipe(int fds[2])
{
	if (device < 0) {
		LightLock_Init(&lock);
		CondVar_Init(&changed);
		device = AddDevice(&pipe_device);
		if (device < 0) {
			errno = ENOMEM;
			return -1;
		}
	}
	struct pipe *p = calloc(1, sizeof *p);
	if (!p) {
		errno = ENOMEM;
		return -1;
	}
	p->readers = p->writers = 1;
	fds[0] = open_end(p, 0);
	if (fds[0] < 0) {
		free(p);
		errno = EMFILE;
		return -1;
	}
	fds[1] = open_end(p, 1);
	if (fds[1] < 0) {
		__release_handle(fds[0]);
		free(p);
		errno = EMFILE;
		return -1;
	}
	return 0;
}

/* the C library's fcntl, for descriptors that aren't pipes */
int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	int arg = va_arg(ap, int);
	va_end(ap);
	struct end *e = end_of(fd);
	if (!e) {
		int r = __real_fcntl(fd, cmd, arg);
		if (r != -1 || fd < 0 || !__get_handle(fd))
			return r;
		/* the 3DS C library's fcntl doesn't handle files (Ruby checks every
		 * new descriptor with F_GETFD and aborts on EBADF) */
		switch (cmd) {
		case F_GETFD:
		case F_SETFD:
		case F_SETFL:
			return 0;
		case F_GETFL:
			return O_RDWR;
		default:
			return r;
		}
	}
	switch (cmd) {
	case F_GETFL:
		return (e->writing ? O_WRONLY : O_RDONLY) | e->flags;
	case F_SETFL:
		e->flags = arg & O_NONBLOCK;
		return 0;
	case F_GETFD:
	case F_SETFD:
		return 0; /* no exec: close-on-exec means nothing */
	default:
		errno = EINVAL;
		return -1;
	}
}

static short ready(struct pollfd *f)
{
	struct end *e = end_of(f->fd);
	if (!e)
		return f->events & (POLLIN | POLLOUT); /* files: always ready */
	struct pipe *p = e->p;
	short r = 0;
	if (!e->writing) {
		if (p->len > 0)
			r |= POLLIN;
		if (p->writers == 0)
			r |= POLLHUP;
	} else {
		if (p->len < PIPE_SIZE)
			r |= POLLOUT;
		if (p->readers == 0)
			r |= POLLERR;
	}
	return r & (f->events | POLLHUP | POLLERR);
}

int poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
	if (device < 0) {
		LightLock_Init(&lock);
		CondVar_Init(&changed);
		device = AddDevice(&pipe_device);
	}
	u64 deadline = timeout_ms > 0 ? osGetTime() + timeout_ms : 0;
	LightLock_Lock(&lock);
	for (;;) {
		int count = 0;
		for (nfds_t i = 0; i < n; i++) {
			fds[i].revents = fds[i].fd < 0 ? 0 : ready(&fds[i]);
			if (fds[i].revents)
				count++;
		}
		if (count || timeout_ms == 0) {
			LightLock_Unlock(&lock);
			return count;
		}
		if (timeout_ms < 0) {
			CondVar_Wait(&changed, &lock);
		} else {
			u64 now = osGetTime();
			if (now >= deadline) {
				LightLock_Unlock(&lock);
				return 0;
			}
			CondVar_WaitTimeout(&changed, &lock, (s64) (deadline - now) * 1000000);
		}
	}
}
