#ifndef QSDB_H
#define QSDB_H

/*
 * qsdb: process-crash-atomic ordered key/value store.
 *
 * The durable index is a paged copy-on-write B+ tree. Pages reachable from
 * an already published root are never overwritten. A commit publishes a new
 * root only after the new pages have been written and flushed.
 *
 * This is not a production database: freed pages are not reclaimed online,
 * so repeated updates grow the file until an offline rebuild.
 *
 * Readers may map fixed windows that lie entirely inside the published
 * high-water mark. A window is never extended in place, and bytes past the
 * published prefix are read with pread so a failed transaction can drop
 * its extension without unmapping another process's view of old pages.
 * Windows has the same shape (CreateFileMapping of an existing range,
 * PAGE_READONLY) and is not exercised by the Linux tests.
 *
 * What a process crash means here: after this process dies, a later open
 * sees either the previous root or the new root, not a mix, as long as the
 * kernel retains writes that fsync already reported as successful. The
 * in-process kill tests check that. They do not drop the page cache and
 * they are not a power-loss test. Power loss is the same protocol only if
 * the device honors fsync; that was not measured on hardware.
 *
 * QSDB_UNCERTAIN means the new metadata may or may not be visible. This
 * process will not reuse those page ids. It is not a promise that a crash
 * plus a disk that reorders writes will still land on one of the two roots.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QSDB_OK = 0,
    QSDB_NOT_FOUND = 1,
    QSDB_CORRUPT = 2,
    QSDB_NOSPC = 3,
    QSDB_BUSY = 4,
    QSDB_INVAL = 5,
    QSDB_IO = 6,
    QSDB_UNCERTAIN = 7,
    QSDB_TOO_BIG = 8,
    QSDB_READONLY = 9,
    QSDB_MISMATCH = 10
};

enum {
    QSDB_OPEN_CREATE = 1,
    QSDB_OPEN_UTF16 = 2,     /* keys are UTF-16LE code units; order is code-unit order */
    QSDB_OPEN_READONLY = 4
};

/* Format caps. They are not a measured capacity. */
enum {
    QSDB_MAX_KEY_BYTES = 1 << 20,          /* 1 MiB */
    QSDB_MAX_VALUE_BYTES = 0xFFFFFFFFu     /* UINT32_MAX, same width as RegSetValueEx cbData */
};

enum {
    QSDB_TAG_INVALID = 1, /* key is present, payload empty */
    QSDB_TAG_BYTES = 2,
    QSDB_TAG_OPAQUE = 10  /* caller-defined payload; core does not interpret it */
};

enum {
    /* Test hook: _exit at this point inside commit. 0 disables. */
    QSDB_CRASH_NONE = 0,
    QSDB_CRASH_AFTER_PAGES = 1,     /* new pages written, before data fsync */
    QSDB_CRASH_AFTER_DATA_FSYNC = 2,
    QSDB_CRASH_TORN_META = 3,       /* partial metadata write, no metadata fsync */
    QSDB_CRASH_AFTER_META_FSYNC = 4 /* commit is durable, then the process dies */
};

typedef struct qsdb qsdb;
typedef struct qsdb_read qsdb_read;
typedef struct qsdb_write qsdb_write;

typedef struct qsdb_stat {
    uint64_t generation;
    uint64_t nkeys;
    uint64_t high_water_pages;
    uint64_t logical_bytes;
    uint64_t physical_bytes;
    uint32_t page_size;
    uint32_t cmp_mode; /* 1 byte, 2 utf16le code units */
} qsdb_stat;

/* return non-zero to stop the scan */
typedef int (*qsdb_scan_fn)(const void *key, size_t key_len,
                            uint32_t tag, uint64_t value_len,
                            void *user);

int qsdb_open(const char *path, int flags, qsdb **out);
void qsdb_close(qsdb *db);
int qsdb_info(qsdb *db, qsdb_stat *out);

int qsdb_begin_read(qsdb *db, qsdb_read **out);
void qsdb_end_read(qsdb_read *tx);
uint64_t qsdb_read_generation(const qsdb_read *tx);

/* buf == NULL queries the size. Otherwise copies min(buf_cap, remaining) from offset. */
int qsdb_get(qsdb_read *tx, const void *key, size_t key_len,
             uint32_t *tag, void *buf, size_t buf_cap, size_t *out_len,
             uint64_t offset);
/* One tree walk. *out_ptr is malloc'd and must be free()'d. Not found leaves it NULL. */
int qsdb_get_alloc(qsdb_read *tx, const void *key, size_t key_len,
                   uint32_t *tag, void **out_ptr, size_t *out_len);
int qsdb_contains(qsdb_read *tx, const void *key, size_t key_len);

int qsdb_scan(qsdb_read *tx, const void *start, size_t start_len,
              qsdb_scan_fn fn, void *user);
/* end == NULL scans through the last key. Otherwise end is exclusive. */
int qsdb_scan_range(qsdb_read *tx, const void *start, size_t start_len,
                    const void *end, size_t end_len,
                    qsdb_scan_fn fn, void *user);

int qsdb_begin_write(qsdb *db, qsdb_write **out);
int qsdb_put(qsdb_write *tx, const void *key, size_t key_len,
             uint32_t tag, const void *val, size_t val_len);
int qsdb_erase(qsdb_write *tx, const void *key, size_t key_len);
/* Delete key itself and keys that start with key + sep. Does not delete key+"bar". */
int qsdb_erase_tree(qsdb_write *tx, const void *key, size_t key_len,
                    const void *sep, size_t sep_len);
int qsdb_clear(qsdb_write *tx);
int qsdb_commit(qsdb_write *tx);
void qsdb_abort(qsdb_write *tx);

/* Test controls. Not part of the durable format. */
void qsdb_debug_crash_at(qsdb *db, int phase);
void qsdb_debug_fail_fsync(qsdb *db, int which); /* 1 data fsync, 2 meta fsync */
void qsdb_debug_max_bytes(qsdb *db, uint64_t bytes); /* 0 = no limit */
/* The next N fsync calls behave as if they returned EINTR, then a real fsync runs. */
void qsdb_debug_eintr_fsync(qsdb *db, int times);
/* Next commit uses this generation as the current one. UINT64_MAX must fail. */
void qsdb_debug_force_generation(qsdb *db, uint64_t generation);
uint64_t qsdb_debug_root(qsdb *db);
/* 1 if this page is inside a live read-only window, else 0. */
int qsdb_debug_page_mapped(qsdb *db, uint64_t page_id);

#ifdef __cplusplus
}
#endif

#endif
