# Read-only P1/P2 capture (Daikin Altherma experiment)

This branch adds a fail-closed **read-only** mode to the existing ESPHome/ESP32-C6 driver. It is intended to capture and analyze frames from an older Daikin Altherma (EKHBH016AB6WN) **without sending** an auxiliary-controller handshake or any control packet.

## Hardware / safety

* MAX22088 DOUT -> ESP32-C6 GPIO4; DIN <- GPIO5; MAX22088 RST (pin 3) <- GPIO8.
* MAX22088 **RST must be HIGH** to inhibit the bus transmitter. In `sniffer_only: true` the firmware drives GPIO8 HIGH immediately during the Homebus setup, skips creation/enabling of the RMT TX channel and rejects calls to `write_bytes`. The climate callback is not registered.
* Keep the existing **10 kOhm hardware pull-up from GPIO8 to 3.3 V**: it keeps RST HIGH during reset, boot, and before ESPHome setup. Verify HIGH at MAX22088 pin 3 with a multimeter before connecting P1/P2. A firmware setting cannot guarantee safety before boot.
* Avoid connecting a grounded PC/USB cable simultaneously to the non-isolated P1/P2 circuit. Power supply / bus capacity and polarity should be verified against the board design and the equipment manual.
* A capture program is not proof that the PCB is electrically safe or that frames will decode correctly. Do not issue Home Assistant climate commands while monitoring.

## Install / capture

On Windows PowerShell, from your local checkout of **DerArtem/daikin-esp**:

```powershell
git fetch origin
git switch feature/altherma-read-only-sniffer
cd esphome
$env:ESPHOME_ESP_IDF_PREFIX = "C:\idf"
esphome config esp-daikin-ctrl.yaml
esphome run esp-daikin-ctrl.yaml --device 192.168.10.138
```

The example YAML uses `sniffer_only: true`, `rx_pin: 4`, `tx_pin: 5`, `enable_pin: 8`, ESP-IDF, and `logger.level: DEBUG`.

To save logs for offline analysis (replace IP if needed):

```powershell
esphome logs esp-daikin-ctrl.yaml --device 192.168.10.138 2>&1 | Tee-Object -FilePath p1p2-capture.txt
```

Each decoded frame produces two `p1p2_sniffer` INFO lines:

```text
[I][p1p2_sniffer]: seq=1 ms=123456 len=... dir=0x00 addr=0x00 type=0x10
[I][p1p2_sniffer]: len=... pkt=[00 00 10 ...]
```

`seq` and `ms` are local reception-processing order and ESP32 uptime milliseconds, respectively; logs also contain ESPHome timestamps. `pkt` includes raw bytes (including a trailing checksum byte if present) and has **not** been interpreted as Altherma measurements. Frame decoding uses the upstream RMT sampler; invalid/framing/parity failures are logged by `homebus` where detectable. High traffic may overrun the two-buffer receiver, so watch for `RX buffer overflow` and gaps.

First record an idle baseline, then alter **one parameter at a time on the original Daikin controller**, noting the exact event time and change (e.g., setpoint up by 1 °C). Restore each parameter after the test. Compare packet sequences and payload changes before assigning any meaning to values. No on-bus writing is enabled in this branch's YAML.

## Limitations

* This change **does not** implement an Altherma-specific decoder or climate controls. The `Ducted AC` entities are retained for compatibility but will not show valid Altherma readings in read-only mode.
* No CRC/checksum validation is applied by the sniffer; frames are recorded for analysis even if their checksums would differ from Greg Davill's Ducted-A/C algorithm.
* ESPHome API log streaming may drop frames under heavy traffic. Inspect `homebus` errors and consider a local serial capture via an isolated test setup if needed.
* This branch has not been hardware-validated on an EKHBH016AB6WN. Build and runtime behavior must be checked before attaching the heat pump.
