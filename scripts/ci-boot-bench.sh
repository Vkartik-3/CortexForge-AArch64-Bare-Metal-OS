#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# ci-boot-bench.sh — headless QEMU boot assertion + PMU benchmark capture.
#
# Boots build/kernel.elf under QEMU, waits (by polling the serial log) for the
# kernel's boot-success marker, then drives the EL0 shell's `bench` built-in and
# captures the [BENCH] result lines into bench/results.txt as a CI artifact.
#
# Exits non-zero if the boot marker never appears or the benchmark never
# completes, so CI fails loudly on a boot regression.
#
# Notes on the QEMU invocation:
#   * -m 8G          : the kernel hard-codes an 8 GiB RAM map (MEM_SIZE in
#                      pmm), so a smaller -m faults the PMM during boot.
#   * virtio-rng+blk : the kernel probes these during init; the FAT32 disk is
#                      required for the VFS mount the shell reads from.
#   * -icount shift=0: instruction-accurate, DETERMINISTIC cycle counting so the
#                      PMU cycle numbers are reproducible run-to-run.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

KERNEL="build/kernel.elf"
DISK="build/disk.img"
LOG="build/serial.log"
MARKER="Welcome to CortexForge"

QEMU="${QEMU:-qemu-system-aarch64}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"   # seconds to wait for the boot marker
BENCH_TIMEOUT="${BENCH_TIMEOUT:-90}"  # seconds to wait for the benchmark to finish
HARD_TIMEOUT="${HARD_TIMEOUT:-240}"   # absolute cap on the QEMU process

mkdir -p bench build
rm -f "$LOG"
FIFO="$(mktemp -u)"; mkfifo "$FIFO"

# Launch QEMU with its serial on stdio: stdin from the FIFO, stdout to the log.
# romfile= disables PCI option-ROM (PXE) loading on each virtio device. Ubuntu's
# qemu-system-arm package ships without efi-virtio.rom, so without this QEMU
# aborts at startup with 'failed to find romfile "efi-virtio.rom"'. We never PXE
# boot, so suppressing the ROM entirely is the portable fix.
timeout "$HARD_TIMEOUT" "$QEMU" \
  -machine virt,gic-version=3 -m 8G -nographic -cpu cortex-a72 -icount shift=0 \
  -device virtio-rng-pci,disable-legacy=on,romfile= \
  -drive file="$DISK",if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on,romfile= \
  -kernel "$KERNEL" < "$FIFO" > "$LOG" 2>&1 &
QEMU_PID=$!

# Hold the FIFO open for writing so QEMU's serial input stays connected.
exec 3> "$FIFO"

cleanup() {
  exec 3>&- 2>/dev/null || true
  kill "$QEMU_PID" 2>/dev/null || true
  wait "$QEMU_PID" 2>/dev/null || true
  rm -f "$FIFO"
}
trap cleanup EXIT

echo "[ci] waiting up to ${BOOT_TIMEOUT}s for boot marker: '$MARKER'"
booted=0
for _ in $(seq 1 "$BOOT_TIMEOUT"); do
  if grep -qF "$MARKER" "$LOG" 2>/dev/null; then booted=1; break; fi
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
  sleep 1
done

if [ "$booted" -ne 1 ]; then
  echo "[ci] FAIL: boot marker not found within ${BOOT_TIMEOUT}s"
  echo "----- last 40 lines of serial log -----"
  tail -n 40 "$LOG" || true
  exit 1
fi
echo "[ci] PASS: kernel booted (marker seen)"

# Small settle delay so the shell is definitely at its prompt, then run bench.
sleep 2
echo "[ci] sending 'bench' to the shell"
printf 'bench\n' >&3

echo "[ci] waiting up to ${BENCH_TIMEOUT}s for benchmark completion"
done_ok=0
for _ in $(seq 1 "$BENCH_TIMEOUT"); do
  if grep -qF "[BENCH] done" "$LOG" 2>/dev/null; then done_ok=1; break; fi
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
  sleep 1
done

# Extract the benchmark lines into the artifact regardless, for inspection.
grep -F "[BENCH]" "$LOG" > bench/results.txt || true

if [ "$done_ok" -ne 1 ]; then
  echo "[ci] FAIL: benchmark did not complete within ${BENCH_TIMEOUT}s"
  echo "----- captured [BENCH] lines so far -----"
  cat bench/results.txt || true
  exit 1
fi

echo "[ci] PASS: benchmark completed"
echo "===================== bench/results.txt ====================="
cat bench/results.txt
echo "============================================================="
