/*
 * idevicefilesharing.c
 * Copy an app's iTunes File Sharing "Documents" folder off a device.
 *
 * Copyright (c) 2026 Lexeprint Inc. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Unlike ifuse, this needs no FUSE (so no macFUSE on a Mac, and it works
 * on Windows): it reads the app's Documents folder through house_arrest +
 * AFC and writes a copy to a local folder. It is read-only toward the
 * device by construction — it only ever opens device files for reading.
 *
 * Unlike afcclient's "get -r", it is built for unattended collection:
 *   - anything that isn't a regular file or folder (named pipes, sockets,
 *     symlinks, devices) is skipped WITHOUT being opened — a Realm
 *     database's named pipes made a plain recursive copy fail;
 *   - a file that can't be copied is reported and the copy carries on;
 *   - file modification times are kept;
 *   - every item is reported on stdout as one tab-separated line, and the
 *     exit status says how it went.
 *
 * Output (stdout, one line per item; <path> is relative to Documents,
 * with '%', tab, CR and LF percent-escaped):
 *   FILE  <path>  <bytes>
 *   SKIP  <path>  <reason>
 *   ERROR <path>  <message>
 *   INBACKUP <path>          (only with --skip-from: not copied)
 *   DONE  <files> <bytes> <skipped> <errors> [<in-backup>]
 *
 * --skip-from FILE lists paths (relative to Documents, separated by NUL
 * bytes) not to copy — files the device backup already holds — so only
 * what the backup lacks is collected. Listed files are never opened.
 * --stream sends the files to stdout as records instead of writing
 * DEST_DIR (see mb2stream.h: F/T/D/E per file, X after E for a device
 * read error, S per item left out, Z with the totals); everything
 * human-readable goes to stderr. --skip-from - reads the list from stdin.
 *
 * Exit status: 0 copied (items may have been skipped), 2 copied with
 * errors, 3 the app has no File Sharing folder (InstallationLookupFailed),
 * 1 anything else (usage, device, service).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define TOOL_NAME "idevicefilesharing"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#define local_mkdir(p) _mkdir(p)
#else
#define local_mkdir(p) mkdir(p, 0700)
#endif

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/afc.h>
#include <plist/plist.h>

#include "mb2stream.h"

static int stream_mode = 0;
/* --media: read named files from the media folder (the AFC root,
 * /var/mobile/Media) instead of an app's Documents. Needs --stream. */
static int media_mode = 0;
static uint64_t n_files = 0, n_bytes = 0, n_skipped = 0, n_errors = 0, n_inbackup = 0;

/* The --skip-from list, sorted for binary search. */
static char **skip_paths = NULL;
static size_t skip_count = 0;

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char * const *)a, *(char * const *)b);
}

/* load_skip_list reads NUL-separated paths from path. */
static int load_skip_list(const char *path)
{
	FILE *f;
	if (!strcmp(path, "-")) {
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
		f = stdin;
	} else {
		f = fopen(path, "rb");
	}
	if (!f) {
		return -1;
	}
	size_t cap = 0, len = 0;
	char *buf = NULL;
	char chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		if (len + n + 1 > cap) {
			cap = (len + n + 1) * 2;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	if (f != stdin) {
		fclose(f);
	}
	if (!buf) {
		return 0;
	}
	buf[len] = '\0';
	size_t count = 0;
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\0') count++;
	}
	count++;
	skip_paths = calloc(count, sizeof(char *));
	char *p = buf, *end = buf + len;
	while (p < end) {
		size_t l = strlen(p);
		if (l > 0) {
			skip_paths[skip_count++] = p;
		}
		p += l + 1;
	}
	qsort(skip_paths, skip_count, sizeof(char *), cmp_str);
	return 0;
}

static int in_skip_list(const char *rel)
{
	return skip_count > 0 && bsearch(&rel, skip_paths, skip_count, sizeof(char *), cmp_str) != NULL;
}

/* print_escaped writes s with '%', tab, CR and LF percent-escaped, so a
 * device-supplied file name can't break the line format. */
static void print_escaped(const char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '%':  fputs("%25", stdout); break;
		case '\t': fputs("%09", stdout); break;
		case '\n': fputs("%0A", stdout); break;
		case '\r': fputs("%0D", stdout); break;
		default:   fputc(*s, stdout);
		}
	}
}

static void report(const char *kind, const char *rel, const char *detail)
{
	if (stream_mode) {
		/* FILE is the F/T/D/E records themselves. */
		if (!strcmp(kind, "SKIP")) mb2s_skip(rel, detail, 's');
		else if (!strcmp(kind, "ERROR")) mb2s_skip(rel, detail, 'e');
		else if (!strcmp(kind, "INBACKUP")) mb2s_skip(rel, "", 'b');
		return;
	}
	fputs(kind, stdout);
	fputc('\t', stdout);
	print_escaped(rel);
	fputc('\t', stdout);
	fputs(detail, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

static void report_error(const char *rel, const char *fmt_detail, const char *arg, int code)
{
	char msg[512];
	snprintf(msg, sizeof(msg), fmt_detail, arg, code);
	report("ERROR", rel, msg);
	n_errors++;
}

/* safe_name refuses a device-supplied entry name that could escape the
 * destination folder or can't be a single local path component. */
static int safe_name(const char *name)
{
	if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
		return 0;
	}
	if (strchr(name, '/')) {
		return 0;
	}
#ifdef _WIN32
	if (strpbrk(name, "\\:*?\"<>|")) {
		return 0;
	}
#endif
	return 1;
}

static char *join(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	char *out = malloc(la + 1 + lb + 1);
	memcpy(out, a, la);
	out[la] = '/';
	memcpy(out + la + 1, b, lb + 1);
	return out;
}

/* copy_file copies one regular device file to local; returns bytes copied
 * or -1 after reporting an error. The device file is only ever opened
 * read-only. */
static int64_t copy_file(afc_client_t afc, const char *dev, const char *local, const char *rel, const char *mtime_ns)
{
	uint64_t fh = 0;
	afc_error_t err = afc_file_open(afc, dev, AFC_FOPEN_RDONLY, &fh);
	if (err != AFC_E_SUCCESS) {
		report_error(rel, "opening on the device failed: %s (%d)", afc_strerror(err), err);
		return -1;
	}
	if (stream_mode) {
		size_t bs = 0x100000;
		char *b = malloc(bs);
		if (!b) {
			afc_file_close(afc, fh);
			report_error(rel, "out of memory (%s, %d)", "malloc", 0);
			return -1;
		}
		int64_t sent = 0;
		int good = 1;
		mb2s_file_begin(rel);
		mb2s_mtime(mtime_ns ? strtoull(mtime_ns, NULL, 10) / 1000000000ULL : 0);
		while (1) {
			uint32_t got = 0;
			err = afc_file_read(afc, fh, b, bs, &got);
			if (err != AFC_E_SUCCESS) {
				char msg[256];
				snprintf(msg, sizeof(msg), "reading from the device failed: %s (%d)", afc_strerror(err), err);
				mb2s_file_end();
				mb2s_file_error(msg);
				report_error(rel, "reading from the device failed: %s (%d)", afc_strerror(err), err);
				good = 0;
				break;
			}
			if (got == 0) {
				break;
			}
			if (mb2s_file_data(b, got) < 0) {
				fprintf(stderr, "ERROR: writing the stream failed\n");
				good = 0;
				break;
			}
			sent += got;
		}
		free(b);
		afc_file_close(afc, fh);
		if (good && mb2s_file_end() < 0) {
			good = 0;
		}
		return good ? sent : -1;
	}
	FILE *f = fopen(local, "wb");
	if (!f) {
		afc_file_close(afc, fh);
		report_error(rel, "creating the local copy failed: %s (%d)", strerror(errno), errno);
		return -1;
	}
	size_t bufsize = 0x100000;
	char *buf = malloc(bufsize);
	if (!buf) {
		fclose(f);
		afc_file_close(afc, fh);
		report_error(rel, "out of memory (%s, %d)", "malloc", 0);
		return -1;
	}
	int64_t total = 0;
	int ok = 1;
	while (1) {
		uint32_t got = 0;
		err = afc_file_read(afc, fh, buf, bufsize, &got);
		if (err != AFC_E_SUCCESS) {
			report_error(rel, "reading from the device failed: %s (%d)", afc_strerror(err), err);
			ok = 0;
			break;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, got, f) != got) {
			report_error(rel, "writing the local copy failed: %s (%d)", strerror(errno), errno);
			ok = 0;
			break;
		}
		total += got;
	}
	free(buf);
	afc_file_close(afc, fh);
	if (fclose(f) != 0 && ok) {
		report_error(rel, "writing the local copy failed: %s (%d)", strerror(errno), errno);
		ok = 0;
	}
	return ok ? total : -1;
}

/* set_mtime gives the local copy the device file's modification time
 * (st_mtime is nanoseconds since the epoch, as a string). Best-effort. */
static void set_mtime(const char *local, const char *mtime_ns)
{
	if (!mtime_ns) {
		return;
	}
	unsigned long long ns = strtoull(mtime_ns, NULL, 10);
	if (ns == 0) {
		return;
	}
	struct utimbuf t;
	t.actime = t.modtime = (time_t)(ns / 1000000000ULL);
	utime(local, &t);
}

static void copy_tree(afc_client_t afc, const char *dev, const char *local, const char *rel)
{
	char **entries = NULL;
	afc_error_t err = afc_read_directory(afc, dev, &entries);
	if (err != AFC_E_SUCCESS) {
		report_error(*rel ? rel : ".", "listing the folder failed: %s (%d)", afc_strerror(err), err);
		return;
	}
	for (char **p = entries; p && *p; p++) {
		if (!strcmp(*p, ".") || !strcmp(*p, "..")) {
			continue;
		}
		char *child_rel = *rel ? join(rel, *p) : strdup(*p);
		if (!safe_name(*p)) {
			report("SKIP", child_rel, "name not usable as a local file name");
			n_skipped++;
			free(child_rel);
			continue;
		}
		char *child_dev = join(dev, *p);
		char *child_local = local ? join(local, *p) : NULL;

		char **info = NULL;
		const char *ifmt = NULL, *size = NULL, *mtime = NULL;
		err = afc_get_file_info(afc, child_dev, &info);
		if (err != AFC_E_SUCCESS || !info) {
			if (err == AFC_E_OBJECT_NOT_FOUND) {
				report("SKIP", child_rel, "gone when read");
				n_skipped++;
			} else {
				report_error(child_rel, "reading file information failed: %s (%d)", afc_strerror(err), err);
			}
		} else {
			for (char **kv = info; kv[0] && kv[1]; kv += 2) {
				if (!strcmp(kv[0], "st_ifmt")) ifmt = kv[1];
				else if (!strcmp(kv[0], "st_size")) size = kv[1];
				else if (!strcmp(kv[0], "st_mtime")) mtime = kv[1];
			}
			(void)size;
			if (ifmt && !strcmp(ifmt, "S_IFDIR") && stream_mode) {
				copy_tree(afc, child_dev, NULL, child_rel);
			} else if (ifmt && !strcmp(ifmt, "S_IFDIR")) {
				if (local_mkdir(child_local) != 0 && errno != EEXIST) {
					report_error(child_rel, "creating the local folder failed: %s (%d)", strerror(errno), errno);
				} else {
					copy_tree(afc, child_dev, child_local, child_rel);
					set_mtime(child_local, mtime);
				}
			} else if (ifmt && !strcmp(ifmt, "S_IFREG") && in_skip_list(child_rel)) {
				report("INBACKUP", child_rel, "");
				n_inbackup++;
			} else if (ifmt && !strcmp(ifmt, "S_IFREG")) {
				int64_t n = copy_file(afc, child_dev, child_local, child_rel, mtime);
				if (n >= 0) {
					char num[32];
					snprintf(num, sizeof(num), "%lld", (long long)n);
					report("FILE", child_rel, num);
					n_files++;
					n_bytes += (uint64_t)n;
					if (!stream_mode) {
						set_mtime(child_local, mtime);
					}
				}
			} else {
				char why[64];
				snprintf(why, sizeof(why), "not a regular file (%s)", ifmt ? ifmt : "unknown type");
				report("SKIP", child_rel, why);
				n_skipped++;
			}
		}
		if (info) {
			afc_dictionary_free(info);
		}
		free(child_dev);
		free(child_local);
		free(child_rel);
	}
	afc_dictionary_free(entries);
}

static void print_usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-u UDID] [--skip-from FILE|-] BUNDLE_ID DEST_DIR\n", name);
	fprintf(stderr, "       %s [-u UDID] [--skip-from FILE|-] --stream BUNDLE_ID\n\n", name);
	fprintf(stderr, "Copy an app's File Sharing Documents folder to DEST_DIR (created if needed).\n");
	fprintf(stderr, "  --skip-from FILE   don't copy the paths listed in FILE (NUL-separated, relative to Documents; - for stdin)\n");
	fprintf(stderr, "  --stream           write the files to stdout as records instead of DEST_DIR\n");
	fprintf(stderr, "       %s [-u UDID] --stream --media PATH...   (read files from the media folder, e.g. PhotoData/Photos.sqlite)\n", name);
}

int main(int argc, char *argv[])
{
	const char *udid = NULL;
	int c;
	static struct option longopts[] = {
		{ "udid", required_argument, NULL, 'u' },
		{ "skip-from", required_argument, NULL, 's' },
		{ "stream", no_argument, NULL, 'S' },
		{ "media", no_argument, NULL, 'M' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	while ((c = getopt_long(argc, argv, "u:s:hSM", longopts, NULL)) != -1) {
		switch (c) {
		case 'u':
			udid = optarg;
			break;
		case 's':
			if (load_skip_list(optarg) != 0) {
				fprintf(stderr, "ERROR: Could not read the skip list %s: %s\n", optarg, strerror(errno));
				return 1;
			}
			break;
		case 'S':
			stream_mode = 1;
			break;
		case 'M':
			media_mode = 1;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}
	if (media_mode && !stream_mode) {
		fprintf(stderr, "ERROR: --media needs --stream\n");
		return 1;
	}
	if (media_mode ? argc - optind < 1 : argc - optind != (stream_mode ? 1 : 2)) {
		print_usage(argv[0]);
		return 1;
	}
	const char *appid = argv[optind];
	const char *dest = stream_mode ? NULL : argv[optind + 1];
	if (stream_mode && mb2s_open_stdout() < 0) {
		fprintf(stderr, "ERROR: Could not set up the output stream\n");
		return 1;
	}

	idevice_t device = NULL;
	lockdownd_client_t lockdown = NULL;
	lockdownd_service_descriptor_t service = NULL;
	house_arrest_client_t ha = NULL;
	afc_client_t afc = NULL;
	int ret = 1;

	if (idevice_new_with_options(&device, udid, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) {
		fprintf(stderr, "ERROR: No device found%s%s\n", udid ? ": " : "", udid ? udid : "");
		return 1;
	}
	if (media_mode) {
		/* Read-only: files are only opened for reading. */
		if (afc_client_start_service(device, &afc, TOOL_NAME) != AFC_E_SUCCESS || !afc) {
			fprintf(stderr, "ERROR: Could not start the media file service\n");
			idevice_free(device);
			return 1;
		}
		for (int i = optind; i < argc; i++) {
			const char *path = argv[i];
			char **info = NULL;
			const char *ifmt = NULL, *mtime = NULL;
			afc_error_t ierr = afc_get_file_info(afc, path, &info);
			if (ierr != AFC_E_SUCCESS || !info) {
				/* Only a genuinely missing file is "not found" ('s'); any
				 * other failure is an error ('e'), so a caller never
				 * mistakes an unreadable file for an absent one. */
				if (ierr == AFC_E_OBJECT_NOT_FOUND) {
					mb2s_skip(path, "not found", 's');
				} else {
					char msg[128];
					snprintf(msg, sizeof(msg), "reading file information failed: %s (%d)", afc_strerror(ierr), ierr);
					mb2s_skip(path, msg, 'e');
					n_errors++;
				}
				continue;
			}
			for (char **kv = info; kv[0] && kv[1]; kv += 2) {
				if (!strcmp(kv[0], "st_ifmt")) ifmt = kv[1];
				else if (!strcmp(kv[0], "st_mtime")) mtime = kv[1];
			}
			if (!ifmt || strcmp(ifmt, "S_IFREG")) {
				mb2s_skip(path, "not a regular file", 's');
			} else {
				int64_t n = copy_file(afc, path, NULL, path, mtime);
				if (n >= 0) {
					n_files++;
					n_bytes += (uint64_t)n;
				}
			}
			afc_dictionary_free(info);
		}
		ret = n_errors ? 2 : 0;
		if (mb2s_end_totals((uint32_t)n_files, n_bytes) < 0) {
			fprintf(stderr, "ERROR: writing the stream failed\n");
			ret = 1;
		}
		afc_client_free(afc);
		idevice_free(device);
		return ret;
	}
	do {
		lockdownd_error_t lerr = lockdownd_client_new_with_handshake(device, &lockdown, TOOL_NAME);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not connect to lockdownd: %s (%d)\n", lockdownd_strerror(lerr), lerr);
			break;
		}
		lerr = lockdownd_start_service(lockdown, HOUSE_ARREST_SERVICE_NAME, &service);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not start %s: %s (%d)\n", HOUSE_ARREST_SERVICE_NAME, lockdownd_strerror(lerr), lerr);
			break;
		}
		if (house_arrest_client_new(device, service, &ha) != HOUSE_ARREST_E_SUCCESS || !ha) {
			fprintf(stderr, "ERROR: Could not start the document sharing service\n");
			break;
		}
		if (house_arrest_send_command(ha, "VendDocuments", appid) != HOUSE_ARREST_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not send the house_arrest command\n");
			break;
		}
		plist_t dict = NULL;
		if (house_arrest_get_result(ha, &dict) != HOUSE_ARREST_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not get a result from the document sharing service\n");
			break;
		}
		plist_t node = plist_dict_get_item(dict, "Error");
		if (node) {
			char *str = NULL;
			plist_get_string_val(node, &str);
			fprintf(stderr, "ERROR: %s\n", str ? str : "unknown");
			if (str && !strcmp(str, "InstallationLookupFailed")) {
				fprintf(stderr, "The app '%s' is not installed, or has no File Sharing (UIFileSharingEnabled).\n", appid);
				ret = 3;
			}
			free(str);
			plist_free(dict);
			break;
		}
		plist_free(dict);
		if (afc_client_new_from_house_arrest_client(ha, &afc) != AFC_E_SUCCESS || !afc) {
			fprintf(stderr, "ERROR: Could not open the app's files\n");
			break;
		}
		if (stream_mode) {
			copy_tree(afc, "/Documents", NULL, "");
			if (mb2s_end_totals((uint32_t)n_files, n_bytes) < 0) {
				fprintf(stderr, "ERROR: writing the stream failed\n");
				break;
			}
			ret = n_errors ? 2 : 0;
			break;
		}
		if (local_mkdir(dest) != 0 && errno != EEXIST) {
			fprintf(stderr, "ERROR: Could not create %s: %s\n", dest, strerror(errno));
			break;
		}
		copy_tree(afc, "/Documents", dest, "");
		printf("DONE\t%llu\t%llu\t%llu\t%llu\t%llu\n", (unsigned long long)n_files, (unsigned long long)n_bytes,
			(unsigned long long)n_skipped, (unsigned long long)n_errors, (unsigned long long)n_inbackup);
		fflush(stdout);
		ret = n_errors ? 2 : 0;
	} while (0);

	if (afc) afc_client_free(afc);
	if (ha) house_arrest_client_free(ha);
	if (service) lockdownd_service_descriptor_free(service);
	if (lockdown) lockdownd_client_free(lockdown);
	idevice_free(device);
	return ret;
}
