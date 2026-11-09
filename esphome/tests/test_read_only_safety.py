"""Static read-only safety regression checks for the ESP32-C6 MAX22088 build.

These deliberately complement, rather than replace, actual firmware/bench tests.
"""

from pathlib import Path
import unittest


BASE = Path(__file__).resolve().parents[1]
DRIVER = (BASE / "components/daikin_ducted/homebus_rmt.cpp").read_text(
    encoding="utf-8"
)
CLIMATE = (BASE / "components/daikin_ducted/daikin_ducted.cpp").read_text(
    encoding="utf-8"
)
CONFIG = (BASE / "esp-daikin-ctrl.yaml").read_text(encoding="utf-8")


class ReadOnlySafetyTest(unittest.TestCase):
    def test_production_example_stays_passive(self):
        self.assertIn("sniffer_only: true", CONFIG)
        self.assertIn("rx_pin: 4", CONFIG)
        self.assertIn("tx_pin: 5", CONFIG)
        self.assertIn("enable_pin: 8", CONFIG)
        self.assertIn("variant: esp32c6", CONFIG)

    def test_rst_is_held_high_in_sniffer_mode(self):
        setup = DRIVER.split("void HomebusRMT::setup()", 1)[1].split(
            "void HomebusRMT::write_bytes", 1
        )[0]
        self.assertIn("gpio_set_level(enable_gpio, 1);", setup)
        self.assertIn("if (!this->sniffer_only_) {", setup)
        self.assertIn("rmt_new_tx_channel", setup)
        self.assertIn("rmt_tx_register_event_callbacks", setup)
        self.assertIn("READ-ONLY SNIFFER: MAX22088 RST held HIGH", setup)

    def test_write_entrypoint_refuses_sniffer_and_limits_size(self):
        write = DRIVER.split("void HomebusRMT::write_bytes", 1)[1].split(
            "void HomebusRMT::dump_config", 1
        )[0]
        self.assertLess(write.index("if (this->sniffer_only_)"),
                        write.index("rmt_symbol_word_t *tx_symbols"))
        self.assertIn("tx_data_size > MAX_TX_BYTES", write)
        self.assertIn("this->tx_pending_ || this->tx_in_progress_", write)
        self.assertNotIn("rmt_tx_wait_all_done", write)

    def test_async_tx_is_gated(self):
        loop = DRIVER.split("void HomebusRMT::loop()", 1)[1]
        self.assertLess(loop.index("if (!this->sniffer_only_)"),
                        loop.index("rmt_transmit("))
        self.assertIn("TX_GUARD_DELAY_MS", loop)

    def test_altherma_callback_never_sends(self):
        callback = CLIMATE.split(
            "void DaikinClimate::altherma_sniffer_callback", 1
        )[1].split("// DaikinClimate Implementation", 1)[0]
        self.assertIn("calculate_crc(buffer, buffer_length - 1)", callback)
        self.assertIn("buffer_length == 20", callback)
        self.assertIn("buffer_length == 24", callback)
        self.assertIn("buffer_length == 10", callback)
        for forbidden in ("write_bytes(", "send_simple_response(", "rmt_transmit("):
            self.assertNotIn(forbidden, callback)


if __name__ == "__main__":
    unittest.main()
