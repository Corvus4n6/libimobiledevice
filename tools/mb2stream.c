/*
 * mb2stream.c — idevicebackup2's --stream mode: the record writer and the
 * index of streamed files. See mb2stream.h.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif
#include "mb2stream.h"

/* ---- record writer ---- */

static int out_fd = -1;
static char obuf[1 << 20];
static size_t olen = 0;
static int failed = 0;

static int write_all(const char *p, size_t n)
{
	while (n > 0) {
#ifdef _WIN32
		int w = _write(out_fd, p, (unsigned int)(n > (1u << 30) ? (1u << 30) : n));
#else
		ssize_t w = write(out_fd, p, n);
#endif
		if (w < 0) {
			if (errno == EINTR) continue;
			failed = 1;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int flush_out(void)
{
	if (olen == 0) return 0;
	int r = write_all(obuf, olen);
	olen = 0;
	return r;
}

static int put(const void *p, size_t n)
{
	if (failed) return -1;
	if (n > sizeof(obuf)) {
		if (flush_out() < 0) return -1;
		return write_all(p, n);
	}
	if (olen + n > sizeof(obuf) && flush_out() < 0) return -1;
	memcpy(obuf + olen, p, n);
	olen += n;
	return 0;
}

static int put_u8(uint8_t v) { return put(&v, 1); }
static int put_u16(uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; return put(b, 2); }
static int put_u32(uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; return put(b, 4); }
static int put_u64(uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i));
	return put(b, 8);
}

/* put_str writes a u16-length-prefixed string; longer than 65535 bytes is
 * truncated for messages (truncate=1) and an error for paths. */
static int put_str(const char *s, int truncate)
{
	size_t n = strlen(s);
	if (n > 0xffff) {
		if (!truncate) { failed = 1; return -1; }
		n = 0xffff;
	}
	if (put_u16((uint16_t)n) < 0) return -1;
	return put(s, n);
}

int mb2s_open(int fd)
{
	out_fd = fd;
	olen = 0;
	failed = 0;
	return 0;
}

int mb2s_open_stdout(void)
{
	fflush(stdout);
#ifdef _WIN32
	int fd = _dup(_fileno(stdout));
	if (fd < 0) return -1;
	_setmode(fd, _O_BINARY);
	if (_dup2(_fileno(stderr), _fileno(stdout)) < 0) return -1;
	setvbuf(stdout, NULL, _IONBF, 0);   /* the MS CRT has no line buffering */
#else
	int fd = dup(STDOUT_FILENO);
	if (fd < 0) return -1;
	if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return -1;
	setvbuf(stdout, NULL, _IOLBF, 0);
#endif
	return mb2s_open(fd);
}

int mb2s_failed(void) { return failed; }

/* ---- index of streamed files ---- */

struct mb2s_entry {
	char *path;      /* NULL: empty slot; TOMB: deleted */
	uint64_t size;
	time_t mtime;
};

static char tomb_marker;
#define TOMB (&tomb_marker)

static struct mb2s_entry *tab = NULL;
static size_t tab_cap = 0;   /* a power of two */
static size_t tab_used = 0;  /* live entries plus tombstones */

static uint64_t hash_str(const char *s)
{
	uint64_t h = 1469598103934665603ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static int live(const struct mb2s_entry *e) { return e->path && e->path != TOMB; }

/* lookup returns path's entry, or NULL. */
static struct mb2s_entry *lookup(const char *path)
{
	if (tab_cap == 0) return NULL;
	for (size_t i = hash_str(path) & (tab_cap - 1);; i = (i + 1) & (tab_cap - 1)) {
		struct mb2s_entry *e = &tab[i];
		if (e->path == NULL) return NULL;
		if (live(e) && strcmp(e->path, path) == 0) return e;
	}
}

/* slot_for_insert returns path's entry if present, else a free slot. */
static struct mb2s_entry *slot_for_insert(const char *path)
{
	struct mb2s_entry *tomb = NULL;
	for (size_t i = hash_str(path) & (tab_cap - 1);; i = (i + 1) & (tab_cap - 1)) {
		struct mb2s_entry *e = &tab[i];
		if (e->path == NULL) return tomb ? tomb : e;
		if (e->path == TOMB) {
			if (!tomb) tomb = e;
		} else if (strcmp(e->path, path) == 0) {
			return e;
		}
	}
}

static int grow(void)
{
	size_t ncap = tab_cap ? tab_cap * 2 : 1024;
	struct mb2s_entry *old = tab;
	size_t ocap = tab_cap;
	struct mb2s_entry *ntab = calloc(ncap, sizeof(*ntab));
	if (!ntab) return -1;
	tab = ntab;
	tab_cap = ncap;
	tab_used = 0;
	for (size_t i = 0; i < ocap; i++) {
		if (live(&old[i])) {
			*slot_for_insert(old[i].path) = old[i];
			tab_used++;
		}
	}
	free(old);
	return 0;
}

static int index_put(const char *path, uint64_t size, time_t mtime)
{
	if ((tab_used + 1) * 10 >= tab_cap * 7 && grow() < 0) return -1;
	struct mb2s_entry *e = slot_for_insert(path);
	if (live(e)) {
		e->size = size;
		e->mtime = mtime;
		return 0;
	}
	char *p = strdup(path);
	if (!p) return -1;
	if (e->path == NULL) tab_used++;
	e->path = p;
	e->size = size;
	e->mtime = mtime;
	return 0;
}

static void index_del(struct mb2s_entry *e)
{
	free(e->path);
	e->path = TOMB;
}

/* under reports whether path is inside folder dir (dir without a trailing slash). */
static int under(const char *path, const char *dir, size_t dlen)
{
	return strncmp(path, dir, dlen) == 0 && path[dlen] == '/';
}

/* trim copies dir without trailing slashes. */
static char *trim(const char *dir)
{
	char *d = strdup(dir);
	if (!d) return NULL;
	size_t n = strlen(d);
	while (n > 0 && d[n - 1] == '/') d[--n] = '\0';
	return d;
}

/* ---- public API ---- */

static int is_hex(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return 0;
	return 1;
}

int mb2s_is_content_path(const char *rel)
{
	const char *p = strchr(rel, '/');
	if (!p || p == rel) return 0;
	p++;
	if (strncmp(p, "Snapshot/", 9) == 0) p += 9;
	if (strlen(p) != 43 || p[2] != '/') return 0;
	return is_hex(p, 2) && is_hex(p + 3, 40) && strncmp(p, p + 3, 2) == 0;
}

static char *cur_name = NULL;
static uint64_t cur_bytes = 0;
static int have_ended = 0;

int mb2s_file_begin(const char *name)
{
	if (cur_name) { failed = 1; return -1; }
	cur_name = strdup(name);
	if (!cur_name) { failed = 1; return -1; }
	cur_bytes = 0;
	if (put_u8('F') < 0 || put_str(name, 0) < 0) return -1;
	return 0;
}

int mb2s_file_data(const char *buf, uint32_t len)
{
	if (!cur_name) { failed = 1; return -1; }
	if (put_u8('D') < 0 || put_u32(len) < 0 || put(buf, len) < 0) return -1;
	cur_bytes += len;
	return 0;
}

int mb2s_file_end(void)
{
	if (!cur_name) { failed = 1; return -1; }
	if (put_u8('E') < 0 || put_u64(cur_bytes) < 0) return -1;
	if (index_put(cur_name, cur_bytes, time(NULL)) < 0) { failed = 1; return -1; }
	free(cur_name);
	cur_name = NULL;
	have_ended = 1;
	return 0;
}

int mb2s_file_error(const char *msg)
{
	if (!have_ended) return 0;
	if (put_u8('X') < 0 || put_str(msg ? msg : "", 1) < 0) return -1;
	return 0;
}

struct moving { char *to; uint64_t size; time_t mtime; };

int mb2s_move(const char *from, const char *to)
{
	int moved = 0;
	struct mb2s_entry *e = lookup(from);
	if (e) {
		uint64_t size = e->size;
		time_t mt = e->mtime;
		index_del(e);
		if (index_put(to, size, mt) < 0) { failed = 1; return -1; }
		moved = 1;
	} else {
		char *f = trim(from);
		char *t = trim(to);
		if (!f || !t) { free(f); free(t); failed = 1; return -1; }
		size_t flen = strlen(f), n = 0, cap = 0;
		struct moving *list = NULL;
		for (size_t i = 0; i < tab_cap; i++) {
			if (!live(&tab[i]) || !under(tab[i].path, f, flen)) continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				struct moving *nl = realloc(list, cap * sizeof(*list));
				if (!nl) { failed = 1; break; }
				list = nl;
			}
			size_t tolen = strlen(t) + strlen(tab[i].path + flen) + 1;
			list[n].to = malloc(tolen);
			if (!list[n].to) { failed = 1; break; }
			snprintf(list[n].to, tolen, "%s%s", t, tab[i].path + flen);
			list[n].size = tab[i].size;
			list[n].mtime = tab[i].mtime;
			index_del(&tab[i]);
			n++;
		}
		for (size_t k = 0; k < n; k++) {
			if (!failed && index_put(list[k].to, list[k].size, list[k].mtime) < 0) failed = 1;
			free(list[k].to);
		}
		free(list);
		free(f);
		free(t);
		if (failed) return -1;
		moved = (int)n;
	}
	if (moved > 0 && (put_u8('M') < 0 || put_str(from, 0) < 0 || put_str(to, 0) < 0)) return -1;
	return moved;
}

int mb2s_remove(const char *path)
{
	int n = 0;
	struct mb2s_entry *e = lookup(path);
	if (e) {
		index_del(e);
		n = 1;
	} else {
		char *d = trim(path);
		if (!d) { failed = 1; return -1; }
		size_t dlen = strlen(d);
		for (size_t i = 0; i < tab_cap; i++) {
			if (live(&tab[i]) && under(tab[i].path, d, dlen)) {
				index_del(&tab[i]);
				n++;
			}
		}
		free(d);
	}
	if (n > 0 && (put_u8('R') < 0 || put_str(path, 0) < 0)) return -1;
	return n;
}

int mb2s_contains(const char *path)
{
	if (lookup(path)) return 1;
	char *d = trim(path);
	if (!d) return 0;
	size_t dlen = strlen(d);
	int found = 0;
	for (size_t i = 0; i < tab_cap && !found; i++) {
		if (live(&tab[i]) && under(tab[i].path, d, dlen)) found = 1;
	}
	free(d);
	return found;
}

void mb2s_list(const char *dir, mb2s_list_cb cb, void *ctx)
{
	char *d = trim(dir);
	if (!d) return;
	size_t dlen = strlen(d);
	for (size_t i = 0; i < tab_cap; i++) {
		if (!live(&tab[i]) || !under(tab[i].path, d, dlen)) continue;
		const char *rest = tab[i].path + dlen + 1;
		const char *slash = strchr(rest, '/');
		if (!slash) {
			cb(rest, 0, tab[i].size, tab[i].mtime, ctx);
		} else {
			char name[256];
			size_t n = (size_t)(slash - rest);
			if (n < sizeof(name)) {
				memcpy(name, rest, n);
				name[n] = '\0';
				cb(name, 1, 0, tab[i].mtime, ctx);
			}
		}
	}
	free(d);
}

int mb2s_mtime(uint64_t t)
{
	return put_u8('T') < 0 || put_u64(t) < 0 ? -1 : 0;
}

int mb2s_skip(const char *path, const char *reason, char kind)
{
	if (put_u8('S') < 0 || put_str(path, 1) < 0 || put_str(reason ? reason : "", 1) < 0 || put_u8((uint8_t)kind) < 0)
		return -1;
	return 0;
}

int mb2s_end_totals(uint32_t files, uint64_t bytes)
{
	if (put_u8('Z') < 0 || put_u32(files) < 0 || put_u64(bytes) < 0 || flush_out() < 0) return -1;
	return failed ? -1 : 0;
}

int mb2s_finish(void)
{
	if (put_u8('Z') < 0 || flush_out() < 0) return -1;
	return failed ? -1 : 0;
}
