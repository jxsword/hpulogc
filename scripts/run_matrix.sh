#!/usr/bin/env bash
# ============================================================================
# hpulogc Phase 1 verification matrix (spec 13.5 / task 11)
#
# Runs {GCC, Clang} x {C99, C11} x {locked, lockfree} x {SPSC, MPSC}
# (16 combinations) plus the four build presets, building and running the
# full CTest suite for each.
#
# Usage: scripts/run_matrix.sh [quick]
#   quick  = skip presets (matrix only)
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/build/matrix"
mkdir -p "${OUT}"

CC_BIN="${CC:-cc}"
CXX_SKIP=1

pass_count=0
fail_count=0
failed_names=""

run_combo() {
    local name="$1"; shift
    local dir="${OUT}/${name}"
    echo "==> [${name}] $*"

    if ! cmake -S "${ROOT}" -B "${dir}" -G Ninja "$@" \
            -DHPULOGC_BUILD_TESTS=ON > "${dir}.cfg.log" 2>&1; then
        echo "    CONFIGURE FAILED (see ${dir}.cfg.log)"
        fail_count=$((fail_count + 1))
        failed_names="${failed_names} ${name}(cfg)"
        return
    fi
    if ! ninja -C "${dir}" > "${dir}.build.log" 2>&1; then
        echo "    BUILD FAILED (see ${dir}.build.log)"
        fail_count=$((fail_count + 1))
        failed_names="${failed_names} ${name}(build)"
        return
    fi
    if ! (cd "${dir}" && ctest --output-on-failure > "${dir}.test.log" 2>&1); then
        echo "    TEST FAILED (see ${dir}.test.log)"
        fail_count=$((fail_count + 1))
        failed_names="${failed_names} ${name}(test)"
        return
    fi
    echo "    PASS"
    pass_count=$((pass_count + 1))
}

echo "==== functional matrix: {gcc,clang} x {99,11} x {locked,lockfree} x {SPSC,MPSC} ===="

for compiler in gcc clang; do
    for std in 99 11; do
        for lockfree in OFF ON; do
            for conc in SPSC MPSC; do
                name="${compiler}_c${std}_$( [ "$lockfree" = ON ] && echo lf || echo lk )_${conc}"
                run_combo "${name}" \
                    -DCMAKE_C_COMPILER="${compiler}" \
                    -DHPULOGC_C_STANDARD="${std}" \
                    -DHPULOGC_LOCKFREE="${lockfree}" \
                    -DHPULOGC_CONCURRENCY="${conc}" \
                    -DHPULOGC_BUILD_EXAMPLES=OFF
            done
        done
    done
done

if [ "${1:-}" != "quick" ]; then
    echo "==== build presets (spec 5) ===="
    for preset in full min sync_thread async_single; do
        name="preset_${preset}"
        run_combo "${name}" \
            -DHPULOGC_BUILD_PRESET="${preset}" \
            -DHPULOGC_BUILD_EXAMPLES=OFF
    done
fi

echo "===="
echo "matrix results: ${pass_count} passed, ${fail_count} failed"
if [ "${fail_count}" != 0 ]; then
    echo "failed:${failed_names}"
    exit 1
fi
exit 0
