/**
 * Numark NS7 (DriverKit) - Mixxx 2.5 controller script
 *
 * For the original Numark NS7 driven by the NumarkNS7 macOS DriverKit
 * driver, which exposes the CoreMIDI device "Numark USB Audio Device"
 * (port "MIDI").
 *
 * Derived from the "Numark NS7" mapping shipped with Mixxx
 * (Numark-NS7-scripts.js v1.9.0, Copyright (C) 2010 Anders Gunnarsson,
 * GPLv2 or later). Scratch handling follows the approach of the Mixxx
 * "Numark V7" mapping (engine.scratchEnable / scratchTick / scratchDisable).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* global engine, midi, print */
/* exported NumarkNS7DK */

var NumarkNS7DK = {};

// ---------------------------------------------------------------------------
// Configuration constants
// ---------------------------------------------------------------------------

/**
 * Platter encoder resolution: jog CC ticks per full platter revolution.
 * The jog CC (0xB0 0x00 deck A, 0xB0 0x02 deck B) is a 7-bit wrap-around
 * counter. 3600 is an initial guess from a single forum report; calibrate it
 * during the guided capture by turning the platter exactly one revolution
 * and summing the deltas (see NumarkNS7DK.DEBUG_JOG).
 */
NumarkNS7DK.INTERVALS_PER_REV = 3600;

/** Platter speed of the virtual record, in RPM. */
NumarkNS7DK.RPM = 33 + 1 / 3;

/** Scratch filter constants (as recommended in the Mixxx scripting docs). */
NumarkNS7DK.SCRATCH_ALPHA = 1.0 / 8;
NumarkNS7DK.SCRATCH_BETA = NumarkNS7DK.SCRATCH_ALPHA / 32;

/** Set to -1 if the platter turns the track the wrong way. (Unverified.) */
NumarkNS7DK.JOG_DIRECTION = 1;

/** Scale applied to jog deltas in nudge (non-scratch) mode. (To be tuned.) */
NumarkNS7DK.JOG_NUDGE_SCALE = 0.1;

/**
 * Set to true once platterTouch() is bound to the platter-touch messages in
 * the XML. While false, scratch mode is entered/left with the Scratch button
 * and, while it is on, all platter movement scratches.
 */
NumarkNS7DK.PLATTER_TOUCH_AVAILABLE = false;

/** Print every jog delta to the Mixxx log (for calibration). */
NumarkNS7DK.DEBUG_JOG = false;

/** Pitch range steps cycled by the "rate range" button. */
NumarkNS7DK.RATE_RANGES = [0.08, 0.10, 0.30, 1.00];

/**
 * VU meter output (0xB0 0x36, from the 2010 mapping; unverified). Disabled by
 * default because it sends a continuous message stream over MIDI OUT.
 */
NumarkNS7DK.ENABLE_VU_METER = false;

// ---------------------------------------------------------------------------
// LED map. All NS7 LEDs are Control Change on status 0xB0,
// value 0x7F = on, 0x00 = off.
// ---------------------------------------------------------------------------

NumarkNS7DK.LED_STATUS = 0xB0;
NumarkNS7DK.LED_ON = 0x7F;
NumarkNS7DK.LED_OFF = 0x00;

NumarkNS7DK.LEDS = {
    "[Channel1]": {
        play: 0x09,         // verified on hardware
        cue: 0x08,          // verified on hardware
        pfl: 0x14,          // from the 2010 mapping, unverified
        endOfTrack: 0x3B,   // from the 2010 mapping, unverified
        scratch: null,      // unknown - fill in after the guided capture
    },
    "[Channel2]": {
        play: 0x1F,         // verified on hardware
        cue: 0x1E,          // verified on hardware
        pfl: 0x18,          // from the 2010 mapping, unverified
        endOfTrack: 0x53,   // from the 2010 mapping, unverified
        scratch: null,      // unknown - fill in after the guided capture
    },
};

NumarkNS7DK.VU_METER_CC = 0x36; // from the 2010 mapping, unverified

// ---------------------------------------------------------------------------
// Per-deck state
// ---------------------------------------------------------------------------

NumarkNS7DK.decks = {
    "[Channel1]": {number: 1, scratchMode: false, touched: false, lastJog: null, endOfTrackLit: false},
    "[Channel2]": {number: 2, scratchMode: false, touched: false, lastJog: null, endOfTrackLit: false},
};

NumarkNS7DK.connections = [];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

NumarkNS7DK.setLed = function(cc, on) {
    if (cc === null || cc === undefined) {
        return;
    }
    midi.sendShortMsg(NumarkNS7DK.LED_STATUS, cc,
        on ? NumarkNS7DK.LED_ON : NumarkNS7DK.LED_OFF);
};

NumarkNS7DK.allLedsOff = function() {
    var group, leds, name;
    for (group in NumarkNS7DK.LEDS) {
        leds = NumarkNS7DK.LEDS[group];
        for (name in leds) {
            NumarkNS7DK.setLed(leds[name], false);
        }
    }
    if (NumarkNS7DK.ENABLE_VU_METER) {
        midi.sendShortMsg(NumarkNS7DK.LED_STATUS, NumarkNS7DK.VU_METER_CC, 0x00);
    }
};

NumarkNS7DK.connectLed = function(group, key, cc) {
    var conn = engine.makeConnection(group, key, function(value) {
        NumarkNS7DK.setLed(cc, value > 0.5);
    });
    if (conn) {
        conn.trigger();
        NumarkNS7DK.connections.push(conn);
    }
};

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

NumarkNS7DK.init = function(_id, _debugging) {
    NumarkNS7DK.allLedsOff();

    var group, leds, deck, conn;
    for (group in NumarkNS7DK.LEDS) {
        leds = NumarkNS7DK.LEDS[group];
        NumarkNS7DK.connectLed(group, "play_indicator", leds.play);
        NumarkNS7DK.connectLed(group, "cue_indicator", leds.cue);
        NumarkNS7DK.connectLed(group, "pfl", leds.pfl);
        NumarkNS7DK.connectEndOfTrack(group);

        deck = NumarkNS7DK.decks[group];
        deck.scratchMode = false;
        deck.touched = false;
        deck.lastJog = null;
        NumarkNS7DK.setLed(leds.scratch, false);
    }

    if (NumarkNS7DK.ENABLE_VU_METER) {
        conn = engine.makeConnection("[Main]", "vu_meter", function(value) {
            midi.sendShortMsg(NumarkNS7DK.LED_STATUS, NumarkNS7DK.VU_METER_CC,
                Math.round(Math.min(Math.max(value, 0), 1) * 0x7F));
        });
        if (conn) {
            NumarkNS7DK.connections.push(conn);
        }
    }
};

NumarkNS7DK.shutdown = function() {
    var i, group;
    for (i = 0; i < NumarkNS7DK.connections.length; i++) {
        NumarkNS7DK.connections[i].disconnect();
    }
    NumarkNS7DK.connections = [];

    for (group in NumarkNS7DK.decks) {
        if (engine.isScratching(NumarkNS7DK.decks[group].number)) {
            engine.scratchDisable(NumarkNS7DK.decks[group].number, false);
        }
    }

    NumarkNS7DK.allLedsOff();
};

/**
 * End-of-track warning LED (the 2010 mapping lit CC 0x3B / 0x53 while
 * playposition was between 0.90 and 0.99). Only sends on state changes,
 * because playposition updates very frequently.
 */
NumarkNS7DK.connectEndOfTrack = function(group) {
    var deck = NumarkNS7DK.decks[group];
    var cc = NumarkNS7DK.LEDS[group].endOfTrack;
    var conn = engine.makeConnection(group, "playposition", function(value) {
        var lit = value >= 0.90 && value <= 0.99;
        if (lit !== deck.endOfTrackLit) {
            deck.endOfTrackLit = lit;
            NumarkNS7DK.setLed(cc, lit);
        }
    });
    if (conn) {
        NumarkNS7DK.connections.push(conn);
    }
};

// ---------------------------------------------------------------------------
// Rate range button (0x90 0x1A deck A, 0x90 0x3B deck B)
// The 2010 script cycled an internal variable but never told Mixxx; this one
// sets [ChannelN],rateRange.
// ---------------------------------------------------------------------------

NumarkNS7DK.rateRange = function(channel, control, value, status, group) {
    if (value === 0) {
        return;
    }
    var ranges = NumarkNS7DK.RATE_RANGES;
    var current = engine.getValue(group, "rateRange");
    var next = ranges[0];
    var i;
    for (i = 0; i < ranges.length; i++) {
        if (ranges[i] > current + 0.0001) {
            next = ranges[i];
            break;
        }
    }
    engine.setValue(group, "rateRange", next);
};

// ---------------------------------------------------------------------------
// Scratch mode button (0x90 0x21 deck A, 0x90 0x42 deck B)
// Fixes the 2010 bug where the LED was always set on: the LED now follows
// the mode. (The scratch LED CC is still unknown, so LEDS.*.scratch = null.)
// ---------------------------------------------------------------------------

NumarkNS7DK.scratchButton = function(channel, control, value, status, group) {
    if (value === 0) {
        return;
    }
    var deck = NumarkNS7DK.decks[group];
    deck.scratchMode = !deck.scratchMode;
    NumarkNS7DK.setLed(NumarkNS7DK.LEDS[group].scratch, deck.scratchMode);

    if (!deck.scratchMode) {
        NumarkNS7DK.stopScratch(deck);
    } else if (!NumarkNS7DK.PLATTER_TOUCH_AVAILABLE) {
        // No touch sensor: the platter is "held" for as long as scratch mode
        // is on, so start scratching right away.
        NumarkNS7DK.startScratch(deck);
    }
};

NumarkNS7DK.startScratch = function(deck) {
    if (!engine.isScratching(deck.number)) {
        engine.scratchEnable(deck.number, NumarkNS7DK.INTERVALS_PER_REV,
            NumarkNS7DK.RPM, NumarkNS7DK.SCRATCH_ALPHA,
            NumarkNS7DK.SCRATCH_BETA, true);
    }
};

NumarkNS7DK.stopScratch = function(deck) {
    if (engine.isScratching(deck.number)) {
        engine.scratchDisable(deck.number, true);
    }
};

/**
 * Platter touch handler - NOT BOUND YET.
 *
 * Once the guided capture identifies the platter-touch messages, add
 * <control> entries in the XML with <key>NumarkNS7DK.platterTouch</key>
 * (Script-Binding) for each deck and set PLATTER_TOUCH_AVAILABLE = true.
 * Then, with scratch mode on, touching the platter scratches and releasing it
 * hands playback back to the engine (ramping to normal speed).
 */
NumarkNS7DK.platterTouch = function(channel, control, value, status, group) {
    var deck = NumarkNS7DK.decks[group];
    if (!deck) {
        return;
    }
    // Note-off (0x8n) or value 0 = released.
    var touched = (status & 0xF0) !== 0x80 && value > 0;
    deck.touched = touched;
    if (touched && deck.scratchMode) {
        NumarkNS7DK.startScratch(deck);
    } else if (!touched) {
        NumarkNS7DK.stopScratch(deck);
    }
};

// ---------------------------------------------------------------------------
// Platter / jog (0xB0 0x00 deck A, 0xB0 0x02 deck B)
// 7-bit wrap-around encoder: the delta is the signed difference from the
// previous value, folded into -64..+63 to handle the 0x7F -> 0x00 wrap.
// ---------------------------------------------------------------------------

NumarkNS7DK.jogDelta = function(deck, value) {
    if (deck.lastJog === null) {
        deck.lastJog = value;
        return 0;
    }
    var delta = value - deck.lastJog;
    deck.lastJog = value;
    if (delta > 63) {
        delta -= 128;
    } else if (delta < -64) {
        delta += 128;
    }
    return delta * NumarkNS7DK.JOG_DIRECTION;
};

NumarkNS7DK.jog = function(channel, control, value, status, group) {
    var deck = NumarkNS7DK.decks[group];
    var delta = NumarkNS7DK.jogDelta(deck, value);
    if (NumarkNS7DK.DEBUG_JOG) {
        print("NumarkNS7DK " + group + " jog value=" + value + " delta=" + delta);
    }
    if (delta === 0) {
        return;
    }

    if (deck.scratchMode &&
        (deck.touched || !NumarkNS7DK.PLATTER_TOUCH_AVAILABLE)) {
        // Scratch mode can be left enabled while the engine dropped it (e.g.
        // after loading a track); re-enable before ticking.
        NumarkNS7DK.startScratch(deck);
        engine.scratchTick(deck.number, delta);
    } else {
        // Nudge / pitch bend.
        engine.setValue(group, "jog", delta * NumarkNS7DK.JOG_NUDGE_SCALE);
    }
};

// ---------------------------------------------------------------------------
// TODO - messages seen on hardware but not yet identified (guided capture):
//
//   0xB0 0x24 xx   unknown (continuous value)
//   0xB0 0x30 0x62 unknown
//   0xB0 0x6E 0x00 unknown
//   0xE0 lsb msb   pitch-bend stream while deck A platter spins
//   0xE2 lsb msb   pitch-bend stream while deck B platter spins
//                  (believed to be a platter timing/speed companion to the
//                  jog CC, not an absolute position)
//   platter touch  not yet seen -> bind NumarkNS7DK.platterTouch
//   scratch LED    CC unknown -> set LEDS["[ChannelN]"].scratch
//   pitch fader    0xB0 0x04 / 0x05 mapped as 7-bit; may have a 14-bit LSB
//   PFL buttons    input messages unknown (only the LEDs are mapped)
// ---------------------------------------------------------------------------
