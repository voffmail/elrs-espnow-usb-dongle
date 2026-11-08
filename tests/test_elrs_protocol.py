import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import elrs_protocol as proto


class TestCrcAndMsp(unittest.TestCase):
    def test_crc8_sample(self):
        self.assertEqual(proto.crc8(bytes.fromhex("070000")), 0x23)

    def test_msp_v2_matches_capture(self):
        crsf = proto.crsf_build(0xC8, 0x07, b"\x00\x00")
        self.assertEqual(crsf.hex(), "c80407000023")
        msp = proto.build_msp_v2(crsf)
        self.assertEqual(msp.hex(), "24583c0011000600c804070000237d")
        self.assertEqual(proto.parse_msp_v2(msp), (0, 0x0011, crsf))

    def test_msp_bad_crc(self):
        msp = bytearray(proto.build_msp_v2(proto.crsf_build(0xC8, 0x07, b"\x00\x00")))
        msp[-1] ^= 0xFF
        self.assertIsNone(proto.parse_msp_v2(bytes(msp)))


class TestCrsfScanner(unittest.TestCase):
    def test_finds_frame_inside_msp_wrapper(self):
        crsf = proto.crsf_build(0xC8, 0x08, bytes(8))
        msp = proto.build_msp_v2(crsf)
        scanner = proto.CrsfScanner()
        frames = scanner.feed(b"\x00\x11boot" + msp)
        self.assertEqual(frames, [crsf])
        self.assertEqual(scanner.frames, 1)
        self.assertEqual(scanner.crc_errors, 0)

    def test_frames_split_across_reads(self):
        crsf = proto.crsf_build(0xC8, 0x14, bytes(range(10)))
        scanner = proto.CrsfScanner()
        self.assertEqual(scanner.feed(crsf[:3]), [])
        self.assertEqual(scanner.feed(crsf[3:]), [crsf])

    def test_bad_crc_is_counted(self):
        crsf = bytearray(proto.crsf_build(0xC8, 0x07, b"\x00\x00"))
        crsf[-1] ^= 0xFF
        scanner = proto.CrsfScanner()
        self.assertEqual(scanner.feed(bytes(crsf)), [])
        self.assertEqual(scanner.crc_errors, 1)


class TestConfigProtocol(unittest.TestCase):
    def test_roundtrip(self):
        payload = bytes([1, 3]) + b"abc"
        frame = proto.build_cfg_frame(proto.CFG_SET_CONFIG, payload)
        self.assertEqual(proto.CfgParser().feed(frame), [(proto.CFG_SET_CONFIG, payload)])

    def test_split_and_noise(self):
        frame = proto.build_cfg_frame(proto.CFG_GET_CONFIG)
        parser = proto.CfgParser()
        self.assertEqual(parser.feed(b"noise" + frame[:4]), [])
        self.assertEqual(parser.feed(frame[4:]), [(proto.CFG_GET_CONFIG, b"")])

    def test_bad_crc(self):
        frame = bytearray(proto.build_cfg_frame(proto.CFG_PING))
        frame[-1] ^= 0xFF
        self.assertEqual(proto.CfgParser().feed(bytes(frame)), [])


class TestUidTools(unittest.TestCase):
    def test_parse_uid_formats(self):
        expected = bytes.fromhex("AABBCCDDEEFF")
        self.assertEqual(proto.parse_uid("AA:BB:CC:DD:EE:FF"), expected)
        self.assertEqual(proto.parse_uid("aabbccddeeff"), expected)
        self.assertEqual(proto.parse_uid("AA-BB-CC-DD-EE-FF"), expected)

    def test_parse_uid_rejects_bad_length(self):
        with self.assertRaises(ValueError):
            proto.parse_uid("AA:BB:CC")
        with self.assertRaises(ValueError):
            proto.parse_uid("GG:BB:CC:DD:EE:FF")

    def test_normalize_clears_multicast_bit(self):
        uid = proto.normalize_uid(bytes.fromhex("AB0000000000"))
        self.assertEqual(uid[0], 0xAA)
        self.assertEqual(proto.uid_is_zero(bytes(6)), True)
        self.assertEqual(proto.uid_is_zero(uid), False)

    def test_set_uid_frame_roundtrip(self):
        uid = bytes.fromhex("AABBCCDDEEFF")
        payload = bytes([1]) + uid
        frame = proto.build_cfg_frame(proto.CFG_SET_UID, payload)
        self.assertEqual(proto.CfgParser().feed(frame),
                         [(proto.CFG_SET_UID, payload)])


class TestUid(unittest.TestCase):
    def test_uid_matches_md5_rule(self):
        phrase = "test-phrase"
        digest = hashlib.md5(
            ('-DMY_BINDING_PHRASE="%s"' % phrase).encode()
        ).digest()
        expected = bytearray(digest[:6])
        expected[0] &= ~0x01
        self.assertEqual(proto.uid_from_phrase(phrase), bytes(expected))


if __name__ == "__main__":
    unittest.main()
