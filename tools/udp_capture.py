#!/usr/bin/env python3
"""Capture raw CRSF UDP packets into the tlm_viewer.py JSONL format.

Usage:
    python3 tools/udp_capture.py --port 14550 --out testdata/capture.jsonl
    python3 tools/udp_capture.py --port 14550          # print only

Each line: {"t": seconds, "src": "ip:port", "data": "<hex payload>"}
This matches the replay format used by the external CRSF viewer
(https://github.com/voffmail/ELRS-backpack-telemetry).
"""

import argparse
import json
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture raw CRSF UDP to JSONL")
    ap.add_argument("--host", default="0.0.0.0", help="listen address")
    ap.add_argument("--port", type=int, default=14550, help="UDP port")
    ap.add_argument("--out", default=None, help="output JSONL path (append)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    except OSError:
        pass
    sock.bind((args.host, args.port))

    out = open(args.out, "a", encoding="utf-8", buffering=1) if args.out else None
    start = time.monotonic()
    count = 0

    print(f"listening on {args.host}:{args.port}", file=sys.stderr)
    if out:
        print(f"writing to {args.out}", file=sys.stderr)

    try:
        while True:
            data, src = sock.recvfrom(65535)
            t = time.monotonic() - start
            line = json.dumps(
                {"t": round(t, 6), "src": f"{src[0]}:{src[1]}", "data": data.hex()},
                separators=(",", ":"),
            )
            if out:
                out.write(line + "\n")
            count += 1
            if count % 20 == 1:
                print(
                    f"{count} packets, last {len(data)} bytes from {src[0]}:{src[1]}",
                    file=sys.stderr,
                )
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
