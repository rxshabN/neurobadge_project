# Neuro-Badge v3 wiring

Everything below is added to the board that already carries the ESP32, MQ135,
DHT11, 16x2 I2C LCD and the fatigue button. Nothing existing moves.

## Power rails

| Rail | Source |
|---|---|
| +5 V | ESP32 `VIN` (USB pass-through) to the breadboard red rail |
| GND | ESP32 `GND` to the breadboard blue rail |

The MQ135 heater alone draws about 150 mA and the relay coil about 70 mA. If
the ESP32 reboots at the instant the relay clicks, that is a brown-out and not a
firmware fault: pull the `VCC`-`JD-VCC` jumper on the relay module and feed
`JD-VCC` from a separate 5 V source (a phone charger or power bank) with the
grounds tied together. Optocoupler isolation is what that jumper is for.

## Relay module (SONGLE SRD-05VDC-SL-C, 1 channel)

| Module pin | Goes to |
|---|---|
| `VCC` | +5 V rail |
| `GND` | GND rail |
| `IN` | ESP32 **GPIO21** |

Leave the `VCC`-`JD-VCC` jumper fitted unless you hit the brown-out above.

The module already carries its own flyback diode across the coil. Do not add
another one.

Output side: **do not wire mains to this.** For the demonstration the evidence
is the audible click plus the module's own channel LED, and, if you want a
visible load, a 5 V DC fan wired `COM` to +5 V and `NO` to the fan, fan negative
to GND. A 230 V load on a breadboard-mounted relay in front of a review panel is
an unnecessary risk and adds nothing the click does not already prove.

## Buzzer driver (2N2222 low-side switch)

The ESP32 cannot drive a 5 V active buzzer directly: the GPIO swings to 3.3 V
and the buzzer wants 5 V at roughly 30 mA. The transistor switches the buzzer's
ground path from the 5 V rail instead.

Identify the 2N2222 pins first. TO-92 package, flat face toward you, legs
pointing down: **left = Emitter, middle = Base, right = Collector**. Verify with
a multimeter on diode-test: red probe on the middle pin, black probe on each
outer pin in turn, both should read about 0.6 to 0.7 V. If they do not, the
middle pin is not the base and the part is not the pinout above.

| From | To |
|---|---|
| ESP32 **GPIO22** | 1 kOhm resistor, then 2N2222 **base** |
| 2N2222 **base** | 1 kOhm resistor, then GND rail (hold-off, see below) |
| 2N2222 **emitter** | GND rail |
| 2N2222 **collector** | buzzer **negative** (short leg) |
| Buzzer **positive** (long leg) | +5 V rail |
| 1N4007 across the buzzer | band (cathode) to +5 V rail, other end to collector |

### What is actually mandatory

Of the three extra parts, only one is required.

| Part | Status | Why |
|---|---|---|
| Base resistor, 1 kOhm | **REQUIRED** | Without it the GPIO drives straight into a base-emitter junction clamped at 0.7 V, through nothing but the pin's own ~40 Ohm output impedance. That is roughly 65 mA out of a pin rated 40 mA absolute maximum. It will stress GPIO22 and can destroy it. |
| Hold-off resistor, base to GND | optional | Only prevents a brief chirp during the ~200 ms at boot when the pin still floats. Cosmetic. |
| Flyback diode across the buzzer | optional at this current | The transistor is rated 40 V and 600 mA; the kick from a 30 mA transducer is nowhere near that. Plenty of commercial buzzer circuits omit it. |

Any value from about **220 Ohm to 4.7 kOhm** works as the base resistor; 1 kOhm
is the sweet spot. 10 kOhm is too high - it starves the base and the buzzer
comes out weak. If no resistor is available at all, leave the buzzer on the
direct-drive wiring above; quiet and safe beats loud and a dead pin.

### Without a multimeter

The EBC order above is what the 2N2222A / PN2222A datasheets specify for TO-92,
so wire it that way first. If the buzzer is silent, or sounds but much weaker
than expected, swap the two OUTER legs and retry - a bipolar transistor with its
emitter and collector reversed still conducts, but in reverse-active mode with a
current gain of only two or three, which is exactly what "weak but not dead"
sounds like.

Nothing is at risk while you try both orientations. The 1 kOhm in the base path
limits the current in every wrong permutation, and the buzzer itself is in series
with the collector.

### Fitting it in place of the direct-drive wiring

Power off first. The buzzer is currently running straight off the pin with two
jumpers; both of them move.

1. Remove the jumper from the buzzer's + row to GPIO22, and the one from its
   - row to GND. The buzzer itself stays where it is.
2. Jumper the buzzer's **+ row** to the **+5 V rail** - the same rail feeding the
   MQ135, not 3V3.
3. Seat the transistor with its three legs in three SEPARATE rows, all on one
   side of the centre channel. TO-92 legs are close together; splay them gently
   so they cannot share a row.
4. Jumper the buzzer's **- row** to the **collector row**.
5. Jumper the **emitter row** to the **GND rail**.
6. 1 kOhm from the **base row** to a free row, then a jumper from that row to
   **GPIO22**.
7. 1 kOhm from the **base row** to the **GND rail**.
8. 1N4007 with its banded end in the **+5 V rail** and the other end in the
   **collector row**.

Then power up. The boot self-test chirps twice, and those chirps should now be
obviously louder than they were on 3.3 V.

**No firmware change is needed.** GPIO22 HIGH sounds the buzzer either way, so
the same binary drives both arrangements.

The second 1 kOhm from base to emitter holds the transistor off during the
200 ms or so between reset and `pinMode()`, when GPIO22 is still floating.
Without it the buzzer can chirp on every boot.

Buzzer polarity: the longer leg is positive. If both legs look the same length,
peel the "remove seal after washing" sticker off the top - the `+` is printed on
the case underneath it. Wired backwards an active buzzer is simply silent, so if
it does not sound, flip it before assuming anything is broken.

You bought two buzzers. Wire one; the second is a spare.

### If you do not have the transistor and resistor yet

The transistor stage is an improvement, not a prerequisite. A small active buzzer
of this size draws roughly 20-30 mA at 5 V and less than that at 3.3 V, and an
ESP32 GPIO is rated to 40 mA, so the buzzer can be driven straight off the pin as
an interim arrangement:

| From | To |
|---|---|
| Buzzer **+** (long leg) | ESP32 **GPIO22**, directly |
| Buzzer **-** (short leg) | GND rail |

Nothing else. No base resistor, no hold-off resistor, no flyback diode.

Two consequences. It runs at 3.3 V instead of 5 V, so it is quieter - clearly
audible in a quiet room, less so in a corridor. And the pin carries the buzzer
current itself rather than a milliamp of base current, which is within the pin's
rating but leaves no margin.

Do NOT put a 1 kOhm resistor in series with the buzzer to "be safe". That limits
the current to about 3 mA and the buzzer will be inaudible or silent. If you want
a series resistor, it has to be around 100 Ohm, and it is not necessary.

Watch for one symptom: if the ESP32 resets, or the LCD flickers or garbles, each
time the buzzer sounds, the buzzer is pulling more than the pin or the USB supply
can give. Fit the transistor stage before going any further.

With a multimeter, measure the buzzer's current in series at 3.3 V before
trusting it: under 20 mA it can stay direct-driven indefinitely, 20-40 mA works
but fit the transistor before the review, above 40 mA do not drive it directly at
all.

**The firmware is identical either way.** Direct-driven or switched through the
2N2222, GPIO22 HIGH sounds the buzzer and LOW silences it, so moving to the
transistor stage later is purely a wiring change with no code change.

GPIO22 was chosen because it carries no boot strapping role, is not touched by
the flash or by PSRAM on any ESP32 variant, and sits on the same header column as
the relay at GPIO21. An active buzzer needs no PWM, so an ordinary output pin is
sufficient. GPIO17 is the alternative if GPIO22 is awkward on the breadboard, but
avoid it on WROVER modules, where GPIO16 and GPIO17 are wired to the PSRAM.

## Severity LED

GPIO2, the onboard LED. No external wiring. v3 drives it through the LEDC
peripheral as an 8-bit PWM channel at 5 kHz instead of `digitalWrite`, so it
carries a 0-100 severity as brightness rather than only on/off.

## Full pin map after v3

| GPIO | Silkscreen | Function | Direction |
|---|---|---|---|
| 34 | D34 | MQ135 AOUT | analog in (input-only pin) |
| 4 | D4 | DHT11 data | bidirectional 1-Wire |
| 5 | D5 | I2C SDA (LCD) | bidirectional |
| 18 | D18 | I2C SCL (LCD) | out |
| 19 | D19 | Fatigue button | in, internal pull-up, falling-edge ISR |
| 2 | D2 | Severity LED (onboard) | PWM out (LEDC ch 0) |
| 21 | D21 | Relay IN | digital out |
| 22 | D22 | Buzzer, via 2N2222 | digital out |

Of the pins used here only GPIO5 is a boot strapping pin, and the LCD module's
I2C pull-ups hold it high at reset, so boot is normal. GPIO19, GPIO21 and GPIO22
have no strapping role at all - which means, unlike the earlier GPIO15 wiring,
holding the fatigue button down through a reset can no longer put the board into
the wrong boot mode.

## Bench check before running the sketch

1. With the board powered and the sketch NOT yet flashed, nothing should sound
   and the relay should be open.
2. Flash, then watch the serial console for
   `[rtos] actuate(c1,p4) sense(c1,p3) uinet(c0,p1) started`.
3. Touch GPIO21 to GND by hand only if the relay never clicks under software -
   a click confirms the coil and module, and isolates the fault to the firmware
   or the pin.

## If the relay logic is inverted

Some SONGLE breakouts energise on `IN` low, some on `IN` high. The firmware
assumes active-low. If the relay sits closed when the air is clean and opens
during a hazard, change one line in `neurobadge.ino`:

```
#define RELAY_ACTIVE_LOW  0
```

Do not invert the logic anywhere else.
