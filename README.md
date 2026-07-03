# CortexForge

## Attribution and Project Status

CortexForge is based on the open-source Fermi OS project:

- Upstream repository: https://github.com/rituparna-ui/fermi-os
- Imported revision: a2f11042bd5b4b7966ed6ba09f4a1a186eb8b6bd
- Upstream license: GNU General Public License version 3

The original copyright and license notices remain intact.

## Planned Extensions

All five planned extensions are complete (see below).

## Contributions by Kartik Vadhawana

Completed extensions (implemented and tested under QEMU):

### GitHub Actions CI + PMU benchmark harness

- **GitHub Actions CI** ([.github/workflows/ci.yml](.github/workflows/ci.yml)) —
  builds the kernel + FAT32 disk, boots headless QEMU, asserts the serial
  boot-success marker, runs the PMU benchmark, and publishes the results as a
  build artifact. Boot/bench are driven by
  [scripts/ci-boot-bench.sh](scripts/ci-boot-bench.sh).
- **PMU benchmark harness** ([src/lib/bench/](src/lib/bench/)) — cycle-level
  latency microbenchmarks (`bench` shell command → `SYS_BENCH` → EL1 harness on
  `PMCCNTR_EL0`). 1000 iterations each; reports min/max/mean/p50/p99. Measured
  under `qemu-system-aarch64 -icount shift=0` (deterministic, instruction-
  accurate cycle counting):

  | Benchmark | Cycles | ns @62.5 MHz (nominal) |
  |---|---|---|
  | Null syscall round-trip (`svc`→dispatch→`eret`) | 299 | 4,784 |
  | Context switch (round-trip; ~131/switch) | 261 | 4,176 |
  | Timer IRQ deadline→handler entry | 6 ticks | 96 |
  | Signal delivery (pending→handler-ready) | 653 | 10,448 |
  | Signal return (`sigreturn` context restore) | 434 | 6,944 |
  | UART frame encode (64-B payload: build+CRC+stuff) | 12,164 | ~196,000 |
  | CRC-16/CCITT over 64 bytes | 8,894 | ~142,000 |
  | CRC-16/CCITT over 256 bytes | 35,602 | ~569,000 |

  > Canonical values are from the GitHub Actions runner (Ubuntu,
  > `aarch64-linux-gnu-gcc`, QEMU `-icount shift=0`) and are published as a CI
  > artifact on every run. PMCCNTR under QEMU/TCG is an *emulated* cycle
  > counter, not silicon cycles; the ns column is a nominal conversion assuming
  > CPU freq == CNTFRQ (62.5 MHz). The cycle counts are real measured values
  > from the running emulator.

### POSIX signal subsystem

- **`sigaction`, `kill`, `sigprocmask`, `sigreturn`, `SIGALRM`**
  ([src/syscall/signal.c](src/syscall/signal.c)) — per-task handlers, pending /
  blocked masks, and a per-task alarm countdown driven by the timer IRQ.
- Signals are delivered lazily on the **return-to-EL0** path only (never to an
  EL1 task); the handler runs at EL0 with a signal frame built on the user
  stack (16-byte aligned, no dynamic allocation), and `sigreturn` restores the
  interrupted context exactly (PC/SP/registers/PSTATE).
- **VDSO-style sigreturn trampoline** — a kernel-owned, read-only, EL0-
  executable page mapped at a fixed VA (`0x300000`) into every address space,
  so a handler's `ret` traps straight into `SYS_SIGRETURN`.
- `SIGKILL` is uncatchable and unignorable; user-stack overflow during frame
  setup terminates the task.
- Verified: `./sigtest` ([user/sigtest.c](user/sigtest.c)) arms a 2 s alarm,
  the handler prints `SIGALRM received!`, and `main` resumes to print
  `signal test PASS` in QEMU.

### Reliable UART framing protocol

- **CRC-16/CCITT framing protocol** ([src/lib/uart/framing.c](src/lib/uart/framing.c),
  [framing.h](src/lib/uart/framing.h)) — frame format
  `START(0xAA) | TYPE | SEQ | LEN(2, BE) | PAYLOAD | CRC16(2, BE)` with
  CRC-16/CCITT (poly `0x1021`, init `0xFFFF`) over TYPE..PAYLOAD, whole-body
  byte stuffing (`0xAA` → `0xAA 0x00`), and sequence numbers.
- **`/dev/uart0` character device on UART1 (INTID 40)**
  ([src/devices/devices.c](src/devices/devices.c)) — interrupt-driven RX with a
  256-byte ring buffer; `read()` auto-ACKs a received frame, `write()` does a
  reliable send. `ioctl` (via a new `SYS_IOCTL`) exposes `SET_TIMEOUT` and
  `GET_STATS`.
- **Physical UART separation** — `/dev/console` stays on UART0 (`0x09000000`,
  polling); the framing protocol runs on UART1 (`0x09040000`, INTID 40) so
  framed bytes never mix with console output. Requires two QEMU serial ports.
- **ACK/NACK + retransmission** — reliable send retransmits up to 3× on timeout
  and immediately on NACK; a corrupted-CRC frame triggers a NACK. Retransmission
  is **verified by a dropped-ACK test**: withholding one ACK makes the kernel
  retransmit the identical echo frame.
- **Python host-side client** ([scripts/uart-client.py](scripts/uart-client.py))
  speaks the same protocol over QEMU's UART1 Unix socket. Against the in-guest
  echo server ([user/uartecho.c](user/uartecho.c)): **10/10 clean exchanges,
  RTT mean 0.32 ms / p99 0.91 ms**, plus verified NACK-on-bad-CRC and
  retransmit-on-dropped-ACK. Runs as phase 2 of CI.

### eBPF/XDP host-side network monitor

- **XDP monitor** ([tools/ebpf/](tools/ebpf/)) — a libbpf **CO-RE** program
  (`vmlinux.h` from kernel BTF) that attaches at the **XDP hook** on the QEMU
  TAP interface and passively classifies traffic from the CortexForge guest:
  Ethernet / ARP / IPv4 (ICMP, UDP, other). Monitoring only — every path returns
  **`XDP_PASS`** (never drops).
- Aggregate counters in a `BPF_MAP_TYPE_ARRAY` (total / arp / icmp / udp / other
  / bytes) plus per-packet events (`src_ip`, `dst_ip`, size, `bpf_ktime_get_ns`)
  streamed through a `BPF_MAP_TYPE_RINGBUF`. The userspace loader
  ([monitor.c](tools/ebpf/monitor.c)) prints per-packet lines and a 5-second
  stats summary, and detaches cleanly on Ctrl-C.
- **Attach mode:** `XDP_FLAGS_SKB_MODE` (generic) — native/driver XDP is not
  available on TAP interfaces. XDP hooks **ingress**, so it observes the guest's
  outbound ARP + ICMP-echo traffic to the host.
- **Runtime requires:** Linux 5.15+, root (`CAP_NET_ADMIN`), and a TAP interface
  — the project's default QEMU uses slirp (no host interface), so the end-to-end
  test ([scripts/test-ebpf.sh](scripts/test-ebpf.sh)) reconfigures QEMU to TAP.
  Toolchain: `clang`, `libbpf-dev`, `bpftool`. **CI verifies compilation only**
  (running XDP needs root + a real interface).

Licensed under GPL-3.0. See LICENSE file for details.

---

# Fermi OS

Fermi OS is a bare-metal `aarch64 (ARMv8-A)` kernel built from scratch in `C` and assembly, targeting QEMU's `virt` machine with a Cortex-A72 processor.


---

## Features

### Boot & Memory
- **PL011 UART Driver** — Full serial I/O, hex/decimal/binary output and formatted print with `%s %d %u %x %p %b %c %%` format specifiers
- **Physical Memory Manager (PMM)** — Bitmap-based page allocator managing 8 GB of RAM, with single and contiguous multi-page allocation
- **MMU (Memory Management Unit)** — 3-level page tables (L0→L1→L2) with 2 MB blocks, 48-bit virtual address space, 4 KB granule
- **Higher-Half Kernel** — Kernel linked at Virt Memory Address `0xFFFF_0000_4000_0000` with physical Load Memory Address `0x4000_0000`. Dual address space with TTBR0 (user) and TTBR1 (kernel)
- **Kernel Heap** — First-fit allocator with block splitting, coalescing, double-free detection, and bounds checking (`kmalloc`/`kfree`)
- **Kernel Panic Handler** — System register dump and CPU halt on unrecoverable errors

### Exceptions & Interrupts
- **Exception Handling** — Full ARMv8-A vector table, trap frame save/restore, ESR decoding, register dump on fault
- **GICv3 Interrupt Controller** — Minimal GICv3 bringup with Distributor/Redistributor initialization, affinity routing, system register interface, IRQ acknowledge/EOI
- **ARM Generic Timer** — Configurable periodic tick (default 1 s) driving the scheduler, routed through GICv3 PPI

### Scheduling & Processes
- **Preemptive Scheduler** — Round-robin task scheduler with timer-driven preemption, per-task kernel stacks, context switching via callee-saved register save/restore, task creation/exit/reaping lifecycle, and a circular run queue
- **Task Sleep** — Tick-based voluntary sleep via `sleep_ms(ms)`, with per-task deadlines and automatic wakeup on timer IRQ (`sched_wake_sleepers`)
- **EL0 User-Space Tasks** — Full EL1→EL0 privilege separation via `eret`, per-task user text and stack mappings with proper permission bits (UXN, PXN, AP), separate kernel and user stacks, and a trampoline that sets `SP_EL0`/`ELR_EL1`/`SPSR_EL1` before dropping to user mode
- **Per-Task Address Spaces (TTBR0 Switching)** — Each task gets its own L0→L1→L2 user page tables allocated from the PMM; `context_switch` swaps `TTBR0_EL1` and performs `TLBI`/`DSB`/`ISB` on every task switch, with recursive page table teardown on task reap
- **System Call Interface (SVC)** — AAPCS64-based `svc #0` dispatch with `x8` as syscall number and `x0`–`x7` for arguments; return value written back via the trap frame
- **POSIX-style Syscalls** — `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_EXIT`, `SYS_YIELD`, `SYS_SLEEP`, `SYS_GETPID`, `SYS_LSEEK`, `SYS_UPTIME`, `SYS_NET_PING`, `SYS_KILL`. I/O syscalls route through the current task's fd table; the dispatch path unmasks IRQs so blocking syscalls don't starve other tasks. User pointers are range-checked against `[0, USER_STACK_TOP)` to close kernel-pointer-injection holes
- **EL0 Page-Fault Handling** — Data and Instruction aborts from a lower EL kill *only* the offending task (logging pid/ELR/FAR/ESR) and continue scheduling; aborts from EL1 still trip a full `kernel_panic` since they indicate a real kernel bug
- **Kernel-Mode (EL1) Tasks** — `sched_create_kernel_task` provides a parallel scheduler path with its own `kernel_task_trampoline` (no `eret`, no TTBR0 swap). Used by the in-tree `netd` daemon

### PCI & VirtIO
- **PCI Express (ECAM)** — Brute-force bus enumeration, device discovery and caching, BAR assignment (32-bit and 64-bit MMIO), Memory Space + Bus Master enable via Command register
- **VirtIO PCI Transport** — Capability list walking (vendor-specific 0x09), common/notify/ISR/device config BAR+offset resolution, MMIO register access via named offsets (virtio spec 4.1.4.3)
- **Virtqueue (Split)** — Reusable split virtqueue module with descriptor table, available ring, used ring, `VIRT_TO_PHYS` DMA address conversion, submit/notify/poll API
- **VirtIO RNG Driver** — Full VirtIO device init sequence (reset → ack → driver → feature negotiation → FEATURES_OK → queue setup → DRIVER_OK), random byte generation via `virtio-rng-pci`
- **VirtIO Block Driver** — `virtio-blk-pci` device init, capacity readout from device config, and synchronous 512-byte sector `blk_read`/`blk_write` using chained descriptors (header + data + status) over the split virtqueue
- **VirtIO Net Driver** — `virtio-net-pci` device init (modern dev id `0x1041`, `VIRTIO_F_VERSION_1` required), MAC + link-status readout, RX queue pre-fill (8 1 600-byte buffers), `net_tx`/`net_rx_poll` synchronous APIs, and per-direction packet counters

### Networking
- **Layer 2 — Ethernet** — Hand-built ARP requests/replies. Boot path sends a broadcast ARP for the slirp gateway, parses the reply, and caches the resulting `gateway_mac`
- **Layer 3 — IPv4 + ICMP echo** — RFC 1071 internet checksum helper, IPv4 header builder, and ICMP echo request that drives a full L3 round-trip to QEMU's slirp gateway (`10.0.2.2`). Verified: ARP → IPv4 → ICMP echo reply with TTL
- **netd kernel daemon** — Periodic background pinger running at EL1: drains incoming RX, sends an ICMP echo every 5 s, and prints reply latency in ticks
- **`SYS_NET_PING`** — EL0-callable ICMP echo so user-space tasks (notably the shell's `ping` command) can fire pings without leaving the syscall ABI

### Filesystem & Devices
- **Virtual Filesystem (VFS)** — Unix-style vnode tree with path resolution (`.`, `..`, multi-slash tolerant). Per-vnode `file_operations` vtable (read/write) and `vnode_operations` vtable (lazy `lookup`). Supports char devices, block devices, directories, regular files
- **Per-Process File Descriptor Table** — `fd_table_t` allocated per task, freed on reap. fd 0/1/2 auto-opened to `/dev/console` (stdin/stdout/stderr). `fd_open`/`fd_read`/`fd_write`/`fd_close`/`fd_seek` (SEEK_SET/SEEK_CUR) dispatching through vnode ops
- **Built-in Char Devices** — `/dev/console` (UART read/write), `/dev/null` (discard/EOF), `/dev/zero` (zero-fill), `/dev/rng` (virtio-rng with bounce buffer for DMA)
- **Block Device Node** — `/dev/blk` exposing the virtio-blk disk with sector-aligned byte-offset read/write
- **FAT32 (VFS-backed)** — Mounted at `/mnt/fat32`. Lazy directory traversal: each `lookup` walks on-disk directory entries and creates a vnode on demand with per-vnode `(first_cluster, size)` state. `open`/`read` on regular files goes through the full VFS → fd → `file_operations.read` path
- **`/proc` synthetic filesystem** — Mounted at `/proc`, regenerates content per-read from live kernel state. Files: `/proc/uptime`, `/proc/meminfo` (PMM + heap), `/proc/tasks` (run-queue snapshot with state names), `/proc/interrupts` (per-INTID counts via the GIC dispatch hook), `/proc/netinfo` (MAC, link, IP, gateway MAC, packet counters), `/proc/cmdline`, `/proc/version`

### EL0 Shell (interactive)
- **`task_shell`** — An EL0 task that loops reading lines from `/dev/console` (with backspace/DEL editing and echo) and dispatches built-ins. Pure user-space — talks to the kernel only via `svc`. Built-ins: `help`, `pid`, `uptime`, `ps`, `free`, `ifconfig`, `irqs`, `version`, `cat <path>`, `kill <pid>`, `top` (5× refresh tasks/mem/net), `ping`, `sleep <ms>`, `clear`, `exit`

---

## Prerequisites
> Note: This project is being developed and tested on Mac M4 chip. There is a possibility that you might encounter environment setup errors on other platforms.

Install [docker](https://www.docker.com) on your host machine.

```bash
git clone https://github.com/rituparna-ui/fermi-os.git
cd fermi-os

docker run -d -it -v .:/root/fermi-os --name osdev ubuntu
```

Once the Docker container is up and running, start a shell in the container that was just created.
```bash
docker exec -it osdev bash
```

Inside the Docker container, install required dependencies.
```bash
apt update && apt upgrade

apt install make qemu-system gcc-aarch64-linux-gnu gdb-multiarch tmux mtools dosfstools
ln -sf aarch64-linux-gnu-as /usr/bin/as
```

## Building & Running

```bash
# Build the kernel ELF
make

# Build and run in QEMU (serial console)
make run

# Clean build artifacts
make clean
```

To exit QEMU: `Ctrl-A` then `X`

## Debugging

```bash
# Launch QEMU paused + GDB in a tmux split
make tmux
```

Or manually in two terminals:

```bash
# Terminal 1: QEMU waiting for debugger
make debug

# Terminal 2: GDB connecting to QEMU
make gdb
```

### Other Utilities

```bash
# Generate compile_commands.json for clangd / IDE support
make compile_commands.json

# Dump QEMU device tree source (DTS)
make dump_dts
```
