/*
 * xdp_monitor.h — layout shared between the BPF program (xdp_monitor.c) and the
 * userspace loader (monitor.c). Both sides must agree byte-for-byte on `struct
 * event` and the stats-array indices. The __uN fixed-width types come from
 * vmlinux.h (BPF side) or <linux/types.h> (userspace side), included first.
 */
#ifndef XDP_MONITOR_H
#define XDP_MONITOR_H

/* Indices into the BPF_MAP_TYPE_ARRAY "stats" (each value is a __u64). */
enum {
  STAT_TOTAL = 0, /* total packets           */
  STAT_ARP,       /* ARP packets             */
  STAT_ICMP,      /* IPv4/ICMP packets       */
  STAT_UDP,       /* IPv4/UDP packets        */
  STAT_OTHER,     /* everything else         */
  STAT_BYTES,     /* total bytes seen        */
  STAT_MAX
};

/* Ethertypes we classify (host byte order after ntohs). */
#define EV_ETH_IP  0x0800
#define EV_ETH_ARP 0x0806

/* Per-packet event pushed to the BPF_MAP_TYPE_RINGBUF "events". */
struct event {
  __u64 timestamp; /* bpf_ktime_get_ns()                    */
  __u32 src_ip;    /* network byte order (0 if unknown)     */
  __u32 dst_ip;    /* network byte order                    */
  __u32 size;      /* frame length on the wire (bytes)      */
  __u16 eth_type;  /* host byte order: EV_ETH_IP / _ARP / … */
  __u8  ip_proto;  /* IPv4 protocol (1=ICMP, 17=UDP), else 0 */
  __u8  icmp_type; /* ICMP type (8=echo request, 0=reply)   */
  __u16 icmp_seq;  /* ICMP echo sequence (host byte order)  */
  __u16 _pad;
};

#endif /* XDP_MONITOR_H */
