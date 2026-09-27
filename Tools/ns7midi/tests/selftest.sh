#!/bin/bash
# selftest.sh <ns7midi binary>
# End-to-end self-test without hardware: ledscan --dry-run and learn with
# --replay + --script, then verify. Nothing here opens a CoreMIDI connection
# (dry-run output and replay input never create a MIDI client).

set -u
BIN="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/fixtures"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/ns7midi-selftest.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok()   { pass=$((pass + 1)); }
bad()  { fail=$((fail + 1)); echo "FAIL: $*"; }
check() { if eval "$1"; then ok; else bad "$2"; fi; }
json() { python3 -c "import json,sys; d=json.load(open('$1')); print($2)"; }

# Every line that sends (or would send) must be a channel-1 B0/90 message
# with 7-bit data. Any other [send]/[dry-run] line fails.
only_led_messages() {
    ! grep -E '\[(send|dry-run)\]' "$1" | grep -vqE '^    \[dry-run\] (B0|90) [0-7][0-9A-F] [0-7][0-9A-F] \(not sent\)$'
}

# ---------------------------------------------------------------- ledscan

"$BIN" ledscan --dry-run --from 08 --to 0A --hold 0 --gap 0 > "$TMP/scan1.txt" 2>&1
check '[ $? -eq 0 ]' "ledscan dry-run exit status"
check 'grep -q "^\[B0 08\] ON$" "$TMP/scan1.txt"' "ledscan prints [B0 08] ON"
check 'grep -q "^\[B0 0A\] OFF$" "$TMP/scan1.txt"' "ledscan prints [B0 0A] OFF"
check '[ "$(grep -c "\[dry-run\]" "$TMP/scan1.txt")" -eq 6 ]' "ledscan 08..0A sends 3 ON + 3 OFF"
check 'grep -q "\[dry-run\] B0 09 7F (not sent)" "$TMP/scan1.txt"' "ledscan ON message printed"
check 'grep -q "\[dry-run\] B0 09 00 (not sent)" "$TMP/scan1.txt"' "ledscan OFF message printed"
check 'only_led_messages "$TMP/scan1.txt"' "ledscan sends only B0/90"

"$BIN" ledscan --dry-run --status both --from 10 --to 11 --value 01 --hold 0 --gap 0 > "$TMP/scan2.txt" 2>&1
check '[ "$(grep -c "\[dry-run\]" "$TMP/scan2.txt")" -eq 8 ]' "ledscan --status both sends 8"
check 'grep -q "\[dry-run\] 90 11 01 (not sent)" "$TMP/scan2.txt"' "ledscan --value 01 on notes"
check 'only_led_messages "$TMP/scan2.txt"' "ledscan both: only B0/90"

"$BIN" ledscan --dry-run --all-off --from 00 --to 7F > "$TMP/scan3.txt" 2>&1
check '[ "$(grep -c "\[dry-run\] B0 .. 00 (not sent)" "$TMP/scan3.txt")" -eq 128 ]' "ledscan --all-off sends 128 OFFs"
check '! grep -q "7F (not sent)" "$TMP/scan3.txt"' "ledscan --all-off sends no ON"

"$BIN" ledscan --dry-run --only B0:09,C0:01 > "$TMP/scan4.txt" 2>&1
check '[ $? -eq 2 ]' "ledscan rejects a C0 address"
check '! grep -q "dry-run\]" "$TMP/scan4.txt"' "ledscan rejected list sends nothing"
"$BIN" ledscan --dry-run --value 80 > /dev/null 2>&1
check '[ $? -eq 2 ]' "ledscan rejects --value 80"
"$BIN" ledscan --dry-run --status c0 > /dev/null 2>&1
check '[ $? -eq 2 ]' "ledscan rejects --status c0"

"$BIN" ledscan --dry-run --only B0:09,B0:08,B0:0A --script "$FIX/ledscan-answers.txt" \
    --out "$TMP/ledmap.json" --gap 0 > "$TMP/scan5.txt" 2>&1
check '[ $? -eq 0 ]' "ledscan interactive script exit status"
check '[ "$(grep -c "^\[B0 08\] ON" "$TMP/scan5.txt")" -eq 2 ]' "ledscan r repeats an address"
check '[ "$(json "$TMP/ledmap.json" "len(d[\"results\"])")" = 2 ]' "ledmap has 2 results (q stops before B0 0A is recorded)"
check '[ "$(json "$TMP/ledmap.json" "\",\".join(d[\"results\"][0][\"controls\"])")" = deckA.play ]' "ledmap records the control id"
check '[ "$(json "$TMP/ledmap.json" "d[\"results\"][0][\"dry_run\"]")" = True ]' "ledmap marks dry runs"
check 'grep -q "\[dry-run\] B0 0A 00" "$TMP/scan5.txt"' "ledscan q still turns the LED off"
check 'only_led_messages "$TMP/scan5.txt"' "ledscan interactive: only B0/90"

printf 'y\n' > "$TMP/inv-answers.txt"
"$BIN" ledscan --dry-run --inventory "$FIX/inventory.json" --script "$TMP/inv-answers.txt" \
    --out "$TMP/ledmap-inv.json" --gap 0 > "$TMP/scan6.txt" 2>&1
check 'grep -q "expected: Play (Deck A)" "$TMP/scan6.txt"' "ledscan --inventory shows the expected control"
check '[ "$(json "$TMP/ledmap-inv.json" "\",\".join(d[\"results\"][0][\"controls\"])")" = deckA.play ]' "ledscan y confirms the expected control"

# ------------------------------------------------------------------ learn

OUT="$TMP/learned.json"
"$BIN" learn "$FIX/inventory.json" "$OUT" --replay "$FIX/replay1.txt" --script "$FIX/script1.txt" \
    --leds --dry-run > "$TMP/learn1.txt" 2>&1
check '[ $? -eq 0 ]' "learn replay exit status"
check 'grep -q "  > r" "$TMP/learn1.txt"' "learn echoes script answers"
check 'only_led_messages "$TMP/learn1.txt"' "learn --leds --dry-run only prints B0/90"
check 'grep -q "\[dry-run\] B0 09 7F (not sent)" "$TMP/learn1.txt"' "learn --leds lights the expected LED"
check '[ "$(json "$OUT" "\",\".join(d[\"learn\"][\"background\"])")" = B0:6F ]' "baseline records background"
p() { json "$OUT" "[c for c in d[\"controls\"] if c[\"id\"]==\"$1\"][0][\"learned\"]$2"; }
check '[ "$(p deckA.play "[\"verified_input\"]")" = True ]' "play verified"
check '[ "$(p deckA.play "[\"inputs\"][0][\"class\"]")" = note_pair ]' "play is a note pair"
check '[ "$(p deckA.play "[\"inputs\"][0][\"presses\"]")" = 1 ]' "redo discarded the first attempt"
check '[ "$(json "$OUT" "\",\".join([c for c in d[\"controls\"] if c[\"id\"]==\"deckA.play\"][0][\"learned\"][\"sample\"])")" = "90 11 7F,90 11 00" ]' "sample excludes background"
check '[ "$(p deckA.play "[\"verified_led\"]")" = False ]' "dry-run LED check never verifies"
check '[ "$(p deckA.pitchFader "[\"inputs\"][0][\"pair_role\"]")" = msb ]' "pitch fader 14-bit MSB"
check '[ "$(p deckA.pitchFader "[\"inputs\"][1][\"key\"]")" = B0:24 ]' "pitch fader 14-bit LSB"
check '[ "$(p deckA.platterRotation "[\"inputs\"][1][\"class\"]")" = pitchbend_14bit ]' "platter pitch bend"
check '[ "$(p browse.scrollKnob "[\"inputs\"][0][\"class\"]")" = cc_relative_2c ]' "encoder relative"
check '[ "$(p deckA.bleepReverse "[\"status\"]")" = skipped ]' "skip recorded"
check '[ "$(json "$OUT" "\",\".join(d[\"learn\"][\"sweep\"][\"unassigned\"])")" = B0:77 ]' "sweep finds the unassigned key"
check '[ "$(json "$OUT" "\",\".join(c[\"id\"] for c in d[\"controls\"] if \"learned\" in c)")" = deckA.play,deckA.pitchFader,deckA.platterRotation,deckA.bleepReverse,browse.scrollKnob ]' "led-only and non-MIDI items are not captured"

"$BIN" verify "$FIX/inventory.json" "$OUT" --md "$TMP/map1.md" > "$TMP/verify1.txt" 2>&1
check '[ $? -eq 1 ]' "verify exits 1 while incomplete"
check 'grep -q "Inputs verified: 4/5" "$TMP/verify1.txt"' "verify counts inputs"
check 'grep -q "B0 77" "$TMP/verify1.txt"' "verify lists the unassigned key"

# Resume from the skipped control; the earlier results stay.
"$BIN" learn "$FIX/inventory.json" "$OUT" --replay "$FIX/replay2.txt" --script "$FIX/script2.txt" \
    --from deckA.bleepReverse > "$TMP/learn2.txt" 2>&1
check '[ $? -eq 0 ]' "learn resume exit status"
check '[ "$(p deckA.bleepReverse "[\"verified_input\"]")" = True ]' "resume verifies the skipped control"
check '[ "$(p deckA.play "[\"verified_input\"]")" = True ]' "resume keeps earlier results"
check 'grep -q "\[1/1\] deckA.bleepReverse" "$TMP/learn2.txt"' "--from starts at the given control"

"$BIN" verify "$FIX/inventory.json" "$OUT" "$TMP/ledmap-inv.json" --md "$TMP/map2.md" > "$TMP/verify2.txt" 2>&1
check '[ $? -eq 1 ]' "verify still exits 1 (unassigned B0 77, dry-run LEDs)"
check 'grep -q "Inputs verified: 5/5" "$TMP/verify2.txt"' "verify sees all inputs"
check 'grep -q "LEDs verified: 0/2" "$TMP/verify2.txt"' "dry-run ledmap results do not verify LEDs"
check 'grep -q "| Play (Deck A) \`deckA.play\` |" "$TMP/map2.md"' "markdown table row"

"$BIN" learn "$FIX/inventory.json" "$TMP/x.json" --from no.such.id --replay "$FIX/replay2.txt" --script "$FIX/script2.txt" > /dev/null 2>&1
check '[ $? -eq 2 ]' "learn rejects an unknown --from id"

echo "self-test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
