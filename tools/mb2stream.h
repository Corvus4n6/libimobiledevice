/*
 * mb2stream.h — idevicebackup2's --stream mode.
 *
 * With --stream, a backup's content files (<UDID>/[Snapshot/]xx/<40 hex>)
 * are written to stdout as a record stream instead of into the backup
 * folder; everything else (Info.plist, Status.plist, Manifest.plist,
 * Manifest.db, anything unexpected) is still written to the folder. An
 * in-memory index of the streamed files answers the device's moves,
 * removals and folder listings. All human-readable output goes to stderr.
 *
 * Records (big-endian, 1-byte type):
 *   'F' u16 len, name      a content file starts (name as the device sent it)
 *   'D' u32 len, bytes     a data chunk of that file
 *   'E' u64 total          the file ended
 *   'X' u16 len, message   the device reported an error for the file that just ended
 *   'M' u16 len, from, u16 len, to   the device moved a streamed path
 *   'R' u16 len, path      the device removed a streamed path
 *   'Z'                    end of stream (written only when the backup succeeded)
 *
 * Tool streams (idevicefilesharing, idevicecrashreport --stream) use the
 * same F/D/E/X records plus:
 *   'T' u64 seconds        the file's modification time (right after 'F')
 *   'S' u16 len, path, u16 len, reason, u8 kind   an item left out
 *                          ('s' skipped, 'e' couldn't be copied, 'b' in the backup)
 *   'Z' u32 files, u64 bytes   end, with the totals
 */
#ifndef MB2STREAM_H
#define MB2STREAM_H

#include <stdint.h>
#include <time.h>

int mb2s_is_content_path(const char *rel);

int mb2s_open(int fd);
int mb2s_open_stdout(void);

int mb2s_file_begin(const char *name);
int mb2s_file_data(const char *buf, uint32_t len);
int mb2s_file_end(void);
int mb2s_file_error(const char *msg);

/* Tool streams (idevicefilesharing, idevicecrashreport --stream). */
int mb2s_mtime(uint64_t unix_seconds);                              /* 'T' u64, after 'F' */
int mb2s_skip(const char *path, const char *reason, char kind);     /* 'S' path, reason, u8 kind: 's' skipped, 'e' error, 'b' in backup */
int mb2s_end_totals(uint32_t files, uint64_t bytes);                /* 'Z' u32 files, u64 bytes, then flush */

int mb2s_move(const char *from, const char *to);
int mb2s_remove(const char *path);
int mb2s_contains(const char *path);

typedef void (*mb2s_list_cb)(const char *name, int is_dir, uint64_t size, time_t mtime, void *ctx);
void mb2s_list(const char *dir, mb2s_list_cb cb, void *ctx);

int mb2s_finish(void);
int mb2s_failed(void);

#endif
