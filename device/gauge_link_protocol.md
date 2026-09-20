# Gauge link protocol (ESP32-S3 ↔ ESP32-WROOM-32)

Compatibility contract between two **separately flashed** boards. Either one can be
reflashed without the other, so treat this like the WebSocket protocol: **only ADD
optional trailing fields; never rename or repurpose an existing one.**

## Why the split

The S3's 8-bit display bus owns GPIO 4–7, 9–12, 15–18; USB owns 19/20; UART0 owns
43/44; flash/PSRAM owns 26–37. That leaves 16 clean GPIOs — and five ULN2003 motors
alone need 20, before any limit switch. So heading + the three altimeter needles moved
to an ESP32-WROOM-32; the speed needle stayed on the S3, where it was already wired.

## Wiring

| | S3 | WROOM |
|---|---|---|
| link TX → RX | GPIO 21 → | GPIO 16 |
| link RX ← TX | GPIO 14 ← | GPIO 17 |
| ground | — common GND between boards — | |

Both are 3.3 V logic; no level shifter. Each board keeps its own USB serial monitor
(S3: native CDC; WROOM: UART0 on GPIO 1/3).

**Motors and switches**

| Slot | Needle | Board | Coils | Limit switch |
|---|---|---|---|---|
| 0 | speed | S3 | 1, 2, 3, 8 | GPIO 13 (internal pull-up) |
| 1 | heading | WROOM | 13, 14, 18, 19 | GPIO 34 † |
| 2 | altimeter 100s | WROOM | 21, 22, 23, 25 | GPIO 35 † |
| 3 | altimeter 1,000s | WROOM | 26, 27, 32, 33 | GPIO 36 † |
| 4 | altimeter 10,000s | WROOM | 4, 5, 15, 2 (unwired) | GPIO 39 † |

† GPIO 34–39 are **input-only and have no internal pull-up**. Each of these four
switches needs an **external 10 kΩ from the pin to 3V3**. This is the most likely
wiring mistake on the WROOM — check it first if a needle homes to the wrong place or
never homes at all.

All switches are normally-open to GND, so closed (needle at the switch) reads LOW.

## Frames

Line-oriented ASCII, `\n`-terminated, 115200 8N1 — readable in any serial monitor.

**S3 → controller**

| Frame | Meaning |
|---|---|
| `T <s0> <s1> <s2> <s3> <s4>` | needle targets, absolute steps within one revolution `[0..2047]`. `-1` = no value for that slot. |
| `H` | re-home every needle this board owns |
| `P` | ping |

**Controller → S3**

| Frame | Meaning |
|---|---|
| `R` | homed and ready (sent after boot homing and after `H`) |
| `E <slot> <code>` | homing failed on a needle (`nosw` = switch never tripped) |
| `O` | pong |
| `# <text>` | log line; the S3 reprints it prefixed `[gauge]` |

### Slot ownership is a pin map, not a protocol

The frame always carries **all five** slots regardless of which board drives which
motor. Each board applies only the slots it owns and ignores the rest. Moving a motor
across the boundary is therefore an edit to two `config.h` files — never a protocol
change. All dial-face maths (kt → steps, ft → steps) lives on the S3 in `gauges.h`,
so the controller knows only pins, homing offsets and `STEPS_PER_REV`.

## Startup and recovery

The two boards boot independently and home in parallel — the S3 homes the speed needle
from `setup()` behind a "calibrating gauges" screen, the controller homes its four
sequentially (one at a time: lower peak current, and a wrong-switch wiring fault is
obvious because only one needle should ever be moving).

The controller can reset on its own (separate power rail, its own reset button). When
it comes back it re-homes and sends `R`; the S3 answers by re-sending the current
needle targets, so the gauges recover without waiting for the next flight update.

**Fail-soft:** a switch that never trips within ~1.1 revolutions gives up, logs, and
leaves the needle where it is, treating that as zero — the pre-limit-switch behaviour.
A dead switch degrades one gauge's accuracy; it never takes the device out of service.

## Calibrating a needle

Each switch sits wherever the mechanics allowed, **not** at the printed zero. The
per-motor `*_HOME_OFFSET` is the signed step count from switch-trip to zero, and
`*_HOME_DIR` is which way the needle rotates while seeking.

Both boards accept the link verbs on their **own USB serial monitor**, so you can jog
needles without reflashing. The S3's console drives all five (forwarding slots 1–4
over the link); the WROOM's drives its four with the S3 detached.

1. Set the motor's `*_HOME_OFFSET` to `0`, flash, and let it home. The needle now
   parks **at its switch**, and step 0 means "at the switch".
2. If the needle ran the wrong way and never found the switch (`E <slot> nosw`), flip
   `*_HOME_DIR` and repeat.
3. In the serial monitor, jog it with `T` until it sits exactly on the printed zero —
   e.g. `T -1 400 -1 -1 -1` moves heading to 400 steps past the switch. Binary-search
   it; 2048 steps = one revolution, 5.69 steps per degree.
4. That final number is the offset. Put it in `*_HOME_OFFSET` and reflash.

For a needle that must travel *backwards* from the switch to reach zero, use a
negative offset rather than a near-full-revolution positive one — it homes faster and
does not drag the needle past a mechanical stop.

## Sketches

| Path | Board (arduino-cli FQBN) |
|---|---|
| `device/flight_tracker/` | `esp32:esp32:esp32s3:CDCOnBoot=cdc` |
| `device/gauge_controller/` | `esp32:esp32:esp32` ("ESP32 Dev Module") |

`stepper.h` is duplicated byte-identically in both sketch directories — Arduino cannot
share headers across sketches (same rationale as the vendored `qrcodegen.c`). Edit one,
copy to the other, recompile **both**.
