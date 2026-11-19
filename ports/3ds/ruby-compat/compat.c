/* stand-ins for what the 3DS C library doesn't have */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/mman.h>
pid_t waitpid(pid_t pid, int *status, int options) { (void) pid; (void) status; (void) options; errno = ECHILD; return -1; }
/* memory "mappings": only anonymous ones (Ruby's fiber stacks), from the heap;
 * no page protection (so no guard pages) */
#include <malloc.h>
#include <string.h>
void *mmap(void *a, size_t l, int p, int f, int fd, off_t o)
{
	(void) a; (void) p; (void) o;
	if (!(f & MAP_ANONYMOUS) || fd != -1) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	void *m = memalign(4096, l);
	if (!m) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(m, 0, l);
	return m;
}
int munmap(void *a, size_t l) { (void) l; free(a); return 0; }
int mprotect(void *a, size_t l, int p) { (void) a; (void) l; (void) p; return 0; }

/* users, processes, pipes: the 3DS has none of these */
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
struct passwd;
int chown(const char *p, uid_t u, gid_t g) { (void) p; (void) u; (void) g; return 0; }
void endpwent(void) {}
struct passwd *getpwnam(const char *n) { (void) n; return NULL; }
int execl(const char *p, const char *a, ...) { (void) p; (void) a; errno = ENOSYS; return -1; }
int execle(const char *p, const char *a, ...) { (void) p; (void) a; errno = ENOSYS; return -1; }
int execv(const char *p, char *const a[]) { (void) p; (void) a; errno = ENOSYS; return -1; }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
pid_t getppid(void) { return 0; }
mode_t umask(mode_t m) { (void) m; return 022; }
int getrusage(int who, struct rusage *r) { (void) who; memset(r, 0, sizeof *r); return 0; }
int pthread_kill(pthread_t t, int sig) { (void) t; (void) sig; return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *old) { (void) how; (void) set; if (old) memset(old, 0, sizeof *old); return 0; }
