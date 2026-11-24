# lightbar-homekit

[![Build](https://github.com/sverdejot/lightbar-homekit/actions/workflows/build.yml/badge.svg)](https://github.com/sverdejot/lightbar-homekit/actions/workflows/build.yml)

Control a **Xiaomi Mi Computer Monitor Light Bar (MJGJD01YL)** from **Apple Home
and Siri** with a single **nice!nano v2** (nRF52840), no extra radio module.

The board joins your home as a **Matter-over-Thread dimmable light** and
translates on/off and brightness into the light bar's proprietary 2.4 GHz
protocol, using the same radio. Put it in a room in the Home app and
*"Hey Siri, turn on the living room lights"* switches it together with the rest
of your lights.

```
 Siri / Home app
       │  Matter
       ▼
 HomePod mini / Apple TV  ── Thread border router
       │  Thread (802.15.4)
       ▼
 nice!nano (nRF52840) ─── Matter dimmable light
       │  proprietary 2.4 GHz, sent in MPSL radio timeslots
       ▼
 Xiaomi light bar MJGJD01YL
```

## How it works

- **Matter**: based on Nordic's Matter light bulb sample (OnOff + LevelControl
  clusters) on nRF Connect SDK v2.9.3, commissioned over Bluetooth LE and
  operated over Thread.
- **Lamp radio** ([`firmware/src/lamp_radio.c`](firmware/src/lamp_radio.c)):
  the nRF52840 has one radio, shared by Thread and BLE through Nordic's MPSL.
  Lamp commands request short **MPSL timeslots** (~2.5 ms). In each one the radio
  is reconfigured as an nRF24-compatible transmitter and the packet is sent on
  the four lamp channels, then handed back to Thread. A command is 12 such
  timeslots ~10 ms apart.
- **Mapping** ([`firmware/src/app_task.cpp`](firmware/src/app_task.cpp)):
  attribute changes are debounced (300 ms) and applied as lamp commands. The lamp
  only has a *toggle* and 16 brightness steps, so the firmware tracks what it
  believes the lamp shows and sends a toggle only when the state must change.

The radio protocol is described in [`docs/protocol.md`](docs/protocol.md).

## Hardware

| Item | Notes |
|------|-------|
| [nice!nano v2](https://nicekeyboards.com/nice-nano) | nRF52840 with the Adafruit UF2 bootloader. Other nRF52840 boards with the same bootloader (Adafruit Feather nRF52840) should work. |
| Thread border router | HomePod mini, HomePod (2nd gen) or a Thread-capable Apple TV 4K. Also acts as the Apple home hub. |
| Xiaomi Mi Computer Monitor Light Bar **MJGJD01YL** | The version with the wireless knob remote. Not the MJGJD02YL ("1S", Wi-Fi/BLE). |

Power the board from any USB charger, within a couple of metres of the light bar
(the nice!nano chip antenna has limited range).

## Repository layout

```
firmware/          Zephyr / nRF Connect SDK application (Matter + lamp radio)
  src/             app_task.cpp (Matter glue, shell), lamp_radio.c (timeslot TX)
  pm_static.yml    flash layout that keeps the UF2 bootloader intact
tools/sniffer/     Arduino firmware + Python tools: capture the remote id, test the lamp over USB
tools/dfu-stub/    minimal Arduino firmware used to reach serial DFU from the UF2 bootloader
docs/protocol.md   light bar radio protocol
.mise.toml         pinned versions, configuration, simple tasks
.mise/tasks/       build / flash tasks
.github/workflows/ CI build and manual release
```

All workflows are [mise](https://mise.jdx.dev) tasks; `mise tasks` lists them:

| Task | Description |
|------|-------------|
| `setup` | install every pinned dependency (`setup:python`, `setup:arduino`, `setup:ncs`) |
| `build [-p] [--remote-id <id>]` | build the firmware (`-p` clean), check its flash range, package it for DFU |
| `flash [--pkg <file>]` | flash over serial DFU, keeping the Matter pairing (builds unless `--pkg` is given) |
| `console` | open the firmware USB shell |
| `sniffer:build`, `sniffer:flash` | build / flash the USB sniffer firmware (UF2) |
| `sniffer:sniff` | capture the original remote's id |
| `sniffer:ui`, `sniffer:send <cmd>` | web UI / single command through the sniffer firmware |
| `dfu-stub:build` | build the DFU stub (used automatically by `flash`) |
| `uf2:copy <file>` | copy a small UF2 to the bootloader drive, refusing unsafe ranges |

Flashing and the serial tools assume macOS (`/dev/cu.usbmodem*`, `ioreg`,
`diskutil`); building works on macOS and Linux.

## Getting started

There are two paths: **prebuilt firmware** from a GitHub release (no SDK
needed) or **building from source**. Both need the remote id first.

### 1. Prerequisites

- [mise](https://mise.jdx.dev), then `mise install` (Python, uv, arduino-cli)
  and `mise run setup:python` (pyserial, adafruit-nrfutil).
- To build from source, also:
  - [`nrfutil`](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) in `PATH`
  - `mise run setup`: Arduino core and library into `.arduino/`, nRF Connect
    SDK toolchain and sources into `~/ncs/<version>` (several GB).

Every version is pinned in [`.mise.toml`](.mise.toml).

To enter the **UF2 bootloader**, double-tap RST↔GND: the board shows up as a
`NICENANO` USB drive.

### 2. Get the remote id

Each remote has a 3-byte id; the bar only obeys commands carrying the id it is
paired with. Capture it with the sniffer firmware (the original remote keeps
working):

1. Enter the UF2 bootloader and load the sniffer, either from source or from
   the `sniffer.uf2` of any release or CI run:

   ```sh
   mise run sniffer:flash                  # build + copy
   mise run uf2:copy path/to/sniffer.uf2   # prebuilt
   ```

2. Start sniffing and turn the remote's knob a few times next to the board:

   ```sh
   mise run sniffer:sniff
   ```

   ```
   sniffing on ch 6... use the remote near the board (send any char to stop)
   pkt id=50F160 cnt=37 cmd=0401 crc=OK
   learned remote id 50F160 (saved)
   ```

   The id is the six hex digits after `learned remote id`: here `0x50F160`.

3. Optionally check it controls the bar: `mise run sniffer:ui` (web UI) or
   `mise run sniffer:send on`.

The sniffer firmware wipes the Matter pairing stored on the board (see
[Flash layout](#flash-layout)); do this before adding the board to Home.

Alternatively, **use any id** and pair the bar to it (the original remote stops
working until re-paired): power-cycle the bar and run `lamp pair` from the
firmware shell within 20 seconds.

### 3a. Prebuilt firmware

The remote id is compiled into the firmware, so releases are built on demand:

1. Fork this repository.
2. In your fork, **Actions → Release → Run workflow**, and enter the remote id
   (`0x50F160`). It builds the firmware and publishes a release with
   `lightbar-matter-<id>.zip`, `sniffer.uf2` and `dfu_stub.uf2`.
3. Enter the UF2 bootloader and copy the DFU stub, which reboots into a small
   Arduino firmware that can reach serial DFU:

   ```sh
   mise run uf2:copy path/to/dfu_stub.uf2
   ```

4. Flash the package:

   ```sh
   mise run flash --pkg path/to/lightbar-matter-0x50F160.zip
   ```

Once the Matter firmware runs, later updates only need step 4.

### 3b. Build from source

Set the id in [`.mise.toml`](.mise.toml) (`LIGHTBAR_REMOTE_ID = "0x50F160"`)
or pass it per build, then flash:

```sh
mise run flash
mise run build --remote-id 0x50F160
```

`build` refuses images outside the application partition, and `flash` takes
the board into serial DFU from whatever it is running (this firmware, the
Arduino tools, or the UF2 bootloader).

### 4. Add it to Apple Home

On first boot the board advertises over Bluetooth for 15 minutes (power-cycle
to restart). In the Home app: **Add Accessory → More options**, and use the
setup code **`3497-011-2332`** (or the QR code printed by
`matter onboardingcodes ble`). Accept the *uncertified accessory* warning,
then assign it to a room.

The board's blue LED blinks while it is not on the Thread network and stays on
once attached.

## USB shell

Connect over USB with `mise run console`:

| Command | Description |
|---------|-------------|
| `lamp toggle` | toggle the lamp |
| `lamp bri <0-15>` | set brightness |
| `lamp id [hex]` | show / set the remote id (persisted) |
| `lamp pair` | send reset to pair the bar with the current id |
| `lamp state [on\|off]` | show state, or correct what the firmware believes the lamp shows |
| `lamp dfu` | reboot into serial DFU (used by `mise run flash`) |
| `lamp bootloader` | reboot into the UF2 bootloader |
| `matter onboardingcodes ble` | print the pairing codes |
| `matter device factoryreset` | remove Matter pairing |

## Flash layout

The Adafruit bootloader needs the MBR at `0x0` and lives at `0xF4000`, so the
partitions are fixed in [`firmware/pm_static.yml`](firmware/pm_static.yml):

| Range | Use |
|-------|-----|
| `0x00000 – 0x26000` | MBR + SoftDevice area (do not touch) |
| `0x26000 – 0xEC000` | application (~790 KB, ~98% used) |
| `0xEC000 – 0xF4000` | settings: Matter fabric, Thread credentials, remote id |
| `0xF4000 – 0x100000` | UF2 bootloader |

Things learned the hard way:

- Zephyr's own `zephyr.uf2` ignores the Partition Manager offset (base `0x0`)
  and would overwrite the MBR. Never copy it to the board.
- The Matter image is too large for the bootloader's UF2 drive (the copy hangs),
  hence serial DFU.
- Arduino sketches that use the Adafruit internal filesystem (`InternalFS`,
  e.g. `tools/sniffer`) store it at `0xED000` and **erase the Matter settings**:
  after `sniffer:flash` the board must be paired with Home again.
  `tools/dfu-stub` does not use it.

## Limitations

- **Toggle-only on/off.** If the original remote is used, Home may show the
  wrong state; fix with `lamp state on|off` or toggle twice from Home.
- **Brightness has 16 steps.** Colour temperature is not exposed (the data
  model is the dimmable light from the Nordic sample).
- **Test credentials.** Uses the Matter test vendor id and default setup code,
  so Apple Home shows it as uncertified.
- **No OTA updates.** No MCUboot/external flash; update over USB.

### Why not Zigbee / Philips Hue?

The first attempt exposed the board as a Zigbee 3.0 color-temperature light
for the Hue Bridge. It associates with the Hue network and leaves immediately
on every attempt (tried ZLL/distributed link keys, legacy commissioning and a
Philips OUI). Apple Home groups lights by room regardless of vendor, so Matter
reaches the same goal (Siri controls the room) without depending on the Hue
Bridge.

## Credits

- Protocol reverse engineering:
  [lamperez/xiaomi-lightbar-nrf24](https://github.com/lamperez/xiaomi-lightbar-nrf24)
  and [ebinf/lightbar2mqtt](https://github.com/ebinf/lightbar2mqtt).
- [nrf_to_nrf](https://github.com/TMRh20/nrf_to_nrf) (nRF24 compatibility on nRF52, used by the sniffer).
- Nordic Semiconductor nRF Connect SDK Matter light bulb sample.

## License

MIT for the code in this repository, see [LICENSE](LICENSE). Files derived
from the nRF Connect SDK samples (`firmware/src/main.cpp`,
`firmware/src/chip_project_config.h`, `firmware/src/default_zap/`,
`firmware/src/light_bulb.matter`) keep their original
`LicenseRef-Nordic-5-Clause` license.
