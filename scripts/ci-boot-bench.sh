#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# ci-boot-bench.sh — headless QEMU CI driver.
#
# Phase 1 (bench): boot build/kernel.elf under -icount, assert the boot marker
#   on the console, run the shell `bench` built-in, capture bench/results.txt.
# Phase 2 (uart):  boot again in real time with UART1 exported as a Unix socket,
#   run `uartecho` in the guest, drive scripts/uart-client.py against the
#   socket, and capture bench/uart-results.txt.
#
# Exits non-zero if either phase fails, so CI fails loudly on a regression.
#
# QEMU notes:
#   * -m 8G          : the kernel hard-codes an 8 GiB RAM map (pmm MEM_SIZE).
#   * TWO -serial     : UART0 (serial0) = console; UART1 (serial1) = framing.
#                       The kernel probes UART1 @0x09040000 at boot, so a second
#                       serial MUST always be present or that MMIO faults.
#   * -icount shift=0 : deterministic cycle counts (phase 1 only). Phase 2 runs
#                       real-time so socket exchanges aren't warped by icount.
#   * virtio-rng+blk : probed at init; the FAT32 disk backs the VFS mount.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

KERNEL="build/kernel.elf"
DISK="build/disk.img"
MARKER="Welcome to CortexForge"
SOCK="${SOCK:-/tmp/cortexforge-uart.sock}"

QEMU="${QEMU:-qemu-system-aarch64}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
BENCH_TIMEOUT="${BENCH_TIMEOUT:-120}"
HARD_TIMEOUT="${HARD_TIMEOUT:-300}"

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

# wait_marker <logfile> <timeout>  — poll the log until MARKER appears.
wait_marker() {
  local log="$1" to="$2" i
  for ((i = 0; i < to; i++)); do
    grep -qF "$MARKER" "$log" 2>/dev/null && return 0
    kill -0 "$QEMU_PID" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

# ============================ Phase 1: bench ================================
echo "===== Phase 1: boot + PMU benchmark (-icount) ====="
LOG="build/serial.log"
rm -f "$LOG"
FIFO="$(mktemp -u)"; mkfifo "$FIFO"

timeout "$HARD_TIMEOUT" "$QEMU" \
  -machine virt,gic-version=3 -m 8G -display none -cpu cortex-a72 -icount shift=0 \
  -serial mon:stdio -serial null \
  -device virtio-rng-pci,disable-legacy=on \
  -drive file="$DISK",if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel "$KERNEL" < "$FIFO" > "$LOG" 2>&1 &
QEMU_PID=$!
exec 3> "$FIFO"

echo "[ci] waiting up to ${BOOT_TIMEOUT}s for boot marker"
if ! wait_marker "$LOG" "$BOOT_TIMEOUT"; then
  echo "[ci] FAIL: boot marker not found"; tail -n 40 "$LOG"; exit 1
fi
echo "[ci] PASS: kernel booted"

sleep 2
echo "[ci] running 'bench'"
printf 'bench\n' >&3

done_ok=0
for ((i = 0; i < BENCH_TIMEOUT; i++)); do
  grep -qF "[BENCH] done" "$LOG" 2>/dev/null && { done_ok=1; break; }
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
grep -F "[BENCH]" "$LOG" > bench/results.txt || true
if [ "$done_ok" -ne 1 ]; then
  echo "[ci] FAIL: benchmark did not complete"; cat bench/results.txt; exit 1
fi
echo "[ci] PASS: benchmark completed"
cat bench/results.txt
cleanup; QEMU_PID=""; FIFO=""

# ============================ Phase 2: UART =================================
echo "===== Phase 2: boot + UART framing protocol test (real-time) ====="
LOG2="build/serial-uart.log"
rm -f "$LOG2" "$SOCK"
FIFO="$(mktemp -u)"; mkfifo "$FIFO"

timeout "$HARD_TIMEOUT" "$QEMU" \
  -machine virt,gic-version=3 -m 8G -display none -cpu cortex-a72 \
  -serial mon:stdio -serial "unix:${SOCK},server,nowait" \
  -device virtio-rng-pci,disable-legacy=on \
  -drive file="$DISK",if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel "$KERNEL" < "$FIFO" > "$LOG2" 2>&1 &
QEMU_PID=$!
exec 3> "$FIFO"

echo "[ci] waiting up to ${BOOT_TIMEOUT}s for boot marker"
if ! wait_marker "$LOG2" "$BOOT_TIMEOUT"; then
  echo "[ci] FAIL: boot marker not found (uart phase)"; tail -n 40 "$LOG2"; exit 1
fi
sleep 2
echo "[ci] starting uartecho in guest"
printf './uartecho\n' >&3

echo_ok=0
for ((i = 0; i < 30; i++)); do
  grep -qF "uartecho: echoing" "$LOG2" 2>/dev/null && { echo_ok=1; break; }
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
if [ "$echo_ok" -ne 1 ]; then
  echo "[ci] FAIL: uartecho did not start"; tail -n 20 "$LOG2"; exit 1
fi

echo "[ci] running uart-client.py against ${SOCK}"
python3 scripts/uart-client.py --sock "$SOCK" --count 10 | tee bench/uart-results.txt
client_rc=${PIPESTATUS[0]}

# Append the guest-side echo log for the artifact.
{ echo ""; echo "===== guest console ([UART] lines) ====="; \
  grep -E "\[UART\]|uartecho:" "$LOG2" || true; } >> bench/uart-results.txt

if [ "$client_rc" -ne 0 ]; then
  echo "[ci] FAIL: uart-client reported failure (rc=$client_rc)"; exit 1
fi
echo "[ci] PASS: UART framing protocol test"
echo "===================== bench/uart-results.txt ====================="
cat bench/uart-results.txt
echo "================================================================="
