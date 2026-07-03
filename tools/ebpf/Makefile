# ---------------------------------------------------------------------------
# tools/ebpf/Makefile — build the XDP monitor (BPF object + skeleton + loader).
#
# Required packages (Debian/Ubuntu):
#   clang llvm libbpf-dev linux-headers-$(uname -r) \
#   linux-tools-generic linux-tools-$(uname -r)   # provides bpftool
#
# Pipeline:
#   xdp_monitor.c  --clang -target bpf-->  xdp_monitor.bpf.o
#   xdp_monitor.bpf.o --bpftool gen skeleton-->  xdp_monitor.skel.h
#   monitor.c + skeleton  --gcc + libbpf-->  monitor
#
# vmlinux.h is generated from the running kernel's BTF (CO-RE); this needs
# /sys/kernel/btf/vmlinux (any modern kernel built with CONFIG_DEBUG_INFO_BTF).
# LINUX ONLY — eBPF/XDP is a Linux-kernel facility.
# ---------------------------------------------------------------------------
CLANG   ?= clang
BPFTOOL ?= bpftool
CC      ?= gcc

# Map `uname -m` to the __TARGET_ARCH_* clang define libbpf expects.
ARCH := $(shell uname -m | sed 's/x86_64/x86/; s/aarch64/arm64/; s/ppc64le/powerpc/; s/mips.*/mips/; s/riscv64/riscv/; s/loongarch64/loongarch/')

BPF_OBJ := xdp_monitor.bpf.o
SKEL    := xdp_monitor.skel.h
VMLINUX := vmlinux.h

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -I.
USER_CFLAGS := -g -O2 -Wall -Wextra -I.
USER_LDLIBS := -lbpf -lelf -lz

.PHONY: all clean
all: monitor

# Generate vmlinux.h from the running kernel's BTF (CO-RE type source).
$(VMLINUX):
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# Compile the BPF program to a CO-RE object.
$(BPF_OBJ): xdp_monitor.c xdp_monitor.h $(VMLINUX)
	$(CLANG) $(BPF_CFLAGS) -c xdp_monitor.c -o $@

# Generate the libbpf skeleton header from the BPF object. `name xdp_monitor`
# pins the skeleton struct/function prefix so monitor.c matches deterministically
# regardless of bpftool's default extension-stripping.
$(SKEL): $(BPF_OBJ)
	$(BPFTOOL) gen skeleton $< name xdp_monitor > $@

# Build the userspace loader against the skeleton and libbpf.
monitor: monitor.c $(SKEL) xdp_monitor.h
	$(CC) $(USER_CFLAGS) monitor.c -o $@ $(USER_LDLIBS)

clean:
	rm -f $(BPF_OBJ) $(SKEL) $(VMLINUX) monitor
