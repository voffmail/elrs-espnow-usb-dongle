#!/usr/bin/env python3
"""Read the dongle serial stream, extract CRSF frames, forward them over UDP.

This is the PC stand-in for the Android app during bench tests.

    python3 tools/serial_bridge.py --serial /dev/ttyACM1 --udp 127.0.0.1:14550

Each valid CRSF frame is sent as one UDP datagram, matching the contract of
tlm_viewer.py and the original Android app.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

import serial

import elrs_protocol as proto
from serial_port import open_port


def parse_udp(value: str) -> tuple[str, int]:
    host, _, port = value.rpartition(":")
    if not host:
        raise argparse.ArgumentTypeError("expected HOST:PORT")
    return host, int(port)


def decode_device_stats(payload: bytes) -> str:
    if len(payload) < 20 or payload[0] != proto.CFG_OK:
        return "stats: bad response"
    packets, nbytes, drops, last_len, last_ms = struct.unpack_from("<IIIHI", payload, 1)
    ready = payload[19]
    channel = payload[20] if len(payload) > 20 else 0
    wifi_channel = payload[21] if len(payload) > 21 else channel
    link = ""
    if len(payload) >= 26:
        link = f" link={payload[23]}/{payload[24]}"
    return (f"stats: ready={ready} channel={channel}/{wifi_channel} packets={packets} "
            f"bytes={nbytes} drops={drops} last={last_len}/{last_ms}ms{link}")


def main() -> int:
    ap = argparse.ArgumentParser(description="ELRS dongle serial -> UDP CRSF bridge")
    ap.add_argument("--serial", required=True, help="dongle serial port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--udp", type=parse_udp, default=("127.0.0.1", 14550),
                    help="UDP destination HOST:PORT (default 127.0.0.1:14550)")
    ap.add_argument("--broadcast", action="store_true", help="enable SO_BROADCAST")
    ap.add_argument("--stats-every", type=float, default=2.0)
    ap.add_argument("--device-stats-every", type=float, default=10.0,
                    help="ask the dongle for GET_STATS every N seconds (0 = off)")
    ap.add_argument("--reconnect", type=float, default=1.0)
    args = ap.parse_args()

    udp_host, udp_port = args.udp
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.broadcast or udp_host.endswith("255"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)

    scanner = proto.CrsfScanner()
    cfg_parser = proto.CfgParser()
    sent = 0
    seq_last = None
    seq_lost = 0
    started = time.monotonic()
    last_stats = started
    last_device_stats = started
    begin = started

    print(f"serial {args.serial} @ {args.baud} -> udp {udp_host}:{udp_port}", file=sys.stderr)

    try:
        while True:
            try:
                ser = open_port(args.serial, args.baud)
            except Exception as exc:
                print(f"serial open failed: {exc}; retrying", file=sys.stderr)
                time.sleep(args.reconnect)
                continue

            try:
                while True:
                    chunk = ser.read(4096)
                    if not chunk:
                        continue
                    for frame in scanner.feed(chunk):
                        try:
                            sock.sendto(frame, (udp_host, udp_port))
                            sent += 1
                        except OSError as exc:
                            print(f"udp send failed: {exc}", file=sys.stderr)
                        if len(frame) >= 7 and frame[2] == 0x7F and frame[1] >= 6:
                            seq = struct.unpack_from("<I", frame, 3)[0]
                            if seq_last is not None and seq > seq_last + 1:
                                seq_lost += seq - seq_last - 1
                            seq_last = seq

                    for msg_type, payload in cfg_parser.feed(chunk):
                        if msg_type == (proto.CFG_GET_STATS | proto.CFG_RESPONSE):
                            print("device " + decode_device_stats(payload), file=sys.stderr)

                    now = time.monotonic()
                    if (args.device_stats_every > 0 and
                            now - last_device_stats >= args.device_stats_every):
                        last_device_stats = now
                        try:
                            ser.write(proto.build_cfg_frame(proto.CFG_GET_STATS))
                        except Exception as exc:
                            print(f"device stats write failed: {exc}", file=sys.stderr)
                    if args.stats_every > 0 and now - last_stats >= args.stats_every:
                        last_stats = now
                        elapsed = now - begin
                        rate = scanner.frames / elapsed if elapsed > 0 else 0.0
                        print(
                            f"t={elapsed:7.1f}s bytes={scanner.bytes} "
                            f"frames={scanner.frames} ({rate:.1f}/s) "
                            f"crc_err={scanner.crc_errors} sent={sent} "
                            f"seq_lost={seq_lost} last=0x{scanner.last_type:02X}/"
                            f"{scanner.last_len}",
                            file=sys.stderr,
                        )
            except (serial.SerialException, OSError) as exc:
                print(f"serial error: {exc}; reopening", file=sys.stderr)
            finally:
                try:
                    ser.close()
                except Exception:
                    pass
            time.sleep(args.reconnect)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    elapsed = time.monotonic() - begin
    print(
        f"stopped: {scanner.frames} frames, {scanner.crc_errors} CRC errors, "
        f"{seq_lost} sequence losses in {elapsed:.1f}s",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
