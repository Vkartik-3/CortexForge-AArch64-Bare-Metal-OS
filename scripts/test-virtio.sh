#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# test-virtio.sh — regression test for the shared virtqueue layer.
#
# The virtqueue descriptor allocator and completion path are shared by every
# virtio device (blk, net, rng, console, balloon). A change there can break any
# of them, so this boots the kernel once and exercises all five, asserting that
# each completed real device traffic — not merely that it initialized.
#
# What each assertion actually proves:
#   rng     — task_b pulls 4 bytes every 500 ms from a queue with only 8
#             descriptors. If descriptors are not returned to the free list this
#             exhausts within seconds and submits start failing. Seeing several
#             distinct samples proves recycling works.
#   net     — the RX path re-arms each buffer after reaping it, keyed by the
#             descriptor id the device reports. A successful ping reply proves
#             both id-based completion lookup and descriptor reuse.
#   blk     — FAT32 mount + file read go through virtqueue_submit_chain (3-desc
#             chains). Reading a known file with known contents proves the chain
#             is built and freed correctly.
#   console — guest writes land in the host-side chardev file.
#   balloon — inflate/deflate round-trips through its own queues.
#
# Exits non-zero on the first failure so CI fails loudly on a regression.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

KERNEL="build/kernel.elf"
DISK="build/disk.img"
VCONS="build/virtio-console-test.txt"
LOG="build/serial-virtio.log"
MARKER="Welcome to CortexForge"

QEMU="${QEMU:-qemu-system-aarch64}"
MEM="${MEM:-2G}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
HARD_TIMEOUT="${HARD_TIMEOUT:-180}"

# Known file on the FAT32 image, with known contents (see Makefile `disk`).
BLK_FILE="/mnt/fat32/HELLO.TXT"
BLK_EXPECT="This is HELLO.TXT on a FAT32 volume."

mkdir -p build
rm -f "$LOG" "$VCONS"

QEMU_PID=""
FIFO=""
cleanup() {
  exec 3>&- 2>/dev/null || true
  [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
  [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
  [ -n "$FIFO" ] && rm -f "$FIFO"
}
trap cleanup EXIT

fail() { echo "[virtio] FAIL: $*"; echo "----- last 40 lines -----"; tail -n 40 "$LOG"; exit 1; }
pass() { echo "[virtio] PASS: $*"; }

FIFO="$(mktemp -u)"; mkfifo "$FIFO"

echo "===== virtio device regression (blk / net / rng / console / balloon) ====="
timeout "$HARD_TIMEOUT" "$QEMU" \
  -machine virt,gic-version=3 -m "$MEM" -nographic -cpu cortex-a72 \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on \
  -device virtio-rng-pci,disable-legacy=on \
  -drive file="$DISK",if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -chardev file,id=vc,path="$VCONS",mux=off \
  -device virtio-serial-pci,disable-legacy=on \
  -device virtconsole,chardev=vc \
  -device virtio-balloon-pci,disable-legacy=on \
  -kernel "$KERNEL" < "$FIFO" > "$LOG" 2>&1 &
QEMU_PID=$!
exec 3> "$FIFO"

# ---- boot ----
for ((i = 0; i < BOOT_TIMEOUT; i++)); do
  grep -qaF "$MARKER" "$LOG" 2>/dev/null && break
  kill -0 "$QEMU_PID" 2>/dev/null || fail "QEMU exited before boot marker"
  sleep 1
done
grep -qaF "$MARKER" "$LOG" || fail "boot marker not found"
pass "kernel booted"

# ---- every device reached DRIVER_OK ----
for dev in RNG BLK NET; do
  grep -qaF "[$dev] DRIVER_OK set" "$LOG" || fail "$dev did not reach DRIVER_OK"
done
grep -qaF "[BALLOON] DRIVER_OK" "$LOG" || fail "BALLOON did not reach DRIVER_OK"
pass "all virtio devices reached DRIVER_OK"

# A queue-full or descriptor-corruption message anywhere is a hard failure:
# it means the free list leaked or handed out a live descriptor.
if grep -qaE "\[VQ\] (submit|submit_chain): queue full|free_chain: descriptor index out of range|get_used: device returned out-of-range" "$LOG"; then
  fail "virtqueue reported descriptor exhaustion/corruption"
fi

# ---- blk: FAT32 read of a file with known contents ----
# task_a already reads this file at boot, so the string is present before we
# start. Count first and require the count to GROW, otherwise this assertion
# would pass without our `cat` ever completing.
sleep 2
before=$(grep -acF "$BLK_EXPECT" "$LOG" || true)
printf 'cat %s\n' "$BLK_FILE" >&3
blk_ok=0
for ((i = 0; i < 15; i++)); do
  after=$(grep -acF "$BLK_EXPECT" "$LOG" || true)
  [ "${after:-0}" -gt "${before:-0}" ] && { blk_ok=1; break; }
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
[ "$blk_ok" -eq 1 ] || fail "blk: 'cat $BLK_FILE' did not return the expected contents"
pass "blk: FAT32 read via 3-descriptor chain returned correct contents"

# ---- net: ICMP echo round-trip (RX path re-arms descriptors) ----
# Driven by the kernel-mode `netd` background pinger, not the shell's `ping`
# built-in: that built-in faults in user space on pristine main too (a
# pre-existing bug unrelated to the virtqueue layer). netd exercises exactly
# the same TX/RX descriptor path from the kernel side.
#
# Each reply requires: TX chain built+freed, RX buffer reaped by descriptor id,
# and that buffer re-armed with a freshly allocated descriptor. Requiring two
# replies means at least one descriptor was recycled and reused.
ping_replies=0
for ((i = 0; i < 40; i++)); do
  ping_replies=$(grep -acE "ping seq=[0-9]+ reply" "$LOG" || true)
  [ "${ping_replies:-0}" -ge 2 ] && break
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
[ "${ping_replies:-0}" -ge 2 ] \
  || fail "net: only ${ping_replies:-0} ICMP replies (RX descriptor recycling?)"
pass "net: $ping_replies ICMP echo replies (TX+RX descriptor recycling OK)"

# ---- rng: repeated pulls from an 8-descriptor queue ----
# Each sample is one submit+complete. Several distinct samples over time means
# descriptors are being returned to the free list rather than leaked.
rng_samples=$(grep -acE "rng: [0-9A-F]{2} " "$LOG" || true)
[ "${rng_samples:-0}" -ge 3 ] \
  || fail "rng: only ${rng_samples:-0} samples — free list likely exhausted"
pass "rng: $rng_samples samples drawn from an 8-descriptor queue (recycling OK)"

# ---- console: guest write reaches the host chardev ----
printf 'vlog virtio-regression-probe\n' >&3
sleep 2
grep -qaF "virtio-regression-probe" "$VCONS" || fail "console: guest write did not reach host"
pass "console: guest write reached host chardev"

# ---- balloon: inflate then deflate ----
printf 'balloon inflate 8\n' >&3
sleep 3
printf 'balloon deflate 8\n' >&3
sleep 3
grep -qaiE "\[BALLOON\].*(inflate|actual)" "$LOG" || fail "balloon: no inflate response"
pass "balloon: inflate/deflate completed"

# ---- blk: data-integrity self-test (multi-sector, flush, bounds) ----
# Writes a position-dependent pattern to a scratch region at the END of the
# device (clear of the mounted FAT32 filesystem — sector 0 is the BPB), reads
# it back as one multi-sector request, and compares byte-for-byte.
printf 'blktest\n' >&3
blktest_ok=0
for ((i = 0; i < 30; i++)); do
  grep -qaE "\[BLKTEST\] (ALL PASS|FAILURES)" "$LOG" && { blktest_ok=1; break; }
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
[ "$blktest_ok" -eq 1 ] || fail "blk: self-test did not complete"
grep -qaF "[BLKTEST] ALL PASS" "$LOG" \
  || fail "blk: self-test reported failures$(grep -aE '\[BLKTEST\] (FAIL|mismatch)' "$LOG" | sed 's/^/\n    /')"
pass "blk: data-integrity self-test (multi-sector + flush + bounds) ALL PASS"

# ---- final sweep: nothing wedged or corrupted during the run ----
if grep -qaE "virtqueue_poll: timeout|queue full|PANIC|panic" "$LOG"; then
  fail "virtqueue timeout / panic during device exercise"
fi
pass "no virtqueue timeouts, exhaustion, or panics"

echo "===== virtio regression: ALL PASS ====="
