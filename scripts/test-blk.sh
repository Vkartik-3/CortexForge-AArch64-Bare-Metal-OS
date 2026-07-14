#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# test-blk.sh — headless QEMU driver for the virtio-blk suites.
#
# Boots the kernel once with a virtio-blk-pci device attached and runs, in
# order:
#   blktest   — data integrity (multi-sector write/read-back, flush, bounds)
#   blkirq    — interrupt-mode completions over the device's INTx line
#   blkfault  — fault injection (invalid sector, queue full, timeout/retry/reset)
#   blkbench  — IOPS / throughput / latency, captured to bench/blk-results.txt
#
# Then boots a SECOND time against a readonly=on drive, because the read-only
# path (VIRTIO_BLK_F_RO detection + write rejection) cannot be exercised on a
# writable device.
#
# Exits non-zero if any data-integrity check fails or any fault scenario fails
# to recover, so CI fails loudly rather than quietly producing wrong bytes.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

KERNEL="build/kernel.elf"
DISK="build/disk.img"
RO_DISK="build/disk-ro.img"
LOG="build/serial-blk.log"
RO_LOG="build/serial-blk-ro.log"
RESULTS="bench/blk-results.txt"
MARKER="Welcome to CortexForge"

QEMU="${QEMU:-qemu-system-aarch64}"
MEM="${MEM:-2G}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
BENCH_TIMEOUT="${BENCH_TIMEOUT:-420}"
HARD_TIMEOUT="${HARD_TIMEOUT:-900}"

mkdir -p bench build

QEMU_PID=""
FIFO=""
cleanup() {
  exec 3>&- 2>/dev/null || true
  [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
  [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
  [ -n "$FIFO" ] && rm -f "$FIFO"
}
trap cleanup EXIT

fail() { echo "[blk] FAIL: $*"; echo "----- last 40 lines -----"; tail -n 40 "${2:-$LOG}"; exit 1; }
pass() { echo "[blk] PASS: $*"; }

# wait_for <log> <regex> <timeout-secs>
wait_for() {
  local log="$1" pat="$2" to="$3" i
  for ((i = 0; i < to; i++)); do
    grep -qaE "$pat" "$log" 2>/dev/null && return 0
    kill -0 "$QEMU_PID" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

boot() { # boot <log> <extra-drive-opts>
  local log="$1" driveopts="$2"
  rm -f "$log"
  FIFO="$(mktemp -u)"; mkfifo "$FIFO"

  # shellcheck disable=SC2086
  timeout "$HARD_TIMEOUT" "$QEMU" \
    -machine virt,gic-version=3 -m "$MEM" -nographic -cpu cortex-a72 \
    -device virtio-rng-pci,disable-legacy=on \
    -drive $driveopts \
    -device virtio-blk-pci,drive=d0,disable-legacy=on \
    -kernel "$KERNEL" < "$FIFO" > "$log" 2>&1 &
  QEMU_PID=$!
  exec 3> "$FIFO"

  wait_for "$log" "$MARKER" "$BOOT_TIMEOUT" || fail "boot marker not found" "$log"
  pass "kernel booted"
  sleep 2
}

# ===================== Pass 1: writable device =============================
echo "===== virtio-blk: writable device ====="
boot "$LOG" "file=$DISK,if=none,format=raw,id=d0"

# Record what the device actually negotiated — the numbers below are only
# meaningful against a known feature set.
grep -aE "^\[BLK\] (Device offers|Driver accepts|Capacity|size_max|MSI-X|INTx)" "$LOG" || true

# ---- 1. data integrity ----
printf 'blktest\n' >&3
wait_for "$LOG" "\[BLKTEST\] (ALL PASS|FAILURES)" 90 || fail "blktest did not complete"
grep -qaF "[BLKTEST] ALL PASS" "$LOG" || fail "data-integrity self-test reported failures"
pass "data integrity: multi-sector write/read-back, flush, bounds"

# ---- 2. interrupt-mode completions ----
printf 'blkirq\n' >&3
wait_for "$LOG" "\[BLKTEST\] (IRQ ALL PASS|IRQ FAILURES)" 90 || fail "blkirq did not complete"
grep -qaF "[BLKTEST] IRQ ALL PASS" "$LOG" || fail "interrupt-mode test reported failures"
pass "interrupt completions over INTx (GIC counter verified)"

# ---- 3. fault injection ----
printf 'blkfault\n' >&3
wait_for "$LOG" "\[BLKTEST\] (FAULT ALL PASS|FAULT FAILURES)" 120 || fail "blkfault did not complete"
grep -qaF "[BLKTEST] FAULT ALL PASS" "$LOG" || fail "fault injection reported failures"
# Any scenario that did not recover is a hard failure.
if grep -qaE "^\[FAULT\] [a-z_]+: FAIL" "$LOG"; then
  fail "a fault scenario failed to detect/recover"
fi
pass "fault injection: all scenarios detected and recovered"
grep -aE "^\[FAULT\]" "$LOG" | grep -v "data abort in task" || true

# ---- 4. benchmark ----
echo "[blk] running benchmark (this takes a few minutes)"
printf 'blkbench\n' >&3
wait_for "$LOG" "===== benchmark done =====" "$BENCH_TIMEOUT" \
  || fail "benchmark did not complete"
pass "benchmark completed"

# Capture the benchmark block as the CI artifact.
{
  echo "virtio-blk benchmark — CortexForge"
  echo "QEMU:    $("$QEMU" --version | head -1)"
  echo "Machine: virt,gic-version=3  CPU: cortex-a72  MEM: $MEM"
  echo "Backend: QEMU virtio-blk-pci over a host raw file (page-cached),"
  echo "         NOT physical storage. Useful for comparing driver paths"
  echo "         (polling vs IRQ, QD=1 vs QD=32), not as disk numbers."
  echo ""
  grep -aE "^\[BLK\] (=====|clock|iters|scratch|latencies|mode:|seq_|rand_|flush|polling_|irq_mode|queue_depth)" "$LOG"
} > "$RESULTS"
cat "$RESULTS"

cleanup; QEMU_PID=""; FIFO=""

# ===================== Pass 2: read-only device ============================
# VIRTIO_BLK_F_RO is only offered when the drive is attached readonly, so the
# detection + write-rejection path is unreachable in pass 1.
echo "===== virtio-blk: read-only device (VIRTIO_BLK_F_RO) ====="
cp "$DISK" "$RO_DISK"
boot "$RO_LOG" "file=$RO_DISK,if=none,format=raw,id=d0,readonly=on"

grep -qaE "^\[BLK\].*read_only=1" "$RO_LOG" \
  || fail "device did not negotiate VIRTIO_BLK_F_RO" "$RO_LOG"
pass "VIRTIO_BLK_F_RO negotiated"

printf 'blktest\n' >&3
wait_for "$RO_LOG" "\[BLKTEST\] (ALL PASS|FAILURES)" 90 \
  || fail "read-only blktest did not complete" "$RO_LOG"
grep -qaF "[BLKTEST] ALL PASS (read-only device)" "$RO_LOG" \
  || fail "read-only device test reported failures" "$RO_LOG"
pass "read-only: writes rejected before reaching the device, reads still work"

printf 'blkfault\n' >&3
wait_for "$RO_LOG" "\[BLKTEST\] (FAULT ALL PASS|FAULT FAILURES)" 120 \
  || fail "read-only blkfault did not complete" "$RO_LOG"
grep -qaF "[FAULT] ro_violation: PASS" "$RO_LOG" \
  || fail "ro_violation scenario did not pass" "$RO_LOG"
pass "fault injection: ro_violation exercised"

echo "===== virtio-blk: ALL PASS ====="
