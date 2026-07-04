#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run-bench.sh — headless PMU benchmark run. Drives the shell `bench` built-in
# over a FIFO (so you never need to edit the kernel to auto-run it) and prints
# the [BENCH] result lines.
#
# Env vars:
#   MEM    guest RAM (default 8G). Use MEM=512M on small hosts (e.g. t4g.micro,
#          1 GiB) where QEMU can't reserve 8 GiB.
#   ACCEL  tcg | kvm (default tcg). `kvm` runs the guest on the real aarch64
#          host cores (-cpu host), so PMCCNTR reads TRUE silicon cycles — the
#          only way to get real-hardware numbers (needs /dev/kvm, aarch64 host).
#   ICOUNT set to 1 to add `-icount shift=0` for deterministic (but emulated,
#          TCG-only) cycle counts. Ignored under kvm.
#   CROSS_COMPILE  passed to make (on a native aarch64 host use CROSS_COMPILE=).
#
# Examples:
#   ./scripts/run-bench.sh                       # emulated, real-time
#   MEM=512M ./scripts/run-bench.sh              # small host
#   ACCEL=kvm MEM=512M CROSS_COMPILE= ./scripts/run-bench.sh   # real silicon on Graviton
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MEM=${MEM:-8G}
ACCEL=${ACCEL:-tcg}
ICOUNT=${ICOUNT:-0}
KERNEL=build/kernel.elf
DISK=build/disk.img
LOG=build/bench-run.log
MARKER="Welcome to CortexForge"

command -v qemu-system-aarch64 >/dev/null 2>&1 || { echo "[run-bench] qemu-system-aarch64 not found"; exit 1; }

echo "[run-bench] building kernel + disk"
if ! make >/dev/null 2>&1 || ! make disk >/dev/null 2>&1; then
  echo "[run-bench] build failed — run 'make && make disk' first"
  echo "            (on a native aarch64 host: make CROSS_COMPILE= && make disk CROSS_COMPILE=)"
  exit 1
fi

if [ "$ACCEL" = kvm ]; then
  ACCEL_ARGS="-accel kvm -cpu host"
  IC_ARGS=""
  echo "[run-bench] KVM: guest runs on real aarch64 cores (-cpu host) — true-silicon PMU"
else
  ACCEL_ARGS="-accel tcg -cpu cortex-a72"
  IC_ARGS=""
  [ "$ICOUNT" = 1 ] && IC_ARGS="-icount shift=0"
fi

rm -f "$LOG"
FIFO="$(mktemp -u)"; mkfifo "$FIFO"
QPID=""
cleanup() { exec 3>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; rm -f "$FIFO"; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout 180 qemu-system-aarch64 -machine virt,gic-version=3 -m "$MEM" -nographic $ACCEL_ARGS $IC_ARGS \
  -device virtio-rng-pci,disable-legacy=on \
  -drive file="$DISK",if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel "$KERNEL" < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3> "$FIFO"

echo "[run-bench] booting (MEM=$MEM ACCEL=$ACCEL ICOUNT=$ICOUNT)"
booted=0
for _ in $(seq 1 90); do
  grep -qF "$MARKER" "$LOG" 2>/dev/null && { booted=1; break; }
  kill -0 "$QPID" 2>/dev/null || break
  sleep 1
done
if [ "$booted" -ne 1 ]; then
  echo "[run-bench] FAIL: guest did not boot"; tail -n 20 "$LOG"; exit 1
fi

sleep 2
printf 'bench\n' >&3
for _ in $(seq 1 120); do
  grep -qF "[BENCH] done" "$LOG" 2>/dev/null && break
  kill -0 "$QPID" 2>/dev/null || break
  sleep 1
done

echo "===== benchmark results (MEM=$MEM ACCEL=$ACCEL ICOUNT=$ICOUNT) ====="
grep -E "\[BENCH\].*cycles$" "$LOG" || { echo "[run-bench] no results captured"; tail -n 20 "$LOG"; exit 1; }
if grep -qF "[BENCH] done" "$LOG"; then
  echo "[run-bench] OK"
else
  echo "[run-bench] FAIL: benchmark did not complete"; exit 1
fi
