#!/usr/bin/env bash
# Build and run the full test suite. Each test runs in its own scratch directory
# so that leftover .db/.log files from one test cannot influence another.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"

# The MinGW runtime DLLs (libstdc++-6, libgcc_s_seh-1, libwinpthread-1) must be on
# PATH or every executable exits 127 before main() runs.
MINGW="/c/Users/jerie/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
[ -d "$MINGW" ] && export PATH="$MINGW:$PATH"

TESTS="test_pager test_page test_heap test_mvcc test_vacuum test_index test_hot
       test_buffer_pool test_wal test_toast test_repl test_buffer_integration
       test_clog test_disk_btree test_checkpoint test_undo test_toast_integration
       test_recovery test_concurrency test_fsm test_executor test_joins test_locks"

if [ "${1:-}" != "--no-build" ]; then
    ninja -C "$BUILD" || exit 1
fi

pass=0; fail=0; failed=""
for t in $TESTS; do
    [ -f "$BUILD/$t.exe" ] || continue
    scratch="$BUILD/_scratch_$t"
    rm -rf "$scratch"; mkdir -p "$scratch"
    out="$(cd "$scratch" && "$BUILD/$t.exe" 2>&1)"
    if [ $? -eq 0 ]; then
        pass=$((pass+1)); printf '  PASS  %s\n' "$t"
    else
        fail=$((fail+1)); failed="$failed $t"
        printf '  FAIL  %s\n' "$t"
        printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
    fi
    rm -rf "$scratch"
done

echo
echo "$pass passed, $fail failed"
[ -n "$failed" ] && echo "failed:$failed"
[ "$fail" -eq 0 ]
