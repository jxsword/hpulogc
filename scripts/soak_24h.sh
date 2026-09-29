#!/usr/bin/env bash
# ============================================================================
# soak_24h.sh - 24h high-concurrency soak harness (spec rd_v0.6 §13.3,
# decision D-R6: local WSL background run, 1h shakedown first).
#
# Usage:
#   scripts/soak_24h.sh --duration 86400 [--builds default,tsan] [--dir DIR]
#                       [--producers N] [--rate-us USEC]
#
# For each build variant ("default" = RelWithDebInfo MPMC, "tsan" =
# ThreadSanitizer MPMC) the script:
#   1. configures build/<dir>/<variant> with -DHPULOGC_CONCURRENCY=MPMC,
#   2. builds the soak_driver target,
#   3. generates an INI config exercising rollingfile rotation + null
#      (per-sink async queue) + console (stderr redirected to a file),
#   4. launches the driver under nohup, detached from this session.
#
# Progress: each driver writes <dir>/<variant>.ini.samples.log (SAMPLE /
# RELOAD / STALL / VIOLATION / FINAL / VERDICT lines). The script
# truncates the console capture file hourly to bound disk usage.
#
# Verdict: driver exit code 0 (no stalls, accounting identity closed);
# check with `tail <dir>/<variant>.ini.samples.log` after completion.
# Exit codes of this script: 0 launched/verified, 1 on setup failure.
# ============================================================================
set -euo pipefail

DURATION=86400
BUILDS="default,tsan"
DIR=build/soak
PRODUCERS=4
RATE_US=5000
ROOT=$(cd "$(dirname "$0")/.." && pwd)

while [ $# -gt 0 ]; do
    case "$1" in
        --duration) DURATION=$2; shift 2 ;;
        --builds) BUILDS=$2; shift 2 ;;
        --dir) DIR=$2; shift 2 ;;
        --producers) PRODUCERS=$2; shift 2 ;;
        --rate-us) RATE_US=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
done

OUT="$ROOT/$DIR"
mkdir -p "$OUT"

generate_config() {
    local variant=$1 path=$2

    # NOTE: "level = INFO " keeps a trailing space - the driver flips the
    # level with a 13-byte in-place swap (INFO is 12 chars, DEBUG 13).
    cat > "$path" <<EOF
[global]
level = INFO 
default format = standard
default outputs = rf, nul, con

[outputs]
rf = rollingfile, path=$OUT/$variant/soak.log, rotate=size, max size=64kb, max files=16
nul = null, async=on, queue size = 256kb
con = console, stream=stderr

[async]
batch size = 256
consumer threads = 4
EOF
}

IFS=',' read -r -a VARIANTS <<< "$BUILDS"
PIDS=()
for variant in "${VARIANTS[@]}"; do
    bdir="$OUT/$variant"
    mkdir -p "$bdir"

    cmake_args=(-S "$ROOT" -B "$bdir" -G Ninja
        -DHPULOGC_CONCURRENCY=MPMC
        -DHPULOGC_BUILD_TESTS=OFF
        -DHPULOGC_BUILD_EXAMPLES=OFF)
    if [ "$variant" = "tsan" ]; then
        cmake_args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DHPULOGC_SANITIZER=thread)
    else
        cmake_args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
    fi
    echo "[soak] configuring $variant ..."
    cmake "${cmake_args[@]}" > "$bdir.configure.log" 2>&1
    echo "[soak] building $variant soak_driver ..."
    cmake --build "$bdir" --target soak_driver >> "$bdir.configure.log" 2>&1

    cfg="$OUT/$variant.ini"
    generate_config "$variant" "$cfg"

    # Best-effort validation before burning hours on a bad config
    # (hpulogc_chk_conf CLI). Build-dependent semantic checks (e.g.
    # consumer threads > 1 requires MPMC) may legitimately fail when the
    # available CLI binary was built SPSC/MPSC - warn only; the driver's
    # own init still fail-fasts.
    if [ -x "$ROOT/build/check/tools/hpulogc_chk_conf" ]; then
        "$ROOT/build/check/tools/hpulogc_chk_conf" "$cfg" > /dev/null ||
            echo "[soak] WARNING: chk_conf flagged $cfg" \
                 "(may be a build-dependent check; driver init fail-fasts)"
    fi

    console_cap="$bdir/console_capture.log"
    : > "$console_cap"
    echo "[soak] launching $variant driver for ${DURATION}s ..."
    nohup "$bdir/tools/soak_driver" --config "$cfg" --duration "$DURATION" \
        --producers "$PRODUCERS" --rate-us "$RATE_US" \
        > "$bdir/driver.stdout.log" 2> "$console_cap" &
    pid=$!
    PIDS+=("$pid")
    echo "$pid" > "$bdir/driver.pid"
    echo "[soak] $variant: pid=$pid config=$cfg samples=$cfg.samples.log"
done

# Hourly console-capture truncation keeps disk usage bounded (the file
# stays open and becomes sparse); runs detached alongside the drivers.
truncate_loop() {
    while :; do
        sleep 3600
        for variant in "${VARIANTS[@]}"; do
            : > "$OUT/$variant/console_capture.log" 2>/dev/null || true
        done
    done
}
nohup bash -c "$(declare -f truncate_loop); truncate_loop" \
    > "$OUT/truncate_loop.log" 2>&1 &
echo $! > "$OUT/truncate_loop.pid"

echo "[soak] launched ${#PIDS[@]} driver(s); monitor with:"
for variant in "${VARIANTS[@]}"; do
    echo "  tail -f $OUT/$variant.ini.samples.log"
done
echo "[soak] kill all: kill \$(cat $OUT/*/driver.pid) \$(cat $OUT/truncate_loop.pid)"
