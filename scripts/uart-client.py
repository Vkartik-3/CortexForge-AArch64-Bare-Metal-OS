#!/usr/bin/env python3
"""
uart-client.py — host-side peer for the CortexForge UART framing protocol.

Connects to QEMU's UART1 chardev (exported as a Unix socket via
`-serial unix:/tmp/uart.sock,server,nowait`) and speaks the exact same frame
format as the kernel (src/lib/uart/framing.c):

  START(0xAA) | TYPE | SEQ | LEN(2, big-endian) | PAYLOAD | CRC16(2, big-endian)

CRC is CRC-16/CCITT (poly 0x1021, init 0xFFFF) over TYPE..PAYLOAD. Any 0xAA in
the body is byte-stuffed as 0xAA 0x00.

Test flow (against the `uartecho` server running in the guest):
  * 10 clean DATA<->echo exchanges, printing per-exchange RTT
  * 1 corrupted-CRC DATA frame, expecting a NACK from the kernel reader
  * 1 dropped-ACK exchange, expecting the kernel to retransmit its echo
  * summary: frames sent/received, RTT mean/p99
"""
import argparse
import socket
import sys
import time

START = 0xAA
DATA, ACK, NACK = 0x01, 0x02, 0x03
TYPE_NAME = {DATA: "DATA", ACK: "ACK", NACK: "NACK"}


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(mtype: int, seq: int, payload: bytes,
                corrupt_crc: bool = False) -> bytes:
    body = bytes([mtype, seq, (len(payload) >> 8) & 0xFF,
                  len(payload) & 0xFF]) + payload
    crc = crc16_ccitt(body)
    if corrupt_crc:
        crc ^= 0xFFFF  # deliberately wrong
    full = body + bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    out = bytearray([START])
    for b in full:
        out.append(b)
        if b == START:          # byte-stuff literal 0xAA in the body
            out.append(0x00)
    return bytes(out)


class Framer:
    """Buffered reader that un-stuffs and parses frames from a stream socket."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()

    def _readb(self, deadline):
        while True:
            if self.buf:
                b = self.buf[0]
                del self.buf[0]
                return b
            to = deadline - time.time()
            if to <= 0:
                return None
            self.sock.settimeout(to)
            try:
                chunk = self.sock.recv(512)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf.extend(chunk)

    def _logical(self, deadline):
        """One un-stuffed body byte: int, ('START', type), or None (timeout)."""
        b = self._readb(deadline)
        if b is None:
            return None
        if b == START:
            n = self._readb(deadline)
            if n is None:
                return None
            if n == 0x00:
                return START          # stuffed literal 0xAA
            return ('START', n)       # a new frame delimiter mid-stream
        return b

    def recv_frame(self, timeout=2.0):
        """Returns (type, seq, payload, crc_ok) or None on timeout."""
        deadline = time.time() + timeout
        # sync to a START + valid type
        while True:
            b = self._readb(deadline)
            if b is None:
                return None
            if b != START:
                continue
            t = self._readb(deadline)
            if t is None:
                return None
            if t in (DATA, ACK, NACK):
                mtype = t
                break
        while True:
            restart = False
            hdr = []
            for _ in range(3):
                x = self._logical(deadline)
                if x is None:
                    return None
                if isinstance(x, tuple):
                    mtype, restart = x[1], True
                    break
                hdr.append(x)
            if restart:
                continue
            seq = hdr[0]
            ln = (hdr[1] << 8) | hdr[2]
            payload = []
            for _ in range(ln):
                x = self._logical(deadline)
                if x is None:
                    return None
                if isinstance(x, tuple):
                    mtype, restart = x[1], True
                    break
                payload.append(x)
            if restart:
                continue
            crcb = []
            for _ in range(2):
                x = self._logical(deadline)
                if x is None:
                    return None
                if isinstance(x, tuple):
                    mtype, restart = x[1], True
                    break
                crcb.append(x)
            if restart:
                continue
            rx_crc = (crcb[0] << 8) | crcb[1]
            body = bytes([mtype, seq, hdr[1], hdr[2]] + payload)
            return (mtype, seq, bytes(payload), crc16_ccitt(body) == rx_crc)


def percentile(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/uart.sock")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--connect-timeout", type=float, default=20.0)
    args = ap.parse_args()

    # Connect (retry: the guest / socket may not be ready immediately).
    sock = None
    deadline = time.time() + args.connect_timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(args.sock)
            sock = s
            break
        except OSError:
            time.sleep(0.3)
    if sock is None:
        print(f"[uart-client] FAIL: could not connect to {args.sock}")
        return 1
    print(f"[uart-client] connected to {args.sock}")

    fr = Framer(sock)
    sent = recv = 0
    rtts = []
    ok_exchanges = 0

    # --- 10 clean exchanges ---
    for seq in range(args.count):
        payload = b"PING"
        t0 = time.time()
        sock.sendall(build_frame(DATA, seq, payload))
        sent += 1

        ack = fr.recv_frame(3.0)
        if not ack or ack[0] != ACK or ack[1] != seq:
            print(f"[uart-client] seq={seq}: no/!bad ACK ({ack})")
            continue

        echo = fr.recv_frame(3.0)
        if not echo or echo[0] != DATA or not echo[3] or echo[2] != payload:
            print(f"[uart-client] seq={seq}: no/!bad echo ({echo})")
            continue

        rtt = (time.time() - t0) * 1000.0
        rtts.append(rtt)
        recv += 1
        ok_exchanges += 1
        # ACK the echo using the ECHO frame's own seq (kernel's TX seq).
        sock.sendall(build_frame(ACK, echo[1], b""))
        print(f"[uart-client] exchange seq={seq} payload={echo[2].decode(errors='replace')} "
              f"rtt={rtt:.2f}ms")

    # --- bad CRC -> expect NACK ---
    nack_ok = False
    bad_seq = args.count & 0xFF
    sock.sendall(build_frame(DATA, bad_seq, b"BADCRC", corrupt_crc=True))
    sent += 1
    f = fr.recv_frame(3.0)
    if f and f[0] == NACK:
        nack_ok = True
        print(f"[uart-client] bad-CRC test: kernel sent NACK (seq={f[1]}) — OK")
    else:
        print(f"[uart-client] bad-CRC test: expected NACK, got {f} — FAIL")

    # --- dropped ACK -> expect kernel retransmit of its echo ---
    retrans_ok = False
    dseq = (args.count + 1) & 0xFF
    sock.sendall(build_frame(DATA, dseq, b"DROP"))
    sent += 1
    a = fr.recv_frame(3.0)          # ACK from the kernel reader
    e1 = fr.recv_frame(3.0)         # first echo
    if a and a[0] == ACK and e1 and e1[0] == DATA:
        recv += 1
        # Withhold the ACK; the kernel's reliable send should time out and
        # retransmit the identical echo frame.
        e2 = fr.recv_frame(4.0)
        if e2 and e2[0] == DATA and e2[1] == e1[1] and e2[2] == e1[2]:
            retrans_ok = True
            print(f"[uart-client] dropped-ACK test: kernel RETRANSMITTED echo "
                  f"(seq={e2[1]}) — OK")
        else:
            print(f"[uart-client] dropped-ACK test: no retransmit ({e2}) — FAIL")
        sock.sendall(build_frame(ACK, e1[1], b""))  # let the kernel finish
    else:
        print(f"[uart-client] dropped-ACK test: setup failed (ack={a}, echo={e1})")

    # --- summary ---
    mean = sum(rtts) / len(rtts) if rtts else 0.0
    p99 = percentile(rtts, 99)
    print("[uart-client] ---- summary ----")
    print(f"[uart-client] frames_sent={sent} frames_recv={recv} "
          f"clean_exchanges={ok_exchanges}/{args.count}")
    print(f"[uart-client] rtt_mean={mean:.2f}ms rtt_p99={p99:.2f}ms")
    print(f"[uart-client] nack_on_bad_crc={'PASS' if nack_ok else 'FAIL'} "
          f"retransmit_on_dropped_ack={'PASS' if retrans_ok else 'FAIL'}")

    good = (ok_exchanges >= 5) and nack_ok and retrans_ok
    print(f"[uart-client] RESULT: {'PASS' if good else 'FAIL'}")
    sock.close()
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
