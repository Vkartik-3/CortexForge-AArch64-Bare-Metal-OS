// SPDX-License-Identifier: GPL-2.0
/*
 * xdp_monitor.c — XDP program that passively monitors traffic on the QEMU TAP
 * interface used by CortexForge. Classifies Ethernet / ARP / IPv4 (ICMP, UDP,
 * other), maintains aggregate counters in a BPF array map, and emits a
 * per-packet event to a ring buffer. It NEVER drops: every path returns
 * XDP_PASS (monitoring only).
 *
 * CO-RE: built against vmlinux.h (generated from the running kernel's BTF by
 * the Makefile), so the same .bpf.o loads across kernel versions. Packet fields
 * are read by direct, bounds-checked access to the frame data (packet bytes are
 * not relocatable kernel structs, so BPF_CORE_READ is unnecessary for them).
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include "xdp_monitor.h"

#define ETH_P_IP_     0x0800
#define ETH_P_ARP_    0x0806
#define IPPROTO_ICMP_ 1
#define IPPROTO_UDP_  17

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, STAT_MAX);
  __type(key, __u32);
  __type(value, __u64);
} stats SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 256 * 1024); /* 256 KiB ring */
} events SEC(".maps");

static __always_inline void stat_add(__u32 idx, __u64 by) {
  __u64 *v = bpf_map_lookup_elem(&stats, &idx);
  if (v) {
    __sync_fetch_and_add(v, by);
  }
}

SEC("xdp")
int xdp_monitor(struct xdp_md *ctx) {
  void *data     = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;
  __u32 pktlen   = (__u32)(data_end - data);

  stat_add(STAT_TOTAL, 1);
  stat_add(STAT_BYTES, pktlen);

  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end) {
    return XDP_PASS; /* runt frame — still passed */
  }
  __u16 h_proto = bpf_ntohs(eth->h_proto);

  struct event ev = {};
  ev.timestamp = bpf_ktime_get_ns();
  ev.size      = pktlen;
  ev.eth_type  = h_proto;

  if (h_proto == ETH_P_ARP_) {
    stat_add(STAT_ARP, 1);
    /* ARP payload starts right after the 14-byte Ethernet header. For IPv4-
     * over-Ethernet ARP: SPA (sender IP) at payload offset 14, TPA (target IP)
     * at offset 24. Bounds-check the whole 28-byte ARP body first. */
    unsigned char *arp = (unsigned char *)(eth + 1);
    if ((void *)(arp + 28) <= data_end) {
      ev.src_ip = *(__u32 *)(arp + 14);
      ev.dst_ip = *(__u32 *)(arp + 24);
    }
  } else if (h_proto == ETH_P_IP_) {
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
      return XDP_PASS;
    }
    ev.src_ip   = ip->saddr;
    ev.dst_ip   = ip->daddr;
    ev.ip_proto = ip->protocol;

    if (ip->protocol == IPPROTO_ICMP_) {
      stat_add(STAT_ICMP, 1);
      /* Assumes a standard 20-byte IPv4 header (no options) — true for the
       * CortexForge stack's echo packets, and keeps the access verifier-
       * friendly (fixed offset). */
      struct icmphdr *icmp = (void *)(ip + 1);
      if ((void *)(icmp + 1) <= data_end) {
        ev.icmp_type = icmp->type;
        ev.icmp_seq  = bpf_ntohs(icmp->un.echo.sequence);
      }
    } else if (ip->protocol == IPPROTO_UDP_) {
      stat_add(STAT_UDP, 1);
    } else {
      stat_add(STAT_OTHER, 1);
    }
  } else {
    stat_add(STAT_OTHER, 1);
  }

  struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
  if (e) {
    *e = ev;
    bpf_ringbuf_submit(e, 0);
  }
  return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
