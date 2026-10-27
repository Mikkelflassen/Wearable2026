# Wearable2026 – group conversation bracelet

A bracelet for group sessions that lets everyone signal without interrupting the speaker:
a talking queue, a new-topic queue, a break vote, and agree / disagree / "I have a comment".

- `firmware/bracelet/` – the sketch for the ESP32-C3 Super Mini. Same sketch on every bracelet.
- `sim/` – a terminal program that plays the bracelets you have not built yet.
- `tests/` – scenario tests for the rules (firmware logic and simulator logic, alone and mixed).

## How it behaves

| You do | What happens |
|---|---|
| Press **agree** once | The speaker feels 1 short buzz |
| Press **disagree** once | The speaker feels 2 short buzzes |
| Press either button **twice** | The speaker feels 3 short buzzes: "someone has a comment on this" |
| Several people send the same signal within 3 s | The speaker feels it once: it means "someone", not how many |
| Snap onto **talk** | You join the talking queue. First in the queue is the speaker. When the person in front of you unsnaps, you feel one long buzz: your turn |
| Snap onto **topic** | You join the new-topic queue. When the talking queue has been empty for 1.5 s, the first person feels two long buzzes. Move the snap to **talk** to start the topic (you have 8 s before the next person is offered the slot). Unsnap earlier and you simply leave the queue |
| Snap onto **pause** | Your wish for a break is remembered, also after you move the snap elsewhere. When more than half of the bracelets present want a break, everyone feels a rapid rattle + one very long buzz, and all wishes are reset |
| Hold **both buttons** 1.5 s | Take back your break wish (small tick as confirmation) |

The on-board LED is lit while you are the speaker. A bracelet that is switched off or out of
range for 5 s is dropped from the queues and from the head-count for the break vote.

All timings and vibration patterns are constants at the top of `bracelet.ino` and `Core.h`.

## Wiring (one bracelet)

Buttons and snaps just connect a pin to GND; no resistors needed (internal pull-ups).

| ESP32-C3 Super Mini | Goes to |
|---|---|
| GPIO 0 | Agree button → GND |
| GPIO 3 | Disagree button → GND |
| GPIO 5 | Fixed snap "talk" |
| GPIO 6 | Fixed snap "topic" |
| GPIO 7 | Fixed snap "pause" |
| GND | The loose snap on the wire |
| GPIO 1 | Motor driver input (see below) |
| 5V pin | Charger `OUT+` through an on/off switch |
| GND | Charger `OUT-` |

Avoid GPIO 2, 8 and 9 for your own parts (they decide how the board boots; 8 is the LED).

**Motor (mini 3.0 V).** A GPIO pin cannot power a vibration motor directly, so do not connect
the motor straight to GPIO 1. Use a small NPN transistor (BC547, PN2222, S8050 … most Arduino
starter kits have one): GPIO 1 → 1 kΩ → base, emitter → GND, collector → motor −,
motor + → the **3V3** pin (not 5V: on USB that pin carries 5 V, too much for a 3 V motor), and
a diode (1N4007 / 1N4148) across the motor with the stripe towards motor +. A 100 µF capacitor
between 3V3 and GND (long leg to 3V3) keeps the radio steady when the motor starts.

**No transistor yet?** Put an LED on GPIO 1 instead (GPIO 1 → 220 Ω → LED long leg, short
leg → GND). It blinks the exact vibration patterns, and the serial monitor prints `# vibe …`.

**No snaps yet?** Plug a jumper wire into GPIO 5, 6 and 7 and one into GND. Touching the GND
wire to one of the three is exactly what the snap will do.

**Battery.** Battery → HW-373 `B+` / `B-`; HW-373 `OUT+` / `OUT-` → switch → 5V pin / GND.

- Never have the battery switched on while the ESP's own USB cable is plugged in: USB would
  push 5 V backwards into the battery. Switch off (or unplug the battery) before flashing.
- Check the charge current. These TP4056 boards normally ship set to 1 A, which is about three
  times too much for a 320 mAh cell. The current is set by one small resistor (R_PROG, usually
  marked `122` = 1.2 kΩ); swapping it for 4.7–10 kΩ gives roughly 250–130 mA.
- Expect about 3 hours per charge with the radio always listening.

While you only have one bracelet on USB you do not need the battery or charger at all.

## Flash it

**Arduino IDE:** install the "esp32" boards package, open `firmware/bracelet/bracelet.ino`,
choose board *ESP32C3 Dev Module* and set *Tools → USB CDC On Boot → Enabled*, then upload.

**PlatformIO:** `pio run -t upload` from this folder.

Open the serial monitor at 115200: it prints `# bracelet 1234 ready`. You can already try it
without anything soldered by typing `!talk`, `!topic`, `!pause`, `!off`, `!a`, `!d`, `!c`,
`!unpause` or `!status`.

## Fake the other two people

```
pip install pyserial
python sim/sim.py                 # three pretend bracelets, no hardware at all
python sim/sim.py --list          # find the port of the real bracelet
python sim/sim.py --port COM5     # the real bracelet + two pretend ones
```

Then type `<who> <action>`: `1 talk`, `2 a`, `1 off`, `2 pause`, … (`h` shows everything).
`r` is the real bracelet, so `r talk` works before its snaps are sewn on. Close the Arduino
serial monitor first; only one program can use the port.

The real bracelet copies everything between USB and radio, so later the same command also
works with two real bracelets and one pretend one.

## How the bracelets talk

ESP-NOW broadcast: it uses the Wi-Fi radio but needs no router, hotspot or password, so it
works in any room (eduroam would not let the bracelets see each other). There is no master.
Each bracelet broadcasts its own state every second and at once when it changes:

```
W26 S <id> <talkTicket> <topicTicket> <wantsBreak> <breakRound>
W26 E <id> <eventId> <A|D|C>
```

Joining a queue means taking a ticket one higher than any ticket seen so far; everybody sorts
by ticket and so sees the same order. All bracelets must use the same `RADIO_CHANNEL`.

## Tests

```
python tests/test_bracelets.py
```

The rules live in `firmware/bracelet/Core.h` (C++) and are mirrored in `sim/core.py` (Python).
If you change a rule, change both; the tests run them against each other.
