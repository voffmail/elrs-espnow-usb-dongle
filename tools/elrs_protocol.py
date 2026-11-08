"""Shared ELRS helpers: UID, MSP v2, CRSF and the dongle config protocol.

No device-specific code here; used by PC tools and host tests.
"""

from __future__ import annotations

import hashlib
import struct

MSP_MAGIC = b"$X<"
MSP_FUNCTION_CRSF_TLM = 0x0011
MSP_MAX_PAYLOAD = 248

CFG_MAGIC = b"\xAA\x55"
CFG_VERSION = 1
CFG_MAX_PAYLOAD = 128
CFG_MAX_PHRASE = 64

CFG_GET_CONFIG = 0x01
CFG_SET_CONFIG = 0x02
CFG_GET_STATS = 0x03
CFG_RESET = 0x04
CFG_PING = 0x05
CFG_SET_UID = 0x06
CFG_RESPONSE = 0x80

UID_MODE_PHRASE = 0
UID_MODE_MANUAL = 1

CFG_OK = 0
CFG_ERR_BAD_REQUEST = 1
CFG_ERR_TOO_LONG = 2
CFG_ERR_INVALID = 3
CFG_ERR_BUSY = 4

CRSF_ADDRESSES = (0xC8, 0xEA, 0xEE)


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def uid_from_phrase(phrase: str) -> bytes:
    payload = ('-DMY_BINDING_PHRASE="%s"' % phrase).encode("utf-8")
    digest = hashlib.md5(payload).digest()
    uid = bytearray(digest[:6])
    uid[0] &= ~0x01
    return bytes(uid)


def uid_to_string(uid: bytes) -> str:
    return ":".join(f"{b:02X}" for b in uid)


def parse_uid(text: str) -> bytes:
    cleaned = text.strip().replace(":", "").replace("-", "").replace(" ", "")
    if len(cleaned) != 12 or any(c not in "0123456789abcdefABCDEF" for c in cleaned):
        raise ValueError("UID must be exactly 6 bytes (12 hex digits)")
    return bytes.fromhex(cleaned)


def normalize_uid(uid: bytes) -> bytes:
    if len(uid) != 6:
        raise ValueError("UID must be exactly 6 bytes")
    out = bytearray(uid)
    out[0] &= 0xFE  # ELRS group address is unicast: clear the multicast bit
    return bytes(out)


def uid_is_zero(uid: bytes) -> bool:
    return not any(uid)


def build_msp_v2(payload: bytes, function: int = MSP_FUNCTION_CRSF_TLM,
                 flag: int = 0) -> bytes:
    if len(payload) > MSP_MAX_PAYLOAD:
        raise ValueError("MSP payload too long")
    size = len(payload)
    body = bytes([flag]) + struct.pack("<HH", function, size) + payload
    return MSP_MAGIC + body + bytes([crc8(body)])


def parse_msp_v2(data: bytes):
    """Returns (flag, function, payload) or None."""
    if len(data) < 9 or data[:3] != MSP_MAGIC:
        return None
    flag = data[3]
    function, size = struct.unpack("<HH", data[4:8])
    if size > MSP_MAX_PAYLOAD or len(data) < 9 + size:
        return None
    payload = data[8:8 + size]
    if crc8(data[3:8 + size]) != data[8 + size]:
        return None
    return flag, function, payload


def crsf_build(address: int, frame_type: int, payload: bytes) -> bytes:
    if len(payload) > 60:
        raise ValueError("CRSF payload too long")
    body = bytes([frame_type]) + payload
    length = len(body) + 1  # type + payload + CRC
    return bytes([address, length, frame_type]) + payload + bytes([crc8(body)])


class CrsfScanner:
    """Incremental CRSF frame extractor tolerant to wrappers and boot noise."""

    def __init__(self, max_buffer: int = 8192):
        self.buf = bytearray()
        self.max_buffer = max_buffer
        self.frames = 0
        self.crc_errors = 0
        self.bytes = 0
        self.last_type = -1
        self.last_len = 0

    def feed(self, data: bytes) -> list[bytes]:
        self.bytes += len(data)
        self.buf.extend(data)
        out: list[bytes] = []
        i = 0
        while i + 1 < len(self.buf):
            address = self.buf[i]
            if address in CRSF_ADDRESSES:
                length = self.buf[i + 1]
                if 2 <= length <= 62:
                    total = length + 2
                    if i + total > len(self.buf):
                        # Incomplete frame at the end: keep it for the next read.
                        break
                    crc_index = i + total - 1
                    if crc8(self.buf[i + 2:crc_index]) == self.buf[crc_index]:
                        frame = bytes(self.buf[i:i + total])
                        out.append(frame)
                        self.frames += 1
                        self.last_type = frame[2]
                        self.last_len = length
                        i += total
                        continue
                    self.crc_errors += 1
            i += 1
        if i:
            del self.buf[:i]
        if len(self.buf) > self.max_buffer:
            del self.buf[:-self.max_buffer]
        return out


class CfgParser:
    """Incremental parser for AA 55 | ver | type | len | payload | crc16."""

    def __init__(self):
        self.buf = bytearray()

    def reset(self):
        self.buf.clear()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self.buf.extend(data)
        out: list[tuple[int, bytes]] = []
        while True:
            idx = self.buf.find(CFG_MAGIC)
            if idx < 0:
                if self.buf and self.buf[-1] == 0xAA:
                    self.buf[:] = b"\xAA"
                else:
                    self.buf.clear()
                break
            if idx:
                del self.buf[:idx]
            if len(self.buf) < 6:
                break
            if self.buf[2] != CFG_VERSION:
                del self.buf[0]
                continue
            size = self.buf[4] | (self.buf[5] << 8)
            if size > CFG_MAX_PAYLOAD:
                del self.buf[0]
                continue
            total = 6 + size + 2
            if len(self.buf) < total:
                break
            expected = self.buf[6 + size] | (self.buf[7 + size] << 8)
            actual = crc16(bytes(self.buf[2:6 + size]))
            if expected != actual:
                del self.buf[0]
                continue
            out.append((self.buf[3], bytes(self.buf[6:6 + size])))
            del self.buf[:total]
        return out


def build_cfg_frame(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > CFG_MAX_PAYLOAD:
        raise ValueError("config payload too long")
    body = bytes([CFG_VERSION, msg_type]) + struct.pack("<H", len(payload)) + payload
    return CFG_MAGIC + body + struct.pack("<H", crc16(body))


def parse_cfg_response(payload: bytes):
    """Decodes the common status prefix of a config response."""
    if not payload:
        return None
    return payload[0]
