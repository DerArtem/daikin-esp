"""Offline golden-frame tests for the observed EKHBH016AB6WN P1/P2 layout.

These tests exercise the known byte offsets / checksum using real captures;
they do not replace an ESPHome firmware build or on-device verification.
Run: python -m unittest discover -s tests -p 'test_*.py'
"""

import unittest


def hbs_crc(data):
    """Reflected CRC-8 (poly 0xD9) used by the captured old P1/P2 frames."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xD9 if crc & 1 else 0)
    return crc


def valid_frame(hex_frame, expected_prefix, expected_len):
    data = bytes.fromhex(hex_frame)
    assert len(data) == expected_len
    assert data.startswith(bytes.fromhex(expected_prefix))
    assert hbs_crc(data[:-1]) == data[-1]
    return data


def s8_8_little_endian(data):
    return int.from_bytes(data, "little", signed=True) / 256.0


class EKHBHProfileFixturesTest(unittest.TestCase):
    def test_heating_setpoint_40_and_45(self):
        request_40 = valid_frame(
            "00 00 10 01 01 00 28 00 07 00 2d 00 00 00 00 00 00 46 0a 54",
            "00 00 10", 20,
        )
        request_45 = valid_frame(
            "00 00 10 01 01 00 2d 00 07 00 2d 00 00 00 00 00 00 46 0a 61",
            "00 00 10", 20,
        )
        response_45 = valid_frame(
            "40 00 10 01 00 01 00 2d 00 07 00 01 00 00 00 00 00 70 05 00 1f 01 a3 59",
            "40 00 10", 24,
        )
        self.assertEqual((request_40[6], request_40[7]), (40, 0))
        self.assertEqual((request_45[6], request_45[7]), (45, 0))
        self.assertEqual((response_45[7], response_45[8]), (45, 0))

    def test_dhw_flow_and_pump_status(self):
        pump_on = valid_frame(
            "40 00 11 07 08 c4 23 7c 25 4f", "40 00 11", 10,
        )
        both_off = valid_frame(
            "40 00 11 07 00 c4 23 7c 25 eb", "40 00 11", 10,
        )
        pump_and_compressor_on = valid_frame(
            "40 00 11 07 09 c4 23 fc 24 3a", "40 00 11", 10,
        )
        self.assertAlmostEqual(s8_8_little_endian(pump_on[5:7]), 35.765625)
        self.assertAlmostEqual(s8_8_little_endian(pump_on[7:9]), 37.484375)
        self.assertTrue(pump_on[4] & 0x08)
        self.assertFalse(pump_on[4] & 0x01)
        self.assertFalse(both_off[4] & 0x08)
        self.assertFalse(both_off[4] & 0x01)
        self.assertTrue(pump_and_compressor_on[4] & 0x08)
        self.assertTrue(pump_and_compressor_on[4] & 0x01)

    def test_outdoor_temperature_candidate(self):
        response = valid_frame(
            "40 00 10 01 00 01 00 2d 00 07 00 01 00 00 00 00 00 32 05 00 1f 01 a3 f4",
            "40 00 10", 24,
        )
        self.assertAlmostEqual(s8_8_little_endian(response[17:19]), 5.1953125)


if __name__ == "__main__":
    unittest.main()
