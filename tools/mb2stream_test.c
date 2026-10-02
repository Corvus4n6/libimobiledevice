/* Standalone tests for mb2stream.c:
 *   cc -I. -o /tmp/mb2stream_test tools/mb2stream_test.c tools/mb2stream.c && /tmp/mb2stream_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mb2stream.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define H "0123456789abcdef0123456789abcdef01234567"

static void test_content_paths(void) {
	CHECK(mb2s_is_content_path("U/Snapshot/01/" H));
	CHECK(mb2s_is_content_path("U/01/" H));
	CHECK(!mb2s_is_content_path("U/Snapshot/Manifest.db"));
	CHECK(!mb2s_is_content_path("U/Status.plist"));
	CHECK(!mb2s_is_content_path("U/02/" H));          /* folder doesn't match the hash */
	CHECK(!mb2s_is_content_path("U/01/" H "x"));      /* too long */
	CHECK(!mb2s_is_content_path("01/" H));            /* no UDID */
	CHECK(!mb2s_is_content_path("U/01/0123456789ABCDEF0123456789abcdef0123456g"));
}

struct seen { int n; char names[16][64]; int dirs; };
static void collect(const char *name, int is_dir, uint64_t size, time_t mtime, void *ctx) {
	struct seen *s = ctx;
	(void)size; (void)mtime;
	if (s->n < 16) snprintf(s->names[s->n], 64, "%s%s", name, is_dir ? "/" : "");
	s->n++;
	s->dirs += is_dir;
}
static int has(struct seen *s, const char *name) {
	for (int i = 0; i < s->n && i < 16; i++) if (!strcmp(s->names[i], name)) return 1;
	return 0;
}

static void test_records_and_index(void) {
	char path[] = "/tmp/mb2stream_test_XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(mb2s_open(fd) == 0);
	CHECK(mb2s_file_begin("U/Snapshot/01/" H) == 0);
	CHECK(mb2s_file_data("hello", 5) == 0);
	CHECK(mb2s_file_data(" world", 6) == 0);
	CHECK(mb2s_file_end() == 0);
	CHECK(mb2s_file_error("oops") == 0);
	CHECK(mb2s_contains("U/Snapshot/01/" H));
	CHECK(mb2s_contains("U/Snapshot"));           /* a folder holding a streamed file */
	CHECK(!mb2s_contains("U/Snapshot/02"));

	struct seen s = {0};
	mb2s_list("U/Snapshot", collect, &s);
	CHECK(s.n == 1 && s.dirs == 1 && has(&s, "01/"));
	memset(&s, 0, sizeof s);
	mb2s_list("U/Snapshot/01/", collect, &s);     /* a trailing slash is ignored */
	CHECK(s.n == 1 && has(&s, H));

	CHECK(mb2s_move("U/Snapshot/01/" H, "U/01/" H) == 1);
	CHECK(!mb2s_contains("U/Snapshot/01/" H));
	CHECK(mb2s_contains("U/01/" H));
	CHECK(mb2s_move("U/Snapshot/nothing", "U/nothing") == 0);   /* not streamed: no record */
	CHECK(mb2s_remove("U/Snapshot") == 0);                       /* empty now: no record */
	CHECK(mb2s_remove("U/01") == 1);                             /* a folder: prefix removal */
	CHECK(!mb2s_contains("U/01/" H));
	CHECK(mb2s_finish() == 0);
	CHECK(!mb2s_failed());

	/* Read the records back. */
	unsigned char buf[512];
	ssize_t n = pread(fd, buf, sizeof buf, 0);
	close(fd);
	unlink(path);
	size_t i = 0;
	#define U16(p) ((size_t)((p)[0] << 8 | (p)[1]))
	CHECK(n > 0 && buf[i] == 'F'); i++;
	size_t len = U16(buf + i); i += 2;
	CHECK(len == strlen("U/Snapshot/01/" H) && !memcmp(buf + i, "U/Snapshot/01/" H, len)); i += len;
	CHECK(buf[i] == 'D'); i++;
	CHECK(buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 5); i += 4;
	CHECK(!memcmp(buf + i, "hello", 5)); i += 5;
	CHECK(buf[i] == 'D'); i += 1 + 4 + 6;
	CHECK(buf[i] == 'E'); i++;
	CHECK(buf[i+7] == 11); i += 8;
	CHECK(buf[i] == 'X'); i++;
	CHECK(U16(buf + i) == 4 && !memcmp(buf + i + 2, "oops", 4)); i += 2 + 4;
	CHECK(buf[i] == 'M'); i++;
	len = U16(buf + i); i += 2 + len;
	len = U16(buf + i); CHECK(!memcmp(buf + i + 2, "U/01/" H, len)); i += 2 + len;
	CHECK(buf[i] == 'R'); i++;
	len = U16(buf + i); CHECK(len == 4 && !memcmp(buf + i + 2, "U/01", 4)); i += 2 + len;
	CHECK(buf[i] == 'Z'); i++;
	CHECK((ssize_t)i == n);
}

static void test_many_entries(void) {
	char path[] = "/tmp/mb2stream_many_XXXXXX";
	int fd = mkstemp(path);
	mb2s_open(fd);
	char name[64];
	for (int k = 0; k < 20000; k++) {
		snprintf(name, sizeof name, "U/Snapshot/%02x/%02x%038x", k & 0xff, k & 0xff, k);
		CHECK(mb2s_file_begin(name) == 0);
		CHECK(mb2s_file_end() == 0);
	}
	int moved = 0;
	for (int k = 0; k < 20000; k++) {
		char to[64];
		snprintf(name, sizeof name, "U/Snapshot/%02x/%02x%038x", k & 0xff, k & 0xff, k);
		snprintf(to, sizeof to, "U/%02x/%02x%038x", k & 0xff, k & 0xff, k);
		moved += mb2s_move(name, to);
	}
	CHECK(moved == 20000);
	struct seen s = {0};
	mb2s_list("U/00", collect, &s);
	CHECK(s.n == 20000 / 256 + 1);   /* k & 0xff == 0: 79 of 20,000 */
	close(fd);
	unlink(path);
}

int main(void) {
	test_content_paths();
	test_records_and_index();
	test_many_entries();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	printf("mb2stream: all tests passed\n");
	return 0;
}
