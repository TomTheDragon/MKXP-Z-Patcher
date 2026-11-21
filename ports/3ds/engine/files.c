/* Finding the game's files: each folder is listed once and kept in memory,
 * because every failed attempt to open a file on the SD card costs about
 * 20 ms (RPG Maker names files without extension, so several are tried). */
#include "engine.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
	char *dir;
	char **names;
	int n;
} Listing;

static Listing *listings;
static int nlistings;

static const Listing *listing_of(const char *dir)
{
	for (int i = 0; i < nlistings; i++)
		if (!strcmp(listings[i].dir, dir))
			return &listings[i];
	Listing *nl = realloc(listings, sizeof *nl * (nlistings + 1));
	if (!nl)
		return NULL;
	listings = nl;
	Listing *l = &listings[nlistings++];
	l->dir = strdup(dir);
	l->names = NULL;
	l->n = 0;
	DIR *d = opendir(*dir ? dir : ".");
	if (!d)
		return l;
	struct dirent *e;
	int cap = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (l->n == cap) {
			cap = cap ? cap * 2 : 64;
			char **nn = realloc(l->names, sizeof *nn * cap);
			if (!nn)
				break;
			l->names = nn;
		}
		l->names[l->n++] = strdup(e->d_name);
	}
	closedir(d);
	return l;
}

/* path (relative to the game, '/' or '\\'; without or with extension):
 * the file with the first of exts that exists (case doesn't matter); exts
 * ends with NULL, "" means the name as it is */
bool file_find(const char *path, const char *const *exts, char *out, size_t n)
{
	char p[512];
	snprintf(p, sizeof p, "%s", path);
	for (char *c = p; *c; c++)
		if (*c == '\\')
			*c = '/';
	char *slash = strrchr(p, '/');
	const char *base = slash ? slash + 1 : p;
	char dir[512] = "";
	if (slash)
		snprintf(dir, sizeof dir, "%.*s", (int) (slash - p), p);
	const Listing *l = listing_of(dir);
	if (!l)
		return false;
	size_t bl = strlen(base);
	for (int k = 0; exts[k]; k++) {
		size_t el = strlen(exts[k]);
		for (int i = 0; i < l->n; i++) {
			const char *nm = l->names[i];
			if (strlen(nm) == bl + el && !strncasecmp(nm, base, bl) && !strcasecmp(nm + bl, exts[k])) {
				snprintf(out, n, "%s%s%s", dir, *dir ? "/" : "", nm);
				return true;
			}
		}
	}
	return false;
}

/* (the game writing files, e.g. saves: listings may be out of date) */
void files_changed(void)
{
	for (int i = 0; i < nlistings; i++) {
		for (int k = 0; k < listings[i].n; k++)
			free(listings[i].names[k]);
		free(listings[i].names);
		free(listings[i].dir);
	}
	free(listings);
	listings = NULL;
	nlistings = 0;
}
