#include "qsdb.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <sys/file.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

constexpr uint32_t kPage = 4096;
constexpr uint32_t kHdr = 48;
constexpr uint32_t kInline = 512;
constexpr uint64_t kExtent = 65536;
constexpr uint32_t kCmpBytes = 1;
constexpr uint32_t kCmpUtf16 = 2;

constexpr char kMetaMagic[4] = {'Q', 'M', 'E', 'T'};
constexpr char kPageMagic[4] = {'Q', 'S', 'P', 'G'};

enum PageType : uint16_t { kLeaf = 1, kInternal = 2, kOverflow = 3 };

uint32_t crc32_ieee(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* Host endian is whatever memcpy uses. The file format is defined as the
 * native layout of this build only when the host is little-endian, which is
 * the tested target. Multi-byte integers are written with memcpy of the host
 * value; the public comparison of UTF-16 keys does NOT use memcmp, so key
 * order stays code-unit order even if a later port swaps endianness of the
 * key bytes themselves. We reject a database whose endian stamp does not
 * match this process. */
constexpr uint32_t kEndianStamp = 0x01020304u;

struct Meta {
    uint64_t generation = 0;
    uint64_t root = 0;
    uint64_t high_water = 0;
    uint64_t nkeys = 0;
    uint32_t cmp = kCmpBytes;
    int slot = 0;
    bool valid = false;
};

struct Page {
    uint8_t b[kPage];
    Page() { memset(b, 0, sizeof b); }
};

struct Item {
    std::vector<uint8_t> key;
    std::vector<uint8_t> val;
    uint64_t key_ovf = 0;
    uint64_t val_ovf = 0;
    uint64_t child = 0;
    uint32_t tag = 0;
    uint32_t val_len = 0;
    bool key_is_ovf = false;
    bool val_is_ovf = false;
};

struct Ins {
    uint64_t page = 0;
    bool split = false;
    bool changed = false;
    std::vector<uint8_t> sep;
    uint64_t sep_ovf = 0;
    bool sep_is_ovf = false;
    uint64_t right = 0;
};

struct Del {
    uint64_t page = 0;
    bool changed = false;
    bool gone = false;
};

#if defined(_WIN32)
struct OsFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string path;
};
#else
struct OsFile {
    int fd = -1;
    std::string path;
};
#endif

int io_err()
{
    return errno == ENOSPC ? QSDB_NOSPC : QSDB_IO;
}

#if defined(_WIN32)

int os_open(OsFile *f, const char *path, bool create, bool readonly)
{
    f->path = path;
    DWORD access = readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD disp = create ? OPEN_ALWAYS : OPEN_EXISTING;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0)
        return QSDB_IO;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &w[0], n);
    f->h = CreateFileW(w.c_str(), access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    /* No file mapping is created. Growth uses SetEndOfFile on this handle. */
    return f->h == INVALID_HANDLE_VALUE ? QSDB_IO : QSDB_OK;
}

void os_close(OsFile *f)
{
    if (f->h != INVALID_HANDLE_VALUE)
        CloseHandle(f->h);
    f->h = INVALID_HANDLE_VALUE;
}

int os_pread(OsFile *f, void *buf, uint32_t n, uint64_t off)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    uint32_t got = 0;
    while (got < n) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD nr = 0;
        if (!ReadFile(f->h, p + got, n - got, &nr, &ov))
            return QSDB_IO;
        if (nr == 0)
            return QSDB_CORRUPT;
        got += nr;
        off += nr;
    }
    return QSDB_OK;
}

int os_pwrite(OsFile *f, const void *buf, uint32_t n, uint64_t off)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    uint32_t put = 0;
    while (put < n) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD nw = 0;
        if (!WriteFile(f->h, p + put, n - put, &nw, &ov))
            return (GetLastError() == ERROR_DISK_FULL) ? QSDB_NOSPC : QSDB_IO;
        if (nw == 0)
            return QSDB_IO;
        put += nw;
        off += nw;
    }
    return QSDB_OK;
}

int os_fsync(OsFile *f)
{
    return FlushFileBuffers(f->h) ? QSDB_OK : QSDB_IO;
}

int os_truncate(OsFile *f, uint64_t size)
{
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN))
        return QSDB_IO;
    if (!SetEndOfFile(f->h))
        return (GetLastError() == ERROR_DISK_FULL) ? QSDB_NOSPC : QSDB_IO;
    return QSDB_OK;
}

int os_size(OsFile *f, uint64_t *out)
{
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f->h, &li))
        return QSDB_IO;
    *out = static_cast<uint64_t>(li.QuadPart);
    return QSDB_OK;
}

int os_lock(OsFile *f)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    return LockFileEx(f->h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) ? QSDB_OK : QSDB_IO;
}

void os_unlock(OsFile *f)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    UnlockFileEx(f->h, 0, 1, 0, &ov);
}

int os_fsync_dir(const std::string &)
{
    /* Directory durability is not implemented on the untested Windows path. */
    return QSDB_OK;
}

#else

int os_open(OsFile *f, const char *path, bool create, bool readonly)
{
    f->path = path;
    int flags = readonly ? O_RDONLY : O_RDWR;
    if (create)
        flags |= O_CREAT;
    f->fd = ::open(path, flags | O_CLOEXEC, 0644);
    return f->fd < 0 ? io_err() : QSDB_OK;
}

void os_close(OsFile *f)
{
    if (f->fd >= 0)
        ::close(f->fd);
    f->fd = -1;
}

int os_pread(OsFile *f, void *buf, uint32_t n, uint64_t off)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    uint32_t got = 0;
    while (got < n) {
        ssize_t nr = ::pread(f->fd, p + got, n - got, static_cast<off_t>(off));
        if (nr < 0) {
            if (errno == EINTR)
                continue;
            return io_err();
        }
        if (nr == 0)
            return QSDB_CORRUPT;
        got += static_cast<uint32_t>(nr);
        off += static_cast<uint64_t>(nr);
    }
    return QSDB_OK;
}

int os_pwrite(OsFile *f, const void *buf, uint32_t n, uint64_t off)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    uint32_t put = 0;
    while (put < n) {
        ssize_t nw = ::pwrite(f->fd, p + put, n - put, static_cast<off_t>(off));
        if (nw < 0) {
            if (errno == EINTR)
                continue;
            return io_err();
        }
        if (nw == 0)
            return QSDB_IO;
        put += static_cast<uint32_t>(nw);
        off += static_cast<uint64_t>(nw);
    }
    return QSDB_OK;
}

int os_fsync(OsFile *f)
{
    return ::fsync(f->fd) == 0 ? QSDB_OK : io_err();
}

int os_truncate(OsFile *f, uint64_t size)
{
    return ::ftruncate(f->fd, static_cast<off_t>(size)) == 0 ? QSDB_OK : io_err();
}

int os_size(OsFile *f, uint64_t *out)
{
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return io_err();
    *out = static_cast<uint64_t>(st.st_size);
    return QSDB_OK;
}

int os_lock(OsFile *f)
{
    while (flock(f->fd, LOCK_EX) != 0) {
        if (errno == EINTR)
            continue;
        return io_err();
    }
    return QSDB_OK;
}

void os_unlock(OsFile *f)
{
    flock(f->fd, LOCK_UN);
}

int os_fsync_dir(const std::string &path)
{
    std::string dir = ".";
    std::string::size_type slash = path.find_last_of('/');
    if (slash != std::string::npos)
        dir = slash == 0 ? "/" : path.substr(0, slash);
    int d = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (d < 0)
        return io_err();
    int rc = ::fsync(d) == 0 ? QSDB_OK : io_err();
    ::close(d);
    return rc;
}

#endif

} // namespace

struct qsdb {
    OsFile file;
#if !defined(_WIN32)
    pthread_mutex_t mu;
    bool mu_ready = false;
#endif
    Meta committed;
    uint64_t physical = 0;
    uint64_t max_bytes = 0;
    int crash_phase = 0;
    int fail_fsync = 0;
    bool writing = false;
    bool readonly = false;
    uint32_t create_cmp = kCmpBytes;
};

struct qsdb_read {
    qsdb *db;
    uint64_t root;
    uint64_t high_water;
    uint64_t generation;
    uint32_t cmp;
};

struct qsdb_write {
    qsdb *db;
    uint64_t root;
    uint64_t hw;
    uint64_t base_hw;
    uint64_t nkeys;
    uint64_t base_physical;
    uint32_t cmp;
    bool active;
};

namespace {

int cmp_keys(uint32_t mode, const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{
    if (mode == kCmpUtf16) {
        uint32_t au = an / 2;
        uint32_t bu = bn / 2;
        uint32_t n = au < bu ? au : bu;
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t ca = static_cast<uint16_t>(a[i * 2] | (static_cast<uint16_t>(a[i * 2 + 1]) << 8));
            uint16_t cb = static_cast<uint16_t>(b[i * 2] | (static_cast<uint16_t>(b[i * 2 + 1]) << 8));
            if (ca < cb)
                return -1;
            if (ca > cb)
                return 1;
        }
        if (au < bu)
            return -1;
        if (au > bu)
            return 1;
        if (an < bn)
            return -1;
        if (an > bn)
            return 1;
        return 0;
    }
    uint32_t n = an < bn ? an : bn;
    int c = n ? memcmp(a, b, n) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return 0;
}

int cmp_vec(uint32_t mode, const std::vector<uint8_t> &a, const uint8_t *b, uint32_t bn)
{
    return cmp_keys(mode, a.data(), static_cast<uint32_t>(a.size()), b, bn);
}

void seal(Page *p)
{
    wr32(p->b + 4, 0);
    wr32(p->b + 4, crc32_ieee(p->b, kPage));
}

int check_page(const Page &p, char const magic[4], uint64_t expect_id, bool check_id)
{
    if (memcmp(p.b, magic, 4) != 0)
        return QSDB_CORRUPT;
    uint32_t stored = rd32(p.b + 4);
    Page tmp = p;
    wr32(tmp.b + 4, 0);
    if (crc32_ieee(tmp.b, kPage) != stored)
        return QSDB_CORRUPT;
    if (check_id && rd64(p.b + 8) != expect_id)
        return QSDB_CORRUPT;
    return QSDB_OK;
}

int read_meta_slot(qsdb *db, int slot, Meta *out)
{
    Page p;
    int rc = os_pread(&db->file, p.b, kPage, static_cast<uint64_t>(slot) * kPage);
    if (rc)
        return rc;
    if (check_page(p, kMetaMagic, 0, false))
        return QSDB_CORRUPT;
    if (rd32(p.b + 8) != 1)
        return QSDB_MISMATCH;
    if (rd32(p.b + 12) != kEndianStamp)
        return QSDB_MISMATCH;
    if (rd32(p.b + 16) != kPage)
        return QSDB_MISMATCH;
    out->cmp = rd32(p.b + 20);
    out->generation = rd64(p.b + 24);
    out->root = rd64(p.b + 32);
    out->high_water = rd64(p.b + 40);
    out->nkeys = rd64(p.b + 48);
    out->slot = slot;
    out->valid = true;
    if (out->cmp != kCmpBytes && out->cmp != kCmpUtf16)
        return QSDB_MISMATCH;
    if (out->high_water < 3 || out->root == 0 || out->root >= out->high_water)
        return QSDB_CORRUPT;
    return QSDB_OK;
}

int load_best_meta(qsdb *db, Meta *out)
{
    Meta a, b;
    int ra = read_meta_slot(db, 0, &a);
    int rb = read_meta_slot(db, 1, &b);
    bool va = ra == QSDB_OK;
    bool vb = rb == QSDB_OK;
    if (!va && !vb)
        return ra == QSDB_MISMATCH || rb == QSDB_MISMATCH ? QSDB_MISMATCH : QSDB_CORRUPT;
    if (va && vb)
        *out = (a.generation >= b.generation) ? a : b;
    else
        *out = va ? a : b;
    return QSDB_OK;
}

int ensure_physical(qsdb *db, uint64_t need)
{
    if (need <= db->physical)
        return QSDB_OK;
    if (db->max_bytes && need > db->max_bytes)
        return QSDB_NOSPC;
    uint64_t target = need;
    if (db->physical > 0) {
        uint64_t step = db->physical + kExtent;
        if (step > target)
            target = step;
    }
    if (db->max_bytes && target > db->max_bytes)
        target = db->max_bytes;
    if (target < need)
        return QSDB_NOSPC;
    int rc = os_truncate(&db->file, target);
    if (rc)
        return rc;
    db->physical = target;
    return QSDB_OK;
}

int read_page(qsdb *db, uint64_t id, uint64_t hw_limit, Page *p)
{
    if (id < 2 || id >= hw_limit)
        return QSDB_CORRUPT;
    int rc = os_pread(&db->file, p->b, kPage, id * kPage);
    if (rc)
        return rc;
    return check_page(*p, kPageMagic, id, true);
}

int write_page(qsdb *db, uint64_t id, Page *p)
{
    if (id < 2)
        return QSDB_INVAL;
    uint64_t need = (id + 1) * kPage;
    int rc = ensure_physical(db, need);
    if (rc)
        return rc;
    wr64(p->b + 8, id);
    memcpy(p->b, kPageMagic, 4);
    seal(p);
    return os_pwrite(&db->file, p->b, kPage, id * kPage);
}

uint64_t alloc_page(qsdb_write *tx, int *rc)
{
    uint64_t id = tx->hw;
    uint64_t need = (id + 1) * static_cast<uint64_t>(kPage);
    *rc = ensure_physical(tx->db, need);
    if (*rc)
        return 0;
    tx->hw++;
    return id;
}

int read_overflow(qsdb *db, uint64_t page, uint64_t hw, uint32_t expect, std::vector<uint8_t> *out)
{
    out->clear();
    out->reserve(expect);
    uint64_t guard = 0;
    while (page) {
        if (++guard > (static_cast<uint64_t>(QSDB_MAX_VALUE_BYTES) / 1024) + 8)
            return QSDB_CORRUPT;
        Page p;
        int rc = read_page(db, page, hw, &p);
        if (rc)
            return rc;
        if (rd16(p.b + 16) != kOverflow)
            return QSDB_CORRUPT;
        uint64_t next = rd64(p.b + 24);
        uint32_t n = static_cast<uint32_t>(rd64(p.b + 32));
        if (n > kPage - kHdr)
            return QSDB_CORRUPT;
        out->insert(out->end(), p.b + kHdr, p.b + kHdr + n);
        page = next;
        if (out->size() > expect)
            return QSDB_CORRUPT;
    }
    if (out->size() != expect)
        return QSDB_CORRUPT;
    return QSDB_OK;
}

int write_overflow(qsdb_write *tx, const uint8_t *data, uint32_t n, uint64_t *first)
{
    if (n == 0) {
        *first = 0;
        return QSDB_OK;
    }
    uint64_t head = 0;
    uint64_t prev = 0;
    uint32_t off = 0;
    while (off < n) {
        int rc = 0;
        uint64_t id = alloc_page(tx, &rc);
        if (rc)
            return rc;
        uint32_t chunk = n - off;
        if (chunk > kPage - kHdr)
            chunk = kPage - kHdr;
        Page p;
        wr16(p.b + 16, kOverflow);
        wr64(p.b + 24, 0);
        wr64(p.b + 32, chunk);
        memcpy(p.b + kHdr, data + off, chunk);
        rc = write_page(tx->db, id, &p);
        if (rc)
            return rc;
        if (!head)
            head = id;
        if (prev) {
            Page prevp;
            rc = read_page(tx->db, prev, tx->hw, &prevp);
            if (rc)
                return rc;
            wr64(prevp.b + 24, id);
            rc = write_page(tx->db, prev, &prevp);
            if (rc)
                return rc;
        }
        prev = id;
        off += chunk;
    }
    *first = head;
    return QSDB_OK;
}

struct KeyStore {
    std::vector<uint8_t> bytes;
    uint64_t ovf = 0;
    bool is_ovf = false;
};

int load_key(qsdb *db, uint64_t hw, const uint8_t *p, KeyStore *ks, const uint8_t **adv)
{
    uint32_t key_len = rd32(p);
    uint16_t flags = rd16(p + 4);
    p += 8;
    ks->bytes.clear();
    ks->ovf = 0;
    ks->is_ovf = (flags & 1) != 0;
    if (ks->is_ovf) {
        ks->ovf = rd64(p);
        p += 8;
        int rc = read_overflow(db, ks->ovf, hw, key_len, &ks->bytes);
        if (rc)
            return rc;
    } else {
        if (key_len > kPage)
            return QSDB_CORRUPT;
        ks->bytes.assign(p, p + key_len);
        p += key_len;
    }
    *adv = p;
    return QSDB_OK;
}

size_t key_wire(const KeyStore &ks)
{
    bool ovf = ks.bytes.size() > kInline;
    size_t n = 8;
    n += ovf ? 8 : ks.bytes.size();
    return n;
}

/* Writes key wire format. May allocate overflow. ovf_reuse is used when the
 * bytes already live in an immutable overflow chain. */
int write_key_wire(qsdb_write *tx, const KeyStore &ks, uint8_t *dst, size_t cap, size_t *used)
{
    bool ovf = ks.bytes.size() > kInline;
    size_t need = 8 + (ovf ? 8 : ks.bytes.size());
    if (need > cap)
        return QSDB_INVAL;
    wr32(dst, static_cast<uint32_t>(ks.bytes.size()));
    wr16(dst + 4, ovf ? 1 : 0);
    wr16(dst + 6, 0);
    if (ovf) {
        uint64_t id = ks.is_ovf ? ks.ovf : 0;
        if (!ks.is_ovf || ks.bytes.size() <= kInline) {
            int rc = write_overflow(tx, ks.bytes.data(), static_cast<uint32_t>(ks.bytes.size()), &id);
            if (rc)
                return rc;
        }
        wr64(dst + 8, id);
    } else if (!ks.bytes.empty()) {
        memcpy(dst + 8, ks.bytes.data(), ks.bytes.size());
    }
    *used = need;
    return QSDB_OK;
}

int load_leaf(qsdb *db, uint64_t hw, const Page &p, std::vector<Item> *items)
{
    items->clear();
    uint16_t n = rd16(p.b + 18);
    if (static_cast<uint32_t>(kHdr) + static_cast<uint32_t>(n) * 2u > kPage)
        return QSDB_CORRUPT;
    items->reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t off = rd16(p.b + kHdr + i * 2);
        if (off < kHdr || off >= kPage)
            return QSDB_CORRUPT;
        const uint8_t *r = p.b + off;
        Item it;
        it.tag = rd32(r + 8);
        it.val_len = rd32(r + 4);
        uint32_t key_len = rd32(r);
        uint16_t flags = rd16(r + 12);
        const uint8_t *q = r + 16;
        it.key_is_ovf = (flags & 1) != 0;
        it.val_is_ovf = (flags & 2) != 0;
        if (it.key_is_ovf) {
            it.key_ovf = rd64(q);
            q += 8;
            int rc = read_overflow(db, it.key_ovf, hw, key_len, &it.key);
            if (rc)
                return rc;
        } else {
            if (static_cast<uint32_t>(q - p.b) + key_len > kPage)
                return QSDB_CORRUPT;
            it.key.assign(q, q + key_len);
            q += key_len;
        }
        if (it.val_is_ovf) {
            it.val_ovf = rd64(q);
            q += 8;
        } else {
            if (static_cast<uint32_t>(q - p.b) + it.val_len > kPage)
                return QSDB_CORRUPT;
            it.val.assign(q, q + it.val_len);
        }
        items->push_back(std::move(it));
    }
    return QSDB_OK;
}

size_t leaf_rec_bytes(const Item &it)
{
    size_t n = 16;
    n += it.key.size() > kInline ? 8 : it.key.size();
    bool vovf = it.val_is_ovf || it.val.size() > kInline;
    n += vovf ? 8 : it.val.size();
    return n;
}

int pack_leaf(qsdb_write *tx, const std::vector<Item> &items, uint64_t dest, uint64_t *right_id, std::vector<uint8_t> *sep)
{
    auto one = [&](size_t begin, size_t end, uint64_t id) -> int {
        size_t bytes = kHdr + (end - begin) * 2;
        for (size_t i = begin; i < end; ++i)
            bytes += leaf_rec_bytes(items[i]);
        if (bytes > kPage)
            return QSDB_INVAL;
        Page p;
        wr16(p.b + 16, kLeaf);
        wr16(p.b + 18, static_cast<uint16_t>(end - begin));
        uint32_t upper = kPage;
        for (size_t i = end; i-- > begin;) {
            const Item &it = items[i];
            size_t rec = leaf_rec_bytes(it);
            upper -= static_cast<uint32_t>(rec);
            uint8_t *r = p.b + upper;
            bool kovf = it.key.size() > kInline;
            bool vovf = it.val_is_ovf || it.val.size() > kInline;
            wr32(r + 0, static_cast<uint32_t>(it.key.size()));
            wr32(r + 4, vovf ? (it.val_is_ovf ? it.val_len : static_cast<uint32_t>(it.val.size()))
                             : static_cast<uint32_t>(it.val.size()));
            wr32(r + 8, it.tag);
            wr16(r + 12, static_cast<uint16_t>((kovf ? 1 : 0) | (vovf ? 2 : 0)));
            wr16(r + 14, 0);
            uint8_t *q = r + 16;
            if (kovf) {
                uint64_t idk = it.key_is_ovf ? it.key_ovf : 0;
                if (!it.key_is_ovf) {
                    int rc = write_overflow(tx, it.key.data(), static_cast<uint32_t>(it.key.size()), &idk);
                    if (rc)
                        return rc;
                }
                wr64(q, idk);
                q += 8;
            } else if (!it.key.empty()) {
                memcpy(q, it.key.data(), it.key.size());
                q += it.key.size();
            }
            if (vovf) {
                uint64_t idv = it.val_ovf;
                if (!it.val_is_ovf) {
                    int rc = write_overflow(tx, it.val.data(), static_cast<uint32_t>(it.val.size()), &idv);
                    if (rc)
                        return rc;
                }
                wr64(q, idv);
            } else if (!it.val.empty()) {
                memcpy(q, it.val.data(), it.val.size());
            }
            wr16(p.b + kHdr + static_cast<uint32_t>(i - begin) * 2, static_cast<uint16_t>(upper));
        }
        return write_page(tx->db, id, &p);
    };

    size_t need = kHdr + items.size() * 2;
    for (size_t i = 0; i < items.size(); ++i)
        need += leaf_rec_bytes(items[i]);
    if (need <= kPage) {
        *right_id = 0;
        return one(0, items.size(), dest);
    }
    if (items.size() < 2)
        return QSDB_INVAL;
    size_t mid = items.size() / 2;
    int rc = one(0, mid, dest);
    if (rc)
        return rc;
    int arc = 0;
    uint64_t rid = alloc_page(tx, &arc);
    if (arc)
        return arc;
    rc = one(mid, items.size(), rid);
    if (rc)
        return rc;
    *right_id = rid;
    *sep = items[mid].key;
    return QSDB_OK;
}

int load_internal(qsdb *db, uint64_t hw, const Page &p, uint64_t *leftmost, std::vector<Item> *seps)
{
    *leftmost = rd64(p.b + 24);
    uint16_t n = rd16(p.b + 18);
    if (static_cast<uint32_t>(kHdr) + static_cast<uint32_t>(n) * 2u > kPage)
        return QSDB_CORRUPT;
    seps->clear();
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t off = rd16(p.b + kHdr + i * 2);
        if (off < kHdr || off >= kPage)
            return QSDB_CORRUPT;
        const uint8_t *r = p.b + off;
        Item it;
        it.child = rd64(r);
        KeyStore ks;
        const uint8_t *adv = NULL;
        int rc = load_key(db, hw, r + 8, &ks, &adv);
        if (rc)
            return rc;
        it.key = std::move(ks.bytes);
        it.key_ovf = ks.ovf;
        it.key_is_ovf = ks.is_ovf && it.key.size() > kInline;
        seps->push_back(std::move(it));
    }
    return QSDB_OK;
}

size_t sep_bytes(const Item &it)
{
    size_t n = 8 + 8;
    n += it.key.size() > kInline ? 8 : it.key.size();
    return n;
}

int pack_internal(qsdb_write *tx, uint64_t leftmost, const std::vector<Item> &seps,
                  uint64_t dest, uint64_t *right_id, std::vector<uint8_t> *sep_key, uint64_t *new_left)
{
    auto one = [&](uint64_t left, size_t begin, size_t end, uint64_t id) -> int {
        size_t bytes = kHdr + (end - begin) * 2;
        for (size_t i = begin; i < end; ++i)
            bytes += sep_bytes(seps[i]);
        if (bytes > kPage)
            return QSDB_INVAL;
        Page p;
        wr16(p.b + 16, kInternal);
        wr16(p.b + 18, static_cast<uint16_t>(end - begin));
        wr64(p.b + 24, left);
        uint32_t upper = kPage;
        for (size_t i = end; i-- > begin;) {
            size_t rec = sep_bytes(seps[i]);
            upper -= static_cast<uint32_t>(rec);
            uint8_t *r = p.b + upper;
            wr64(r, seps[i].child);
            KeyStore ks;
            ks.bytes = seps[i].key;
            ks.ovf = seps[i].key_ovf;
            ks.is_ovf = seps[i].key_is_ovf && seps[i].key.size() > kInline;
            size_t used = 0;
            int rc = write_key_wire(tx, ks, r + 8, rec - 8, &used);
            if (rc)
                return rc;
            wr16(p.b + kHdr + static_cast<uint32_t>(i - begin) * 2, static_cast<uint16_t>(upper));
        }
        return write_page(tx->db, id, &p);
    };

    size_t need = kHdr + seps.size() * 2;
    for (size_t i = 0; i < seps.size(); ++i)
        need += sep_bytes(seps[i]);
    if (need <= kPage) {
        *right_id = 0;
        *new_left = dest;
        return one(leftmost, 0, seps.size(), dest);
    }
    if (seps.size() < 2)
        return QSDB_INVAL;
    size_t mid = seps.size() / 2;
    /* seps[mid] moves up; left node keeps seps[0, mid), right keeps (mid, end) and leftmost = seps[mid].child */
    int rc = one(leftmost, 0, mid, dest);
    if (rc)
        return rc;
    int arc = 0;
    uint64_t rid = alloc_page(tx, &arc);
    if (arc)
        return arc;
    rc = one(seps[mid].child, mid + 1, seps.size(), rid);
    if (rc)
        return rc;
    *right_id = rid;
    *sep_key = seps[mid].key;
    *new_left = dest;
    return QSDB_OK;
}

int find_child(uint32_t cmp, const std::vector<Item> &seps, const uint8_t *key, uint32_t klen)
{
    int i = 0;
    while (i < static_cast<int>(seps.size())
           && cmp_vec(cmp, seps[static_cast<size_t>(i)].key, key, klen) <= 0)
        ++i;
    return i;
}

int cow_id(qsdb_write *tx, uint64_t id, Page *page, uint64_t *out)
{
    int rc = read_page(tx->db, id, tx->hw, page);
    if (rc)
        return rc;
    if (id >= tx->base_hw) {
        *out = id;
        return QSDB_OK;
    }
    int arc = 0;
    uint64_t nid = alloc_page(tx, &arc);
    if (arc)
        return arc;
    *out = nid;
    return QSDB_OK;
}

Ins insert_page(qsdb_write *tx, uint64_t page, const uint8_t *key, uint32_t klen,
                uint32_t tag, const uint8_t *val, uint32_t vlen);

Ins insert_page(qsdb_write *tx, uint64_t page, const uint8_t *key, uint32_t klen,
                uint32_t tag, const uint8_t *val, uint32_t vlen)
{
    Ins result;
    Page raw;
    uint64_t dest = 0;
    int rc = cow_id(tx, page, &raw, &dest);
    if (rc) {
        result.page = static_cast<uint64_t>(rc);
        result.changed = false;
        result.split = false;
        /* overload: page field 0 and we use a side channel? Better throw via split sentinel.
         * Use right==0 and page>0xfffffff as error by storing rc in sep size? 
         * I'll use a convention: if page==0 and !changed, error code is in right. */
        result.page = 0;
        result.right = static_cast<uint64_t>(rc);
        return result;
    }
    uint16_t type = rd16(raw.b + 16);
    if (type == kLeaf) {
        std::vector<Item> items;
        rc = load_leaf(tx->db, tx->hw, raw, &items);
        if (rc) {
            result.page = 0;
            result.right = static_cast<uint64_t>(rc);
            return result;
        }
        int lo = 0, hi = static_cast<int>(items.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            int c = cmp_vec(tx->cmp, items[static_cast<size_t>(mid)].key, key, klen);
            if (c < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        Item neu;
        neu.key.assign(key, key + klen);
        neu.tag = tag;
        neu.val_len = vlen;
        if (vlen > kInline) {
            uint64_t ov = 0;
            rc = write_overflow(tx, val, vlen, &ov);
            if (rc) {
                result.page = 0;
                result.right = static_cast<uint64_t>(rc);
                return result;
            }
            neu.val_is_ovf = true;
            neu.val_ovf = ov;
            neu.val_len = vlen;
        } else {
            neu.val.assign(val, val + vlen);
            neu.val_len = vlen;
        }
        if (lo < static_cast<int>(items.size())
            && cmp_vec(tx->cmp, items[static_cast<size_t>(lo)].key, key, klen) == 0) {
            items[static_cast<size_t>(lo)] = std::move(neu);
        } else {
            items.insert(items.begin() + lo, std::move(neu));
            tx->nkeys++;
        }
        uint64_t right = 0;
        std::vector<uint8_t> sep;
        rc = pack_leaf(tx, items, dest, &right, &sep);
        if (rc) {
            result.page = 0;
            result.right = static_cast<uint64_t>(rc);
            return result;
        }
        result.page = dest;
        result.changed = true;
        result.split = right != 0;
        result.right = right;
        result.sep = std::move(sep);
        return result;
    }
    if (type != kInternal) {
        result.page = 0;
        result.right = QSDB_CORRUPT;
        return result;
    }
    uint64_t leftmost = 0;
    std::vector<Item> seps;
    rc = load_internal(tx->db, tx->hw, raw, &leftmost, &seps);
    if (rc) {
        result.page = 0;
        result.right = static_cast<uint64_t>(rc);
        return result;
    }
    int ci = find_child(tx->cmp, seps, key, klen);
    uint64_t child = (ci == 0) ? leftmost : seps[static_cast<size_t>(ci - 1)].child;
    Ins sub = insert_page(tx, child, key, klen, tag, val, vlen);
    if (sub.page == 0 && !sub.changed) {
        return sub;
    }
    if (sub.page == child && !sub.split) {
        Ins same;
        same.page = page;
        same.changed = false;
        same.split = false;
        return same;
    }
    if (ci == 0)
        leftmost = sub.page;
    else
        seps[static_cast<size_t>(ci - 1)].child = sub.page;
    if (sub.split) {
        Item s;
        s.key = sub.sep;
        s.child = sub.right;
        seps.insert(seps.begin() + ci, std::move(s));
    }
    uint64_t right = 0;
    std::vector<uint8_t> sep;
    uint64_t new_left = dest;
    rc = pack_internal(tx, leftmost, seps, dest, &right, &sep, &new_left);
    if (rc) {
        result.page = 0;
        result.right = static_cast<uint64_t>(rc);
        return result;
    }
    result.page = dest;
    result.changed = true;
    result.split = right != 0;
    result.right = right;
    result.sep = std::move(sep);
    return result;
}

Del delete_page(qsdb_write *tx, uint64_t page, const uint8_t *key, uint32_t klen, bool is_root);

Del delete_page(qsdb_write *tx, uint64_t page, const uint8_t *key, uint32_t klen, bool is_root)
{
    Del d;
    Page raw;
    uint64_t dest = 0;
    int rc = cow_id(tx, page, &raw, &dest);
    if (rc) {
        d.page = 0;
        d.changed = false;
        d.gone = false;
        /* stash error in page by using a parallel: if gone && page==0, caller checks a global? 
         * Use page==0 && changed==true to mean error code stored in... I'll set gone=true page=rc with a sentinel.
         * Convention: page==0 && !changed && !gone is "not found at this level"? 
         * Error: set changed=true, gone=true, page=rc (small). Caller treats page<4096 && gone && changed as error if page<100.
         */
        d.page = static_cast<uint64_t>(rc);
        d.changed = true;
        d.gone = true;
        return d;
    }
    uint16_t type = rd16(raw.b + 16);
    if (type == kLeaf) {
        std::vector<Item> items;
        rc = load_leaf(tx->db, tx->hw, raw, &items);
        if (rc) {
            d.page = static_cast<uint64_t>(rc);
            d.changed = true;
            d.gone = true;
            return d;
        }
        int lo = 0, hi = static_cast<int>(items.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            int c = cmp_vec(tx->cmp, items[static_cast<size_t>(mid)].key, key, klen);
            if (c < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo >= static_cast<int>(items.size())
            || cmp_vec(tx->cmp, items[static_cast<size_t>(lo)].key, key, klen) != 0) {
            d.page = page;
            d.changed = false;
            return d;
        }
        items.erase(items.begin() + lo);
        tx->nkeys--;
        if (items.empty() && !is_root) {
            d.gone = true;
            d.changed = true;
            d.page = 0;
            return d;
        }
        uint64_t right = 0;
        std::vector<uint8_t> sep;
        rc = pack_leaf(tx, items, dest, &right, &sep);
        if (rc || right) {
            d.page = static_cast<uint64_t>(rc ? rc : QSDB_INVAL);
            d.changed = true;
            d.gone = true;
            return d;
        }
        d.page = dest;
        d.changed = true;
        return d;
    }
    uint64_t leftmost = 0;
    std::vector<Item> seps;
    rc = load_internal(tx->db, tx->hw, raw, &leftmost, &seps);
    if (rc) {
        d.page = static_cast<uint64_t>(rc);
        d.changed = true;
        d.gone = true;
        return d;
    }
    int ci = find_child(tx->cmp, seps, key, klen);
    uint64_t child = (ci == 0) ? leftmost : seps[static_cast<size_t>(ci - 1)].child;
    Del sub = delete_page(tx, child, key, klen, false);
    if (sub.changed && sub.gone && sub.page != 0 && sub.page < 64) {
        return sub; /* error */
    }
    if (!sub.changed) {
        Del same;
        same.page = page;
        same.changed = false;
        same.gone = false;
        return same;
    }
    if (sub.gone) {
        if (ci == 0) {
            if (seps.empty()) {
                d.gone = !is_root;
                d.changed = true;
                d.page = 0;
                return d;
            }
            leftmost = seps[0].child;
            seps.erase(seps.begin());
        } else {
            seps.erase(seps.begin() + (ci - 1));
        }
    } else if (ci == 0) {
        leftmost = sub.page;
    } else {
        seps[static_cast<size_t>(ci - 1)].child = sub.page;
    }
    if (seps.empty()) {
        /* Collapse to the single remaining child. */
        d.page = leftmost;
        d.changed = true;
        d.gone = false;
        return d;
    }
    uint64_t right = 0;
    std::vector<uint8_t> sep;
    uint64_t dummy = 0;
    rc = pack_internal(tx, leftmost, seps, dest, &right, &sep, &dummy);
    if (rc || right) {
        d.page = static_cast<uint64_t>(rc ? rc : QSDB_INVAL);
        d.changed = true;
        d.gone = true;
        return d;
    }
    d.page = dest;
    d.changed = true;
    return d;
}

bool prefix_hit(const uint8_t *key, size_t klen, const uint8_t *pfx, size_t plen,
                const uint8_t *sep, size_t seplen)
{
    if (klen == plen && (plen == 0 || memcmp(key, pfx, plen) == 0))
        return true;
    if (seplen == 0 || klen < plen + seplen)
        return false;
    if (memcmp(key, pfx, plen) != 0)
        return false;
    return memcmp(key + plen, sep, seplen) == 0;
}

struct WalkCtx {
    qsdb *db;
    uint64_t hw;
    uint32_t cmp;
    const uint8_t *start;
    uint32_t start_len;
    bool begun;
    qsdb_scan_fn fn;
    void *user;
    int rc;
    int stop;
};

int walk(WalkCtx *w, uint64_t page, int depth)
{
    if (w->rc || w->stop)
        return w->rc;
    if (depth > 64)
        return w->rc = QSDB_CORRUPT;
    Page p;
    int rc = read_page(w->db, page, w->hw, &p);
    if (rc)
        return w->rc = rc;
    uint16_t type = rd16(p.b + 16);
    if (type == kLeaf) {
        std::vector<Item> items;
        rc = load_leaf(w->db, w->hw, p, &items);
        if (rc)
            return w->rc = rc;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!w->begun) {
                if (cmp_vec(w->cmp, items[i].key, w->start, w->start_len) < 0)
                    continue;
                w->begun = true;
            }
            uint64_t vlen = items[i].val_is_ovf ? items[i].val_len : items[i].val.size();
            int s = w->fn(items[i].key.data(), items[i].key.size(), items[i].tag, vlen, w->user);
            if (s) {
                w->stop = 1;
                return 0;
            }
        }
        return 0;
    }
    uint64_t leftmost = 0;
    std::vector<Item> seps;
    rc = load_internal(w->db, w->hw, p, &leftmost, &seps);
    if (rc)
        return w->rc = rc;
    for (size_t i = 0; i <= seps.size(); ++i) {
        if (!w->begun && i < seps.size()
            && cmp_vec(w->cmp, seps[i].key, w->start, w->start_len) <= 0)
            continue;
        uint64_t child = (i == 0) ? leftmost : seps[i - 1].child;
        rc = walk(w, child, depth + 1);
        if (rc || w->stop)
            return rc;
    }
    return 0;
}

int lookup_item(qsdb_read *tx, const uint8_t *key, uint32_t klen, Item *found)
{
    uint64_t page = tx->root;
    for (int depth = 0; depth < 64; ++depth) {
        Page p;
        int rc = read_page(tx->db, page, tx->high_water, &p);
        if (rc)
            return rc;
        uint16_t type = rd16(p.b + 16);
        if (type == kLeaf) {
            std::vector<Item> items;
            rc = load_leaf(tx->db, tx->high_water, p, &items);
            if (rc)
                return rc;
            int lo = 0, hi = static_cast<int>(items.size());
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                int c = cmp_vec(tx->cmp, items[static_cast<size_t>(mid)].key, key, klen);
                if (c < 0)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < static_cast<int>(items.size())
                && cmp_vec(tx->cmp, items[static_cast<size_t>(lo)].key, key, klen) == 0) {
                *found = std::move(items[static_cast<size_t>(lo)]);
                return QSDB_OK;
            }
            return QSDB_NOT_FOUND;
        }
        if (type != kInternal)
            return QSDB_CORRUPT;
        uint64_t leftmost = 0;
        std::vector<Item> seps;
        rc = load_internal(tx->db, tx->high_water, p, &leftmost, &seps);
        if (rc)
            return rc;
        int ci = find_child(tx->cmp, seps, key, klen);
        page = (ci == 0) ? leftmost : seps[static_cast<size_t>(ci - 1)].child;
    }
    return QSDB_CORRUPT;
}

int stream_value(qsdb *db, uint64_t hw, const Item &it, void *buf, size_t buf_cap,
                 size_t *out_len, uint64_t offset)
{
    uint64_t total = it.val_is_ovf ? it.val_len : it.val.size();
    if (!buf) {
        *out_len = static_cast<size_t>(total);
        return QSDB_OK;
    }
    if (offset > total)
        offset = total;
    uint64_t remain = total - offset;
    size_t want = buf_cap < remain ? buf_cap : static_cast<size_t>(remain);
    uint8_t *dst = static_cast<uint8_t *>(buf);
    if (!it.val_is_ovf) {
        if (want)
            memcpy(dst, it.val.data() + static_cast<size_t>(offset), want);
        *out_len = want;
        return QSDB_OK;
    }
    std::vector<uint8_t> all;
    int rc = read_overflow(db, it.val_ovf, hw, it.val_len, &all);
    if (rc)
        return rc;
    if (want)
        memcpy(dst, all.data() + static_cast<size_t>(offset), want);
    *out_len = want;
    return QSDB_OK;
}

int make_empty_leaf(qsdb_write *tx, uint64_t *id)
{
    int arc = 0;
    uint64_t nid = alloc_page(tx, &arc);
    if (arc)
        return arc;
    std::vector<Item> none;
    uint64_t right = 0;
    std::vector<uint8_t> sep;
    int rc = pack_leaf(tx, none, nid, &right, &sep);
    if (rc)
        return rc;
    *id = nid;
    return QSDB_OK;
}

bool is_err(const Ins &r)
{
    return r.page == 0 && !r.changed && r.right != 0 && r.right < 64;
}

bool del_err(const Del &d)
{
    return d.changed && d.gone && d.page != 0 && d.page < 64;
}

int finish_lock(qsdb *db, bool have_file_lock)
{
    db->writing = false;
    if (have_file_lock)
        os_unlock(&db->file);
#if !defined(_WIN32)
    pthread_mutex_unlock(&db->mu);
#endif
    return QSDB_OK;
}

} // namespace

int qsdb_open(const char *path, int flags, qsdb **out)
{
    if (!path || !out)
        return QSDB_INVAL;
    qsdb *db = new qsdb();
#if !defined(_WIN32)
    pthread_mutex_init(&db->mu, NULL);
    db->mu_ready = true;
#endif
    db->create_cmp = (flags & QSDB_OPEN_UTF16) ? kCmpUtf16 : kCmpBytes;
    db->readonly = (flags & QSDB_OPEN_READONLY) != 0;
    bool create = (flags & QSDB_OPEN_CREATE) != 0;
    int rc = os_open(&db->file, path, create && !db->readonly, db->readonly);
    if (rc) {
        qsdb_close(db);
        return rc;
    }
    rc = os_size(&db->file, &db->physical);
    if (rc) {
        qsdb_close(db);
        return rc;
    }
    if (db->physical == 0) {
        if (!create || db->readonly) {
            qsdb_close(db);
            return QSDB_NOT_FOUND;
        }
        rc = ensure_physical(db, 3ull * kPage);
        if (rc) {
            qsdb_close(db);
            return rc;
        }
        qsdb_write tx;
        memset(&tx, 0, sizeof tx);
        tx.db = db;
        tx.hw = 2;
        tx.base_hw = 2;
        tx.cmp = db->create_cmp;
        tx.active = true;
        uint64_t leaf = 0;
        rc = make_empty_leaf(&tx, &leaf);
        if (rc) {
            qsdb_close(db);
            return rc;
        }
        Page meta;
        memcpy(meta.b, kMetaMagic, 4);
        wr32(meta.b + 8, 1);
        wr32(meta.b + 12, kEndianStamp);
        wr32(meta.b + 16, kPage);
        wr32(meta.b + 20, db->create_cmp);
        wr64(meta.b + 24, 1);
        wr64(meta.b + 32, leaf);
        wr64(meta.b + 40, tx.hw);
        wr64(meta.b + 48, 0);
        seal(&meta);
        rc = os_pwrite(&db->file, meta.b, kPage, 0);
        if (!rc)
            rc = os_fsync(&db->file);
        if (!rc)
            rc = os_fsync_dir(db->file.path);
        if (rc) {
            qsdb_close(db);
            return rc;
        }
        db->committed.generation = 1;
        db->committed.root = leaf;
        db->committed.high_water = tx.hw;
        db->committed.nkeys = 0;
        db->committed.cmp = db->create_cmp;
        db->committed.slot = 0;
        db->committed.valid = true;
        *out = db;
        return QSDB_OK;
    }
    Meta m;
    rc = load_best_meta(db, &m);
    if (rc) {
        qsdb_close(db);
        return rc;
    }
    if ((flags & QSDB_OPEN_UTF16) && m.cmp != kCmpUtf16) {
        qsdb_close(db);
        return QSDB_MISMATCH;
    }
    db->committed = m;
    *out = db;
    return QSDB_OK;
}

void qsdb_close(qsdb *db)
{
    if (!db)
        return;
    os_close(&db->file);
#if !defined(_WIN32)
    if (db->mu_ready)
        pthread_mutex_destroy(&db->mu);
#endif
    delete db;
}

int qsdb_info(qsdb *db, qsdb_stat *out)
{
    if (!db || !out)
        return QSDB_INVAL;
    Meta m;
    int rc = load_best_meta(db, &m);
    if (rc)
        return rc;
    uint64_t phys = 0;
    os_size(&db->file, &phys);
    out->generation = m.generation;
    out->nkeys = m.nkeys;
    out->high_water_pages = m.high_water;
    out->logical_bytes = m.high_water * kPage;
    out->physical_bytes = phys;
    out->page_size = kPage;
    out->cmp_mode = m.cmp;
    return QSDB_OK;
}

int qsdb_begin_read(qsdb *db, qsdb_read **out)
{
    if (!db || !out)
        return QSDB_INVAL;
    Meta m;
    int rc = QSDB_CORRUPT;
    for (int attempt = 0; attempt < 8; ++attempt) {
        rc = load_best_meta(db, &m);
        if (rc == QSDB_OK)
            break;
    }
    if (rc)
        return rc;
    qsdb_read *tx = new qsdb_read();
    tx->db = db;
    tx->root = m.root;
    tx->high_water = m.high_water;
    tx->generation = m.generation;
    tx->cmp = m.cmp;
    *out = tx;
    return QSDB_OK;
}

void qsdb_end_read(qsdb_read *tx)
{
    delete tx;
}

uint64_t qsdb_read_generation(const qsdb_read *tx)
{
    return tx ? tx->generation : 0;
}

int qsdb_get(qsdb_read *tx, const void *key, size_t key_len,
             uint32_t *tag, void *buf, size_t buf_cap, size_t *out_len,
             uint64_t offset)
{
    if (!tx || !key || !out_len || key_len > QSDB_MAX_KEY_BYTES)
        return QSDB_INVAL;
    Item it;
    int rc = lookup_item(tx, static_cast<const uint8_t *>(key), static_cast<uint32_t>(key_len), &it);
    if (rc)
        return rc;
    if (tag)
        *tag = it.tag;
    return stream_value(tx->db, tx->high_water, it, buf, buf_cap, out_len, offset);
}

int qsdb_contains(qsdb_read *tx, const void *key, size_t key_len)
{
    uint32_t tag = 0;
    size_t n = 0;
    return qsdb_get(tx, key, key_len, &tag, NULL, 0, &n, 0);
}

int qsdb_scan(qsdb_read *tx, const void *start, size_t start_len, qsdb_scan_fn fn, void *user)
{
    if (!tx || !fn || start_len > QSDB_MAX_KEY_BYTES)
        return QSDB_INVAL;
    WalkCtx w;
    w.db = tx->db;
    w.hw = tx->high_water;
    w.cmp = tx->cmp;
    w.start = static_cast<const uint8_t *>(start);
    w.start_len = static_cast<uint32_t>(start_len);
    w.begun = start_len == 0 && start == NULL;
    w.fn = fn;
    w.user = user;
    w.rc = 0;
    w.stop = 0;
    if (start == NULL)
        w.begun = true;
    walk(&w, tx->root, 0);
    return w.rc;
}

int qsdb_begin_write(qsdb *db, qsdb_write **out)
{
    if (!db || !out)
        return QSDB_INVAL;
    if (db->readonly)
        return QSDB_READONLY;
#if !defined(_WIN32)
    pthread_mutex_lock(&db->mu);
#endif
    if (db->writing) {
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return QSDB_BUSY;
    }
    int rc = os_lock(&db->file);
    if (rc) {
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return rc;
    }
    Meta m;
    rc = load_best_meta(db, &m);
    if (rc) {
        os_unlock(&db->file);
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return rc;
    }
    uint64_t phys = 0;
    os_size(&db->file, &phys);
    db->physical = phys;
    db->committed = m;
    db->writing = true;
    qsdb_write *tx = new qsdb_write();
    tx->db = db;
    tx->root = m.root;
    tx->hw = m.high_water;
    tx->base_hw = m.high_water;
    tx->nkeys = m.nkeys;
    tx->base_physical = phys;
    tx->cmp = m.cmp;
    tx->active = true;
    *out = tx;
    return QSDB_OK;
}

static int put_raw(qsdb_write *tx, const void *key, size_t key_len, uint32_t tag,
                   const void *val, size_t val_len)
{
    if (!tx || !tx->active || !key || key_len == 0 || key_len > QSDB_MAX_KEY_BYTES)
        return QSDB_INVAL;
    if (val_len > QSDB_MAX_VALUE_BYTES)
        return QSDB_TOO_BIG;
    if (val_len && !val)
        return QSDB_INVAL;
    Ins r = insert_page(tx, tx->root, static_cast<const uint8_t *>(key),
                        static_cast<uint32_t>(key_len), tag,
                        static_cast<const uint8_t *>(val), static_cast<uint32_t>(val_len));
    if (is_err(r))
        return static_cast<int>(r.right);
    tx->root = r.page;
    if (r.split) {
        int arc = 0;
        uint64_t id = alloc_page(tx, &arc);
        if (arc)
            return arc;
        Item sep;
        sep.key = r.sep;
        sep.child = r.right;
        std::vector<Item> seps(1, sep);
        uint64_t right = 0;
        std::vector<uint8_t> up;
        uint64_t dummy = 0;
        int rc = pack_internal(tx, r.page, seps, id, &right, &up, &dummy);
        if (rc)
            return rc;
        if (right)
            return QSDB_INVAL;
        tx->root = id;
    }
    return QSDB_OK;
}

int qsdb_put(qsdb_write *tx, const void *key, size_t key_len,
             uint32_t tag, const void *val, size_t val_len)
{
    return put_raw(tx, key, key_len, tag, val, val_len);
}

int qsdb_erase(qsdb_write *tx, const void *key, size_t key_len)
{
    if (!tx || !tx->active || !key || key_len == 0 || key_len > QSDB_MAX_KEY_BYTES)
        return QSDB_INVAL;
    Del d = delete_page(tx, tx->root, static_cast<const uint8_t *>(key),
                        static_cast<uint32_t>(key_len), true);
    if (del_err(d))
        return static_cast<int>(d.page);
    if (!d.changed)
        return QSDB_NOT_FOUND;
    if (!d.gone)
        tx->root = d.page;
    return QSDB_OK;
}

struct Collect {
    std::vector<std::vector<uint8_t> > keys;
    const uint8_t *pfx;
    size_t plen;
    const uint8_t *sep;
    size_t seplen;
};

int collect_cb(const void *key, size_t key_len, uint32_t, uint64_t, void *user)
{
    Collect *c = static_cast<Collect *>(user);
    const uint8_t *k = static_cast<const uint8_t *>(key);
    if (!prefix_hit(k, key_len, c->pfx, c->plen, c->sep, c->seplen)) {
        if (c->keys.empty())
            return 0;
        /* scanned past the prefix */
        if (key_len < c->plen || memcmp(k, c->pfx, c->plen) != 0)
            return 1;
        return 0;
    }
    c->keys.push_back(std::vector<uint8_t>(k, k + key_len));
    return 0;
}

int qsdb_erase_tree(qsdb_write *tx, const void *key, size_t key_len,
                    const void *sep, size_t sep_len)
{
    if (!tx || !tx->active || !key || key_len == 0 || !sep || sep_len == 0)
        return QSDB_INVAL;
    /* Scan using a temporary read view of the current write image. */
    qsdb_read view;
    view.db = tx->db;
    view.root = tx->root;
    view.high_water = tx->hw;
    view.generation = 0;
    view.cmp = tx->cmp;
    Collect col;
    col.pfx = static_cast<const uint8_t *>(key);
    col.plen = key_len;
    col.sep = static_cast<const uint8_t *>(sep);
    col.seplen = sep_len;
    int rc = qsdb_scan(&view, key, key_len, collect_cb, &col);
    if (rc)
        return rc;
    if (col.keys.empty())
        return QSDB_NOT_FOUND;
    for (size_t i = 0; i < col.keys.size(); ++i) {
        rc = qsdb_erase(tx, col.keys[i].data(), col.keys[i].size());
        if (rc && rc != QSDB_NOT_FOUND)
            return rc;
        view.root = tx->root;
        view.high_water = tx->hw;
    }
    return QSDB_OK;
}

int qsdb_clear(qsdb_write *tx)
{
    if (!tx || !tx->active)
        return QSDB_INVAL;
    uint64_t leaf = 0;
    int rc = make_empty_leaf(tx, &leaf);
    if (rc)
        return rc;
    tx->root = leaf;
    tx->nkeys = 0;
    return QSDB_OK;
}

int qsdb_commit(qsdb_write *tx)
{
    if (!tx || !tx->active)
        return QSDB_INVAL;
    qsdb *db = tx->db;
    int crash = db->crash_phase;
    if (crash == QSDB_CRASH_AFTER_PAGES)
        _exit(42);
    int rc = QSDB_OK;
    if (db->fail_fsync == 1)
        rc = QSDB_IO;
    else
        rc = os_fsync(&db->file);
    if (rc) {
        os_truncate(&db->file, tx->base_physical);
        db->physical = tx->base_physical;
        tx->active = false;
        delete tx;
        db->writing = false;
        os_unlock(&db->file);
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return rc;
    }
    if (crash == QSDB_CRASH_AFTER_DATA_FSYNC)
        _exit(42);
    int slot = db->committed.slot == 0 ? 1 : 0;
    Page meta;
    memcpy(meta.b, kMetaMagic, 4);
    wr32(meta.b + 8, 1);
    wr32(meta.b + 12, kEndianStamp);
    wr32(meta.b + 16, kPage);
    wr32(meta.b + 20, tx->cmp);
    wr64(meta.b + 24, db->committed.generation + 1);
    wr64(meta.b + 32, tx->root);
    wr64(meta.b + 40, tx->hw);
    wr64(meta.b + 48, tx->nkeys);
    seal(&meta);
    if (crash == QSDB_CRASH_TORN_META) {
        uint8_t torn[32];
        memset(torn, 0, sizeof torn);
        os_pwrite(&db->file, torn, sizeof torn, static_cast<uint64_t>(slot) * kPage);
        _exit(42);
    }
    rc = os_pwrite(&db->file, meta.b, kPage, static_cast<uint64_t>(slot) * kPage);
    if (rc) {
        tx->active = false;
        delete tx;
        db->writing = false;
        os_unlock(&db->file);
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return rc;
    }
    if (db->fail_fsync == 2) {
        tx->active = false;
        delete tx;
        db->writing = false;
        os_unlock(&db->file);
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return QSDB_UNCERTAIN;
    }
    rc = os_fsync(&db->file);
    if (rc) {
        tx->active = false;
        delete tx;
        db->writing = false;
        os_unlock(&db->file);
#if !defined(_WIN32)
        pthread_mutex_unlock(&db->mu);
#endif
        return QSDB_UNCERTAIN;
    }
    if (crash == QSDB_CRASH_AFTER_META_FSYNC)
        _exit(42);
    db->committed.generation += 1;
    db->committed.root = tx->root;
    db->committed.high_water = tx->hw;
    db->committed.nkeys = tx->nkeys;
    db->committed.cmp = tx->cmp;
    db->committed.slot = slot;
    db->committed.valid = true;
    tx->active = false;
    delete tx;
    db->writing = false;
    os_unlock(&db->file);
#if !defined(_WIN32)
    pthread_mutex_unlock(&db->mu);
#endif
    return QSDB_OK;
}

void qsdb_abort(qsdb_write *tx)
{
    if (!tx || !tx->active)
        return;
    qsdb *db = tx->db;
    os_truncate(&db->file, tx->base_physical);
    db->physical = tx->base_physical;
    tx->active = false;
    delete tx;
    db->writing = false;
    os_unlock(&db->file);
#if !defined(_WIN32)
    pthread_mutex_unlock(&db->mu);
#endif
}

void qsdb_debug_crash_at(qsdb *db, int phase)
{
    if (db)
        db->crash_phase = phase;
}

void qsdb_debug_fail_fsync(qsdb *db, int which)
{
    if (db)
        db->fail_fsync = which;
}

void qsdb_debug_max_bytes(qsdb *db, uint64_t bytes)
{
    if (db)
        db->max_bytes = bytes;
}
