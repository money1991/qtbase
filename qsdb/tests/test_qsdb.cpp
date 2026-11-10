#include "qsdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>
#include <map>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#endif

static int g_fail = 0;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static std::string as_utf16(const char *s)
{
    std::string o;
    for (; *s; ++s) {
        o.push_back(*s);
        o.push_back('\0');
    }
    return o;
}

static std::string u16(uint16_t a, uint16_t b)
{
    char buf[4];
    buf[0] = static_cast<char>(a & 0xff);
    buf[1] = static_cast<char>(a >> 8);
    buf[2] = static_cast<char>(b & 0xff);
    buf[3] = static_cast<char>(b >> 8);
    return std::string(buf, buf + 4);
}

static int scan_collect(const void *key, size_t key_len, uint32_t, uint64_t, void *user)
{
    std::vector<std::string> *out = static_cast<std::vector<std::string> *>(user);
    out->push_back(std::string(static_cast<const char *>(key), key_len));
    return 0;
}

static void put_str(qsdb_write *w, const char *k, const char *v)
{
    int rc = qsdb_put(w, k, strlen(k), QSDB_TAG_BYTES, v, strlen(v));
    expect(rc == QSDB_OK, "put");
}

static std::string get_str(qsdb_read *r, const char *k)
{
    uint32_t tag = 0;
    size_t n = 0;
    int rc = qsdb_get(r, k, strlen(k), &tag, NULL, 0, &n, 0);
    if (rc == QSDB_NOT_FOUND)
        return std::string("<missing>");
    expect(rc == QSDB_OK, "get size");
    std::string s(n, '\0');
    size_t got = 0;
    rc = qsdb_get(r, k, strlen(k), &tag, &s[0], s.size(), &got, 0);
    expect(rc == QSDB_OK && got == n, "get bytes");
    s.resize(got);
    return s;
}

static void test_order_and_basic(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE | QSDB_OPEN_UTF16, &db) == QSDB_OK, "open");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "begin write");
    /* 0x00FF then 0x0100 must sort as code units, not raw UTF-16LE bytes. */
    std::string lo = u16(0x00FF, 0x0041);
    std::string hi = u16(0x0100, 0x0041);
    expect(qsdb_put(w, hi.data(), hi.size(), QSDB_TAG_BYTES, "H", 1) == QSDB_OK, "put hi");
    expect(qsdb_put(w, lo.data(), lo.size(), QSDB_TAG_BYTES, "L", 1) == QSDB_OK, "put lo");
    put_str(w, "foo", "1");
    put_str(w, "foobar", "no");
    put_str(w, "foo/a", "A");
    put_str(w, "foo/b", "B");
    put_str(w, "zzz", "z");
    expect(qsdb_put(w, "empty", 5, QSDB_TAG_INVALID, NULL, 0) == QSDB_OK, "invalid");
    expect(qsdb_commit(w) == QSDB_OK, "commit");

    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "read");
    expect(qsdb_contains(r, "empty", 5) == QSDB_OK, "contains invalid");
    expect(qsdb_contains(r, "nope", 4) == QSDB_NOT_FOUND, "missing");
    uint32_t tag = 0;
    size_t n = 0;
    expect(qsdb_get(r, "empty", 5, &tag, NULL, 0, &n, 0) == QSDB_OK, "get invalid");
    expect(tag == QSDB_TAG_INVALID && n == 0, "invalid payload");
    std::vector<std::string> keys;
    expect(qsdb_scan(r, NULL, 0, scan_collect, &keys) == QSDB_OK, "scan");
    expect(keys.size() == 7, "scan count");
    for (size_t i = 1; i < keys.size(); ++i) {
        const std::string &a = keys[i - 1];
        const std::string &b = keys[i];
        uint32_t an = static_cast<uint32_t>(a.size()) / 2;
        uint32_t bn = static_cast<uint32_t>(b.size()) / 2;
        uint32_t m = an < bn ? an : bn;
        int cmp = 0;
        for (uint32_t u = 0; u < m; ++u) {
            uint16_t ca = static_cast<uint8_t>(a[u * 2]) | (static_cast<uint16_t>(static_cast<uint8_t>(a[u * 2 + 1])) << 8);
            uint16_t cb = static_cast<uint8_t>(b[u * 2]) | (static_cast<uint16_t>(static_cast<uint8_t>(b[u * 2 + 1])) << 8);
            if (ca < cb) { cmp = -1; break; }
            if (ca > cb) { cmp = 1; break; }
        }
        if (cmp == 0)
            cmp = an < bn ? -1 : an > bn ? 1 : 0;
        expect(cmp < 0, "sorted");
    }
    /* lo (U+00FF) must come before hi (U+0100). Byte memcmp would do the opposite. */
    int lo_at = -1, hi_at = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == lo) lo_at = static_cast<int>(i);
        if (keys[i] == hi) hi_at = static_cast<int>(i);
    }
    expect(lo_at >= 0 && hi_at >= 0 && lo_at < hi_at, "utf16 code unit order");
    qsdb_end_read(r);

    expect(qsdb_begin_write(db, &w) == QSDB_OK, "w2");
    /* These keys are raw bytes, so the separator is a single slash byte.
     * UTF-16 callers pass the two-byte code unit 0x002F instead. */
    const char sep[] = {'/'};
    expect(qsdb_erase_tree(w, "foo", 3, sep, 1) == QSDB_OK, "erase tree");
    expect(qsdb_commit(w) == QSDB_OK, "commit erase");
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "r2");
    expect(get_str(r, "foo") == "<missing>", "foo gone");
    expect(get_str(r, "foo/a") == "<missing>", "foo/a gone");
    expect(get_str(r, "foobar") == "no", "foobar kept");
    expect(get_str(r, "zzz") == "z", "zzz kept");
    qsdb_end_read(r);

    expect(qsdb_begin_write(db, &w) == QSDB_OK, "w utf");
    std::string ufoo = as_utf16("foo");
    std::string ufoos = as_utf16("foo/a");
    std::string ufoobar = as_utf16("foobar");
    expect(qsdb_put(w, ufoo.data(), ufoo.size(), QSDB_TAG_BYTES, "1", 1) == QSDB_OK, "u foo");
    expect(qsdb_put(w, ufoos.data(), ufoos.size(), QSDB_TAG_BYTES, "2", 1) == QSDB_OK, "u foo/a");
    expect(qsdb_put(w, ufoobar.data(), ufoobar.size(), QSDB_TAG_BYTES, "3", 1) == QSDB_OK, "u foobar");
    const char usep[] = {'/', 0};
    expect(qsdb_erase_tree(w, ufoo.data(), ufoo.size(), usep, 2) == QSDB_OK, "utf16 erase tree");
    expect(qsdb_commit(w) == QSDB_OK, "commit utf erase");
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "r utf");
    expect(qsdb_contains(r, ufoo.data(), ufoo.size()) == QSDB_NOT_FOUND, "utf foo gone");
    expect(qsdb_contains(r, ufoos.data(), ufoos.size()) == QSDB_NOT_FOUND, "utf foo/a gone");
    expect(qsdb_get(r, ufoobar.data(), ufoobar.size(), &tag, NULL, 0, &n, 0) == QSDB_OK, "utf foobar kept");
    qsdb_end_read(r);

    expect(qsdb_begin_write(db, &w) == QSDB_OK, "w3");
    expect(qsdb_clear(w) == QSDB_OK, "clear");
    put_str(w, "only", "x");
    expect(qsdb_commit(w) == QSDB_OK, "commit clear");
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "r3");
    expect(get_str(r, "zzz") == "<missing>", "cleared");
    expect(get_str(r, "only") == "x", "after clear");
    qsdb_stat st;
    expect(qsdb_info(db, &st) == QSDB_OK, "info");
    expect(st.nkeys == 1, "nkeys");
    expect(st.logical_bytes >= 12288, "not empty file");
    qsdb_end_read(r);
    qsdb_close(db);
}

static void test_overflow_and_growth(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "open ovf");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "bw");
    std::string bigkey(200000, 'K');
    bigkey[0] = 'A';
    std::string bigval(2 * 1024 * 1024, 'V');
    bigval[100] = 'Q';
    expect(qsdb_put(w, bigkey.data(), bigkey.size(), QSDB_TAG_BYTES, bigval.data(), bigval.size()) == QSDB_OK, "big put");
    expect(qsdb_put(w, "small", 5, QSDB_TAG_BYTES, "s", 1) == QSDB_OK, "small");
    expect(qsdb_commit(w) == QSDB_OK, "commit big");
    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "br");
    size_t n = 0;
    expect(qsdb_get(r, bigkey.data(), bigkey.size(), NULL, NULL, 0, &n, 0) == QSDB_OK, "big size");
    expect(n == bigval.size(), "big len");
    std::string chunk(65536, '\0');
    size_t off = 0;
    int mismatch = 0;
    while (off < bigval.size()) {
        size_t got = 0;
        int rc = qsdb_get(r, bigkey.data(), bigkey.size(), NULL, &chunk[0], chunk.size(), &got, off);
        expect(rc == QSDB_OK, "stream");
        if (got == 0)
            break;
        if (memcmp(chunk.data(), bigval.data() + off, got) != 0)
            mismatch++;
        off += got;
    }
    expect(mismatch == 0 && off == bigval.size(), "stream matches");
    qsdb_stat before;
    qsdb_info(db, &before);
    qsdb_end_read(r);
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "bw2");
    expect(qsdb_put(w, "other", 5, QSDB_TAG_BYTES, "o", 1) == QSDB_OK, "one");
    expect(qsdb_commit(w) == QSDB_OK, "c2");
    qsdb_stat after;
    qsdb_info(db, &after);
    uint64_t grew = after.high_water_pages - before.high_water_pages;
    expect(grew > 0 && grew < 64, "single commit does not rewrite the tree");
    expect(after.physical_bytes > 12288, "grew from small file");
    qsdb_close(db);
}

static void test_random_oracle(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "open rnd");
    std::map<std::string, std::string> oracle;
    unsigned seed = 0xC0FFEE;
    for (int round = 0; round < 30; ++round) {
        qsdb_write *w = NULL;
        expect(qsdb_begin_write(db, &w) == QSDB_OK, "rw");
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            int op = seed % 5;
            char key[16];
            int kn = static_cast<int>(seed % 50);
            snprintf(key, sizeof key, "k%04d", kn);
            if (op == 0) {
                char val[32];
                snprintf(val, sizeof val, "v%u", seed);
                expect(qsdb_put(w, key, strlen(key), QSDB_TAG_BYTES, val, strlen(val)) == QSDB_OK, "rp");
                oracle[key] = val;
            } else if (op == 1) {
                int rc = qsdb_erase(w, key, strlen(key));
                if (oracle.count(key)) {
                    expect(rc == QSDB_OK, "erase hit");
                    oracle.erase(key);
                } else {
                    expect(rc == QSDB_NOT_FOUND, "erase miss");
                }
            } else if (op == 2 && (seed % 7) == 0) {
                expect(qsdb_clear(w) == QSDB_OK, "clear rnd");
                oracle.clear();
            }
        }
        expect(qsdb_commit(w) == QSDB_OK, "commit rnd");
        qsdb_read *r = NULL;
        expect(qsdb_begin_read(db, &r) == QSDB_OK, "rr");
        for (std::map<std::string, std::string>::iterator it = oracle.begin(); it != oracle.end(); ++it) {
            if (get_str(r, it->first.c_str()) != it->second)
                expect(0, "oracle mismatch");
        }
        qsdb_stat st;
        qsdb_info(db, &st);
        expect(st.nkeys == oracle.size(), "nkeys oracle");
        qsdb_end_read(r);
    }
    qsdb_close(db);
}

static void test_enospc(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "open enospc");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "w");
    put_str(w, "keep", "yes");
    expect(qsdb_commit(w) == QSDB_OK, "c");
    qsdb_stat st;
    qsdb_info(db, &st);
    qsdb_debug_max_bytes(db, st.physical_bytes);
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "w2");
    std::string big(100000, 'B');
    int rc = qsdb_put(w, "new", 3, QSDB_TAG_BYTES, big.data(), big.size());
    expect(rc == QSDB_NOSPC || rc == QSDB_OK, "put may fail");
    if (rc == QSDB_OK)
        rc = qsdb_commit(w);
    else
        qsdb_abort(w);
    expect(rc == QSDB_NOSPC || rc == QSDB_OK, "commit space");
    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "still opens");
    expect(get_str(r, "keep") == "yes", "old value intact");
    qsdb_end_read(r);
    qsdb_close(db);
}

static void test_crash(const char *path, int phase, int expect_new)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "crash open");
    qsdb_write *w = NULL;
    qsdb_begin_write(db, &w);
    put_str(w, "old", "O");
    expect(qsdb_commit(w) == QSDB_OK, "seed");
    qsdb_close(db);
#if !defined(_WIN32)
    pid_t pid = fork();
    expect(pid >= 0, "fork");
    if (pid == 0) {
        qsdb *c = NULL;
        if (qsdb_open(path, 0, &c) != QSDB_OK)
            _exit(2);
        qsdb_debug_crash_at(c, phase);
        qsdb_write *cw = NULL;
        if (qsdb_begin_write(c, &cw) != QSDB_OK)
            _exit(3);
        if (qsdb_put(cw, "new", 3, QSDB_TAG_BYTES, "N", 1) != QSDB_OK)
            _exit(4);
        qsdb_commit(cw);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    expect(WIFEXITED(status), "child exited");
    db = NULL;
    expect(qsdb_open(path, 0, &db) == QSDB_OK, "reopen after crash");
    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "read after crash");
    std::string old = get_str(r, "old");
    std::string neu = get_str(r, "new");
    expect(old == "O", "old survived");
    if (expect_new)
        expect(neu == "N", "new visible after durable commit");
    else
        expect(neu == "<missing>", "new not visible before durable commit");
    /* No mixed tree: old is never missing when new is missing, and both reads succeed. */
    qsdb_end_read(r);
    qsdb_close(db);
#else
    (void)phase;
    (void)expect_new;
#endif
}

static void test_uncertain(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "unc open");
    qsdb_write *w = NULL;
    qsdb_begin_write(db, &w);
    put_str(w, "old", "O");
    qsdb_commit(w);
    qsdb_debug_fail_fsync(db, 2);
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "unc w");
    put_str(w, "new", "N");
    int rc = qsdb_commit(w);
    expect(rc == QSDB_UNCERTAIN, "uncertain commit");
    qsdb_close(db);
    expect(qsdb_open(path, 0, &db) == QSDB_OK, "reopen uncertain");
    qsdb_read *r = NULL;
    qsdb_begin_read(db, &r);
    std::string old = get_str(r, "old");
    std::string neu = get_str(r, "new");
    expect(old == "O", "old still there");
    expect(neu == "N" || neu == "<missing>", "all old or all new");
    if (neu == "<missing>")
        expect(old == "O", "consistent old");
    qsdb_end_read(r);
    qsdb_close(db);
}

static void test_snapshot_reader(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    qsdb_open(path, QSDB_OPEN_CREATE, &db);
    qsdb_write *w = NULL;
    qsdb_begin_write(db, &w);
    put_str(w, "k", "1");
    qsdb_commit(w);
    qsdb_read *r = NULL;
    qsdb_begin_read(db, &r);
    uint64_t gen = qsdb_read_generation(r);
    qsdb_begin_write(db, &w);
    put_str(w, "k", "2");
    put_str(w, "n", "3");
    qsdb_commit(w);
    expect(get_str(r, "k") == "1", "snapshot stays on old root");
    expect(get_str(r, "n") == "<missing>", "snapshot misses new key");
    qsdb_end_read(r);
    qsdb_begin_read(db, &r);
    expect(qsdb_read_generation(r) != gen, "new generation");
    expect(get_str(r, "k") == "2" && get_str(r, "n") == "3", "new snapshot");
    qsdb_end_read(r);
    qsdb_close(db);
}

static uint64_t mono_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

static uint64_t median_at(std::vector<uint64_t> s)
{
    if (s.empty())
        return 0;
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

static uint64_t p95_at(std::vector<uint64_t> s)
{
    if (s.empty())
        return 0;
    std::sort(s.begin(), s.end());
    return s[(s.size() - 1) * 95 / 100];
}

static int count_stop(const void *, size_t, uint32_t, uint64_t, void *user)
{
    int *left = static_cast<int *>(user);
    if (*left <= 0)
        return 1;
    *left -= 1;
    return 0;
}

static int count_scan(const void *, size_t, uint32_t, uint64_t, void *user)
{
    *static_cast<int *>(user) += 1;
    return 0;
}

static void test_range_delete(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "range open");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "range w");
    put_str(w, "a", "1");
    put_str(w, "foo", "2");
    put_str(w, "foobar", "3");
    put_str(w, "z", "4");
    for (int i = 0; i < 800; ++i) {
        char key[32];
        snprintf(key, sizeof key, "foo/%04d", i);
        put_str(w, key, "v");
    }
    expect(qsdb_commit(w) == QSDB_OK, "range commit");
    qsdb_stat st;
    qsdb_info(db, &st);
    expect(st.nkeys == 804, "range nkeys before");
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "range w2");
    const char sep[] = "/";
    expect(qsdb_erase_tree(w, "foo", 3, sep, 1) == QSDB_OK, "range erase");
    expect(qsdb_commit(w) == QSDB_OK, "range commit2");
    qsdb_info(db, &st);
    expect(st.nkeys == 3, "range nkeys after");
    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "range read");
    expect(get_str(r, "a") == "1", "range kept a");
    expect(get_str(r, "foobar") == "3", "range kept foobar");
    expect(get_str(r, "z") == "4", "range kept z");
    expect(get_str(r, "foo") == "<missing>", "range removed foo");
    expect(get_str(r, "foo/0000") == "<missing>", "range removed child");
    expect(get_str(r, "foo/0799") == "<missing>", "range removed last child");
    int seen = 0;
    expect(qsdb_scan(r, NULL, 0, count_scan, &seen) == QSDB_OK, "range scan");
    expect(seen == 3, "range scan count");
    qsdb_end_read(r);
    qsdb_close(db);
}

static int put_n(qsdb_write *w, const char *fmt, int begin, int count)
{
    for (int i = 0; i < count; ++i) {
        char key[32];
        int n = snprintf(key, sizeof key, fmt, begin + i);
        int rc = qsdb_put(w, key, static_cast<size_t>(n), QSDB_TAG_BYTES, "x", 1);
        if (rc != QSDB_OK)
            return rc;
    }
    return QSDB_OK;
}

static void test_reader_across_growth(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "grow open");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "grow w");
    put_str(w, "keep", "v1");
    /* Enough keys that the published file covers more than one 256 KiB window. */
    expect(put_n(w, "n%06d", 0, 40000) == QSDB_OK, "grow batch");
    expect(qsdb_commit(w) == QSDB_OK, "grow commit");
    qsdb_stat st;
    qsdb_info(db, &st);
    expect(st.physical_bytes > 512ull * 1024ull, "grew past one window");
    uint64_t root = qsdb_debug_root(db);
    qsdb_read *old = NULL;
    expect(qsdb_begin_read(db, &old) == QSDB_OK, "old reader");
    expect(get_str(old, "keep") == "v1", "old sees keep");
    expect(qsdb_debug_page_mapped(db, 2) == 1, "first tree page mapped");
    uint64_t root_end = (root / 64 + 1) * 64ull * 4096ull;
    if (st.physical_bytes >= root_end)
        expect(qsdb_debug_page_mapped(db, root) == 1, "root window mapped");
    uint64_t gen = qsdb_read_generation(old);
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "grow w2");
    expect(put_n(w, "m%06d", 0, 40000) == QSDB_OK, "grow batch2");
    expect(qsdb_commit(w) == QSDB_OK, "grow commit2");
    qsdb_info(db, &st);
    expect(st.physical_bytes > 1024ull * 1024ull, "file grew again");
    expect(qsdb_read_generation(old) == gen, "old generation stable");
    expect(get_str(old, "keep") == "v1", "old root still readable");
    expect(get_str(old, "m000000") == "<missing>", "old root hides new keys");
    expect(qsdb_debug_page_mapped(db, 2) == 1, "old window still mapped");
    qsdb_end_read(old);
    qsdb_read *neu = NULL;
    expect(qsdb_begin_read(db, &neu) == QSDB_OK, "new reader");
    expect(get_str(neu, "keep") == "v1", "new sees keep");
    expect(get_str(neu, "m000000") == "x", "new sees appended keys");
    expect(get_str(neu, "n000000") == "x", "new sees first batch");
    qsdb_end_read(neu);
    qsdb_close(db);
}

static void test_crc_and_fork(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "crc open");
    qsdb_write *w = NULL;
    qsdb_begin_write(db, &w);
    put_str(w, "a", "A");
    expect(qsdb_commit(w) == QSDB_OK, "crc commit");
    uint64_t root = qsdb_debug_root(db);
    qsdb_close(db);
    FILE *f = fopen(path, "r+b");
    expect(f != NULL, "crc fopen");
    if (f) {
        long off = static_cast<long>(root * 4096ull + 80ull);
        expect(fseek(f, off, SEEK_SET) == 0, "crc seek");
        int b = fgetc(f);
        expect(b >= 0, "crc read");
        fseek(f, off, SEEK_SET);
        fputc(b ^ 0x5a, f);
        fclose(f);
    }
    expect(qsdb_open(path, 0, &db) == QSDB_OK, "crc reopen meta");
    qsdb_read *r = NULL;
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "crc meta still valid");
    uint32_t tag = 0;
    size_t n = 0;
    int rc = qsdb_get(r, "a", 1, &tag, NULL, 0, &n, 0);
    expect(rc == QSDB_CORRUPT, "flipped tree page fails CRC");
    qsdb_end_read(r);
    qsdb_close(db);

#if !defined(_WIN32)
    unlink(path);
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "fork open");
    qsdb_begin_write(db, &w);
    put_str(w, "old", "O");
    expect(qsdb_commit(w) == QSDB_OK, "fork seed");
    qsdb_read *held = NULL;
    expect(qsdb_begin_read(db, &held) == QSDB_OK, "fork held");
    pid_t pid = fork();
    expect(pid >= 0, "fork merge");
    if (pid == 0) {
        qsdb *c = NULL;
        if (qsdb_open(path, 0, &c) != QSDB_OK)
            _exit(2);
        qsdb_write *cw = NULL;
        if (qsdb_begin_write(c, &cw) != QSDB_OK)
            _exit(3);
        if (qsdb_put(cw, "from-child", 10, QSDB_TAG_BYTES, "C", 1) != QSDB_OK)
            _exit(4);
        if (qsdb_commit(cw) != QSDB_OK)
            _exit(5);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child committed");
    expect(get_str(held, "old") == "O", "held reader keeps old");
    expect(get_str(held, "from-child") == "<missing>", "held reader misses child commit");
    qsdb_end_read(held);
    expect(qsdb_begin_read(db, &r) == QSDB_OK, "parent refresh");
    expect(get_str(r, "old") == "O", "parent sees old");
    expect(get_str(r, "from-child") == "C", "parent sees child after new snapshot");
    qsdb_end_read(r);
    qsdb_close(db);
#endif
}

static void test_million(const char *path)
{
    unlink(path);
    qsdb *db = NULL;
    expect(qsdb_open(path, QSDB_OPEN_CREATE, &db) == QSDB_OK, "m open");
    qsdb_write *w = NULL;
    expect(qsdb_begin_write(db, &w) == QSDB_OK, "m w");
    const int N = 1000000;
    char key[32];
    char val[16];
    for (int i = 0; i < N; ++i) {
        int n = snprintf(key, sizeof key, "k%07d", i);
        int m = snprintf(val, sizeof val, "%d", i);
        int rc = qsdb_put(w, key, static_cast<size_t>(n), QSDB_TAG_BYTES, val, static_cast<size_t>(m));
        if (rc != QSDB_OK) {
            fprintf(stderr, "million put failed at %d rc %d\n", i, rc);
            expect(0, "million put");
            qsdb_abort(w);
            qsdb_close(db);
            return;
        }
    }
    int rc = qsdb_commit(w);
    expect(rc == QSDB_OK, "million commit");
    qsdb_stat st;
    qsdb_info(db, &st);
    fprintf(stderr, "million logical_bytes=%llu physical_bytes=%llu pages=%llu nkeys=%llu\n",
            (unsigned long long)st.logical_bytes,
            (unsigned long long)st.physical_bytes,
            (unsigned long long)st.high_water_pages,
            (unsigned long long)st.nkeys);
    expect(st.nkeys == static_cast<uint64_t>(N), "million nkeys");
    expect(st.physical_bytes > 12288 && st.physical_bytes < (1ull << 32), "grew incrementally under 4GiB");
    qsdb_close(db);
    expect(qsdb_open(path, 0, &db) == QSDB_OK, "reopen million");
    qsdb_read *r = NULL;
    qsdb_begin_read(db, &r);
    unsigned seed = 1;
    std::vector<uint64_t> get_ns, miss_ns, contains_ns;
    get_ns.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int id = static_cast<int>(seed % N);
        snprintf(key, sizeof key, "k%07d", id);
        snprintf(val, sizeof val, "%d", id);
        char buf[32];
        uint32_t tag = 0;
        size_t gotn = 0;
        uint64_t t0 = mono_ns();
        int grc = qsdb_get(r, key, strlen(key), &tag, buf, sizeof buf, &gotn, 0);
        uint64_t t1 = mono_ns();
        get_ns.push_back(t1 - t0);
        if (grc != QSDB_OK || std::string(buf, gotn) != val) {
            expect(0, "million random get");
            break;
        }
    }
    for (int i = 0; i < 1000; ++i) {
        uint64_t t0 = mono_ns();
        int rc = qsdb_contains(r, "missing-million", 15);
        uint64_t t1 = mono_ns();
        miss_ns.push_back(t1 - t0);
        if (rc != QSDB_NOT_FOUND) {
            expect(0, "million miss");
            break;
        }
        t0 = mono_ns();
        rc = qsdb_contains(r, "k0000000", 8);
        t1 = mono_ns();
        contains_ns.push_back(t1 - t0);
        if (rc != QSDB_OK) {
            expect(0, "million contains");
            break;
        }
    }
    int left = 1000;
    uint64_t s0 = mono_ns();
    expect(qsdb_scan(r, "k0001000", 8, count_stop, &left) == QSDB_OK, "million scan");
    uint64_t s1 = mono_ns();
    fprintf(stderr,
            "million_core samples=1000 get_median_ns=%llu get_p95_ns=%llu miss_median_ns=%llu miss_p95_ns=%llu contains_median_ns=%llu contains_p95_ns=%llu scan_1000_ns=%llu\n",
            (unsigned long long)median_at(get_ns),
            (unsigned long long)p95_at(get_ns),
            (unsigned long long)median_at(miss_ns),
            (unsigned long long)p95_at(miss_ns),
            (unsigned long long)median_at(contains_ns),
            (unsigned long long)p95_at(contains_ns),
            (unsigned long long)(s1 - s0));
    expect(get_str(r, "k0000000") == "0", "first");
    expect(get_str(r, "k0999999") == "999999", "last");
    expect(qsdb_contains(r, "missing", 7) == QSDB_NOT_FOUND, "million miss");
    qsdb_end_read(r);
    uint64_t pages_before = 0;
    qsdb_info(db, &st);
    pages_before = st.high_water_pages;
    qsdb_begin_write(db, &w);
    qsdb_put(w, "extra", 5, QSDB_TAG_BYTES, "e", 1);
    qsdb_commit(w);
    qsdb_info(db, &st);
    uint64_t grew = st.high_water_pages - pages_before;
    fprintf(stderr, "million one-key page growth=%llu\n", (unsigned long long)grew);
    expect(grew > 0 && grew < 32, "one key does not copy a million-key index");
    uint64_t before_pages = st.high_water_pages;
    uint64_t before_bytes = st.physical_bytes;
    qsdb_begin_write(db, &w);
    uint64_t e0 = mono_ns();
    expect(qsdb_erase_tree(w, "k000", 4, "1", 1) == QSDB_OK, "million range");
    uint64_t e1 = mono_ns();
    expect(qsdb_commit(w) == QSDB_OK, "million range commit");
    uint64_t e2 = mono_ns();
    qsdb_info(db, &st);
    fprintf(stderr, "million_range_delete_ns=%llu range_commit_ns=%llu pages_after=%llu bytes_after=%llu nkeys=%llu\n",
            (unsigned long long)(e1 - e0), (unsigned long long)(e2 - e1),
            (unsigned long long)st.high_water_pages, (unsigned long long)st.physical_bytes,
            (unsigned long long)st.nkeys);
    expect(st.nkeys == static_cast<uint64_t>(N + 1 - 1000), "range removed 1000 keys");
    expect(st.high_water_pages >= before_pages, "range delete does not shrink high water");
    (void)before_bytes;
    qsdb_begin_write(db, &w);
    uint64_t c0 = mono_ns();
    expect(qsdb_clear(w) == QSDB_OK, "million clear");
    uint64_t c1 = mono_ns();
    expect(qsdb_commit(w) == QSDB_OK, "million clear commit");
    uint64_t c2 = mono_ns();
    fprintf(stderr, "million_clear_ns=%llu clear_commit_ns=%llu\n",
            (unsigned long long)(c1 - c0), (unsigned long long)(c2 - c1));
    qsdb_close(db);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/qsdb-basic.db", dir);
    test_order_and_basic(path);
    snprintf(path, sizeof path, "%s/qsdb-ovf.db", dir);
    test_overflow_and_growth(path);
    snprintf(path, sizeof path, "%s/qsdb-rnd.db", dir);
    test_random_oracle(path);
    snprintf(path, sizeof path, "%s/qsdb-enospc.db", dir);
    test_enospc(path);
    snprintf(path, sizeof path, "%s/qsdb-snap.db", dir);
    test_snapshot_reader(path);
    snprintf(path, sizeof path, "%s/qsdb-range.db", dir);
    test_range_delete(path);
    snprintf(path, sizeof path, "%s/qsdb-grow.db", dir);
    test_reader_across_growth(path);
    snprintf(path, sizeof path, "%s/qsdb-crc.db", dir);
    test_crc_and_fork(path);
    snprintf(path, sizeof path, "%s/qsdb-unc.db", dir);
    test_uncertain(path);
    struct {
        int phase;
        int expect_new;
        const char *name;
    } crashes[] = {
        {QSDB_CRASH_AFTER_PAGES, 0, "after-pages"},
        {QSDB_CRASH_AFTER_DATA_FSYNC, 0, "after-data-fsync"},
        {QSDB_CRASH_TORN_META, 0, "torn-meta"},
        {QSDB_CRASH_AFTER_META_FSYNC, 1, "after-meta-fsync"}
    };
    for (unsigned i = 0; i < sizeof crashes / sizeof crashes[0]; ++i) {
        snprintf(path, sizeof path, "%s/qsdb-crash-%s.db", dir, crashes[i].name);
        fprintf(stderr, "crash phase %s\n", crashes[i].name);
        test_crash(path, crashes[i].phase, crashes[i].expect_new);
    }
    int do_million = argc > 2 && strcmp(argv[2], "million") == 0;
    if (do_million) {
        snprintf(path, sizeof path, "%s/qsdb-million.db", dir);
        test_million(path);
    }
    fprintf(stderr, g_fail ? "FAILED %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
