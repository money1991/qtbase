#include <QtCore/qsettings.h>
#include "qsettings_p.h"

#ifdef QT_BOOTSTRAPPED

QT_BEGIN_NAMESPACE
QSettingsPrivate *qsdbCreateIfEnabled(QSettings::Format, QSettings::Scope,
                                      const QString &, const QString &)
{
    return 0;
}

QSettingsPrivate *qsdbCreateFileIfEnabled(const QString &, QSettings::Format)
{
    return 0;
}
QT_END_NAMESPACE

#else

#include "qsdb.h"

#include <stdlib.h>

#include <QtCore/qdatastream.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qhash.h>
#include <QtCore/qmap.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qmutex.h>

#include <algorithm>
#include <string.h>

/*
 * Opt-in qsdb backend. Business code keeps calling QSettings.
 * Set QSDB_BACKEND=1 before constructing the object.
 * With the variable unset, create() returns null from the hooks below and
 * the original INI / registry backend is unchanged.
 *
 * Same-process objects that share a path share the uncommitted overlay.
 * A committed read uses the snapshot taken at open or at the last sync.
 * sync() is what imports another process's commit. The adapter cannot know
 * which setValue calls the application meant to group: one flush commits
 * whatever overlay is pending.
 */

QT_BEGIN_NAMESPACE

namespace {

static bool backendEnabledFor(QSettings::Format format)
{
    if (qgetenv("QSDB_BACKEND") != QByteArray("1"))
        return false;
    if (format == QSettings::IniFormat)
        return true;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
    if (format == QSettings::NativeFormat)
        return true;
#endif
    return false;
}

static QByteArray keyBytes(const QString &key)
{
    QByteArray out;
    out.resize(key.size() * 2);
    for (int i = 0; i < key.size(); ++i) {
        ushort u = key.at(i).unicode();
        out[i * 2] = char(u & 0xff);
        out[i * 2 + 1] = char((u >> 8) & 0xff);
    }
    return out;
}

static QString keyFromBytes(const void *data, size_t len)
{
    const uchar *p = static_cast<const uchar *>(data);
    int n = int(len / 2);
    QString s(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i)
        s[i] = QChar(ushort(p[i * 2] | (p[i * 2 + 1] << 8)));
    return s;
}

static QString utf16Slice(const uchar *p, int from, int count)
{
    QString s(count, Qt::Uninitialized);
    for (int i = 0; i < count; ++i) {
        const int o = (from + i) * 2;
        s[i] = QChar(ushort(p[o] | (p[o + 1] << 8)));
    }
    return s;
}

static bool sameSlice(const QString &prev, const uchar *p, int from, int count)
{
    if (prev.size() != count)
        return false;
    for (int i = 0; i < count; ++i) {
        const int o = (from + i) * 2;
        if (prev.at(i).unicode() != ushort(p[o] | (p[o + 1] << 8)))
            return false;
    }
    return true;
}

/* Exclusive upper bound of a UTF-16LE prefix. Empty means unbounded. */
static QByteArray exclusiveEnd(const QByteArray &start)
{
    if (start.size() < 2 || (start.size() % 2) != 0)
        return QByteArray();
    QByteArray end = start;
    for (int u = end.size() - 2; u >= 0; u -= 2) {
        unsigned c = uchar(end[u]) | (unsigned(uchar(end[u + 1])) << 8);
        if (c != 0xffffu) {
            ++c;
            end[u] = char(c & 0xff);
            end[u + 1] = char((c >> 8) & 0xff);
            return end;
        }
        end[u] = 0;
        end[u + 1] = 0;
    }
    return QByteArray();
}

static bool containsUserType(const QVariant &v)
{
    if (v.userType() >= QMetaType::User)
        return true;
    if (v.type() == QVariant::List || v.type() == QVariant::StringList) {
        const QVariantList list = v.toList();
        for (int i = 0; i < list.size(); ++i) {
            if (containsUserType(list.at(i)))
                return true;
        }
    }
    return false;
}

static bool encodeVariant(const QVariant &v, QTextCodec *codec, uint32_t *tag, QByteArray *payload)
{
    if (!v.isValid()) {
        *tag = QSDB_TAG_INVALID;
        payload->clear();
        return true;
    }
    if (containsUserType(v)) {
        const char *name = QMetaType::typeName(v.userType());
        if (!name || !name[0])
            return false;
        QByteArray body;
        QDataStream stream(&body, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_12);
        if (!QMetaType::save(stream, v.userType(), v.constData()))
            return false;
        QDataStream framed(payload, QIODevice::WriteOnly);
        framed.setVersion(QDataStream::Qt_5_12);
        framed << QByteArray(name) << body;
        *tag = QSDB_TAG_OPAQUE;
        return framed.status() == QDataStream::Ok;
    }
    payload->clear();
    const bool asList = v.type() == QVariant::StringList
            || (v.type() == QVariant::List && v.toList().size() != 1);
    if (asList)
        QSettingsPrivate::iniEscapedStringList(
                QSettingsPrivate::variantListToStringList(v.toList()), *payload, codec);
    else
        QSettingsPrivate::iniEscapedString(QSettingsPrivate::variantToString(v), *payload, codec);
    *tag = QSDB_TAG_BYTES;
    return true;
}

static QVariant decodeVariant(uint32_t tag, const QByteArray &payload, QTextCodec *codec)
{
    if (tag == QSDB_TAG_INVALID)
        return QVariant();
    if (tag == QSDB_TAG_OPAQUE) {
        QDataStream framed(payload);
        framed.setVersion(QDataStream::Qt_5_12);
        QByteArray name;
        QByteArray body;
        framed >> name >> body;
        if (framed.status() != QDataStream::Ok)
            return QVariant();
        const int type = QMetaType::type(name.constData());
        if (type == QMetaType::UnknownType)
            return QVariant();
        void *data = QMetaType::create(type);
        if (!data)
            return QVariant();
        QDataStream stream(body);
        stream.setVersion(QDataStream::Qt_5_12);
        const bool ok = QMetaType::load(stream, type, data);
        QVariant out;
        if (ok && stream.status() == QDataStream::Ok)
            out = QVariant(type, data);
        QMetaType::destroy(type, data);
        return out;
    }
    QString text;
    QStringList list;
    const bool isList = QSettingsPrivate::iniUnescapedStringList(
            payload, 0, payload.size(), text, list, codec);
    if (isList)
        return QSettingsPrivate::stringListToVariantList(list);
    return QSettingsPrivate::stringToVariant(text);
}

struct QsdbShared {
    QMutex mu;
    qsdb *db;
    qsdb_read *snap;
    QMap<QString, QVariant> added;
    QStringList removed;
    bool cleared;
    int ref;
    QString path;

    QsdbShared() : db(0), snap(0), cleared(false), ref(1) {}
};

QMutex gStoreMu;
QHash<QString, QsdbShared *> gStores;

static bool covered(const QStringList &removed, const QString &key)
{
    for (int i = 0; i < removed.size(); ++i) {
        const QString &prefix = removed.at(i);
        if (key == prefix || key.startsWith(prefix + QLatin1Char('/')))
            return true;
    }
    return false;
}

static void refreshSnap(QsdbShared *shared)
{
    if (shared->snap) {
        qsdb_end_read(shared->snap);
        shared->snap = 0;
    }
    if (shared->db)
        qsdb_begin_read(shared->db, &shared->snap);
}

static QsdbShared *retainStore(const QString &path, bool create, bool *ok)
{
    *ok = false;
    const QString abs = QFileInfo(path).absoluteFilePath();
    QMutexLocker lock(&gStoreMu);
    QsdbShared *existing = gStores.value(abs);
    if (existing) {
        existing->ref++;
        *ok = true;
        return existing;
    }
    QDir().mkpath(QFileInfo(abs).absolutePath());
    const QByteArray native = QFile::encodeName(abs);
    qsdb *db = 0;
    int flags = QSDB_OPEN_UTF16;
    if (create)
        flags |= QSDB_OPEN_CREATE;
    const int rc = qsdb_open(native.constData(), flags, &db);
    if (rc != QSDB_OK && !(rc == QSDB_NOT_FOUND && !create))
        return 0;
    QsdbShared *shared = new QsdbShared;
    shared->db = db;
    shared->path = abs;
    if (db)
        refreshSnap(shared);
    if (db && !shared->snap) {
        qsdb_close(db);
        delete shared;
        return 0;
    }
    gStores.insert(abs, shared);
    *ok = true;
    return shared;
}

static void releaseStore(QsdbShared *shared)
{
    if (!shared)
        return;
    QMutexLocker lock(&gStoreMu);
    if (--shared->ref > 0)
        return;
    gStores.remove(shared->path);
    lock.unlock();
        if (shared->snap)
            qsdb_end_read(shared->snap);
        if (shared->db)
            qsdb_close(shared->db);
        delete shared;
}

class QsdbSettingsPrivate : public QSettingsPrivate
{
public:
    QVector<QsdbShared *> stores;

    QsdbSettingsPrivate(QSettings::Format fmt, QSettings::Scope sc,
                        const QString &org, const QString &app)
        : QSettingsPrivate(fmt, sc, org, app)
    {
        if (org.isEmpty())
            setStatus(QSettings::AccessError);
        const QStringList paths = qsettingsStorageFiles(fmt, sc, org, app);
        for (int i = 0; i < paths.size(); ++i) {
            bool ok = false;
            QsdbShared *shared = retainStore(paths.at(i) + QLatin1String(".qsdb"), i == 0, &ok);
            if (!ok) {
                if (i == 0)
                    setStatus(QSettings::AccessError);
                continue;
            }
            stores.append(shared);
        }
    }

    QsdbSettingsPrivate(const QString &fileName, QSettings::Format fmt)
        : QSettingsPrivate(fmt)
    {
        bool ok = false;
        QsdbShared *shared = retainStore(fileName, true, &ok);
        if (!ok)
            setStatus(QSettings::AccessError);
        else
            stores.append(shared);
    }

    ~QsdbSettingsPrivate()
    {
        for (int i = 0; i < stores.size(); ++i)
            releaseStore(stores.at(i));
    }

    QsdbShared *primary() const
    {
        return stores.isEmpty() ? 0 : stores.at(0);
    }

    bool readCommitted(QsdbShared *shared, const QString &key, QVariant *value) const
    {
        if (!shared->snap)
            return false;
        const QByteArray kb = keyBytes(key);
        if (kb.size() > QSDB_MAX_KEY_BYTES)
            return false;
        if (!value) {
            const int rc = qsdb_contains(shared->snap, kb.constData(), size_t(kb.size()));
            if (rc == QSDB_NOT_FOUND)
                return false;
            if (rc != QSDB_OK) {
                setStatus(rc == QSDB_CORRUPT ? QSettings::FormatError : QSettings::AccessError);
                return false;
            }
            return true;
        }
        uint32_t tag = 0;
        void *mem = 0;
        size_t len = 0;
        const int rc = qsdb_get_alloc(shared->snap, kb.constData(), size_t(kb.size()),
                                      &tag, &mem, &len);
        if (rc == QSDB_NOT_FOUND)
            return false;
        if (rc != QSDB_OK) {
            setStatus(rc == QSDB_CORRUPT ? QSettings::FormatError : QSettings::AccessError);
            return false;
        }
        if (len > 0x7fffffffU) {
            free(mem);
            setStatus(QSettings::AccessError);
            return false;
        }
        QByteArray payload;
        if (len)
            payload = QByteArray(static_cast<const char *>(mem), int(len));
        free(mem);
        *value = decodeVariant(tag, payload, iniCodec);
        return true;
    }

    void remove(const QString &key) override
    {
        QsdbShared *shared = primary();
        if (!shared)
            return;
        QMutexLocker lock(&shared->mu);
        QMap<QString, QVariant>::iterator it = shared->added.begin();
        while (it != shared->added.end()) {
            if (it.key() == key || it.key().startsWith(key + QLatin1Char('/')))
                it = shared->added.erase(it);
            else
                ++it;
        }
        shared->removed.append(key);
    }

    void set(const QString &key, const QVariant &value) override
    {
        QsdbShared *shared = primary();
        if (!shared)
            return;
        if (key.size() > QSDB_MAX_KEY_BYTES / 2) {
            setStatus(QSettings::AccessError);
            return;
        }
        QMutexLocker lock(&shared->mu);
        shared->added.insert(key, value);
    }

    bool get(const QString &key, QVariant *value) const override
    {
        for (int i = 0; i < stores.size(); ++i) {
            QsdbShared *shared = stores.at(i);
            QMutexLocker lock(&shared->mu);
            if (i == 0) {
                const QMap<QString, QVariant>::const_iterator found = shared->added.constFind(key);
                if (found != shared->added.constEnd()) {
                    if (value)
                        *value = found.value();
                    return true;
                }
                if (shared->cleared || covered(shared->removed, key)) {
                    if (!fallbacks)
                        return false;
                    continue;
                }
            }
            if (readCommitted(shared, key, value))
                return true;
            if (!fallbacks)
                return false;
        }
        return false;
    }

    QStringList children(const QString &prefix, ChildSpec spec) const override
    {
        if (stores.size() == 1) {
            QsdbShared *shared = stores.at(0);
            QMutexLocker lock(&shared->mu);
            if (shared->snap && !shared->cleared && shared->added.isEmpty() && shared->removed.isEmpty()) {
                struct Fast {
                    int startChars;
                    int spec;
                    QStringList out;
                    QString lastGroup;
                    static int cb(const void *key, size_t keyLen, uint32_t, uint64_t, void *user)
                    {
                        Fast *f = static_cast<Fast *>(user);
                        if ((keyLen % 2) != 0 || int(keyLen / 2) < f->startChars)
                            return 0;
                        const uchar *p = static_cast<const uchar *>(key);
                        const int nchars = int(keyLen / 2);
                        const int rel = nchars - f->startChars;
                        int slash = -1;
                        for (int i = 0; i < rel; ++i) {
                            const int o = (f->startChars + i) * 2;
                            if (ushort(p[o] | (p[o + 1] << 8)) == '/') {
                                slash = i;
                                break;
                            }
                        }
                        if (f->spec == int(AllKeys)) {
                            f->out.append(utf16Slice(p, f->startChars, rel));
                        } else if (f->spec == int(ChildKeys)) {
                            if (slash < 0)
                                f->out.append(utf16Slice(p, f->startChars, rel));
                        } else if (slash >= 0 && !sameSlice(f->lastGroup, p, f->startChars, slash)) {
                            f->lastGroup = utf16Slice(p, f->startChars, slash);
                            f->out.append(f->lastGroup);
                        }
                        return 0;
                    }
                } fast;
                fast.startChars = prefix.size();
                fast.spec = int(spec);
                const QByteArray start = prefix.isEmpty() ? QByteArray() : keyBytes(prefix);
                const QByteArray end = exclusiveEnd(start);
                qsdb_scan_range(shared->snap, start.isEmpty() ? 0 : start.constData(),
                                size_t(start.size()),
                                end.isEmpty() ? 0 : end.constData(), size_t(end.size()),
                                Fast::cb, &fast);
                return fast.out;
            }
        }
        QMap<QString, int> merged;
        for (int i = stores.size() - 1; i >= 0; --i) {
            QsdbShared *shared = stores.at(i);
            QMutexLocker lock(&shared->mu);
            if (!(i == 0 && shared->cleared) && shared->snap) {
                struct Acc {
                    QString prefix;
                    QStringList keys;
                    static int cb(const void *key, size_t keyLen, uint32_t, uint64_t, void *user)
                    {
                        Acc *a = static_cast<Acc *>(user);
                        const QString full = keyFromBytes(key, keyLen);
                        if (!a->prefix.isEmpty() && !full.startsWith(a->prefix)) {
                            if (full > a->prefix)
                                return 1;
                            return 0;
                        }
                        a->keys.append(full);
                        return 0;
                    }
                } acc;
                acc.prefix = prefix;
                const QByteArray start = prefix.isEmpty() ? QByteArray() : keyBytes(prefix);
                const QByteArray end = exclusiveEnd(start);
                qsdb_scan_range(shared->snap, start.isEmpty() ? 0 : start.constData(),
                                size_t(start.size()),
                                end.isEmpty() ? 0 : end.constData(), size_t(end.size()),
                                Acc::cb, &acc);
                for (int k = 0; k < acc.keys.size(); ++k) {
                    const QString &full = acc.keys.at(k);
                    if (!prefix.isEmpty() && full != prefix && !full.startsWith(prefix))
                        continue;
                    if (i == 0 && covered(shared->removed, full))
                        continue;
                    merged.insert(full, 1);
                }
            }
            if (i == 0) {
                for (QMap<QString, QVariant>::const_iterator it = shared->added.constBegin();
                     it != shared->added.constEnd(); ++it)
                    merged.insert(it.key(), 1);
            }
        }
        QStringList result;
        const int startPos = prefix.size();
        for (QMap<QString, int>::const_iterator it = merged.constBegin(); it != merged.constEnd(); ++it) {
            QStringRef rel = it.key().midRef(startPos);
            processChild(rel, spec, result);
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    void clear() override
    {
        QsdbShared *shared = primary();
        if (!shared)
            return;
        QMutexLocker lock(&shared->mu);
        shared->added.clear();
        shared->removed.clear();
        shared->cleared = true;
    }

    int commitOne(QsdbShared *shared)
    {
        QMutexLocker lock(&shared->mu);
        if (!shared->cleared && shared->added.isEmpty() && shared->removed.isEmpty()) {
            refreshSnap(shared);
            return QSDB_OK;
        }
        if (!shared->db) {
            const QByteArray native = QFile::encodeName(shared->path);
            int orc = qsdb_open(native.constData(), QSDB_OPEN_UTF16 | QSDB_OPEN_CREATE, &shared->db);
            if (orc != QSDB_OK)
                return orc;
        }
        qsdb_write *tx = 0;
        int rc = qsdb_begin_write(shared->db, &tx);
        if (rc)
            return rc;
        if (shared->cleared) {
            rc = qsdb_clear(tx);
            if (rc) {
                qsdb_abort(tx);
                return rc;
            }
        }
        const QByteArray sep = keyBytes(QString(QLatin1Char('/')));
        for (int i = 0; i < shared->removed.size() && rc == QSDB_OK; ++i) {
            const QByteArray kb = keyBytes(shared->removed.at(i));
            rc = qsdb_erase_tree(tx, kb.constData(), size_t(kb.size()), sep.constData(), size_t(sep.size()));
            if (rc == QSDB_NOT_FOUND)
                rc = QSDB_OK;
        }
        for (QMap<QString, QVariant>::const_iterator it = shared->added.constBegin();
             it != shared->added.constEnd() && rc == QSDB_OK; ++it) {
            uint32_t tag = 0;
            QByteArray payload;
            if (!encodeVariant(it.value(), iniCodec, &tag, &payload)) {
                qsdb_abort(tx);
                return QSDB_INVAL;
            }
            if (uint64_t(payload.size()) > QSDB_MAX_VALUE_BYTES) {
                qsdb_abort(tx);
                return QSDB_TOO_BIG;
            }
            const QByteArray kb = keyBytes(it.key());
            rc = qsdb_put(tx, kb.constData(), size_t(kb.size()), tag,
                          payload.constData(), size_t(payload.size()));
        }
        if (rc) {
            qsdb_abort(tx);
            return rc;
        }
        rc = qsdb_commit(tx);
        if (rc == QSDB_OK) {
            shared->added.clear();
            shared->removed.clear();
            shared->cleared = false;
            refreshSnap(shared);
        }
        return rc;
    }

    void sync() override
    {
        if (stores.isEmpty())
            return;
        const int rc = commitOne(stores.at(0));
        if (rc == QSDB_NOSPC || rc == QSDB_IO || rc == QSDB_BUSY || rc == QSDB_UNCERTAIN
            || rc == QSDB_TOO_BIG || rc == QSDB_INVAL)
            setStatus(QSettings::AccessError);
        else if (rc == QSDB_CORRUPT || rc == QSDB_MISMATCH)
            setStatus(QSettings::FormatError);
        for (int i = 1; i < stores.size(); ++i) {
            QMutexLocker lock(&stores.at(i)->mu);
            refreshSnap(stores.at(i));
        }
    }

    void flush() override { sync(); }

    bool isWritable() const override
    {
        const QsdbShared *shared = primary();
        if (!shared)
            return false;
        const QFileInfo info(shared->path);
        if (info.exists())
            return info.isWritable();
        return QFileInfo(info.absolutePath()).isWritable();
    }

    QString fileName() const override
    {
        const QsdbShared *shared = primary();
        return shared ? shared->path : QString();
    }
};

} // namespace

QSettingsPrivate *qsdbCreateIfEnabled(QSettings::Format format, QSettings::Scope scope,
                                      const QString &organization, const QString &application)
{
    if (!backendEnabledFor(format))
        return 0;
    return new QsdbSettingsPrivate(format, scope, organization, application);
}

QSettingsPrivate *qsdbCreateFileIfEnabled(const QString &fileName, QSettings::Format format)
{
    if (!backendEnabledFor(format))
        return 0;
    return new QsdbSettingsPrivate(fileName, format);
}

QT_END_NAMESPACE

#endif // QT_BOOTSTRAPPED
