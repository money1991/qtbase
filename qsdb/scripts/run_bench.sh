#!/bin/sh
# Run from anywhere. Does not print setting keys.
# Usage: run_bench.sh /path/to/qt/bin/qmake /path/to/input.ini /path/to/output_dir
set -eu
QMAKE=$1
INI=$2
OUT=$3
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WORKDIR=$(mktemp -d)
cp "$INI" "$WORKDIR/input.ini"
mkdir -p "$OUT"
g++ -std=c++11 -O2 -I"$ROOT/include" "$ROOT/src/qsdb.cpp" "$ROOT/tests/test_qsdb.cpp" -o "$WORKDIR/test_qsdb"
"$WORKDIR/test_qsdb" "$WORKDIR" million > "$OUT/standalone.log" 2>&1
cd "$ROOT/tests"
"$QMAKE" qt_driver.pro -o "$WORKDIR/Makefile.driver"
make -C "$WORKDIR" -f Makefile.driver -j2
"$WORKDIR/qt_driver" selftest
"$WORKDIR/qt_driver" import "$WORKDIR/input.ini" "$WORKDIR/input.qsdb" | tee "$OUT/import.txt"
"$WORKDIR/qt_driver" bench "$WORKDIR/input.ini" "$WORKDIR/input.qsdb" "$OUT"
echo "artifacts $OUT"
