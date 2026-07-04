#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# test-ebpf.sh — end-to-end test of the XDP monitor against a live CortexForge
# guest, using a real TAP interface (the project's default QEMU uses slirp/user
# networking, which has NO host interface for XDP; this test reconfigures QEMU
# to a TAP device).
#
# REQUIRES: Linux, root (TAP creation + XDP attach need CAP_NET_ADMIN), and the
# eBPF toolchain (clang, bpftool, libbpf-dev). Skips gracefully (exit 0) if any
# prerequisite is missing so it never blocks a non-Linux / unprivileged run.
#
# Traffic model: XDP attaches at INGRESS on the host TAP, so it sees packets the
# GUEST sends to the host (10.0.2.2) — the guest's ARP probe and its ICMP echo
# requests (shell `ping` + the netd background pinger). We drive the guest shell
# to ping repeatedly to generate ICMP, then assert the monitor counted them.
# ---------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAP=cfxtap0
HOST_IP=10.0.2.2
GUEST_IP=10.0.2.15
GUEST_MAC=52:54:00:12:34:56
XDP_LOG=/tmp/cfx-xdp.log
CON_LOG=/tmp/cfx-con.log
# Guest RAM: the kernel maps 8 GiB, but it boots and runs networking fine with
# less. Override MEM=512M on small hosts (e.g. a t3.micro with 1 GiB) where QEMU
# can't reserve 8 GiB ("Cannot allocate memory").
MEM=${MEM:-8G}
# The dominant ICMP source is the guest's netd background pinger (one echo to
# 10.0.2.2 every ~5 s), so we need a ~55 s capture window to observe >= 10. The
# driven shell pings below are supplementary. PINGS * PING_INTERVAL + FINAL_WAIT
# sets the window; defaults ~ 50 s.
PINGS=${PINGS:-40}
PING_INTERVAL=${PING_INTERVAL:-1}
FINAL_WAIT=${FINAL_WAIT:-10}

skip() { echo "[test-ebpf] SKIP: $*"; exit 0; }

# ---- prerequisite checks (graceful skip) ----
[ "$(uname -s)" = "Linux" ] || skip "not Linux (eBPF/XDP is Linux-only)"
for t in clang bpftool qemu-system-aarch64; do
  command -v "$t" >/dev/null 2>&1 || skip "missing tool: $t"
done
[ -r /sys/kernel/btf/vmlinux ] || skip "no /sys/kernel/btf/vmlinux (kernel BTF/CO-RE unavailable)"
ldconfig -p 2>/dev/null | grep -q libbpf || skip "libbpf runtime not found (install libbpf-dev)"
if [ "$(id -u)" -ne 0 ]; then skip "not root (TAP + XDP attach need CAP_NET_ADMIN)"; fi

# ---- build eBPF tools + kernel ----
echo "[test-ebpf] building eBPF tools"
make -C tools/ebpf clean >/dev/null 2>&1 || true
if ! make -C tools/ebpf; then echo "[test-ebpf] FAIL: eBPF build failed"; exit 1; fi
[ -f tools/ebpf/xdp_monitor.bpf.o ] && [ -x tools/ebpf/monitor ] || {
  echo "[test-ebpf] FAIL: build artifacts missing"; exit 1; }

echo "[test-ebpf] building kernel + disk"
make >/dev/null 2>&1 && make disk >/dev/null 2>&1 || {
  echo "[test-ebpf] FAIL: kernel build failed"; exit 1; }

# ---- TAP setup ----
ip link del "$TAP" 2>/dev/null || true
ip tuntap add dev "$TAP" mode tap 2>/dev/null || skip "cannot create TAP (no tun support?)"
ip addr add "$HOST_IP/24" dev "$TAP"
ip link set "$TAP" up
echo "[test-ebpf] TAP $TAP up as $HOST_IP/24"

FIFO=$(mktemp -u); mkfifo "$FIFO"
QEMU_PID=""; MON_PID=""
cleanup() {
  [ -n "$MON_PID" ] && kill -INT "$MON_PID" 2>/dev/null || true
  sleep 1
  [ -n "$MON_PID" ] && kill "$MON_PID" 2>/dev/null || true
  exec 3>&- 2>/dev/null || true
  [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
  rm -f "$FIFO"
  ip link del "$TAP" 2>/dev/null || true
}
trap cleanup EXIT

# ---- boot QEMU on the TAP ----
rm -f "$XDP_LOG" "$CON_LOG"
qemu-system-aarch64 -machine virt,gic-version=3 -m "$MEM" -nographic -cpu cortex-a72 \
  -netdev tap,id=n0,ifname="$TAP",script=no,downscript=no \
  -device virtio-net-pci,netdev=n0,mac="$GUEST_MAC",disable-legacy=on \
  -device virtio-rng-pci,disable-legacy=on \
  -drive file=build/disk.img,if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel build/kernel.elf < "$FIFO" > "$CON_LOG" 2>&1 &
QEMU_PID=$!
exec 3> "$FIFO"

echo "[test-ebpf] waiting for guest boot"
for _ in $(seq 1 60); do grep -q "Welcome to CortexForge" "$CON_LOG" && break; sleep 1; done
grep -q "Welcome to CortexForge" "$CON_LOG" || { echo "[test-ebpf] FAIL: guest did not boot"; tail -20 "$CON_LOG"; exit 1; }

# ---- attach the XDP monitor ----
./tools/ebpf/monitor "$TAP" > "$XDP_LOG" 2>&1 &
MON_PID=$!
for _ in $(seq 1 10); do grep -q "attached to $TAP" "$XDP_LOG" && break; sleep 0.5; done
grep -q "attached to $TAP" "$XDP_LOG" || { echo "[test-ebpf] FAIL: XDP attach failed"; cat "$XDP_LOG"; exit 1; }
echo "[test-ebpf] XDP attached; driving $PINGS guest pings to $HOST_IP"

# ---- generate ICMP: drive the guest shell to ping the host TAP, and let the
# netd background pinger (every ~5 s) accumulate over the window ----
sleep 2
for _ in $(seq 1 "$PINGS"); do printf 'ping\n' >&3; sleep "$PING_INTERVAL"; done
sleep "$FINAL_WAIT"

# ---- stop monitor to flush its FINAL summary, then evaluate ----
kill -INT "$MON_PID" 2>/dev/null; sleep 1

ICMP_EVENTS=$(grep -c "\[XDP\] ICMP" "$XDP_LOG" 2>/dev/null); ICMP_EVENTS=${ICMP_EVENTS:-0}
STATS_LINE=$(grep "\[XDP\] STATS" "$XDP_LOG" | tail -1)
FINAL_LINE=$(grep "\[XDP\] FINAL" "$XDP_LOG" | tail -1)
ICMP_STAT=$(echo "$STATS_LINE" | grep -oE "icmp=[0-9]+" | cut -d= -f2)
BYTES_STAT=$(echo "$STATS_LINE" | grep -oE "bytes=[0-9]+" | cut -d= -f2)
ICMP_STAT=${ICMP_STAT:-0}; BYTES_STAT=${BYTES_STAT:-0}

echo "[test-ebpf] ---- captured ----"
echo "[test-ebpf] ICMP event lines: $ICMP_EVENTS"
echo "[test-ebpf] $STATS_LINE"
echo "[test-ebpf] $FINAL_LINE"

if [ "${ICMP_STAT}" -ge 10 ] && [ "${BYTES_STAT}" -gt 0 ]; then
  echo "[test-ebpf] PASS: icmp=$ICMP_STAT (>=10) bytes=$BYTES_STAT (>0)"
  exit 0
else
  echo "[test-ebpf] FAIL: icmp=$ICMP_STAT bytes=$BYTES_STAT (need icmp>=10, bytes>0)"
  echo "----- last XDP log -----"; tail -30 "$XDP_LOG"
  exit 1
fi
