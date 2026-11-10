#!/bin/sh
# Run from anywhere. Does not print setting keys or values.
# Usage: run_bench.sh /path/to/qt/bin/qmake /path/to/input.ini /path/to/output_dir [libdir]
# libdir, when set, is prepended to LD_LIBRARY_PATH so the same driver can be
# timed against another build of QtCore. fresh_* rows are new processes with
# a warm OS page cache, not a dropped-cache disk read.
set -eu
QMAKE=$1
INI=$2
OUT=$3
LIBDIR=${4:-}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WORKDIR=$(mktemp -d)
cp "$INI" "$WORKDIR/input.ini"
mkdir -p "$OUT"
if [ -n "$LIBDIR" ]; then
    export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
{
    echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    uname -a
    echo "compiler=$(g++ --version | head -n 1)"
    echo "qmake=$QMAKE"
    echo "libdir=${LIBDIR:-<default>}"
    echo "ld_library_path=${LD_LIBRARY_PATH:-}"
    echo "input_bytes=$(wc -c < "$INI" | tr -d ' ')"
    echo "clock=CLOCK_MONOTONIC"
    echo "fresh_process=new process, OS cache warm, not cold disk"
    echo "median=sorted[n/2]"
    echo "p95=sorted[(n-1)*95/100]"
} > "$OUT/environment.txt"
g++ -std=c++11 -O2 -pthread -I"$ROOT/include" "$ROOT/src/qsdb.cpp" "$ROOT/tests/test_qsdb.cpp" -o "$WORKDIR/test_qsdb"
"$WORKDIR/test_qsdb" "$WORKDIR" million > "$OUT/standalone.log" 2>&1
cd "$ROOT/tests"
"$QMAKE" qt_driver.pro -o "$WORKDIR/Makefile.driver"
make -C "$WORKDIR" -f Makefile.driver -j2
"$WORKDIR/qt_driver" selftest
"$WORKDIR/qt_driver" import "$WORKDIR/input.ini" "$WORKDIR/input.qsdb" | tee "$OUT/import.txt"
"$WORKDIR/qt_driver" bench "$WORKDIR/input.ini" "$WORKDIR/input.qsdb" "$OUT"
echo "artifacts $OUT"
