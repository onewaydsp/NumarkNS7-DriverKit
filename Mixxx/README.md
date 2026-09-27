# Mixxx mapping: Numark NS7 (DriverKit)

A Mixxx 2.5 controller mapping for the original Numark NS7 running on the
DriverKit driver in this repo. The driver exposes the CoreMIDI device
**Numark USB Audio Device** (port **MIDI**).

Files:

- `Numark NS7 (DriverKit).midi.xml`: control assignments
- `Numark-NS7-DriverKit-scripts.js`: platter/scratch logic, rate range, LEDs

## Install

Mixxx from the Mac App Store / mixxx.org is sandboxed, so user mappings live in
its container:

```sh
DEST="$HOME/Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx/controllers"
mkdir -p "$DEST"
cp "Mixxx/Numark NS7 (DriverKit).midi.xml" Mixxx/Numark-NS7-DriverKit-scripts.js "$DEST/"
```

## Select it in Mixxx

1. Preferences → Controllers → pick the NS7 device (**Numark USB Audio Device MIDI**).
2. Load Mapping → **Numark NS7 (DriverKit)**.
3. Tick **Enabled** → Apply.

If the mapping isn't listed, restart Mixxx. It only scans the folder at startup.

## What's mapped

Uses the same MIDI numbers as the Mixxx built-in "Numark NS7" mapping (2010):

| Section | Controls |
|---|---|
| Transport | Play, Cue, Sync (`beatsync`), Keylock, Search back/fwd, Strip search (`playposition`) |
| Hotcues | 1–5 per deck |
| Loops | Loop in / out per deck, Reloop on deck B |
| Pitch | Pitch fader (7-bit), pitch bend −/+, pitch range button (cycles 8/10/30/100 %) |
| Platter | Scratch button toggles scratch mode. With it on, the platter scratches (`engine.scratchTick`). With it off, the platter nudges (`jog`). |
| Library | Load deck A / B |
| Mixer | Crossfader, headphone mix / volume, main volume, per-channel gain, high/mid/low EQ, channel faders |
| LEDs | Play, Cue, PFL, end-of-track warning (optional VU meter, off by default) |

Scratch settings are constants at the top of the script. Start with
`INTERVALS_PER_REV` (a guess of 3600 ticks per revolution; turn on `DEBUG_JOG`
to calibrate it) and `JOG_DIRECTION`.

With the motor running, the platter sends jog data all the time. In nudge
mode, that keeps bending the tempo. Turn the motor off to nudge, or turn
scratch mode on to use the platter as a virtual record.

## Pending (guided capture)

- Platter touch messages. `NumarkNS7DK.platterTouch` is written but not bound yet.
- Scratch LED CC, and the PFL button inputs.
- Unknown messages: `B0 24`, `B0 30`, `B0 6E`, and the `E0`/`E2` pitch-bend
  stream sent while a platter spins.
- Whether the pitch faders are 14-bit.
- Deck A Reloop is disabled. The 2010 mapping put it on `90 31`, which is also
  Deck B Cue. Deck B Loop out is `90 50` there, but the A/B pattern suggests
  `90 4A`.
- The PFL, end-of-track and VU LED numbers come from the 2010 mapping and
  haven't been checked. Play and Cue LEDs are verified.

## License

GPL-2.0-or-later. Derived from the "Numark NS7" mapping by Anders Gunnarsson
(2010), which ships with Mixxx under the GPLv2+.
