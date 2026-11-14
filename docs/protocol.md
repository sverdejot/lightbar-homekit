# Light bar radio protocol

The Xiaomi Mi Computer Monitor Light Bar **MJGJD01YL** (the version without
Bluetooth/app) is controlled by a 2.4 GHz remote. The bar uses a Telink TLSR8368
receiver with a proprietary packet format that an nRF24L01+ compatible
transmitter can produce. This was reverse engineered by
[lamperez/xiaomi-lightbar-nrf24](https://github.com/lamperez/xiaomi-lightbar-nrf24);
this page summarises what this project relies on.

> The MJGJD02YL ("1S") is a different device (ESP32, BLE/Wi-Fi, Mi Home app)
> and is **not** covered here.

## Radio

| Parameter   | Value                                                   |
|-------------|---------------------------------------------------------|
| Modulation  | GFSK, 2 Mbit/s (nRF24 / Nordic proprietary 2 Mbit mode) |
| Channels    | 6, 15, 43, 68 (2406, 2415, 2443, 2468 MHz)              |
| Address     | `0x5555555555` (acts as a sync pattern)                 |
| Payload     | 17 bytes, static length                                 |
| CRC (radio) | disabled (the packet carries its own CRC16)             |
| Auto-ack    | none                                                    |

The bar does not acknowledge anything, so every command is sent many times on
all four channels. With the nRF52840 radio driven directly:

```
MODE      = Nrf_2Mbit
PCNF0     = 0                       (no S0 / LENGTH / S1 fields)
PCNF1     = WHITEEN=0, ENDIAN=Big, BALEN=4, STATLEN=17, MAXLEN=17
BASE0     = 0xAAAAAAAA, PREFIX0 = 0xAA   (0x55 bytes, bit-reversed as on nRF24)
CRCCNF    = 0
TXPOWER   = +8 dBm
```

## Packet (17 bytes)

| Offset | Size | Field     | Notes                                       |
|-------:|-----:|-----------|---------------------------------------------|
| 0      | 8    | preamble  | `53 39 14 DD 1C 49 34 12`, fixed            |
| 8      | 3    | remote id | unique per remote, big endian               |
| 11     | 1    | separator | `FF`                                        |
| 12     | 1    | counter   | must change between commands (dedup)        |
| 13     | 1    | command   | see below                                   |
| 14     | 1    | option    | signed step for the rotary commands         |
| 15     | 2    | CRC16     | over bytes 0..14, big endian                |

CRC16: poly `0x1021`, init `0xFFFE`, no reflection, no final xor.

The bar ignores a packet whose counter equals the previous one, so repeating the
same packet many times is safe and only executes once.

## Commands

| Command | Option          | Action                                         |
|--------:|-----------------|------------------------------------------------|
| `01`    | `00`            | toggle on/off (there is no separate on or off) |
| `02`    | `+n` / `-n`     | cooler / warmer by `n` (1..15)                 |
| `04`    | `+n` / `-n`     | brighter / dimmer by `n` (1..15)               |
| `06`    | `00`            | reset (medium brightness, warm white)          |

Negative steps are two's complement (`-1` = `FF`). An out-of-range step `-16`
(`F0`) saturates to the lowest value, which gives absolute control:
`04 F0` then `04 n` sets brightness `n` (0..15); same with `02` for colour
temperature (0 = 2700 K, 15 = 6500 K).

## Pairing a new remote id

Like the original remote, a new id is accepted if a reset command (`06`) is
received within ~20 s after the bar is powered on. This is what `lamp pair`
does in the firmware and `pair` in the sniffer tool; afterwards the original
remote stops working until it is paired again (long press within 20 s of
power-on).

## Capturing the remote id

Listening with address `0xAAAAAAAAAA` and an 18-byte payload yields the packet
shifted by a few bits; `tools/sniffer` re-aligns it (same approach as
[ebinf/lightbar2mqtt](https://github.com/ebinf/lightbar2mqtt)), checks the
preamble and CRC, and stores the id.
