/* Included into every file of the 3DS Ruby build (-include) */
#ifndef MKXPZ_3DS_COMPAT_H
#define MKXPZ_3DS_COMPAT_H
#include <sys/types.h>
/* no signals beyond signal() */
#define posix_signal(sig, handler) signal((sig), (handler))
/* no text/binary file modes */
#define setmode(fd, mode) ((void) (fd), 0)
pid_t waitpid(pid_t pid, int *status, int options);
#include <stddef.h>
void *memrchr(const void *s, int c, size_t n);
#endif
