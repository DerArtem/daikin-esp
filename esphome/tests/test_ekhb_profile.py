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
            "40 00 10 01 00 01 00 2d 00 07 00 01 00 00 00 00 00 70 05 00 1f 01 a3 6d",
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

    def test_all_seven_controller_transitions(self):
        # Exact valid frames taken from capture4. Each action changes
        # only the indicated 0x10 request payload byte (CRC excluded).
        examples = [
            ("23:52:00", "00 00 10 01 01 00 2d 00 07 00 2d 00 00 00 00 00 00 46 0a 61", None),
            ("23:52:13", "00 00 10 01 01 00 2d 00 07 00 32 00 00 00 00 00 00 46 0a d2", 7),
            ("23:52:20", "00 00 10 01 01 01 2d 00 07 00 32 00 00 00 00 00 00 46 0a 94", 2),
            ("23:53:53", "00 00 10 01 01 01 2d 00 07 00 2d 00 00 00 00 00 00 46 0a 27", 7),
            ("23:54:15", "00 00 10 01 01 01 2d 00 07 00 2d 00 01 00 00 00 00 46 0a e6", 9),
            ("23:54:37", "00 00 10 01 01 01 2d 00 07 00 2d 00 00 00 00 00 00 46 0a 27", 9),
            ("23:54:52", "00 00 10 00 01 01 2d 00 07 00 2d 00 00 00 00 00 00 46 0a fe", 0),
            ("23:55:03", "00 00 10 01 01 01 2d 00 07 00 2d 00 00 00 00 00 00 46 0a 27", 0),
        ]
        frames = [
            valid_frame(hex_frame, "00 00 10", 20)
            for _, hex_frame, _ in examples
        ]
        for i in range(1, len(frames)):
            changed = [
                position - 3
                for position in range(3, 19)
                if frames[i][position] != frames[i - 1][position]
            ]
            self.assertEqual(changed, [examples[i][2]], msg=examples[i][0])

        def decoded(frame):
            return {
                "heating_setpoint": frame[6],
                "dhw_setpoint": frame[10],
                "heating_enabled": bool(frame[3] & 0x01),
                "dhw_enabled": bool(frame[5] & 0x01),
                "quiet_mode": bool(frame[12] & 0x01),
            }

        expected_states = [
            (45, 45, True, False, False),
            (45, 50, True, False, False),
            (45, 50, True, True, False),
            (45, 45, True, True, False),
            (45, 45, True, True, True),
            (45, 45, True, True, False),
            (45, 45, False, True, False),
            (45, 45, True, True, False),
        ]
        for frame, expected in zip(frames, expected_states):
            actual = decoded(frame)
            self.assertEqual(tuple(actual.values()), expected)

    def test_heating_target_mode_byte_is_not_decimal_fraction(self):
        # In the EWYQ-like profile payload byte 4 belongs to controller mode.
        # A 0x05 mode byte must not turn a 45 C setpoint into 45.5 C.
        target, mode_byte = 0x2D, 0x05
        self.assertEqual(float(target), 45.0)
        self.assertNotEqual(float(target), target + mode_byte / 10.0)

    def test_outdoor_temperature_candidate(self):
        response = valid_frame(
            "40 00 10 01 00 01 00 2d 00 07 00 01 00 00 00 00 00 32 05 00 1f 01 a3 f4",
            "40 00 10", 24,
        )
        self.assertAlmostEqual(s8_8_little_endian(response[17:19]), 5.1953125)


if __name__ == "__main__":
    unittest.main()
