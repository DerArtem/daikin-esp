# Read-only EKHBH / old EWYQ P1/P2 monitor

This branch implements **passive P1/P2 reception and a first ESPHome sensor decoder** for an old Daikin EKHBH016AB6WN. Its frame structure matches the separately documented EWYQ005ADVP P1/P2 profile. A non-blocking **TX transport is included for future lab experiments**, but is NOT available in `sniffer_only: true` mode and there is no EKHBH-specific safe write/control implementation.

## Electrical safety

- MAX22088 DOUT -> ESP32-C6 GPIO4; DIN <- GPIO5; RST (MAX22088 pin 3) <- GPIO8.
- **RST HIGH disables the MAX22088 bus transmitter.** With `sniffer_only: true` the firmware drives GPIO8 HIGH, never initializes the RMT TX channel, blocks `write_bytes`, and uses a dedicated receive-only packet callback.
- Retain the 10 kOhm **hardware pull-up** on GPIO8 to 3.3 V. Confirm HIGH at MAX22088 pin 3 across boot/reset before connecting the bus; firmware cannot protect the pin before initialization.
- This circuit is **not galvanically isolated** from the heat pump bus. Do not attach a grounded USB-connected PC at the same time. Work on wiring with the system safely powered off.
- Keep the original Daikin controller connected in parallel. Do not change `sniffer_only: true` while investigating this older system.

## ESPHome/Home Assistant entities

`esphome/esp-daikin-ctrl.yaml` demonstrates the following optional entities, exposed via the normal ESPHome API:

| YAML key inside `climate: - platform: daikin_ducted` | Displayed meaning | Packet | Confidence |
| --- | --- | --- | --- |
| `altherma_heating_setpoint` | Heating water target | `00 00 10` / `40 00 10` | Verified by 40 -> 45 C setpoint change |
| `altherma_dhw_setpoint` | DHW target | `00 00 10`, payload byte 7 | Confirmed by 45 -> 50 -> 45 C |
| `altherma_heating_enabled` | Space heating allowed by controller | `00 00 10`, payload byte 0 bit 0 | Confirmed by off -> on test |
| `altherma_dhw_enabled` | DHW heating allowed by controller | `00 00 10`, payload byte 2 bit 0 | Confirmed by DHW on test |
| `altherma_quiet_mode` | Quiet mode enabled by controller | `00 00 10`, payload byte 9 bit 0 | Confirmed by on -> off test |
| `altherma_flow_temperature` | Leaving water / flow temperature | `40 00 11`, bytes 7-8 | Compared with Daikin display (37.5 C) |
| `altherma_dhw_temperature` | Domestic hot water temperature | `40 00 11`, bytes 5-6 | Compared with Daikin display (35.7 C) |
| `altherma_outdoor_temperature` | Outside temperature | `40 00 10`, bytes 17-18 | Experimental: 5.195 C vs display ~5.2 C |
| `altherma_circulation_pump` | Water circulation pump running | `40 00 11`, byte 4 bit 3 | Correlated with pump off/on at 23:09:56 / 23:10:58 |
| `altherma_compressor` | Compressor running | `40 00 11`, byte 4 bit 0 | Correlated with compressor start ~23:10:58 |
| `altherma_dhw_electric_heater` | Electric DHW heater active | `40 00 11`, byte 4 bit 4 (0x10) | Confirmed by capture5 user observation and 0x19 -> 0x18 when compressor stopped |
| `altherma_heating_control_mode_raw` | Heating control/regulation byte (raw) | `00 00 10`, payload byte 4 | 0 -> 2 at weather-compensation enable; then 2 -> 1 -> 2. Meanings of 1/2 unknown |
| `altherma_status_flags_raw` | Full raw 0x11 status byte (decimal 0–255) | `40 00 11`, byte 4 | Useful for future tests; do NOT assign unknown bits to backup heater stages |
| `altherma_cooling_setpoint` | Cooling water target (optional) | `00 00 10` / `40 00 10` | Not independently checked; disabled in example |

The `Daikin Ducted` climate entity is now `internal: true` in the example: it is an implementation container only, **not a usable thermostat**. Old Ducted/HomeKit entities and their unrelated temperature data have been removed from the example YAML.

### Frame details

- **Accepted only after matching CRC:** reflected CRC-8, polynomial `0xD9`, initial value 0, as already used by the driver.
- **Strict lengths:** `00 00 10` must be 20 bytes; `40 00 10` 24 bytes; `40 00 11` 10 bytes. Other traffic is still available in the raw log, but is not decoded.
- **Temperature encoding:** flow, DHW, outdoor candidate: signed *little-endian* 16-bit / 256. Heating and DHW targets: whole-degree byte, **not** tenths in the next byte (the byte after heating target is a distinct controller mode field). CRC occupies the final byte and is not part of the payload.
- **Binary status:** `0x08` = pump ON, compressor OFF; `0x00` = both OFF; `0x09` = pump + compressor ON with electric DHW heater OFF. `0x19`/`0x18` = electric DHW heater ON, with compressor ON/OFF respectively.
- Numeric entities publish on first valid update or meaningful change (at least 0.02 degrees C), rather than on every ~0.3 s frame. Binary sensors publish only on state changes.

### Seven confirmed control-panel actions (November 9, 2026)

The latest capture (`p1p2-capture4.txt`) contains 3,705 CRC-valid frames over 23:52:00–23:55:14 with consecutive sequence numbers. All seven deliberate controller actions appear as isolated changes in the `00 00 10` request:

| Time | Controller action | Payload offset, old -> new |
| --- | --- | --- |
| 23:52:13.192 | DHW target 45 -> 50 C | 7, `2D -> 32` |
| 23:52:20.447 | DHW heating enabled | 2, `00 -> 01` |
| 23:53:53.768 | DHW target 50 -> 45 C | 7, `32 -> 2D` |
| 23:54:15.842 | Quiet mode enabled | 9, `00 -> 01` |
| 23:54:37.081 | Quiet mode disabled | 9, `01 -> 00` |
| 23:54:52.803 | Space heating disabled | 0, `01 -> 00` |
| 23:55:03.724 | Space heating enabled | 0, `00 -> 01` |

These flags indicate **controller settings / permission**, not necessarily actual pump, compressor, space-heating, or DHW activity. In the observed capture the compressor continued running while space heating was temporarily disabled, with DHW still permitted.

### Capture 5: electric DHW heater and weather compensation (November 10, 2026)

In `p1p2-capture5.txt` (01:03:19–01:04:33), all 1,414 decoded frames
had valid CRC and their `seq` field showed no gaps. The user confirms
the **electric domestic-hot-water heater was ON from before the recording
until after the recording ended**, and that the compressor really stopped.

- Older recordings had status `0x09` with electric DHW heating OFF.
- All 265 `40 00 11` replies in capture5 had `0x10` status bit set:
  189 with `0x19` (pump + compressor + electric DHW heater)
  followed by 76 with `0x18` (pump + electric DHW heater, compressor OFF).
- `0x19 XOR 0x18 = 0x01`: the compressor bit changed independently of
  the electric heater bit. This supports **bit 4** as an electric DHW
  heater activity indicator on this specific device.
- Weather compensation was enabled at 01:04:07.234. The original
  controller's `00 00 10` request then changed the heating target
  `0x2D -> 0x24` (45 -> 36 C) and payload byte 4 `0x00 -> 0x02`.
  The same two values appear in the following `40 00 10` response.
- Without another reported UI action, payload byte 4 changed
  `0x02 -> 0x01` at 01:04:24.584 and `0x01 -> 0x02`
  at 01:04:32.735. Thus `1` and `2` are **not** labeled simply
  as weather-compensation OFF/ON. They remain raw values in HA.
- The main space-heating backup heater **stage 1** and **stage 2** were
  never toggled or observed. No stage-specific status bits are claimed
  or exposed. `altherma_status_flags_raw` is an optional diagnostic
  foundation for later experiments.

`sniffer_only: true` remains mandatory. All three new entities only
publish observed, CRC-validated data; none sends commands.

### Validation captures

Known examples from the user's November 9, 2026 recordings:

```text
# 40 C target (0x28), request
00 00 10 01 01 00 28 00 07 00 2D 00 00 00 00 00 00 46 0A 54
# 45 C target (0x2D), request
00 00 10 01 01 00 2D 00 07 00 2D 00 00 00 00 00 00 46 0A 61

# Pump on, compressor off; DHW 35.765625 C, flow 37.484375 C
40 00 11 07 08 C4 23 7C 25 4F
# Both off
40 00 11 07 00 C4 23 7C 25 EB
# Pump and compressor on
40 00 11 07 09 C4 23 FC 24 3A
```

Offline frame-layout fixtures: `python -m unittest discover -s tests -p 'test_*.py'` from the `esphome` directory.

## Upstream `dev` backports (November 2026)

To keep individual sources auditable, each port was committed separately to
`feature/altherma-read-only-sniffer` with the original upstream SHA in
`Upstream-commit:` and an upstream URL. These are **manual, conflict-resolved
backports**, not byte-identical Git `cherry-pick` operations: our component has
additional read-only logic absent upstream.

| Original `gregdavill/daikin-esp` commit | Applied change |
| --- | --- |
| [`36adade`](https://github.com/gregdavill/daikin-esp/commit/36adade797a1a71cade3ae12720654fd5a1566ef) | RMT final-symbol RX boundary fix (other bounds checks were already present) |
| [`f3caa05`](https://github.com/gregdavill/daikin-esp/commit/f3caa05072e6f4f8765c878771c3cf3ebc8fe59f) | Limit device-name logging to frame length |
| [`3222711`](https://github.com/gregdavill/daikin-esp/commit/32227111bd3b0ebe7af74d43cb32f6a0cb04ac81) | Size guard for optional active-mode response |
| [`95a542d`](https://github.com/gregdavill/daikin-esp/commit/95a542dde0375f1222308089277153ce0c13176d) | GitHub Actions checkout v6 |
| [`3a78a9d`](https://github.com/gregdavill/daikin-esp/commit/3a78a9d70f33599e44b12be41c71bc761c8702d4) | Async RMT TX queue, completion callback and 10ms guard delay |
| [`9ae98fb`](https://github.com/gregdavill/daikin-esp/commit/9ae98fb215f1c5a7a57207b0aabbe4808ae27f8b) | GitHub Actions ESPHome 2026.10.2 |
| [`0d9ddc8`](https://github.com/gregdavill/daikin-esp/commit/0d9ddc8db5e780f3d8c9508276dd0598b4d872cf) | Correct printf format specifiers in RMT diagnostics |
| [`062d9c4`](https://github.com/gregdavill/daikin-esp/commit/062d9c497f980d107a42d1d79a4290d70fd1df7a) | `flash_mode: dio` and `flash_chip: generic`, adapted to ESP32-C6 |
| [`107f588`](https://github.com/gregdavill/daikin-esp/commit/107f58835823574b6a002bfebb0fba7b1eca6c49) | Bounded TX wait applied to remaining active-mode idle setup |

Additional local hardening: TX frame size/channel validation, reject active writes while
queued/in progress, suppress TX queue processing in read-only mode, correct the
TX-complete initialization race, validate length before parsing optional active
controller frames, and bound diagnostic symbol dumps by actual symbol count.

The old Ducted AC's AUTO/fan/mode control changes were **not** transplanted:
they address a different P1/P2 packet family and would not make an EKHBH
controller safe or functional.

### Future TX development: important limitations

- `sniffer_only: true` must stay enabled for current installations.
  The MAX22088 is kept in reset / TX disabled (GPIO8 HIGH), and no TX RMT
  channel, TX callback, or queued TX loop is enabled.
- The asynchronous TX code is only a **transport foundation**, not a working
  command protocol for this model. `sniffer_only: false` currently activates
  the unrelated Ducted AC responder logic: **do not set it on the live heat pump**.
- The upstream 10ms delay alone does **not** implement bus arbitration or
  collision avoidance, nor response validation or command authorization.
  Before transmitting anything, a separate explicitly controlled test mode,
  model-specific request/response handling, and bench testing with an
  isolated/disconnected bus interface are necessary.
- Current GitHub CI runs recorded-frame tests and checks that passive mode
  remains guarded; passing static checks is not equivalent to on-device testing.

## Update firmware (Windows PowerShell)

```powershell
cd C:\source\daikin-esp\esphome
git switch feature/altherma-read-only-sniffer
git pull --ff-only origin feature/altherma-read-only-sniffer
$env:ESPHOME_ESP_IDF_PREFIX = "C:\idf"
esphome config esp-daikin-ctrl.yaml
esphome compile esp-daikin-ctrl.yaml
esphome upload esp-daikin-ctrl.yaml --device 192.168.10.138
esphome logs esp-daikin-ctrl.yaml --device 192.168.10.138
```

The example keeps `logger.logs.p1p2_sniffer: INFO`, which prints **two lines per raw frame** to help later analysis. To reduce Wi-Fi/ESPHome log load after validation, set `p1p2_sniffer: WARN` and reflash; the Home Assistant sensors will continue to work independently.

For a raw capture:

```powershell
esphome logs esp-daikin-ctrl.yaml --device 192.168.10.138 2>&1 | Tee-Object -FilePath p1p2-capture.txt
```

During boot, check that the driver logs `READ-ONLY SNIFFER: MAX22088 RST held HIGH, TX hardware not initialized`. With config logging enabled, `Mode: READ ONLY (TX disabled)` is also shown.

## Limitations

- This experimental profile is based on a small number of tests on **one EKHBH016AB6WN**. Other old Daikin models may have different field positions, status bits, or scaling.
- `altherma_outdoor_temperature` is a **candidate** until verified at matching timestamps during further measurements.
- `altherma_dhw_temperature` and `altherma_flow_temperature` were matched against one display reading; more independent comparisons are advisable.
- Space-heating electric backup heater stages 1 and 2 have not been observed or decoded. No speculative status bits are published for them.
- Raw heating control values 1 and 2 are not yet understood; the diagnostic sensor is intentionally numeric, not a binary weather-compensation indicator.
- No return water temperature mapping is established. No validated EKHBH P1/P2 writing or control is implemented. The dormant active TX transport is not suitable for operation on this older device.
- The four controller settings above are **read-only sensors** (not ESPHome switches). Changing these settings through Home Assistant remains unsupported and intentionally disabled.
- The raw sniffer prints decoded bytes even if their CRC is bad; **the new HA decoder discards CRC-invalid frames**. Raw log transport can lag/drop under heavy load. Watch for RMT overflow warnings.
- The changes require local compilation using your ESPHome 2026.10.2 toolchain and a real-device test; they have not been compiled by the GitHub editing environment.

### References

- https://github.com/Arnold-n/P1P2MQTT/blob/master/doc/LogicalFormat/Daikin-protocol-EWYQ005ADVP.md
- https://github.com/Arnold-n/P1P2MQTT/blob/master/doc/LogicalFormat/README-Daikin.md
