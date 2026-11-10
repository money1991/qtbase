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
    std::vector<uint64_t> s = samples;
    std::sort(s.begin(), s.end());
    const int n = int(s.size());
    const uint64_t med = s[n / 2];
    const int p95i = std::max(0, (n - 1) * 95 / 100);
    const uint64_t p95 = s[size_t(p95i)];
    const double us = double(med) / 1000.0;
    const double ops = med ? 1e9 / double(med) : 0;
    summary << backend << "," << scenario << ",us," << us << "," << (double(p95) / 1000.0)
            << "," << ops << "," << n << "\n";
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

static int bench(const QString &iniPath, const QString &dbPath, const QString &outDir)
{
    QDir().mkpath(outDir);
    QFile rawFile(outDir + QStringLiteral("/samples.csv"));
    if (!rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    FILE *raw = fdopen(rawFile.handle(), "w");
    fprintf(raw, "backend,scenario,trial,ns\n");
    QFile sumFile(outDir + QStringLiteral("/summary.csv"));
    if (!sumFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 3;
    QTextStream summary(&sumFile);
    summary << "backend,scenario,median_unit,median_us,p95_us,ops_per_s,samples\n";

    qunsetenv("QSDB_BACKEND");
    QSettings ini(iniPath, QSettings::IniFormat);
    const QStringList keys = ini.allKeys();
    if (keys.isEmpty())
        return 4;
    QStringList groups = ini.childGroups();
    const int trials = 21;
    unsigned seed = 20261110u;

    struct Target {
        const char *name;
        QString path;
        bool qsdb;
    } targets[2] = {
        {"ini", iniPath, false},
        {"qsdb", dbPath, true}
    };

    for (int t = 0; t < 2; ++t) {
        const QString pristine = targets[t].path + QStringLiteral(".pristine");
        QFile::remove(pristine);
        QFile::copy(targets[t].path, pristine);
        std::vector<uint64_t> openNs, firstNs, hitNs, missNs, containsNs;
        std::vector<uint64_t> setOldNs, setNewNs, syncOneNs, batchSetNs, batchSyncNs;
        std::vector<uint64_t> childKeysNs, childGroupsNs, allKeysNs, syncNoopNs;
        QString hitKey = keys.at(seed % keys.size());
        for (int i = 0; i < trials; ++i) {
            seed = seed * 1664525u + 1013904223u;
            hitKey = keys.at(int(seed % unsigned(keys.size())));
            const uint64_t t0 = mono();
            QSettings *s = openBackend(targets[t].path, targets[t].qsdb);
            const uint64_t t1 = mono();
            QVariant v = s->value(hitKey);
            const uint64_t t2 = mono();
            openNs.push_back(t1 - t0);
            firstNs.push_back(t2 - t1);
            Q_UNUSED(v);
            uint64_t accHit = 0, accMiss = 0, accCon = 0;
            for (int n = 0; n < 200; ++n) {
                seed = seed * 1664525u + 1013904223u;
                const QString k = keys.at(int(seed % unsigned(keys.size())));
                uint64_t a = mono();
                s->value(k);
                uint64_t b = mono();
                s->contains(QStringLiteral("zz-missing-bench"));
                uint64_t c = mono();
                s->contains(k);
                uint64_t d = mono();
                accHit += b - a;
                accMiss += c - b;
                accCon += d - c;
            }
            hitNs.push_back(accHit / 200);
            missNs.push_back(accMiss / 200);
            containsNs.push_back(accCon / 200);

            const uint64_t s0 = mono();
            s->setValue(hitKey, s->value(hitKey).toString() + QString());
            const uint64_t s1 = mono();
            s->setValue(QStringLiteral("bench-new-single"), QStringLiteral("x"));
            const uint64_t s2 = mono();
            s->sync();
            const uint64_t s3 = mono();
            setOldNs.push_back(s1 - s0);
            setNewNs.push_back(s2 - s1);
            syncOneNs.push_back(s3 - s2);
            const uint64_t b0 = mono();
            for (int n = 0; n < 50; ++n)
                s->setValue(QStringLiteral("bench-batch-%1").arg(n), n);
            const uint64_t b1 = mono();
            s->sync();
            const uint64_t b2 = mono();
            batchSetNs.push_back((b1 - b0) / 50);
            batchSyncNs.push_back(b2 - b1);
            const uint64_t c0 = mono();
            s->childKeys();
            const uint64_t c1 = mono();
            s->childGroups();
            const uint64_t c2 = mono();
            s->allKeys();
            const uint64_t c3 = mono();
            s->sync();
            const uint64_t c4 = mono();
            childKeysNs.push_back(c1 - c0);
            childGroupsNs.push_back(c2 - c1);
            allKeysNs.push_back(c3 - c2);
            syncNoopNs.push_back(c4 - c3);
            delete s;
        }
        summarize(targets[t].name, "construct", openNs, raw, summary);
        summarize(targets[t].name, "first_value", firstNs, raw, summary);
        summarize(targets[t].name, "hot_get_hit", hitNs, raw, summary);
        summarize(targets[t].name, "hot_get_miss", missNs, raw, summary);
        summarize(targets[t].name, "hot_contains_hit", containsNs, raw, summary);
        summarize(targets[t].name, "set_existing_uncommitted", setOldNs, raw, summary);
        summarize(targets[t].name, "set_new_uncommitted", setNewNs, raw, summary);
        summarize(targets[t].name, "sync_after_two_sets", syncOneNs, raw, summary);
        summarize(targets[t].name, "set_batch50_each", batchSetNs, raw, summary);
        summarize(targets[t].name, "sync_batch50", batchSyncNs, raw, summary);
        summarize(targets[t].name, "childKeys", childKeysNs, raw, summary);
        summarize(targets[t].name, "childGroups", childGroupsNs, raw, summary);
        summarize(targets[t].name, "allKeys", allKeysNs, raw, summary);
        summarize(targets[t].name, "sync_noop", syncNoopNs, raw, summary);

        std::vector<uint64_t> rmKey, rmGroup, clearNs;
        for (int i = 0; i < trials; ++i) {
            QSettings *s = openBackend(targets[t].path, targets[t].qsdb);
            const QString victim = keys.at(i % keys.size());
            uint64_t a = mono();
            s->remove(victim);
            uint64_t b = mono();
            s->sync();
            uint64_t c = mono();
            rmKey.push_back(c - a);
            Q_UNUSED(b);
            const QStringList g = s->childGroups();
            if (!g.isEmpty()) {
                a = mono();
                s->remove(g.at(0));
                b = mono();
                s->sync();
                c = mono();
                rmGroup.push_back(c - a);
            }
            a = mono();
            s->clear();
            b = mono();
            s->sync();
            c = mono();
            clearNs.push_back(c - a);
            delete s;
            QFile::remove(targets[t].path);
            QFile::copy(pristine, targets[t].path);
        }
        summarize(targets[t].name, "remove_one_key_and_sync", rmKey, raw, summary);
        if (!rmGroup.empty())
            summarize(targets[t].name, "remove_one_group_and_sync", rmGroup, raw, summary);
        summarize(targets[t].name, "clear_and_sync", clearNs, raw, summary);
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
        fprintf(stderr, "usage: qt_driver selftest|import|bench ...\n");
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
    if (cmd == QLatin1String("external-writer")) {
        qputenv("QSDB_BACKEND", "1");
        QSettings s(args.at(2), QSettings::IniFormat);
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
