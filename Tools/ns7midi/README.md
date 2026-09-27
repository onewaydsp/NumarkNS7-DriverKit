# ns7midi

`ns7midi` is a CoreMIDI command-line tool for the NS7 dext. It checks that the device shows up in CoreMIDI. It also records and checks every control and light on the Numark NS7, measured against the inventory in [`docs/controls/ns7-controls.json`](../../docs/controls/ns7-controls.json).

```
make -C Tools/ns7midi          # build/ns7midi (-Wall -Wextra -Werror)
make -C Tools/ns7midi test     # unit tests + hardware-free self-test
```

| Command | What it does |
|---|---|
| `list` | Lists every MIDI device, entity and endpoint. |
| `monitor [s] [--record f]` | Prints MIDI from the NS7 (20 s by default). `--record` also writes the messages in replay format. |
| `send <hex…>` | Sends channel voice messages, e.g. `send B0 09 7F`. |
| `learn <inventory> <out> …` | Guided capture of every input control, with an optional LED check. |
| `ledscan …` | Lights LED addresses one at a time and records what lit. |
| `verify <inventory> <learned> [ledmap]` | Prints a coverage report and writes `docs/controls/NS7-MIDI-MAP.md`. |

The NS7 is found by its CoreMIDI device name, "Numark USB Audio Device". The dext must be loaded and running, because MIDI only flows while the dext streams the audio pipes. MIDIServer starts per client and stops the device IO when the last client leaves. Each command therefore keeps one MIDI client, with an input connection, open for its whole run. Don't run two sessions at once.

## Safety

- Every message that is sent, or would be sent, is printed first: `    [send] B0 09 7F`. With `--dry-run` the line reads `    [dry-run] B0 09 7F (not sent)`, and no MIDI client is created.
- `ledscan` only sends channel-1 `B0` or `90` messages with 7-bit data. The data1 values stay inside `--from`/`--to`, the `--only` list, or the inventory's `expected_led` entries. A final whitelist check refuses anything else.
- `learn` sends nothing unless you pass `--leds`, and then only the inventory's `expected_led` messages, which must also be B0/90.
- `learn --replay` and `ledscan --dry-run` never open CoreMIDI. `make test` uses only these two modes.

## learn

```
ns7midi learn <inventory.json> <out.json> [--from id] [--only a,b] [--all] [--retry-skipped]
              [--timeout s] [--min s] [--script file|fifo] [--replay file]
              [--baseline s] [--ignore B0:00,E0] [--no-sweep] [--leds] [--dry-run]
```

1. **Baseline.** The tool first captures 3 s with hands off (`--baseline`). Anything that arrives then, such as a spinning platter, goes into `learn.background` and is ignored in every later step. Turn the platter motors off first (SCRATCH OFF). Otherwise the platter keys become background and the platter steps capture nothing.
2. **One step per control** that sends MIDI. LED-only items and `midi_expected: false` switches are skipped. The step prints the label, what to do, and the prior message. It then echoes incoming MIDI (`< 90 11 7F`) until you answer:
   - **Enter**: accept
   - **`s`**: skip
   - **`r`**: redo the step
   - **`q`**: save and quit
   `--timeout N` ends a step after N s. A step that times out is accepted if it captured anything, and otherwise recorded as `timeout`.
3. **Summary.** Messages are grouped by key (`90:11`, `B0:04`, `E0`). Note-off is folded into note-on. Each group gets a class:
   - `note_pair`, `note_on_only`
   - `cc_button` (00 and one on value), `cc_single_value`
   - `cc_absolute` (range, direction, "full range")
   - `cc_relative_2c` or `cc_relative_offset64` (ticks + and -)
   - `pitchbend_14bit`
   It also flags 14-bit CC pairs, where CC n is the MSB and CC n+32 the LSB.
4. **LED check (`--leds`).** After an accepted control that has a known `expected_led`, the tool sends ON and asks "Is the … light ON now? [y/n]", then sends OFF.
5. **Final sweep.** "Move, press and touch EVERY control once." Anything that isn't assigned to a verified control is listed and stored in `learn.sweep.unassigned`.

`out.json` is the inventory plus a `learned` object per control, and it is saved after every step. That object holds:
- `status`: accepted / skipped / empty / timeout
- `verified_input`
- `inputs`: the groups
- `sample`: the first 24 raw messages
- `conflicts`
- `led_tests` and `verified_led`

The file also gets a top-level `learn` object with `background`, `observed` (every key seen, with counts) and `sweep`.

**Resuming.** Run the same command again. Controls that already have a result are left alone. Use `--retry-skipped` to revisit skipped, empty or timed-out controls, `--from <id>` to redo one control and carry on from there, `--only a,b` to do just those, and `--all` to redo everything.

**Driving it from a script (coordinator).** Answers come from `--script <file>` instead of the terminal: one answer per line, an empty line means Enter, and `#` starts a comment. The script is echoed as `  > s`. End of file means save and quit.
- **FIFO (recommended for a live session).** `mkfifo /tmp/ns7answers`, then start `learn … --script /tmp/ns7answers`. Write a line (`echo > /tmp/ns7answers`, `echo s > …`) when the human says the step is done. Each step waits for its line.
- **Regular file with live hardware.** Add `--min N` so that each step captures at least N seconds before it reads the next answer. The human then has a fixed N-second window per step.
- **`--replay <file>`** feeds recorded MIDI instead of CoreMIDI. The format is one message per line in hex, with `---` ending the block for one capture. Blocks are used in order: baseline first (unless `--baseline 0`), then one per step (including redone ones), then the sweep. `monitor --record f` writes the same format. See `tests/fixtures/`.

Example (from the self-test):

```
[1/5] deckA.play: Play (Deck A) (button+led)
  Do: press and release.
  Prior input: 90 11 (mixxx)
  Then: Enter = accept, s = skip, r = redo, q = save & quit
    < 90 11 7F
    < 90 11 00
  > accept
  = note 90:11: 1 press(es), 1 release(s), 1 pair(s)
  = 1 background message(s) ignored
  LED check B0 09: sending ON
    [dry-run] B0 09 7F (not sent)
  Is the Play (Deck A) light ON now? [y = yes, n or Enter = no]
  > y
    [dry-run] B0 09 00 (not sent)

[2/5] deckA.pitchFader: Pitch fader (Deck A) (fader)
  ...
  = CC B0:04 absolute 00..7F (full range), rising, 5 distinct, 5 msgs; 14-bit MSB, LSB on B0:24
  = CC B0:24 absolute 00..7F (full range), rising, 5 distinct, 5 msgs; 14-bit LSB of B0:04
```

## ledscan

```
ns7midi ledscan [--status b0|90|both] [--from hex --to hex] [--only B0:09,B0:08]
                [--inventory inv.json] [--value 7f] [--off-value 00] [--hold ms] [--gap ms]
                [--interactive] [--script file|fifo] [--out ledmap.json] [--all-off] [--dry-run]
```

By default the scan covers CC `B0 00`–`B0 7F` with value 7F. Numbers are hex. For each address it prints `[B0 0A] ON`, sends ON, holds for `--hold` ms, sends OFF and prints `[B0 0A] OFF`.

- **`--interactive`** keeps each LED lit until you type what lit. You can type:
  - a control id (`deckA.sync`)
  - several ids separated by commas
  - free text
  - Enter, meaning nothing lit
  - `r` to repeat the address
  - `q` to save and quit (the LED is still switched off first)
  Results are merged into `--out` (default `ledmap.json`), keyed by address and ON value, and saved after every address.
- **`--inventory <file>`** tests every `expected_led` in the inventory and shows the expected control. Answer `y` if that control lit.
- **`--all-off`** only sends OFF for the whole range. Use it for cleanup after an interrupted scan.
- **Values other than 7F.** Some NS7 LEDs may be multi-level: the level meters (`B0 36`), the strip-search LEDs (`B0 3B`, `B0 53`) and possibly others. Such an LED may stay dark at 7F, or show a level or position. Re-test those addresses with, for example, `--only B0:36 --value 20` and `--value 40`. The ON value is recorded in the result.

```
$ ns7midi ledscan --dry-run --from 08 --to 09 --hold 0
ns7midi ledscan: 2 address(es), ON value 7F, OFF value 00 (dry run: nothing is sent)
[B0 08] ON
    [dry-run] B0 08 7F (not sent)
    [dry-run] B0 08 00 (not sent)
[B0 08] OFF
[B0 09] ON
...
```

## verify

```
ns7midi verify <inventory.json> <learned.json|-> [ledmap.json] [--md file | --no-md]
```

`verify` lists each inventory item by section: whether its input is verified (`OK`, `MISS`, or `--` when it has none), the learned keys and classes, and its LED state.

- **An input counts as verified** when `learn` accepted a non-empty capture.
- **An LED counts as verified** when one of these holds:
  - `learn --leds` got a `y` in a real run (not a dry run)
  - a real (not dry-run) `ledscan` result names the control's id
  - the inventory's prior is `"verified": true` (the four LEDs confirmed on 2026-09-27)

It then lists:
- observed input keys that no verified control owns (from learning, the sweep and the inventory's `unidentified_observed`)
- LED addresses that lit something outside the inventory
- input keys that are verified on two controls

The exit status is 1 until every input and LED is verified and nothing observed is left unassigned. It also writes `NS7-MIDI-MAP.md` next to the inventory, or to the path given with `--md`. Pass `-` as the learned file to render the priors only.

```
[deckA]
  in  OK   led MISS  deckA.play                 90 11 note_pair  | LED prior: B0 09 (mixxx)
  in  OK   led  --   deckA.pitchFader           B0 04 cc_absolute, B0 24 cc_absolute
  in MISS  led  --   browse.scrollKnob          skipped
Observed input messages not assigned to any control:
  B0 77 (x2, learn)
Inputs verified: 4/5   LEDs verified: 0/2   unassigned inputs: 1   => INCOMPLETE
```

## A full verification session

**Roles.** The human operates the NS7 and says what they see. The coordinator runs the commands and writes the answers. Paths are from the repo root. `ns7=Tools/ns7midi/build/ns7midi` and `inv=docs/controls/ns7-controls.json`. Results go to `docs/controls/learned.json` and `docs/controls/ledmap.json`.

0. **Prepare (2 min).**
   - Build with `make -C Tools/ns7midi test`.
   - Plug in the NS7 and check that `$ns7 list` shows "Numark USB Audio Device" / "MIDI".
   - Run `$ns7 monitor 5` and check that a knob produces messages.
   - Turn both platter motors off (SCRATCH OFF on each deck). Center the pitch faders and set the crossfader to the middle.
   - Quit Mixxx and any other MIDI app, so nothing else sends LED messages.
1. **Known LEDs (3 min).** Run `$ns7 ledscan --inventory $inv --interactive --out docs/controls/ledmap.json`. Answer `y` or Enter for each address. This confirms the Mixxx priors (PFL 14/18, strip 3B/53, meters 36) and the four already verified.
2. **Input capture (35–45 min).** 100 input controls take about 20 s each. Run:
   ```
   $ns7 learn $inv docs/controls/learned.json --leds
   ```
   With a coordinator, add `--script /tmp/ns7answers` (a FIFO). Go deck A → deck B → mixer → browse → front panel, as the inventory orders them.
   - **Fader or knob:** move it slowly, end to end, twice.
   - **Platter:** spin forward, then backward, without touching the top (the touch sensor has its own step).
   - **Switch:** visit every position and come back.
   - **Something that doesn't exist or doesn't respond:** answer `s`. Note this in the inventory `notes` afterwards.
   - **Break:** `q`, then rerun the same command to resume.
3. **LED sweep (12 min for CCs, plus 12 min if notes are needed).**
   ```
   $ns7 ledscan --interactive --out docs/controls/ledmap.json            # B0 00–7F
   $ns7 ledscan --interactive --status 90 --out docs/controls/ledmap.json  # only if LEDs remain unexplained
   $ns7 ledscan --all-off                                                   # cleanup
   ```
   For each address, type the control id of whatever lit. If it isn't in the inventory, type free text.
4. **Multi-level LEDs (5 min).** For the meter, strip and any address that lit dimly or oddly, run `$ns7 ledscan --only B0:36,B0:3B,B0:53 --interactive --value 20 --out docs/controls/ledmap.json`. Repeat with `--value 40` and `--value 7F`.
5. **Report (2 min).** Run `$ns7 verify $inv docs/controls/learned.json docs/controls/ledmap.json`.
   - Anything `MISS`: rerun `learn --only <ids>` or `ledscan --only …`.
   - Unassigned keys: find the control with `monitor` and add it to the inventory (for example, a 14-bit LSB belongs with its MSB control, since `learn` records both keys in the same step).
   - Rerun `verify` until it exits 0.
6. **Commit** `docs/controls/learned.json`, `ledmap.json` and the regenerated `NS7-MIDI-MAP.md`.

Total: about 60–80 minutes, most of it in step 2.

## Layout

```
src/json.*          small JSON value, parser and pretty printer (no dependencies)
src/midi_msg.*      message struct, strict hex parsing, keys, replay format
src/summarize.*     step summarizer (pure)
src/inventory.*     inventory helpers (pure)
src/verify_core.*   coverage logic and text/Markdown rendering (pure)
src/midi_io.*       CoreMIDI session, replay input, dry-run output; list/monitor/send
src/answers.*       answer lines from the terminal, a script file or a FIFO
src/learn.cpp, src/ledscan.cpp, src/verify.cpp, src/main.cpp
tests/test_main.cpp unit tests for the pure modules
tests/selftest.sh   end-to-end: ledscan --dry-run, learn --replay --script, verify
tests/fixtures/     small inventory, replay files and scripts (not real NS7 data)
```
