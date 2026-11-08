#!/usr/bin/env python3
"""Read or change the dongle/simulator runtime configuration over USB/UART.

Binding can be a phrase (UID is derived) or a manual 6-byte UID. An empty
phrase and an all-zero UID are rejected; clearing is only done by --reset.
The phrase is never written to this repo.

Examples:
    python3 tools/dongle_config.py --port /dev/ttyACM0 --show
    ELRS_BINDING_PHRASE='...' python3 tools/dongle_config.py --port /dev/ttyACM0 --channel 1
    python3 tools/dongle_config.py --port /dev/ttyACM0 --uid AA:BB:CC:DD:EE:FF
    python3 tools/dongle_config.py --port /dev/ttyACM0 --stats
    python3 tools/dongle_config.py --port /dev/ttyACM0 --reset
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time

import serial

import elrs_protocol as proto
from serial_port import open_port


STATUS_NAMES = {
    proto.CFG_OK: "ok",
    proto.CFG_ERR_BAD_REQUEST: "bad request",
    proto.CFG_ERR_TOO_LONG: "too long",
    proto.CFG_ERR_INVALID: "invalid",
    proto.CFG_ERR_BUSY: "busy",
}


def send_command(ser: serial.Serial, msg_type: int, payload: bytes = b"",
                 timeout: float = 2.0) -> bytes:
    expected = msg_type | proto.CFG_RESPONSE
    ser.reset_input_buffer()
    ser.write(proto.build_cfg_frame(msg_type, payload))
    ser.flush()
    parser = proto.CfgParser()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if not chunk:
            continue
        for response_type, response in parser.feed(chunk):
            if response_type == expected:
                return response
            if response_type == (proto.CFG_GET_STATS | proto.CFG_RESPONSE):
                continue
    raise TimeoutError("no response from device (check port and phrase)")


def decode_config(payload: bytes) -> int:
    if len(payload) < 10:
        print("short config response:", payload.hex())
        return 1
    status = payload[0]
    if status != proto.CFG_OK:
        print("device returned:", STATUS_NAMES.get(status, hex(status)))
        return 1
    channel = payload[1]
    uid = payload[2:8]
    phrase_len = payload[8]
    phrase = payload[9:9 + phrase_len].decode("utf-8", "replace")
    version = ".".join(str(b) for b in payload[9 + phrase_len:12 + phrase_len])
    mode = (payload[12 + phrase_len]
            if len(payload) > 12 + phrase_len else proto.UID_MODE_PHRASE)
    mode_name = "manual UID" if mode == proto.UID_MODE_MANUAL else "phrase"
    print(f"mode:    {mode_name}")
    print(f"channel: {channel}")
    print(f"uid:     {proto.uid_to_string(uid)}")
    print(f"phrase:  {phrase if phrase else '<empty>'}")
    print(f"version: {version}")
    return 0


def decode_stats(payload: bytes) -> int:
    if len(payload) < 20:
        print("short stats response:", payload.hex())
        return 1
    status = payload[0]
    if status != proto.CFG_OK:
        print("device returned:", STATUS_NAMES.get(status, hex(status)))
        return 1
    rx_packets, rx_bytes, drops, last_len, last_ms = struct.unpack_from("<IIIHI", payload, 1)
    ready = payload[19]
    channel = payload[20] if len(payload) > 20 else 0
    wifi_channel = payload[21] if len(payload) > 21 else channel
    host = payload[22] if len(payload) > 22 else None
    print(f"ready:   {ready}")
    print(f"channel: {channel}")
    print(f"wifi:    {wifi_channel}")
    if host is not None:
        print(f"host:    {host}")
    if len(payload) >= 26:
        print(f"link:    upLQ={payload[23]} downLQ={payload[24]} seen={payload[25]}")
    print(f"packets: {rx_packets}")
    print(f"bytes:   {rx_bytes}")
    print(f"drops:   {drops}")
    print(f"last:    {last_len} bytes, {last_ms} ms")
    return 0


def current_config(ser: serial.Serial, timeout: float):
    payload = send_command(ser, proto.CFG_GET_CONFIG, timeout=timeout)
    if len(payload) < 10 or payload[0] != proto.CFG_OK:
        raise RuntimeError("device did not return a valid config")
    channel = payload[1]
    uid = payload[2:8]
    phrase_len = payload[8]
    phrase = payload[9:9 + phrase_len].decode("utf-8", "replace")
    mode = (payload[12 + phrase_len]
            if len(payload) > 12 + phrase_len else proto.UID_MODE_PHRASE)
    return channel, mode, uid, phrase


def set_phrase(ser: serial.Serial, channel: int, phrase: str,
               timeout: float) -> int:
    phrase_bytes = phrase.encode("utf-8")
    if len(phrase_bytes) > proto.CFG_MAX_PHRASE:
        print("phrase is too long (max 64 bytes)", file=sys.stderr)
        return 2
    payload = bytes([channel, len(phrase_bytes)]) + phrase_bytes
    response = send_command(ser, proto.CFG_SET_CONFIG, payload, timeout)
    if response and response[0] == proto.CFG_OK:
        print(f"set phrase: ok, channel {response[1]}, uid {proto.uid_to_string(response[2:8])}")
        return 0
    print("set phrase:", STATUS_NAMES.get(response[0] if response else -1, "error"))
    return 1


def set_uid(ser: serial.Serial, channel: int, uid: bytes,
            timeout: float) -> int:
    uid = proto.normalize_uid(uid)
    if proto.uid_is_zero(uid):
        print("UID must not be all zero", file=sys.stderr)
        return 2
    payload = bytes([channel]) + uid
    response = send_command(ser, proto.CFG_SET_UID, payload, timeout)
    if response and response[0] == proto.CFG_OK:
        print(f"set UID: ok, channel {response[1]}, uid {proto.uid_to_string(response[2:8])}")
        return 0
    print("set UID:", STATUS_NAMES.get(response[0] if response else -1, "error"))
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Configure ELRS ESP-NOW dongle/sim")
    ap.add_argument("--port", required=True, help="serial port (ttyACM*, ttyUSB*, COM*)")
    ap.add_argument("--baud", type=int, default=115200)
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--phrase", default=None,
                       help="binding phrase; defaults to $ELRS_BINDING_PHRASE")
    group.add_argument("--uid", default=None,
                       help="manual UID: 12 hex digits or AA:BB:CC:DD:EE:FF")
    ap.add_argument("--channel", type=int, default=None, choices=range(1, 14))
    ap.add_argument("--show", action="store_true", help="read config")
    ap.add_argument("--stats", action="store_true", help="read counters")
    ap.add_argument("--ping", action="store_true")
    ap.add_argument("--reset", action="store_true", help="clear NVS (unbound)")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    uid = None
    if args.uid is not None:
        try:
            uid = proto.normalize_uid(proto.parse_uid(args.uid))
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 2
        if proto.uid_is_zero(uid):
            print("UID must not be all zero", file=sys.stderr)
            return 2

    phrase = args.phrase
    if phrase is None and uid is None:
        phrase = os.environ.get("ELRS_BINDING_PHRASE")
    do_set = uid is not None or phrase is not None or args.channel is not None

    try:
        ser = open_port(args.port, args.baud)
    except Exception as exc:
        print(f"cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    try:
        if args.ping:
            response = send_command(ser, proto.CFG_PING, timeout=args.timeout)
            print("ping:", STATUS_NAMES.get(response[0], hex(response[0])))
        if args.reset:
            response = send_command(ser, proto.CFG_RESET, timeout=args.timeout)
            if response and response[0] == proto.CFG_OK:
                print("reset: ok")
                if len(response) >= 8:
                    print("uid:", proto.uid_to_string(response[2:8]))
            else:
                print("reset:", STATUS_NAMES.get(response[0] if response else -1, "error"))
                return 1
        if do_set:
            try:
                old_channel, old_mode, old_uid, old_phrase = current_config(ser, args.timeout)
            except Exception:
                old_channel, old_mode, old_uid, old_phrase = 1, proto.UID_MODE_PHRASE, bytes(6), ""

            channel = args.channel if args.channel is not None else old_channel
            if uid is not None:
                rc = set_uid(ser, channel, uid, args.timeout)
            elif phrase is not None:
                rc = set_phrase(ser, channel, phrase, args.timeout)
            elif old_mode == proto.UID_MODE_MANUAL:
                rc = set_uid(ser, channel, old_uid, args.timeout)
            elif old_phrase:
                rc = set_phrase(ser, channel, old_phrase, args.timeout)
            else:
                print("no UID configured; pass --phrase or --uid", file=sys.stderr)
                return 2
            if rc != 0:
                return rc
        if args.stats:
            response = send_command(ser, proto.CFG_GET_STATS, timeout=args.timeout)
            return decode_stats(response)
        if args.show or (not do_set and not args.ping and not args.reset and not args.stats):
            response = send_command(ser, proto.CFG_GET_CONFIG, timeout=args.timeout)
            return decode_config(response)
        return 0
    except TimeoutError as exc:
        print(exc, file=sys.stderr)
        return 1
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
