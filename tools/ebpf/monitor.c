// SPDX-License-Identifier: GPL-2.0
/*
 * monitor.c — userspace loader for xdp_monitor.bpf.o (libbpf + CO-RE skeleton).
 *
 *   ./monitor <ifname>
 *
 * Attaches the XDP program to <ifname> in SKB (generic) mode — native XDP is not
 * available on TAP interfaces — streams per-packet events from the ring buffer,
 * prints an aggregate stats summary every 5 s, and on Ctrl-C detaches cleanly
 * and prints a final summary.
 *
 * Needs CAP_NET_ADMIN (run as root). Build via the Makefile in this directory.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/if_link.h> /* XDP_FLAGS_SKB_MODE */
#include <linux/types.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "xdp_monitor.h"
#include "xdp_monitor.skel.h"

static volatile sig_atomic_t exiting = 0;
static void on_signal(int sig) {
  (void)sig;
  exiting = 1;
}

static __u64 now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static const char *ip_str(__u32 be_ip, char *buf, size_t len) {
  struct in_addr a = {.s_addr = be_ip};
  if (!inet_ntop(AF_INET, &a, buf, len)) {
    snprintf(buf, len, "?");
  }
  return buf;
}

static int handle_event(void *ctx, void *data, size_t sz) {
  (void)ctx;
  if (sz < sizeof(struct event)) {
    return 0;
  }
  const struct event *e = data;
  char s[INET_ADDRSTRLEN], d[INET_ADDRSTRLEN];

  if (e->eth_type == EV_ETH_ARP) {
    printf("[XDP] ARP  src=%s dst=%s size=%uB\n",
           ip_str(e->src_ip, s, sizeof(s)), ip_str(e->dst_ip, d, sizeof(d)),
           e->size);
  } else if (e->eth_type == EV_ETH_IP && e->ip_proto == 1 /* ICMP */) {
    printf("[XDP] ICMP src=%s dst=%s seq=%u type=%u size=%uB\n",
           ip_str(e->src_ip, s, sizeof(s)), ip_str(e->dst_ip, d, sizeof(d)),
           e->icmp_seq, e->icmp_type, e->size);
  } else if (e->eth_type == EV_ETH_IP && e->ip_proto == 17 /* UDP */) {
    printf("[XDP] UDP  src=%s dst=%s size=%uB\n",
           ip_str(e->src_ip, s, sizeof(s)), ip_str(e->dst_ip, d, sizeof(d)),
           e->size);
  } else {
    printf("[XDP] OTHER eth=0x%04x size=%uB\n", e->eth_type, e->size);
  }
  fflush(stdout);
  return 0;
}

static __u64 stat_get(int map_fd, __u32 idx) {
  __u64 v = 0;
  if (bpf_map_lookup_elem(map_fd, &idx, &v) != 0) {
    return 0;
  }
  return v;
}

static void print_stats(int map_fd) {
  printf("[XDP] STATS: total=%llu arp=%llu icmp=%llu udp=%llu other=%llu "
         "bytes=%llu\n",
         (unsigned long long)stat_get(map_fd, STAT_TOTAL),
         (unsigned long long)stat_get(map_fd, STAT_ARP),
         (unsigned long long)stat_get(map_fd, STAT_ICMP),
         (unsigned long long)stat_get(map_fd, STAT_UDP),
         (unsigned long long)stat_get(map_fd, STAT_OTHER),
         (unsigned long long)stat_get(map_fd, STAT_BYTES));
  fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <ifname>\n", argv[0]);
    return 2;
  }
  const char *ifname = argv[1];
  unsigned int ifindex = if_nametoindex(ifname);
  if (ifindex == 0) {
    fprintf(stderr, "[XDP] no such interface '%s': %s\n", ifname,
            strerror(errno));
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  struct xdp_monitor *skel = xdp_monitor__open_and_load();
  if (!skel) {
    fprintf(stderr, "[XDP] failed to open/load BPF skeleton\n");
    return 1;
  }

  /* SKB (generic) mode — TAP interfaces do not support native/driver XDP. */
  __u32 flags = XDP_FLAGS_SKB_MODE;
  int prog_fd = bpf_program__fd(skel->progs.xdp_monitor);
  if (bpf_xdp_attach(ifindex, prog_fd, flags, NULL) != 0) {
    fprintf(stderr, "[XDP] attach to %s failed: %s (need root/CAP_NET_ADMIN)\n",
            ifname, strerror(errno));
    xdp_monitor__destroy(skel);
    return 1;
  }
  printf("[XDP] attached to %s (ifindex=%u) in SKB mode\n", ifname, ifindex);
  fflush(stdout);

  int events_fd = bpf_map__fd(skel->maps.events);
  int stats_fd  = bpf_map__fd(skel->maps.stats);

  struct ring_buffer *rb = ring_buffer__new(events_fd, handle_event, NULL, NULL);
  if (!rb) {
    fprintf(stderr, "[XDP] failed to create ring buffer\n");
    bpf_xdp_detach(ifindex, flags, NULL);
    xdp_monitor__destroy(skel);
    return 1;
  }

  __u64 start = now_ns();
  __u64 last_stats = start;
  while (!exiting) {
    int err = ring_buffer__poll(rb, 200 /* ms */);
    if (err < 0 && err != -EINTR) {
      fprintf(stderr, "[XDP] ring buffer poll error: %d\n", err);
      break;
    }
    __u64 t = now_ns();
    if (t - last_stats >= 5000000000ULL) { /* every 5 s */
      print_stats(stats_fd);
      last_stats = t;
    }
  }

  /* Final summary. */
  __u64 runtime_ns = now_ns() - start;
  double runtime_s = (double)runtime_ns / 1e9;
  __u64 packets    = stat_get(stats_fd, STAT_TOTAL);
  __u64 bytes      = stat_get(stats_fd, STAT_BYTES);
  __u64 icmp       = stat_get(stats_fd, STAT_ICMP);
  double icmp_pps  = runtime_s > 0 ? (double)icmp / runtime_s : 0.0;
  double mean_size = packets > 0 ? (double)bytes / (double)packets : 0.0;

  printf("[XDP] FINAL: runtime=%.0fs packets=%llu bytes=%llu icmp_pps=%.1f "
         "mean_size=%.0fB\n",
         runtime_s, (unsigned long long)packets, (unsigned long long)bytes,
         icmp_pps, mean_size);
  print_stats(stats_fd);
  fflush(stdout);

  ring_buffer__free(rb);
  bpf_xdp_detach(ifindex, flags, NULL);
  xdp_monitor__destroy(skel);
  printf("[XDP] detached from %s cleanly\n", ifname);
  return 0;
}
