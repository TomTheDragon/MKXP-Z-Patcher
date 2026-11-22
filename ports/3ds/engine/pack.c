/* The game's files in one pack (Game.data, made by
 * ports/3ds/tools/make-3ds-package.py): Data, Graphics (.mkxi), Audio...
 * in a single file instead of thousands on the SD card. Served through
 * the SD card's own file functions (main.c hooks them): a file that isn't
 * there as a plain file is opened for reading from the pack, so Ruby
 * (load_data, File.open, File.exist?), the images, the music and the
 * fonts all read it without knowing. Plain files come first (the game's
 * own writes, e.g. a language file, saves and settings, stay plain).
 *
 * Format: "MKXPZPK1", u32 count, u32 index length; the index (sorted by
 * name): per file u32 offset, u32 size, u16 name length, the name
 * (lowercase, "/", relative to the game folder); then the files. */
#include "engine.h"
#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *name; u32 offset, size; } PackEntry;

static const devoptab_t *dev;       /* sdmc's own functions */
static void *pack_fs;               /* the pack opened through them */
static PackEntry *entries;
static int nentries;
static char *names;                 /* the index's names */
static char game_dir[256];          /* without "sdmc:", e.g. /3ds/Changed */
static LightLock lock;

/* the pack's folders that exist as plain folders too (e.g. Changed's
 * Audio, with the lang.txt the game writes): there a plain file comes
 * first; elsewhere the pack is asked first (a failed open on the SD card
 * costs several ms) */
static char **plain_dirs;
static int nplain_dirs;

/* files opened from the pack (newlib's fileStruct of each -> its state) */
#define PACK_OPEN 48
static struct { void *fd; int entry; u32 pos; } open_files[PACK_OPEN];

bool pack_active(void) { return pack_fs != NULL; }

/* a path as the pack names it (NULL: outside the game folder) */
static bool pack_name(const char *path, char *out, size_t n)
{
	if (!strncasecmp(path, "sdmc:", 5))
		path += 5;
	char buf[512];
	if (path[0] == '/') {
		size_t gl = strlen(game_dir);
		if (strncasecmp(path, game_dir, gl) || (path[gl] != '/' && path[gl]))
			return false;
		path += gl;
	}
	/* normalize: "\\" -> "/", no "./" or "//", lowercase */
	size_t o = 0;
	for (const char *p = path; *p && o + 1 < sizeof buf; p++) {
		char c = *p == '\\' ? '/' : *p;
		if (c == '/' && (o == 0 || buf[o - 1] == '/'))
			continue;
		if (c == '.' && (o == 0 || buf[o - 1] == '/') && (p[1] == '/' || p[1] == '\\' || !p[1]))
			continue;
		buf[o++] = (char) (c >= 'A' && c <= 'Z' ? c + 32 : c);
	}
	while (o > 0 && buf[o - 1] == '/')
		o--;
	buf[o] = 0;
	if (!o || o >= n)
		return false;
	memcpy(out, buf, o + 1);
	return true;
}

static int find(const char *name)
{
	int lo = 0, hi = nentries - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(entries[mid].name, name);
		if (!c)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* a folder in the pack: some name starts with it and "/" */
static bool is_dir(const char *name)
{
	size_t l = strlen(name);
	int lo = 0, hi = nentries;
	while (lo < hi) { /* the first name >= name */
		int mid = (lo + hi) / 2;
		if (strcmp(entries[mid].name, name) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (int i = lo; i < nentries && !strncmp(entries[i].name, name, l); i++)
		if (entries[i].name[l] == '/')
			return true;
	return false;
}

static bool pack_read_at(u32 at, void *buf, size_t len)
{
	struct _reent r = { 0 };
	LightLock_Lock(&lock);
	bool ok = dev->seek_r(&r, pack_fs, (off_t) at, SEEK_SET) == (off_t) at &&
	          dev->read_r(&r, pack_fs, buf, len) == (ssize_t) len;
	LightLock_Unlock(&lock);
	return ok;
}

void pack_init(const void *sd_devoptab)
{
	const devoptab_t *sd = sd_devoptab;
	if (!sd || !sd->open_r || !sd->read_r || !sd->seek_r)
		return;
	char cwd[256];
	if (!getcwd(cwd, sizeof cwd))
		return;
	snprintf(game_dir, sizeof game_dir, "%s", !strncasecmp(cwd, "sdmc:", 5) ? cwd + 5 : cwd);
	size_t gl = strlen(game_dir);
	while (gl > 1 && game_dir[gl - 1] == '/')
		game_dir[--gl] = 0;
	dev = sd;
	void *fs = calloc(1, sd->structSize ? sd->structSize : 64);
	struct _reent r = { 0 };
	if (!fs || sd->open_r(&r, fs, "Game.data", O_RDONLY, 0) == -1) {
		free(fs);
		return;
	}
	pack_fs = fs;
	LightLock_Init(&lock);
	unsigned char head[16];
	u32 count = 0, ilen = 0;
	if (pack_read_at(0, head, 16) && !memcmp(head, "MKXPZPK1", 8)) {
		count = head[8] | head[9] << 8 | head[10] << 16 | (u32) head[11] << 24;
		ilen = head[12] | head[13] << 8 | head[14] << 16 | (u32) head[15] << 24;
	}
	unsigned char *idx = count && ilen ? malloc(ilen) : NULL;
	entries = idx ? calloc(count, sizeof *entries) : NULL;
	names = idx ? malloc(ilen) : NULL;
	if (!idx || !entries || !names || !pack_read_at(16, idx, ilen)) {
		log_only("pack: Game.data unreadable\n");
		free(idx);
		free(entries);
		free(names);
		entries = NULL;
		sd->close_r(&r, fs);
		free(fs);
		pack_fs = NULL;
		return;
	}
	size_t p = 0, np = 0;
	for (u32 i = 0; i < count && p + 10 <= ilen; i++) {
		u32 off = idx[p] | idx[p + 1] << 8 | idx[p + 2] << 16 | (u32) idx[p + 3] << 24;
		u32 size = idx[p + 4] | idx[p + 5] << 8 | idx[p + 6] << 16 | (u32) idx[p + 7] << 24;
		unsigned nl = idx[p + 8] | idx[p + 9] << 8;
		p += 10;
		if (p + nl > ilen || np + nl + 1 > ilen)
			break;
		memcpy(names + np, idx + p, nl);
		names[np + nl] = 0;
		entries[nentries++] = (PackEntry) { names + np, off, size };
		np += nl + 1;
		p += nl;
	}
	free(idx);
	/* its folders: which exist as plain folders */
	char last[512] = "";
	for (int i = 0; i < nentries; i++) {
		const char *slash = strrchr(entries[i].name, '/');
		if (!slash)
			continue;
		size_t dl = (size_t) (slash - entries[i].name);
		if (dl >= sizeof last || (strlen(last) == dl && !strncmp(last, entries[i].name, dl)))
			continue;
		memcpy(last, entries[i].name, dl);
		last[dl] = 0;
		struct stat st;
		struct _reent r2 = { 0 };
		if (sd->stat_r && sd->stat_r(&r2, last, &st) == 0) {
			char **nd = realloc(plain_dirs, sizeof *nd * (nplain_dirs + 1));
			if (nd && (nd[nplain_dirs] = strdup(last))) {
				plain_dirs = nd;
				nplain_dirs++;
			} else if (nd) {
				plain_dirs = nd;
			}
		}
	}
	log_only("pack: Game.data, %d files (%d folders also plain)\n", nentries, nplain_dirs);
}

static int slot_of(void *fd)
{
	for (int i = 0; i < PACK_OPEN; i++)
		if (open_files[i].fd == fd)
			return i;
	return -1;
}

bool pack_is(void *fd) { return pack_fs && slot_of(fd) >= 0; }

/* the game writes a file: its folder is a plain one from now on (a file
 * of the pack written over is read from the SD card then) */
void pack_note_write(const char *path)
{
	char name[512];
	if (!pack_fs || !pack_name(path, name, sizeof name))
		return;
	char *slash = strrchr(name, '/');
	if (!slash)
		return;
	*slash = 0;
	for (int i = 0; i < nplain_dirs; i++)
		if (!strcmp(plain_dirs[i], name))
			return;
	char **nd = realloc(plain_dirs, sizeof *nd * (nplain_dirs + 1));
	if (!nd)
		return;
	plain_dirs = nd;
	if ((nd[nplain_dirs] = strdup(name)))
		nplain_dirs++;
}

/* a path to read from the pack without asking the SD card first: in the
 * pack, and its folder isn't a plain one too */
bool pack_first(const char *path)
{
	char name[512];
	if (!pack_fs || !pack_name(path, name, sizeof name) || find(name) < 0)
		return false;
	const char *slash = strrchr(name, '/');
	if (!slash)
		return false; /* (the game folder itself: plain) */
	size_t dl = (size_t) (slash - name);
	for (int i = 0; i < nplain_dirs; i++)
		if (strlen(plain_dirs[i]) == dl && !strncmp(plain_dirs[i], name, dl))
			return false;
	return true;
}

/* opening for reading a file that isn't there as a plain file; 0: from
 * the pack, -1: not in it either (r->_errno as it was) */
int pack_open(struct _reent *r, void *fileStruct, const char *path, int flags)
{
	char name[512];
	if (!pack_fs || (flags & O_ACCMODE) != O_RDONLY || !pack_name(path, name, sizeof name))
		return -1;
	int e = find(name);
	if (e < 0)
		return -1;
	int s = slot_of(NULL);
	if (s < 0) {
		r->_errno = EMFILE;
		return -1;
	}
	open_files[s].fd = fileStruct;
	open_files[s].entry = e;
	open_files[s].pos = 0;
	r->_errno = 0;
	return 0;
}

int pack_close(void *fd)
{
	int s = slot_of(fd);
	if (s >= 0)
		open_files[s].fd = NULL;
	return 0;
}

long pack_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	int s = slot_of(fd);
	if (s < 0) {
		r->_errno = EBADF;
		return -1;
	}
	PackEntry *e = &entries[open_files[s].entry];
	u32 pos = open_files[s].pos;
	if (pos >= e->size)
		return 0;
	if (len > e->size - pos)
		len = e->size - pos;
	if (len && !pack_read_at(e->offset + pos, ptr, len)) {
		r->_errno = EIO;
		return -1;
	}
	open_files[s].pos = pos + (u32) len;
	return (long) len;
}

long pack_seek(struct _reent *r, void *fd, long pos, int dir)
{
	int s = slot_of(fd);
	if (s < 0) {
		r->_errno = EBADF;
		return -1;
	}
	long base = dir == SEEK_CUR ? (long) open_files[s].pos : dir == SEEK_END ? (long) entries[open_files[s].entry].size : 0;
	long to = base + pos;
	if (to < 0) {
		r->_errno = EINVAL;
		return -1;
	}
	open_files[s].pos = (u32) to;
	return to;
}

static void fill_stat(struct stat *st, u32 size, bool dir)
{
	memset(st, 0, sizeof *st);
	st->st_mode = dir ? S_IFDIR | 0555 : S_IFREG | 0444;
	st->st_size = size;
	st->st_nlink = 1;
	st->st_blksize = 512;
}

int pack_fstat(void *fd, struct stat *st)
{
	int s = slot_of(fd);
	if (s < 0)
		return -1;
	fill_stat(st, entries[open_files[s].entry].size, false);
	return 0;
}

/* a file or folder in the pack (stat of a path that isn't plain): 0 */
int pack_stat(const char *path, struct stat *st)
{
	char name[512];
	if (!pack_fs || !pack_name(path, name, sizeof name))
		return -1;
	int e = find(name);
	if (e >= 0) {
		fill_stat(st, entries[e].size, false);
		return 0;
	}
	if (is_dir(name)) {
		fill_stat(st, 0, true);
		return 0;
	}
	return -1;
}

/* the pack's names directly in a folder ("" the game folder), for the
 * engine's file search (files.c listings): files and subfolders */
void pack_list(const char *dir, void (*add)(const char *name, void *ctx), void *ctx)
{
	if (!pack_fs)
		return;
	char d[512] = "";
	if (*dir && !pack_name(dir, d, sizeof d))
		return;
	size_t dl = strlen(d);
	char last[256] = "";
	for (int i = 0; i < nentries; i++) {
		const char *n = entries[i].name;
		if (dl && (strncmp(n, d, dl) || n[dl] != '/'))
			continue;
		const char *rest = dl ? n + dl + 1 : n;
		const char *slash = strchr(rest, '/');
		size_t len = slash ? (size_t) (slash - rest) : strlen(rest);
		if (!len || len >= sizeof last || (strlen(last) == len && !strncmp(last, rest, len)))
			continue;
		memcpy(last, rest, len);
		last[len] = 0;
		add(last, ctx);
	}
}
