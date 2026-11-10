#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtCore/QVariant>

#include <algorithm>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static void onAbort(int)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    _exit(99);
}

static uint64_t mono()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static void summarize(const char *backend, const char *scenario, const std::vector<uint64_t> &samples,
                      FILE *raw, QTextStream &summary)
{
    if (samples.empty())
        return;
    std::vector<uint64_t> s = samples;
    std::sort(s.begin(), s.end());
    const int n = int(s.size());
    const uint64_t first = samples[0];
    const uint64_t med = s[n / 2];
    const int p95i = std::max(0, (n - 1) * 95 / 100);
    const uint64_t p95 = s[size_t(p95i)];
    const double ops = med ? 1e9 / double(med) : 0;
    summary << backend << "," << scenario << "," << (double(first) / 1000.0) << ","
            << (double(med) / 1000.0) << "," << (double(p95) / 1000.0) << ","
            << ops << "," << n << "\n";
    for (int i = 0; i < n; ++i)
        fprintf(raw, "%s,%s,%d,%llu\n", backend, scenario, i, (unsigned long long)samples[size_t(i)]);
}

static QByteArray fingerprint(QSettings &s, int *keys, int *typeString, int *typeOther)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QStringList all = s.allKeys();
    *keys = all.size();
    *typeString = 0;
    *typeOther = 0;
    for (int i = 0; i < all.size(); ++i) {
        const QVariant v = s.value(all.at(i));
        if (v.type() == QVariant::String)
            (*typeString)++;
        else
            (*typeOther)++;
        hash.addData(all.at(i).toUtf8());
        hash.addData("\0", 1);
        hash.addData(QByteArray::number(int(v.userType())));
        hash.addData("\0", 1);
        hash.addData(v.toString().toUtf8());
        if (v.type() == QVariant::ByteArray) {
            hash.addData("\0", 1);
            hash.addData(v.toByteArray());
        }
        if (v.type() == QVariant::StringList) {
            const QStringList list = v.toStringList();
            for (int j = 0; j < list.size(); ++j) {
                hash.addData("\n", 1);
                hash.addData(list.at(j).toUtf8());
            }
        }
    }
    return hash.result().toHex();
}

static int selfTest()
{
    const QString root = QDir::temp().filePath(QStringLiteral("qsdb-selftest"));
    QDir(root).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/user"));
    QDir().mkpath(root + QStringLiteral("/system"));
    qputenv("QSDB_BACKEND", "1");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + QStringLiteral("/user"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + QStringLiteral("/system"));

    {
        QSettings sys(QSettings::IniFormat, QSettings::SystemScope,
                      QStringLiteral("Org"), QStringLiteral("App"));
        sys.setValue(QStringLiteral("shared"), QStringLiteral("from-system"));
        sys.setValue(QStringLiteral("foo"), 1);
        sys.setValue(QStringLiteral("foobar"), 2);
        sys.setValue(QStringLiteral("foo/a"), 3);
        sys.sync();
        if (sys.status() != QSettings::NoError)
            return 2;
    }
    {
        QSettings user(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Org"), QStringLiteral("App"));
        QSettings user2(QSettings::IniFormat, QSettings::UserScope,
                        QStringLiteral("Org"), QStringLiteral("App"));
        user.setValue(QStringLiteral("only"), QStringLiteral("u"));
        if (user2.value(QStringLiteral("only")).toString() != QLatin1String("u"))
            return 3;
        if (user.value(QStringLiteral("shared")).toString() != QLatin1String("from-system"))
            return 4;
        user.remove(QStringLiteral("shared"));
        user.sync();
        if (user.value(QStringLiteral("shared")).toString() != QLatin1String("from-system"))
            return 5;
        user.setValue(QStringLiteral("gone"), QVariant());
        if (!user.contains(QStringLiteral("gone")))
            return 6;
        if (user.contains(QStringLiteral("missing-key")))
            return 7;
        /* foo* live in the system store. Removing them from the user store
         * must not hide the fallback values. */
        user.remove(QStringLiteral("foo"));
        user.sync();
        if (!user.contains(QStringLiteral("foo")) || !user.contains(QStringLiteral("foobar")))
            return 8;

        user.beginGroup(QStringLiteral("main"));
        user.beginWriteArray(QStringLiteral("arr"));
        user.setArrayIndex(0);
        user.setValue(QStringLiteral("name"), QStringLiteral("a"));
        user.endArray();
        user.endGroup();
        user.sync();
    }
    {
        QSettings user(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Org"), QStringLiteral("App"));
        user.beginGroup(QStringLiteral("main"));
        const int n = user.beginReadArray(QStringLiteral("arr"));
        if (n != 1)
            return 10;
        user.setArrayIndex(0);
        if (user.value(QStringLiteral("name")).toString() != QLatin1String("a"))
            return 11;
        user.endArray();
        user.endGroup();
        const QStringList groups = user.childGroups();
        if (!groups.contains(QStringLiteral("main")))
            return 12;
    }
    {
        const QString db = root + QStringLiteral("/tree.qsdb");
        QSettings s(db, QSettings::IniFormat);
        s.setValue(QStringLiteral("foo"), 1);
        s.setValue(QStringLiteral("foobar"), 2);
        s.setValue(QStringLiteral("foo/a"), 3);
        s.remove(QStringLiteral("foo"));
        s.sync();
        if (s.contains(QStringLiteral("foo")) || s.contains(QStringLiteral("foo/a")))
            return 13;
        if (!s.contains(QStringLiteral("foobar")))
            return 14;
        if (s.value(QStringLiteral("foobar")).toInt() != 2)
            return 15;
    }
    QDir(root).removeRecursively();
    return 0;
}

static int importCheck(const QString &iniPath, const QString &dbPath)
{
    qunsetenv("QSDB_BACKEND");
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        fprintf(stderr, "ini status %d\n", int(ini.status()));
        return 2;
    }
    int ik = 0, is = 0, io = 0;
    const QByteArray ihash = fingerprint(ini, &ik, &is, &io);
    const QStringList keys = ini.allKeys();

    QFile::remove(dbPath);
    qputenv("QSDB_BACKEND", "1");
    {
        QSettings db(dbPath, QSettings::IniFormat);
        for (int i = 0; i < keys.size(); ++i)
            db.setValue(keys.at(i), ini.value(keys.at(i)));
        db.sync();
        if (db.status() != QSettings::NoError) {
            fprintf(stderr, "import status %d\n", int(db.status()));
            return 3;
        }
    }
    QSettings db(dbPath, QSettings::IniFormat);
    db.sync();
    int dk = 0, ds = 0, dother = 0;
    const QByteArray dhash = fingerprint(db, &dk, &ds, &dother);
    int mismatch = 0;
    int typeMismatch = 0;
    for (int i = 0; i < keys.size(); ++i) {
        const QVariant a = ini.value(keys.at(i));
        const QVariant b = db.value(keys.at(i));
        if (a.userType() != b.userType())
            typeMismatch++;
        if (a.toString() != b.toString() || a.toByteArray() != b.toByteArray())
            mismatch++;
    }
    const QFileInfo info(dbPath);
    printf("ini_keys=%d ini_string=%d ini_other=%d ini_sha256=%s\n",
           ik, is, io, ihash.constData());
    printf("db_keys=%d db_string=%d db_other=%d db_sha256=%s\n",
           dk, ds, dother, dhash.constData());
    printf("value_mismatches=%d type_mismatches=%d db_bytes=%lld\n",
           mismatch, typeMismatch, (long long)info.size());
    return (mismatch || typeMismatch || ik != dk) ? 4 : 0;
}

static QSettings *openBackend(const QString &path, bool qsdb)
{
    if (qsdb)
        qputenv("QSDB_BACKEND", "1");
    else
        qunsetenv("QSDB_BACKEND");
    return new QSettings(path, QSettings::IniFormat);
}

static void restoreFile(const QString &pristine, const QString &path)
{
    QFile::remove(path);
    if (!QFile::copy(pristine, path))
        fprintf(stderr, "restore failed\n");
}

static bool fieldNs(const QString &text, const char *name, uint64_t *out)
{
    const QString needle = QLatin1String(name) + QLatin1Char('=');
    const int at = text.indexOf(needle);
    if (at < 0)
        return false;
    bool ok = false;
    *out = text.mid(at + needle.size()).section(QLatin1Char('\n'), 0, 0).toULongLong(&ok);
    return ok;
}

static int freshOne(const QString &kind, const QString &path, const QString &key)
{
    const bool qsdb = kind == QLatin1String("qsdb");
    const uint64_t t0 = mono();
    QSettings *s = openBackend(path, qsdb);
    const uint64_t t1 = mono();
    const QVariant first = s->value(key);
    const uint64_t t2 = mono();
    const QVariant second = s->value(key);
    const uint64_t t3 = mono();
    s->contains(QStringLiteral("zz-missing-bench"));
    const uint64_t t4 = mono();
    s->contains(key);
    const uint64_t t5 = mono();
    const int status = int(s->status());
    delete s;
    printf("construct_ns=%llu\nfirst_value_ns=%llu\nsecond_value_ns=%llu\nmiss_ns=%llu\ncontains_ns=%llu\nstatus=%d\nvalid=%d\n",
           (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
           (unsigned long long)(t3 - t2), (unsigned long long)(t4 - t3),
           (unsigned long long)(t5 - t4), status, int(first.isValid() && second.isValid()));
    return status == int(QSettings::NoError) ? 0 : 2;
}

static int runFresh(const QString &exe, const QString &kind, const QString &path, const QString &key,
                    std::vector<uint64_t> *construct, std::vector<uint64_t> *first,
                    std::vector<uint64_t> *second, std::vector<uint64_t> *miss,
                    std::vector<uint64_t> *contains)
{
    for (int i = 0; i < 21; ++i) {
        QProcess child;
        child.start(exe, QStringList() << QStringLiteral("fresh-one") << kind << path << key);
        if (!child.waitForFinished(120000) || child.exitCode() != 0)
            return 2;
        const QString text = QString::fromUtf8(child.readAllStandardOutput());
        uint64_t a = 0, b = 0, c = 0, d = 0, e = 0;
        if (!fieldNs(text, "construct_ns", &a) || !fieldNs(text, "first_value_ns", &b)
            || !fieldNs(text, "second_value_ns", &c) || !fieldNs(text, "miss_ns", &d)
            || !fieldNs(text, "contains_ns", &e))
            return 3;
        construct->push_back(a);
        first->push_back(b);
        second->push_back(c);
        miss->push_back(d);
        contains->push_back(e);
    }
    return 0;
}

static int bench(const QString &iniPath, const QString &dbPath, const QString &outDir)
{
    QDir().mkpath(outDir);
    QFile rawFile(outDir + QStringLiteral("/samples.csv"));
    if (!rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    FILE *raw = fdopen(dup(rawFile.handle()), "w");
    fprintf(raw, "backend,scenario,trial,ns\n");
    QFile sumFile(outDir + QStringLiteral("/summary.csv"));
    if (!sumFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 3;
    QTextStream summary(&sumFile);
    summary << "backend,scenario,first_us,median_us,p95_us,ops_per_s,samples\n";
    QFile noteFile(outDir + QStringLiteral("/timing.txt"));
    if (noteFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream note(&noteFile);
        note << "clock=CLOCK_MONOTONIC\n";
        note << "samples=21 for one-shot scenarios; hot_get/miss/contains are 21*200 individual calls\n";
        note << "median=sorted[n/2]\n";
        note << "p95=sorted[(n-1)*95/100]\n";
        note << "first_us=sample 0 before sorting, so it is the first call in that process\n";
        note << "fresh_*=new process, no in-process QSettings cache. OS page cache is warm. This is not a cold-disk number.\n";
        note << "reuse_construct=second QSettings on the same path while the first object is still alive\n";
        note << "hot_*=calls on that already-open object, after one untimed value() warmup\n";
        note << "pure_set_existing=setValue only. rmw_set_existing=value()+setValue of the same string\n";
        note << "open/restore is outside remove, clear, and sync timers\n";
        note << "import parse is not included in any get number\n";
    }

    qunsetenv("QSDB_BACKEND");
    QStringList keys;
    QString group;
    {
        QSettings ini(iniPath, QSettings::IniFormat);
        keys = ini.allKeys();
        const QStringList groups = ini.childGroups();
        if (!groups.isEmpty())
            group = groups.at(0);
    }
    if (keys.isEmpty())
        return 4;
    const QString hitKey = keys.at(keys.size() / 2);
    const QString exe = QCoreApplication::applicationFilePath();

    struct Target {
        const char *name;
        QString path;
        bool qsdb;
    } targets[2] = {
        {"ini", iniPath + QStringLiteral(".bench"), false},
        {"qsdb", dbPath + QStringLiteral(".bench"), true}
    };
    QFile::remove(targets[0].path);
    QFile::remove(targets[1].path);
    if (!QFile::copy(iniPath, targets[0].path) || !QFile::copy(dbPath, targets[1].path))
        return 5;

    for (int t = 0; t < 2; ++t) {
        const QString pristine = targets[t].path + QStringLiteral(".pristine");
        QFile::remove(pristine);
        QFile::copy(targets[t].path, pristine);

        std::vector<uint64_t> freshOpen, freshFirst, freshSecond, freshMiss, freshContains;
        const int fr = runFresh(exe, targets[t].qsdb ? QStringLiteral("qsdb") : QStringLiteral("ini"),
                                targets[t].path, hitKey, &freshOpen, &freshFirst, &freshSecond,
                                &freshMiss, &freshContains);
        if (fr)
            return 10 + fr;
        summarize(targets[t].name, "fresh_construct", freshOpen, raw, summary);
        summarize(targets[t].name, "fresh_first_value", freshFirst, raw, summary);
        summarize(targets[t].name, "fresh_second_value", freshSecond, raw, summary);
        summarize(targets[t].name, "fresh_miss", freshMiss, raw, summary);
        summarize(targets[t].name, "fresh_contains", freshContains, raw, summary);

        QSettings *live = openBackend(targets[t].path, targets[t].qsdb);
        std::vector<uint64_t> reuse;
        for (int i = 0; i < 21; ++i) {
            const uint64_t a = mono();
            QSettings *extra = openBackend(targets[t].path, targets[t].qsdb);
            const uint64_t b = mono();
            delete extra;
            reuse.push_back(b - a);
        }
        summarize(targets[t].name, "reuse_construct", reuse, raw, summary);
        live->value(hitKey);

        std::vector<uint64_t> hit, miss, contains;
        unsigned seed = 20261110u;
        for (int n = 0; n < 21 * 200; ++n) {
            seed = seed * 1664525u + 1013904223u;
            const QString k = keys.at(int(seed % unsigned(keys.size())));
            const uint64_t a = mono();
            live->value(k);
            const uint64_t b = mono();
            live->contains(QStringLiteral("zz-missing-bench"));
            const uint64_t c = mono();
            live->contains(k);
            const uint64_t d = mono();
            hit.push_back(b - a);
            miss.push_back(c - b);
            contains.push_back(d - c);
        }
        summarize(targets[t].name, "hot_get_hit", hit, raw, summary);
        summarize(targets[t].name, "hot_get_miss", miss, raw, summary);
        summarize(targets[t].name, "hot_contains_hit", contains, raw, summary);

        std::vector<uint64_t> childKeys, childGroups, allKeys, syncNoop;
        for (int i = 0; i < 21; ++i) {
            uint64_t a = mono();
            live->childKeys();
            uint64_t b = mono();
            live->childGroups();
            uint64_t c = mono();
            live->allKeys();
            uint64_t d = mono();
            live->sync();
            uint64_t e = mono();
            childKeys.push_back(b - a);
            childGroups.push_back(c - b);
            allKeys.push_back(d - c);
            syncNoop.push_back(e - d);
        }
        summarize(targets[t].name, "childKeys", childKeys, raw, summary);
        summarize(targets[t].name, "childGroups", childGroups, raw, summary);
        summarize(targets[t].name, "allKeys", allKeys, raw, summary);
        summarize(targets[t].name, "sync_noop", syncNoop, raw, summary);

        std::vector<uint64_t> pureSet, rmw, pureNew;
        for (int i = 0; i < 21; ++i) {
            const uint64_t a = mono();
            live->setValue(hitKey, QStringLiteral("bench-pure"));
            const uint64_t b = mono();
            pureSet.push_back(b - a);
            const uint64_t c = mono();
            const QString cur = live->value(hitKey).toString();
            live->setValue(hitKey, cur);
            const uint64_t d = mono();
            rmw.push_back(d - c);
            const uint64_t e = mono();
            live->setValue(QStringLiteral("bench-new-%1").arg(i), i);
            const uint64_t f = mono();
            pureNew.push_back(f - e);
        }
        summarize(targets[t].name, "pure_set_existing", pureSet, raw, summary);
        summarize(targets[t].name, "rmw_set_existing", rmw, raw, summary);
        summarize(targets[t].name, "pure_set_new", pureNew, raw, summary);
        delete live;

        std::vector<uint64_t> oneSet, oneSync, batchEach, batchSync;
        std::vector<uint64_t> rmKey, rmGroup, clearNs;
        qint64 sizeOne = 0, sizeGroup = 0, sizeClear = 0;
        for (int i = 0; i < 21; ++i) {
            restoreFile(pristine, targets[t].path);
            QSettings *s = openBackend(targets[t].path, targets[t].qsdb);
            const uint64_t a = mono();
            s->setValue(QStringLiteral("bench-sync-one"), QStringLiteral("1"));
            const uint64_t b = mono();
            s->sync();
            const uint64_t c = mono();
            oneSet.push_back(b - a);
            oneSync.push_back(c - b);
            if (i == 0)
                sizeOne = QFileInfo(targets[t].path).size();
            delete s;

            restoreFile(pristine, targets[t].path);
            s = openBackend(targets[t].path, targets[t].qsdb);
            const uint64_t d0 = mono();
            for (int n = 0; n < 50; ++n)
                s->setValue(QStringLiteral("bench-batch-%1").arg(n), n);
            const uint64_t d1 = mono();
            s->sync();
            const uint64_t d2 = mono();
            batchEach.push_back((d1 - d0) / 50);
            batchSync.push_back(d2 - d1);
            delete s;

            restoreFile(pristine, targets[t].path);
            s = openBackend(targets[t].path, targets[t].qsdb);
            const uint64_t r0 = mono();
            s->remove(keys.at(i % keys.size()));
            s->sync();
            const uint64_t r1 = mono();
            rmKey.push_back(r1 - r0);
            if (!group.isEmpty()) {
                const uint64_t g0 = mono();
                s->remove(group);
                s->sync();
                const uint64_t g1 = mono();
                rmGroup.push_back(g1 - g0);
                if (i == 0)
                    sizeGroup = QFileInfo(targets[t].path).size();
            }
            const uint64_t c0 = mono();
            s->clear();
            s->sync();
            const uint64_t c1 = mono();
            clearNs.push_back(c1 - c0);
            if (i == 0)
                sizeClear = QFileInfo(targets[t].path).size();
            delete s;
        }
        summarize(targets[t].name, "pure_set_new_isolated", oneSet, raw, summary);
        summarize(targets[t].name, "sync_after_one_set", oneSync, raw, summary);
        summarize(targets[t].name, "set_batch50_each", batchEach, raw, summary);
        summarize(targets[t].name, "sync_batch50", batchSync, raw, summary);
        summarize(targets[t].name, "remove_one_key_and_sync", rmKey, raw, summary);
        if (!rmGroup.empty())
            summarize(targets[t].name, "remove_one_group_and_sync", rmGroup, raw, summary);
        summarize(targets[t].name, "clear_and_sync", clearNs, raw, summary);

        std::vector<uint64_t> externalSync;
        int externalFail = 0;
        for (int i = 0; i < 21; ++i) {
            restoreFile(pristine, targets[t].path);
            if (targets[t].qsdb)
                qputenv("QSDB_BACKEND", "1");
            else
                qunsetenv("QSDB_BACKEND");
            QSettings parent(targets[t].path, QSettings::IniFormat);
            const bool seenBefore = parent.contains(QStringLiteral("k-external-probe"));
            QProcess child;
            child.start(exe, QStringList() << QStringLiteral("external-writer")
                        << (targets[t].qsdb ? QStringLiteral("qsdb") : QStringLiteral("ini"))
                        << targets[t].path);
            if (!child.waitForFinished(60000) || child.exitCode() != 0)
                return 30;
            const bool seenMid = parent.contains(QStringLiteral("k-external-probe"));
            const uint64_t t0 = mono();
            parent.sync();
            const uint64_t t1 = mono();
            const bool seenAfter = parent.value(QStringLiteral("k-external-probe")).toString()
                    == QLatin1String("from-other-process");
            externalSync.push_back(t1 - t0);
            if (seenBefore || seenMid || !seenAfter)
                externalFail++;
        }
        summarize(targets[t].name, "external_sync", externalSync, raw, summary);
        printf("%s external_fail=%d initial_bytes=%lld after_one_sync_bytes=%lld after_group_delete_bytes=%lld after_clear_bytes=%lld\n",
               targets[t].name, externalFail,
               (long long)QFileInfo(pristine).size(),
               (long long)sizeOne, (long long)sizeGroup, (long long)sizeClear);
        if (externalFail)
            return 31;
    }
    fflush(raw);
    summary.flush();
    printf("wrote %s\n", qPrintable(outDir));
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGABRT, onAbort);
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        fprintf(stderr, "usage: qt_driver selftest|import|bench|fresh-one|external ...\n");
        return 1;
    }
    const QString cmd = args.at(1);
    if (cmd == QLatin1String("selftest"))
        return selfTest();
    if (cmd == QLatin1String("import")) {
        if (args.size() < 4)
            return 1;
        return importCheck(args.at(2), args.at(3));
    }
    if (cmd == QLatin1String("bench")) {
        if (args.size() < 5)
            return 1;
        return bench(args.at(2), args.at(3), args.at(4));
    }
    if (cmd == QLatin1String("fresh-one")) {
        if (args.size() < 5)
            return 1;
        return freshOne(args.at(2), args.at(3), args.at(4));
    }
    if (cmd == QLatin1String("external-writer")) {
        QString kind = QStringLiteral("qsdb");
        QString path;
        if (args.size() >= 4 && (args.at(2) == QLatin1String("ini") || args.at(2) == QLatin1String("qsdb"))) {
            kind = args.at(2);
            path = args.at(3);
        } else if (args.size() >= 3) {
            path = args.at(2);
        } else {
            return 1;
        }
        if (kind == QLatin1String("qsdb"))
            qputenv("QSDB_BACKEND", "1");
        else
            qunsetenv("QSDB_BACKEND");
        QSettings s(path, QSettings::IniFormat);
        s.setValue(QStringLiteral("k-external-probe"), QStringLiteral("from-other-process"));
        s.sync();
        return s.status() == QSettings::NoError ? 0 : 2;
    }
    if (cmd == QLatin1String("external")) {
        qputenv("QSDB_BACKEND", "1");
        QSettings parent(args.at(2), QSettings::IniFormat);
        const bool seenBefore = parent.contains(QStringLiteral("k-external-probe"));
        QProcess child;
        child.start(args.at(0), QStringList() << QStringLiteral("external-writer") << args.at(2));
        if (!child.waitForFinished(60000) || child.exitCode() != 0)
            return 2;
        const bool seenMid = parent.value(QStringLiteral("k-external-probe")).toString()
                == QLatin1String("from-other-process");
        const uint64_t t0 = mono();
        parent.sync();
        const uint64_t t1 = mono();
        const bool seenAfter = parent.value(QStringLiteral("k-external-probe")).toString()
                == QLatin1String("from-other-process");
        printf("external_seen_before=%d seen_without_sync=%d seen_after_sync=%d sync_ns=%llu\n",
               int(seenBefore), int(seenMid), int(seenAfter),
               (unsigned long long)(t1 - t0));
        return seenAfter && !seenMid ? 0 : 3;
    }
    return 1;
}
