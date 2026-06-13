/*
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_TEXT_VERBOSE = 0,
    ANN_TEXT_SYSREAL_VERBOSE,
    ANN_TEXT_ERROR,
    NUM_ANN,
};

enum midi_state {
    MIDI_IDLE,
    MIDI_HANDLE_CHANNEL_MSG,
    MIDI_HANDLE_SYSEX_MSG,
    MIDI_HANDLE_SYSCOMMON_MSG,
    MIDI_HANDLE_SYSREALTIME_MSG,
    MIDI_BUFFER_GARBAGE,
};

typedef struct {
    enum midi_state state;
    uint8_t status_byte;
    int explicit_status_byte;
    uint8_t cmd[512];
    int cmd_len;
    uint64_t ss;
    uint64_t es;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
} midi_state;

static const char *midi_inputs[] = {"uart", NULL};
static const char *midi_tags[] = {"Audio", "PC", NULL};

static const char *midi_ann_labels[][3] = {
    {"", "text-verbose", "Human-readable text (verbose)"},
    {"", "text-sysreal-verbose", "Human-readable SysReal text (verbose)"},
    {"", "text-error", "Human-readable Error text"},
};

static const int midi_row_normal_classes[] = {ANN_TEXT_VERBOSE, ANN_TEXT_ERROR, -1};
static const int midi_row_sysreal_classes[] = {ANN_TEXT_SYSREAL_VERBOSE, -1};

static const struct srd_c_ann_row midi_ann_rows[] = {
    {"normal", "Normal", midi_row_normal_classes, 2},
    {"sys-real", "SysReal", midi_row_sysreal_classes, 1},
};

/* Status byte names: indexed by (byte >> 4) for 0x80-0xE0, and 0xF0-0xFF */
static const char *status_bytes[][3] = {
    /* 0x80 */ {"note off", "note off", "N off"},
    /* 0x90 */ {"note on", "note on", "N on"},
    /* 0xA0 */ {"polyphonic key pressure / aftertouch", "key pressure", "KP"},
    /* 0xB0 */ {"control change", "ctrl chg", "CC"},
    /* 0xC0 */ {"program change", "prgm chg", "PC"},
    /* 0xD0 */ {"channel pressure / aftertouch", "channel pressure", "CP"},
    /* 0xE0 */ {"pitch bend change", "pitch bend", "PB"},
    /* 0xF0 */ {"system exclusive", "SysEx", "SE"},
    /* 0xF1 */ {"MIDI time code quarter frame", "MIDI time code", "MIDI time"},
    /* 0xF2 */ {"song position pointer", "song position", "song pos"},
    /* 0xF3 */ {"song select", "song select", "song sel"},
    /* 0xF4 */ {"undefined 0xf4", "undef 0xf4", "undef"},
    /* 0xF5 */ {"undefined 0xf5", "undef 0xf5", "undef"},
    /* 0xF6 */ {"tune request", "tune request", "tune req"},
    /* 0xF7 */ {"end of system exclusive (EOX)", "end of SysEx", "EOX"},
    /* 0xF8 */ {"timing clock", "timing clock", "clock"},
    /* 0xF9 */ {"undefined 0xf9", "undef 0xf9", "undef"},
    /* 0xFA */ {"start", "start", "s"},
    /* 0xFB */ {"continue", "continue", "cont"},
    /* 0xFC */ {"stop", "stop", "st"},
    /* 0xFD */ {"undefined 0xfd", "undef 0xfd", "undef"},
    /* 0xFE */ {"active sensing", "active sensing", "sensing"},
    /* 0xFF */ {"system reset", "reset", "rst"},
};

/* Get status_bytes index from MIDI byte */
static int status_index(uint8_t byte)
{
    if (byte >= 0x80 && byte <= 0xEF)
        return (byte >> 4) - 8; /* 0x80->0, 0x90->1, ... 0xE0->6 */
    if (byte >= 0xF0)
        return 7 + (byte - 0xF0); /* 0xF0->7, 0xF1->8, ... 0xFF->22 */
    return -1;
}

/* Control function names */
static const char *control_functions[][3] = {
    /* 0x00 */ {"bank select MSB", "bank MSB", "bank-M"},
    /* 0x01 */ {"modulation wheel/lever MSB", "modulation MSB", "mod-M"},
    /* 0x02 */ {"breath controller MSB", "breath MSB", "breath-M"},
    /* 0x03 */ {"undefined MSB", "undef MSB", "undef-M"},
    /* 0x04 */ {"foot controller MSB", "foot MSB", "foot-M"},
    /* 0x05 */ {"portamento time MSB", "portamento MSB", "porta-M"},
    /* 0x06 */ {"data entry MSB", "data entry MSB", "data-M"},
    /* 0x07 */ {"channel volume MSB", "channel volume MSB", "ch vol-M"},
    /* 0x08 */ {"balance MSB", "bal MSB", "bal-M"},
    /* 0x09 */ {"undefined MSB", "undef MSB", "undef-M"},
    /* 0x0A */ {"pan MSB", "pan MSB", "pan-M"},
    /* 0x0B */ {"expression controller MSB", "expression MSB", "expr-M"},
    /* 0x0C */ {"effect control 1 MSB", "effect 1 MSB", "eff-1-M"},
    /* 0x0D */ {"effect control 2 MSB", "effect 2 MSB", "eff-2-M"},
    /* 0x0E */ {"undefined MSB", "undef MSB", "undef-M"},
    /* 0x0F */ {"undefined MSB", "undef MSB", "undef-M"},
    /* 0x10 */ {"general purpose controller 1 MSB", "GP ctrl 1 MSB", "GPC-1-M"},
    /* 0x11 */ {"general purpose controller 2 MSB", "GP ctrl 2 MSB", "GPC-2-M"},
    /* 0x12 */ {"general purpose controller 3 MSB", "GP ctrl 3 MSB", "GPC-3-M"},
    /* 0x13 */ {"general purpose controller 4 MSB", "GP ctrl 4 MSB", "GPC-4-M"},
    /* 0x14-0x1F undefined MSB */
    /* 0x20 */ {"bank select LSB", "bank LSB", "bank-L"},
    /* 0x21 */ {"modulation wheel/lever LSB", "modulation LSB", "mod-L"},
    /* 0x22 */ {"breath controller LSB", "breath LSB", "breath-L"},
    /* 0x23 */ {"undefined LSB", "undef LSB", "undef-L"},
    /* 0x24 */ {"foot controller LSB", "foot LSB", "foot-L"},
    /* 0x25 */ {"portamento time LSB", "portamento LSB", "porta-L"},
    /* 0x26 */ {"data entry LSB", "data entry LSB", "data-L"},
    /* 0x27 */ {"channel volume LSB", "channel volume LSB", "ch vol-L"},
    /* 0x28 */ {"balance LSB", "bal LSB", "bal-L"},
    /* 0x29 */ {"undefined LSB", "undef LSB", "undef-L"},
    /* 0x2A */ {"pan LSB", "pan LSB", "pan-L"},
    /* 0x2B */ {"expression controller LSB", "expression LSB", "expr-L"},
    /* 0x2C */ {"effect control 1 LSB", "effect 1 LSB", "eff-1-L"},
    /* 0x2D */ {"effect control 2 LSB", "effect 2 LSB", "eff-2-L"},
    /* 0x2E */ {"undefined LSB", "undef LSB", "undef-L"},
    /* 0x2F */ {"undefined LSB", "undef LSB", "undef-L"},
    /* 0x30 */ {"general purpose controller 1 LSB", "GP ctrl 1 LSB", "GPC-1-L"},
    /* 0x31 */ {"general purpose controller 2 LSB", "GP ctrl 2 LSB", "GPC-2-L"},
    /* 0x32 */ {"general purpose controller 3 LSB", "GP ctrl 3 LSB", "GPC-3-L"},
    /* 0x33 */ {"general purpose controller 4 LSB", "GP ctrl 4 LSB", "GPC-4-L"},
    /* 0x34-0x3F undefined LSB */
    /* 0x40 */ {"damper pedal (sustain)", "sustain", "sust"},
    /* 0x41 */ {"portamento on/off", "porta on/off", "porta on/off"},
    /* 0x42 */ {"sostenuto", "sostenuto", "sostenuto"},
    /* 0x43 */ {"soft pedal", "soft pedal", "soft pedal"},
    /* 0x44 */ {"legato footswitch", "legato switch", "legato"},
    /* 0x45 */ {"hold 2", "hold 2", "hold 2"},
    /* 0x46 */ {"sound controller 1", "sound ctrl 1", "snd ctrl 1"},
    /* 0x47 */ {"sound controller 2", "sound ctrl 2", "snd ctrl 2"},
    /* 0x48 */ {"sound controller 3", "sound ctrl 3", "snd ctrl 3"},
    /* 0x49 */ {"sound controller 4", "sound ctrl 4", "snd ctrl 4"},
    /* 0x4A */ {"sound controller 5", "sound ctrl 5", "snd ctrl 5"},
    /* 0x4B */ {"sound controller 6", "sound ctrl 6", "snd ctrl 6"},
    /* 0x4C */ {"sound controller 7", "sound ctrl 7", "snd ctrl 7"},
    /* 0x4D */ {"sound controller 8", "sound ctrl 8", "snd ctrl 8"},
    /* 0x4E */ {"sound controller 9", "sound ctrl 9", "snd ctrl 9"},
    /* 0x4F */ {"sound controller 10", "sound ctrl 10", "snd ctrl 10"},
    /* 0x50 */ {"general purpose controller 5", "GP controller 5", "GPC-5"},
    /* 0x51 */ {"general purpose controller 6", "GP controller 6", "GPC-6"},
    /* 0x52 */ {"general purpose controller 7", "GP controller 7", "GPC-7"},
    /* 0x53 */ {"general purpose controller 8", "GP controller 8", "GPC-8"},
    /* 0x54 */ {"portamento control", "portamento ctrl", "porta ctrl"},
    /* 0x55-0x5A undefined */
    /* 0x5B */ {"effects 1 depth", "effects 1 depth", "eff 1 depth"},
    /* 0x5C */ {"effects 2 depth", "effects 2 depth", "eff 2 depth"},
    /* 0x5D */ {"effects 3 depth", "effects 3 depth", "eff 3 depth"},
    /* 0x5E */ {"effects 4 depth", "effects 4 depth", "eff 4 depth"},
    /* 0x5F */ {"effects 5 depth", "effects 5 depth", "eff 5 depth"},
    /* 0x60 */ {"data increment", "data inc", "data++"},
    /* 0x61 */ {"data decrement", "data dec", "data--"},
    /* 0x62 */ {"Non-Registered Parameter Number LSB", "NRPN LSB", "NRPN-L"},
    /* 0x63 */ {"Non-Registered Parameter Number MSB", "NRPN MSB", "NRPN-M"},
    /* 0x64 */ {"Registered Parameter Number LSB", "RPN LSB", "RPN-L"},
    /* 0x65 */ {"Registered Parameter Number MSB", "RPN MSB", "RPN-M"},
    /* 0x66-0x77 undefined */
    /* 0x78 */ {"all sound off", "all snd off", "snd off"},
    /* 0x79 */ {"reset all controllers", "reset all ctrls", "reset ctrls"},
    /* 0x7A */ {"local control", "local ctrl", "local ctrl"},
    /* 0x7B */ {"all notes off", "notes off", "notes off"},
    /* 0x7C */ {"omni mode off", "omni off", "omni off"},
    /* 0x7D */ {"omni mode on", "omni on", "omni on"},
    /* 0x7E */ {"mono mode on", "mono on", "mono"},
    /* 0x7F */ {"poly mode on", "poly on", "poly"},
};

/* Get control function name by CC number */
static const char *midi_get_ctrl_fn(uint8_t cc, int level)
{
    /* Handle undefined ranges */
    if ((cc >= 0x14 && cc <= 0x1F) || (cc >= 0x34 && cc <= 0x3F) ||
        (cc >= 0x55 && cc <= 0x5A) || (cc >= 0x66 && cc <= 0x77)) {
        static char buf[3][32];
        snprintf(buf[level], sizeof(buf[level]), "undefined 0x%02x", cc);
        return buf[level];
    }
    if (cc <= 0x7F)
        return control_functions[cc][level];
    return "undefined";
}

/* Chromatic note names */
static const char *chromatic_notes[] = {
    "C-1", "C#-1", "D-1", "D#-1", "E-1", "F-1", "F#-1", "G-1", "G#-1", "A-1", "A#-1", "B-1",
    "C0", "C#0", "D0", "D#0", "E0", "F0", "F#0", "G0", "G#0", "A0", "A#0", "B0",
    "C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1",
    "C2", "C#2", "D2", "D#2", "E2", "F2", "F#2", "G2", "G#2", "A2", "A#2", "B2",
    "C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3", "G#3", "A3", "A#3", "B3",
    "C4", "C#4", "D4", "D#4", "E4", "F4", "F#4", "G4", "G#4", "A4", "A#4", "B4",
    "C5", "C#5", "D5", "D#5", "E5", "F5", "F#5", "G5", "G#5", "A5", "A#5", "B5",
    "C6", "C#6", "D6", "D#6", "E6", "F6", "F#6", "G6", "G#6", "A6", "A#6", "B6",
    "C7", "C#7", "D7", "D#7", "E7", "F7", "F#7", "G7", "G#7", "A7", "A#7", "B7",
    "C8", "C#8", "D8", "D#8", "E8", "F8", "F#8", "G8", "G#8", "A8", "A#8", "B8",
    "C9", "C#9", "D9", "D#9", "E9", "F9", "F#9", "G9",
};

/* Percussion note names */
static const char *percussion_notes[] = {
    /* 35-81 */
    [35] = "Acoustic Bass Drum", [36] = "Bass Drum 1", [37] = "Side Stick",
    [38] = "Acoustic Snare", [39] = "Hand Clap", [40] = "Electric Snare",
    [41] = "Low Floor Tom", [42] = "Closed Hi Hat", [43] = "High Floor Tom",
    [44] = "Pedal Hi-Hat", [45] = "Low Tom", [46] = "Open Hi-Hat",
    [47] = "Low-Mid Tom", [48] = "Hi Mid Tom", [49] = "Crash Cymbal 1",
    [50] = "High Tom", [51] = "Ride Cymbal 1", [52] = "Chinese Cymbal",
    [53] = "Ride Bell", [54] = "Tambourine", [55] = "Splash Cymbal",
    [56] = "Cowbell", [57] = "Crash Cymbal 2", [58] = "Vibraslap",
    [59] = "Ride Cymbal 2", [60] = "Hi Bongo", [61] = "Low Bongo",
    [62] = "Mute Hi Conga", [63] = "Open Hi Conga", [64] = "Low Conga",
    [65] = "High Timbale", [66] = "Low Timbale", [67] = "High Agogo",
    [68] = "Low Agogo", [69] = "Cabasa", [70] = "Maracas",
    [71] = "Short Whistle", [72] = "Long Whistle", [73] = "Short Guiro",
    [74] = "Long Guiro", [75] = "Claves", [76] = "Hi Wood Block",
    [77] = "Low Wood Block", [78] = "Mute Cuica", [79] = "Open Cuica",
    [80] = "Mute Triangle", [81] = "Open Triangle",
};

/* GM instruments */
static const char *gm_instruments[] = {
    [1] = "Acoustic Grand Piano", [2] = "Bright Acoustic Piano",
    [3] = "Electric Grand Piano", [4] = "Honky-tonk Piano",
    [5] = "Electric Piano 1", [6] = "Electric Piano 2",
    [7] = "Harpsichord", [8] = "Clavi", [9] = "Celesta",
    [10] = "Glockenspiel", [11] = "Music Box", [12] = "Vibraphone",
    [13] = "Marimba", [14] = "Xylophone", [15] = "Tubular Bells",
    [16] = "Dulcimer", [17] = "Drawbar Organ", [18] = "Percussive Organ",
    [19] = "Rock Organ", [20] = "Church Organ", [21] = "Reed Organ",
    [22] = "Accordion", [23] = "Harmonica", [24] = "Tango Accordion",
    [25] = "Acoustic Guitar (nylon)", [26] = "Acoustic Guitar (steel)",
    [27] = "Electric Guitar (jazz)", [28] = "Electric Guitar (clean)",
    [29] = "Electric Guitar (muted)", [30] = "Overdriven Guitar",
    [31] = "Distortion Guitar", [32] = "Guitar harmonics",
    [33] = "Acoustic Bass", [34] = "Electric Bass (finger)",
    [35] = "Electric Bass (pick)", [36] = "Fretless Bass",
    [37] = "Slap Bass 1", [38] = "Slap Bass 2",
    [39] = "Synth Bass 1", [40] = "Synth Bass 2",
    [41] = "Violin", [42] = "Viola", [43] = "Cello",
    [44] = "Contrabass", [45] = "Tremolo Strings",
    [46] = "Pizzicato Strings", [47] = "Orchestral Harp",
    [48] = "Timpani", [49] = "String Ensemble 1",
    [50] = "String Ensemble 2", [51] = "SynthStrings 1",
    [52] = "SynthStrings 2", [53] = "Choir Aahs",
    [54] = "Voice Oohs", [55] = "Synth Voice",
    [56] = "Orchestra Hit", [57] = "Trumpet",
    [58] = "Trombone", [59] = "Tuba", [60] = "Muted Trumpet",
    [61] = "French Horn", [62] = "Brass Section",
    [63] = "SynthBrass 1", [64] = "SynthBrass 2",
    [65] = "Soprano Sax", [66] = "Alto Sax",
    [67] = "Tenor Sax", [68] = "Baritone Sax",
    [69] = "Oboe", [70] = "English Horn",
    [71] = "Bassoon", [72] = "Clarinet",
    [73] = "Piccolo", [74] = "Flute", [75] = "Recorder",
    [76] = "Pan Flute", [77] = "Blown Bottle",
    [78] = "Shakuhachi", [79] = "Whistle", [80] = "Ocarina",
    [81] = "Lead 1 (square)", [82] = "Lead 2 (sawtooth)",
    [83] = "Lead 3 (calliope)", [84] = "Lead 4 (chiff)",
    [85] = "Lead 5 (charang)", [86] = "Lead 6 (voice)",
    [87] = "Lead 7 (fifths)", [88] = "Lead 8 (bass + lead)",
    [89] = "Pad 1 (new age)", [90] = "Pad 2 (warm)",
    [91] = "Pad 3 (polysynth)", [92] = "Pad 4 (choir)",
    [93] = "Pad 5 (bowed)", [94] = "Pad 6 (metallic)",
    [95] = "Pad 7 (halo)", [96] = "Pad 8 (sweep)",
    [97] = "FX 1 (rain)", [98] = "FX 2 (soundtrack)",
    [99] = "FX 3 (crystal)", [100] = "FX 4 (atmosphere)",
    [101] = "FX 5 (brightness)", [102] = "FX 6 (goblins)",
    [103] = "FX 7 (echoes)", [104] = "FX 8 (sci-fi)",
    [105] = "Sitar", [106] = "Banjo", [107] = "Shamisen",
    [108] = "Koto", [109] = "Kalimba", [110] = "Bag pipe",
    [111] = "Fiddle", [112] = "Shanai", [113] = "Tinkle Bell",
    [114] = "Agogo", [115] = "Steel Drums", [116] = "Woodblock",
    [117] = "Taiko Drum", [118] = "Melodic Tom",
    [119] = "Synth Drum", [120] = "Reverse Cymbal",
    [121] = "Guitar Fret Noise", [122] = "Breath Noise",
    [123] = "Seashore", [124] = "Bird Tweet",
    [125] = "Telephone Ring", [126] = "Helicopter",
    [127] = "Applause", [128] = "Gunshot",
};

/* Drum kit names */
static const char *drum_kit[] = {
    [1] = "GM Standard Kit", [9] = "GS Room Kit",
    [17] = "GS Power Kit", [25] = "GS Power Kit",
    [26] = "GS TR-808 Kit", [33] = "GS Jazz Kit",
    [41] = "GS Brush Kit", [49] = "GS Orchestra Kit",
    [57] = "GS Sound FX Kit", [128] = "GS CM-64/CM-32 Kit",
};

/* Quarter frame type names */
static const char *quarter_frame_type[][2] = {
    {"frame count LS nibble", "frame LSN"},
    {"frame count MS nibble", "frame MSN"},
    {"seconds LS nibble", "sec LSN"},
    {"seconds MS nibble", "sec MSN"},
    {"minutes LS nibble", "min LSN"},
    {"minutes MS nibble", "min MSN"},
    {"hours LS nibble", "hrs LSN"},
    {"hours MS nibble and SMPTE type", "hrs MSN"},
};

/* SMPTE type names */
static const char *smpte_type[] = {
    "24 fps", "25 fps", "30 fps (drop-frame)", "30 fps (non-drop)",
};

/* SysEx manufacturer IDs - simplified lookup */
typedef struct {
    uint8_t bytes[3];
    int num_bytes;
    const char *name;
} sysex_manu_entry;

static const sysex_manu_entry sysex_manufacturers[] = {
    {{0x01}, 1, "Sequential"}, {{0x02}, 1, "IDP"},
    {{0x03}, 1, "Voyetra/Octave-Plateau"}, {{0x04}, 1, "Moog"},
    {{0x05}, 1, "Passport Designs"}, {{0x06}, 1, "Lexicon"},
    {{0x07}, 1, "Kurzweil"}, {{0x08}, 1, "Fender"},
    {{0x09}, 1, "Gulbransen"}, {{0x0A}, 1, "AKG Acoustics"},
    {{0x0B}, 1, "Voyce Music"}, {{0x0C}, 1, "Waveframe Corp"},
    {{0x0D}, 1, "ADA Signal Processors"}, {{0x0E}, 1, "Garfield Electronics"},
    {{0x0F}, 1, "Ensoniq"}, {{0x10}, 1, "Oberheim"},
    {{0x11}, 1, "Apple Computer"}, {{0x12}, 1, "Grey Matter Response"},
    {{0x13}, 1, "Digidesign"}, {{0x14}, 1, "Palm Tree Instruments"},
    {{0x15}, 1, "JLCooper Electronics"}, {{0x16}, 1, "Lowrey"},
    {{0x17}, 1, "Adams-Smith"}, {{0x18}, 1, "Emu Systems"},
    {{0x19}, 1, "Harmony Systems"}, {{0x1A}, 1, "ART"},
    {{0x1B}, 1, "Baldwin"}, {{0x1C}, 1, "Eventide"},
    {{0x1D}, 1, "Inventronics"}, {{0x1F}, 1, "Clarity"},
    {{0x20}, 1, "Passac"}, {{0x21}, 1, "SIEL"},
    {{0x22}, 1, "Synthaxe"}, {{0x24}, 1, "Hohner"},
    {{0x25}, 1, "Twister"}, {{0x26}, 1, "Solton"},
    {{0x27}, 1, "Jellinghaus MS"}, {{0x28}, 1, "Southworth Music Systems"},
    {{0x29}, 1, "PPG"}, {{0x2A}, 1, "JEN"},
    {{0x2B}, 1, "SSL Limited"}, {{0x2C}, 1, "Audio Veritrieb"},
    {{0x2F}, 1, "Elka"}, {{0x30}, 1, "Dynacord"},
    {{0x31}, 1, "Viscount"}, {{0x33}, 1, "Clavia Digital Instruments"},
    {{0x34}, 1, "Audio Architecture"}, {{0x35}, 1, "GeneralMusic Corp."},
    {{0x39}, 1, "Soundcraft Electronics"}, {{0x3B}, 1, "Wersi"},
    {{0x3C}, 1, "Avab Elektronik Ab"}, {{0x3D}, 1, "Digigram"},
    {{0x3E}, 1, "Waldorf Electronics"}, {{0x3F}, 1, "Quasimidi"},
    {{0x40}, 1, "Kawai"}, {{0x41}, 1, "Roland"},
    {{0x42}, 1, "Korg"}, {{0x43}, 1, "Yamaha"},
    {{0x44}, 1, "Casio"}, {{0x46}, 1, "Kamiya Studio"},
    {{0x47}, 1, "Akai"}, {{0x48}, 1, "Japan Victor"},
    {{0x49}, 1, "Mesosha"}, {{0x4A}, 1, "Hoshino Gakki"},
    {{0x4B}, 1, "Fujitsu Elect"}, {{0x4C}, 1, "Sony"},
    {{0x4D}, 1, "Nisshin Onpa"}, {{0x4E}, 1, "TEAC"},
    {{0x50}, 1, "Matsushita Electric"}, {{0x51}, 1, "Fostex"},
    {{0x52}, 1, "Zoom"}, {{0x53}, 1, "Midori Electronics"},
    {{0x54}, 1, "Matsushita Communication Industrial"},
    {{0x55}, 1, "Suzuki Musical Inst. Mfg."},
    {{0x7D}, 1, "Non-Commercial"},
    {{0x7E}, 1, "Universal Non-Realtime"},
    {{0x7F}, 1, "Universal Realtime"},
    {{0x00, 0x00, 0x01}, 3, "Time Warner Interactive"},
    {{0x00, 0x00, 0x07}, 3, "Digital Music Corp."},
    {{0x00, 0x00, 0x08}, 3, "IOTA Systems"},
    {{0x00, 0x00, 0x09}, 3, "New England Digital"},
    {{0x00, 0x00, 0x0A}, 3, "Artisyn"},
    {{0x00, 0x00, 0x0B}, 3, "IVL Technologies"},
    {{0x00, 0x00, 0x0C}, 3, "Southern Music Systems"},
    {{0x00, 0x00, 0x0D}, 3, "Lake Butler Sound Company"},
    {{0x00, 0x00, 0x0E}, 3, "Alesis"},
    {{0x00, 0x00, 0x10}, 3, "DOD Electronics"},
    {{0x00, 0x00, 0x11}, 3, "Studer-Editech"},
    {{0x00, 0x00, 0x14}, 3, "Perfect Fretworks"},
    {{0x00, 0x00, 0x15}, 3, "KAT"},
    {{0x00, 0x00, 0x16}, 3, "Opcode"},
    {{0x00, 0x00, 0x17}, 3, "Rane Corp."},
    {{0x00, 0x00, 0x18}, 3, "Anadi Inc."},
    {{0x00, 0x00, 0x19}, 3, "KMX"},
    {{0x00, 0x00, 0x1A}, 3, "Allen & Heath Brenell"},
    {{0x00, 0x00, 0x1B}, 3, "Peavy Electronics"},
    {{0x00, 0x00, 0x1C}, 3, "360 Systems"},
    {{0x00, 0x00, 0x1D}, 3, "Spectrum Design and Development"},
    {{0x00, 0x00, 0x1E}, 3, "Marquis Music"},
    {{0x00, 0x00, 0x1F}, 3, "Zeta Systems"},
    {{0x00, 0x00, 0x40}, 3, "Richmond Sound Design"},
    {{0x00, 0x00, 0x41}, 3, "Microsoft"},
    {{0x00, 0x00, 0x42}, 3, "The Software Toolworks"},
    {{0x00, 0x00, 0x43}, 3, "Niche/RJMG"},
    {{0x00, 0x00, 0x44}, 3, "Intone"},
    {{0x00, 0x00, 0x47}, 3, "GT Electronics / Groove Tubes"},
    {{0x00, 0x00, 0x49}, 3, "Timeline Vista"},
    {{0x00, 0x00, 0x4A}, 3, "Mesa Boogie"},
    {{0x00, 0x00, 0x4C}, 3, "Sequoia Development"},
    {{0x00, 0x00, 0x4D}, 3, "Studio Electronics"},
    {{0x00, 0x00, 0x4E}, 3, "Euphonix"},
    {{0x00, 0x00, 0x4F}, 3, "InterMIDI, Inc."},
    {{0x00, 0x00, 0x50}, 3, "MIDI Solutions"},
    {{0x00, 0x00, 0x51}, 3, "3DO Company"},
    {{0x00, 0x00, 0x52}, 3, "Lightwave Research"},
    {{0x00, 0x00, 0x53}, 3, "Micro-W"},
    {{0x00, 0x00, 0x54}, 3, "Spectral Synthesis"},
    {{0x00, 0x00, 0x55}, 3, "Lone Wolf"},
    {{0x00, 0x00, 0x56}, 3, "Studio Technologies"},
    {{0x00, 0x00, 0x57}, 3, "Peterson EMP"},
    {{0x00, 0x00, 0x58}, 3, "Atari"},
    {{0x00, 0x00, 0x59}, 3, "Marion Systems"},
    {{0x00, 0x00, 0x5A}, 3, "Design Event"},
    {{0x00, 0x00, 0x5B}, 3, "Winjammer Software"},
    {{0x00, 0x00, 0x5C}, 3, "AT&T Bell Labs"},
    {{0x00, 0x00, 0x5E}, 3, "Symetrix"},
    {{0x00, 0x00, 0x5F}, 3, "MIDI the World"},
    {{0x00, 0x20, 0x00}, 3, "Dream"},
    {{0x00, 0x20, 0x01}, 3, "Strand Lighting"},
    {{0x00, 0x20, 0x02}, 3, "Amek Systems"},
    {{0x00, 0x20, 0x04}, 3, "Boehm Electronic"},
    {{0x00, 0x20, 0x06}, 3, "Trident Audio"},
    {{0x00, 0x20, 0x07}, 3, "Real World Studio"},
    {{0x00, 0x20, 0x09}, 3, "Yes Technology"},
    {{0x00, 0x20, 0x0A}, 3, "Audiomatica"},
    {{0x00, 0x20, 0x0B}, 3, "Bontempi/Farfisa"},
    {{0x00, 0x20, 0x0C}, 3, "F.B.T. Elettronica"},
    {{0x00, 0x20, 0x0D}, 3, "MidiTemp"},
    {{0x00, 0x20, 0x0E}, 3, "LA Audio (Larking Audio)"},
    {{0x00, 0x20, 0x0F}, 3, "Zero 88 Lighting Limited"},
    {{0x00, 0x20, 0x20}, 3, "Doepfer Musikelektronik"},
    {{0x00, 0x20, 0x21}, 3, "Creative Technology Pte"},
    {{0x00, 0x20, 0x29}, 3, "Novation EMS"},
    {{0x00, 0x01, 0x01}, 3, "Crystalake Multimedia"},
    {{0x00, 0x01, 0x02}, 3, "Crystal Semiconductor"},
    {{0x00, 0x01, 0x03}, 3, "Rockwell Semiconductor"},
};

#define NUM_SYSEX_MANU (sizeof(sysex_manufacturers) / sizeof(sysex_manufacturers[0]))

static const char *midi_find_manufacturer(const uint8_t *bytes, int num_bytes)
{
    for (int i = 0; i < (int)NUM_SYSEX_MANU; i++) {
        if (sysex_manufacturers[i].num_bytes != num_bytes)
            continue;
        int match = 1;
        for (int j = 0; j < num_bytes; j++) {
            if (sysex_manufacturers[i].bytes[j] != bytes[j]) {
                match = 0;
                break;
            }
        }
        if (match)
            return sysex_manufacturers[i].name;
    }
    return NULL;
}

static const char *midi_get_note_name(int channel, int note)
{
    if (channel != 10) {
        if (note >= 0 && note < 128)
            return chromatic_notes[note];
        return "undefined";
    } else {
        if (note >= 35 && note <= 81 && percussion_notes[note])
            return percussion_notes[note];
        return "undefined";
    }
}

static void midi_putx(struct srd_decoder_inst *di, midi_state *s, int cls, const char *text)
{
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, text);
}

static enum midi_state midi_get_next_state(midi_state *s, uint8_t newbyte)
{
    if (newbyte >= 0x80 && newbyte <= 0xEF)
        return MIDI_HANDLE_CHANNEL_MSG;
    if (newbyte == 0xF0)
        return MIDI_HANDLE_SYSEX_MSG;
    if (newbyte >= 0xF1 && newbyte <= 0xF6)
        return MIDI_HANDLE_SYSCOMMON_MSG;
    if (newbyte >= 0xF8)
        return MIDI_HANDLE_SYSREALTIME_MSG;
    if (newbyte == 0xF7)
        return MIDI_BUFFER_GARBAGE;
    /* Running status */
    if (s->status_byte < 0x80)
        return MIDI_BUFFER_GARBAGE;
    return midi_get_next_state(s, s->status_byte);
}

static void midi_handle_garbage_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte_is_none)
{
    if (!newbyte_is_none) {
        if (s->cmd_len < (int)sizeof(s->cmd))
            s->cmd[s->cmd_len++] = (uint8_t)newbyte_is_none; /* just append */
        return;
    }
    s->es_block = s->es;
    char payload[256];
    int pos = 0;
    int max_bytes = 16;
    for (int i = 0; i < s->cmd_len && pos < 200; i++) {
        if (i == max_bytes) {
            pos += snprintf(payload + pos, sizeof(payload) - pos, " ...");
            break;
        }
        if (i == 0)
            pos += snprintf(payload + pos, sizeof(payload) - pos, "0x%02x", s->cmd[i]);
        else
            pos += snprintf(payload + pos, sizeof(payload) - pos, " 0x%02x", s->cmd[i]);
    }
    if (s->cmd_len == 0)
        snprintf(payload, sizeof(payload), "<empty>");
    char buf[300];
    snprintf(buf, sizeof(buf), "UNHANDLED DATA: %s", payload);
    midi_putx(di, s, ANN_TEXT_ERROR, buf);
    s->cmd_len = 0;
    s->state = MIDI_IDLE;
    s->status_byte = 0;
    s->explicit_status_byte = 0;
}

static void midi_handle_channel_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte);
static void midi_handle_sysex_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte);
static void midi_handle_syscommon_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte);
static void midi_handle_sysrealtime_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte);

static void midi_handle_state(struct srd_decoder_inst *di, midi_state *s,
                              enum midi_state state, int newbyte)
{
    int is_none = (newbyte < 0) ? 1 : 0;
    switch (state) {
    case MIDI_HANDLE_CHANNEL_MSG:
        midi_handle_channel_msg(di, s, newbyte);
        break;
    case MIDI_HANDLE_SYSEX_MSG:
        midi_handle_sysex_msg(di, s, newbyte);
        break;
    case MIDI_HANDLE_SYSCOMMON_MSG:
        midi_handle_syscommon_msg(di, s, newbyte);
        break;
    case MIDI_HANDLE_SYSREALTIME_MSG:
        midi_handle_sysrealtime_msg(di, s, newbyte);
        break;
    case MIDI_BUFFER_GARBAGE:
        midi_handle_garbage_msg(di, s, is_none);
        break;
    default:
        break;
    }
}

static void midi_handle_channel_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte)
{
    if (newbyte >= 0) {
        if (newbyte >= 0x80) {
            s->status_byte = (uint8_t)newbyte;
            s->explicit_status_byte = 1;
        } else {
            if (s->cmd_len < (int)sizeof(s->cmd))
                s->cmd[s->cmd_len++] = (uint8_t)newbyte;
        }
    }

    int msg_type = s->status_byte & 0xF0;
    int chan = (s->status_byte & 0x0F) + 1;
    int idx = status_index(s->status_byte);
    const char *sb0 = (idx >= 0) ? status_bytes[idx][0] : "unknown";
    const char *sb1 = (idx >= 0) ? status_bytes[idx][1] : "unknown";
    const char *sb2 = (idx >= 0) ? status_bytes[idx][2] : "???";
    (void)sb1; (void)sb2;
    char buf[512];

    switch (msg_type) {
    case 0x80: { /* Note off */
        if (s->cmd_len < 2) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int note = s->cmd[0], velocity = s->cmd[1];
        const char *note_name = midi_get_note_name(chan, note);
        snprintf(buf, sizeof(buf), "Channel %d: %s (note = %d '%s', velocity = %d)",
                 chan, sb0, note, note_name, velocity);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0x90: { /* Note on */
        if (s->cmd_len < 2) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int note = s->cmd[0], velocity = s->cmd[1];
        const char *sname = (velocity == 0) ? status_bytes[0][0] : sb0;
        const char *note_name = midi_get_note_name(chan, note);
        snprintf(buf, sizeof(buf), "Channel %d: %s (note = %d '%s', velocity = %d)",
                 chan, sname, note, note_name, velocity);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0xA0: { /* Polyphonic key pressure */
        if (s->cmd_len < 2) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int note = s->cmd[0], pressure = s->cmd[1];
        const char *note_name = midi_get_note_name(chan, note);
        snprintf(buf, sizeof(buf), "Channel %d: %s of %d for note = %d '%s'",
                 chan, sb0, pressure, note, note_name);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0xB0: { /* Control change / Channel mode */
        if (s->cmd_len < 2) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int fn = s->cmd[0], param = s->cmd[1];
        if (fn >= 0x78 && fn <= 0x7F) {
            /* Channel mode */
            const char *mode_fn0 = midi_get_ctrl_fn(fn, 0);
            const char *mode_fn2 = midi_get_ctrl_fn(fn, 2);
            (void)mode_fn2;
            char vv_str[64] = "";
            if (fn == 122) {
                snprintf(vv_str, sizeof(vv_str), "%s", (param == 0) ? "off" : (param == 127) ? "on" : "(non-standard)");
            } else if (fn == 126) {
                snprintf(vv_str, sizeof(vv_str), "%s", (param != 0) ? "(channels)" : "(ch 'basic' thru 16)");
            } else if (param != 0) {
                snprintf(vv_str, sizeof(vv_str), "(non-standard 0x%02x)", param);
            }
            snprintf(buf, sizeof(buf), "Channel %d: %s '%s' %s", chan, sb0, mode_fn0, vv_str);
            midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        } else {
            /* Control change */
            const char *ctrl0 = midi_get_ctrl_fn(fn, 0);
            const char *ctrl1 = midi_get_ctrl_fn(fn, 1);
            const char *ctrl2 = midi_get_ctrl_fn(fn, 2);
            (void)ctrl1; (void)ctrl2;
            snprintf(buf, sizeof(buf), "Channel %d: %s '%s' (param = 0x%02x)",
                     chan, sb0, ctrl0, param);
            midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        }
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0xC0: { /* Program change */
        if (s->cmd_len < 1) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int pp = s->cmd[0] + 1;
        const char *change_type = "instrument";
        const char *name = "undefined";
        if (chan != 10) {
            if (pp >= 1 && pp <= 128 && gm_instruments[pp])
                name = gm_instruments[pp];
        } else {
            change_type = "drum kit";
            if (pp >= 0 && pp <= 128 && drum_kit[pp])
                name = drum_kit[pp];
        }
        snprintf(buf, sizeof(buf), "Channel %d: %s to %s %d (assuming %s)",
                 chan, sb0, change_type, pp, name);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0xD0: { /* Channel pressure */
        if (s->cmd_len < 1) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int vv = s->cmd[0];
        snprintf(buf, sizeof(buf), "Channel %d: %s %d", chan, sb0, vv);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    case 0xE0: { /* Pitch bend */
        if (s->cmd_len < 2) {
            if (newbyte < 0) {
                if (s->explicit_status_byte) {
                    memmove(s->cmd + 1, s->cmd, s->cmd_len);
                    s->cmd[0] = s->status_byte;
                    s->cmd_len++;
                }
                midi_handle_garbage_msg(di, s, 1);
            }
            return;
        }
        s->es_block = s->es;
        int ll = s->cmd[0], mm = s->cmd[1];
        int decimal = (mm << 7) + ll;
        snprintf(buf, sizeof(buf), "Channel %d: %s 0x%02x 0x%02x (%d)",
                 chan, sb0, ll, mm, decimal);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    default: {
        s->es_block = s->es;
        snprintf(buf, sizeof(buf), "Unknown channel message type: 0x%02x", msg_type);
        midi_putx(di, s, ANN_TEXT_ERROR, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        s->explicit_status_byte = 0;
        break;
    }
    }
}

static void midi_handle_sysex_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte)
{
    /* SysEx clears status byte */
    s->status_byte = 0;
    s->explicit_status_byte = 0;

    if (newbyte != 0xF7 && newbyte >= 0) {
        if (s->cmd_len < (int)sizeof(s->cmd))
            s->cmd[s->cmd_len++] = (uint8_t)newbyte;
        return;
    }

    s->es_block = s->es;

    if (s->cmd_len < 1) {
        midi_putx(di, s, ANN_TEXT_ERROR, "SysEx: no data");
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        return;
    }

    int idx = 0;
    uint8_t msg = s->cmd[idx++];
    int msg_idx = status_index(msg);
    const char *sb0 = (msg_idx >= 0) ? status_bytes[msg_idx][0] : "SysEx";
    const char *sb1 = (msg_idx >= 0) ? status_bytes[msg_idx][1] : "SysEx";
    const char *sb2 = (msg_idx >= 0) ? status_bytes[msg_idx][2] : "SE";
    (void)sb1; (void)sb2;

    /* Extract manufacturer */
    if (s->cmd_len - idx < 1) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: truncated manufacturer code (<1 bytes)", sb0);
        midi_putx(di, s, ANN_TEXT_ERROR, buf);
        s->cmd_len = 0;
        s->state = MIDI_IDLE;
        return;
    }

    uint8_t m1 = s->cmd[idx++];
    uint8_t manu_bytes[3];
    int manu_len = 1;
    manu_bytes[0] = m1;
    const char *manu_name = NULL;

    if (m1 == 0x00) {
        if (s->cmd_len - idx < 2) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: truncated manufacturer code (<3 bytes)", sb0);
            midi_putx(di, s, ANN_TEXT_ERROR, buf);
            s->cmd_len = 0;
            s->state = MIDI_IDLE;
            return;
        }
        manu_bytes[1] = s->cmd[idx++];
        manu_bytes[2] = s->cmd[idx++];
        manu_len = 3;
    }

    manu_name = midi_find_manufacturer(manu_bytes, manu_len);

    char manu_buf[128];
    if (manu_name) {
        snprintf(manu_buf, sizeof(manu_buf), "%s", manu_name);
    } else {
        if (manu_len == 3)
            snprintf(manu_buf, sizeof(manu_buf), "undefined (0x%02x 0x%02x 0x%02x)",
                     manu_bytes[0], manu_bytes[1], manu_bytes[2]);
        else
            snprintf(manu_buf, sizeof(manu_buf), "undefined (0x%02x)", manu_bytes[0]);
    }

    /* Build payload string */
    char payload[512];
    int pos = 0;
    for (int i = idx; i < s->cmd_len && pos < 400; i++)
        pos += snprintf(payload + pos, sizeof(payload) - pos, "0x%02x ", s->cmd[i]);
    if (pos == 0)
        snprintf(payload, sizeof(payload), "<empty>");

    char buf[768];
    snprintf(buf, sizeof(buf), "%s: for '%s' with payload %s", sb0, manu_buf, payload);
    midi_putx(di, s, ANN_TEXT_VERBOSE, buf);

    s->cmd_len = 0;
    s->state = MIDI_IDLE;
}

static void midi_handle_syscommon_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte)
{
    s->status_byte = 0;
    s->explicit_status_byte = 0;

    if (newbyte >= 0) {
        if (s->cmd_len < (int)sizeof(s->cmd))
            s->cmd[s->cmd_len++] = (uint8_t)newbyte;
    }

    if (s->cmd_len < 1)
        return;

    uint8_t msg = s->cmd[0];
    int msg_idx = status_index(msg);
    const char *sb0 = (msg_idx >= 0) ? status_bytes[msg_idx][0] : "SysCom";
    const char *sb1 = (msg_idx >= 0) ? status_bytes[msg_idx][1] : "SC";
    const char *sb2 = (msg_idx >= 0) ? status_bytes[msg_idx][2] : "SC";
    const char *group0 = "System Common";
    const char *group1 = "SysCom";
    const char *group2 = "SC";
    (void)sb1; (void)sb2; (void)group1; (void)group2;
    char buf[512];

    if (msg == 0xF1) {
        /* MIDI time code quarter frame */
        if (s->cmd_len < 2) {
            if (newbyte < 0)
                midi_handle_garbage_msg(di, s, 1);
            return;
        }
        int nn = (s->cmd[1] & 0x70) >> 4;
        int dd = s->cmd[1] & 0x0F;
        s->es_block = s->es;
        if (nn != 7) {
            const char *qft0 = (nn < 8) ? quarter_frame_type[nn][0] : "undefined";
            const char *qft1 = (nn < 8) ? quarter_frame_type[nn][1] : "undef";
            (void)qft1;
            snprintf(buf, sizeof(buf), "%s: %s of %s, value 0x%01x", group0, sb0, qft0, dd);
            midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        } else {
            int tt = (dd & 0x6) >> 1;
            const char *qft0 = quarter_frame_type[7][0];
            const char *qft1 = quarter_frame_type[7][1];
            (void)qft1;
            const char *smt = (tt < 4) ? smpte_type[tt] : "unknown";
            snprintf(buf, sizeof(buf), "%s: %s of %s, value 0x%01x for %s",
                     group0, sb0, qft0, dd, smt);
            midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
        }
    } else if (msg == 0xF2) {
        /* Song position pointer */
        if (s->cmd_len < 3) {
            if (newbyte < 0)
                midi_handle_garbage_msg(di, s, 1);
            return;
        }
        int ll = s->cmd[1], mm = s->cmd[2];
        int decimal = (mm << 7) + ll;
        s->es_block = s->es;
        snprintf(buf, sizeof(buf), "%s: %s 0x%02x 0x%02x (%d)",
                 group0, sb0, ll, mm, decimal);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
    } else if (msg == 0xF3) {
        /* Song select */
        if (s->cmd_len < 2) {
            if (newbyte < 0)
                midi_handle_garbage_msg(di, s, 1);
            return;
        }
        int ss = s->cmd[1];
        s->es_block = s->es;
        snprintf(buf, sizeof(buf), "%s: %s number %d", group0, sb0, ss);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
    } else if (msg == 0xF4 || msg == 0xF5 || msg == 0xF6) {
        s->es_block = s->es;
        snprintf(buf, sizeof(buf), "%s: %s", group0, sb0);
        midi_putx(di, s, ANN_TEXT_VERBOSE, buf);
    }

    s->cmd_len = 0;
    s->state = MIDI_IDLE;
}

static void midi_handle_sysrealtime_msg(struct srd_decoder_inst *di, midi_state *s, int newbyte)
{
    uint64_t old_ss_block = s->ss_block;
    uint64_t old_es_block = s->es_block;
    s->ss_block = s->ss;
    s->es_block = s->es;

    int idx = status_index((uint8_t)newbyte);
    const char *sb0 = (idx >= 0) ? status_bytes[idx][0] : "unknown";
    const char *sb1 = (idx >= 0) ? status_bytes[idx][1] : "???";
    const char *sb2 = (idx >= 0) ? status_bytes[idx][2] : "?";
    (void)sb1; (void)sb2;

    char buf[256];
    snprintf(buf, sizeof(buf), "System Realtime: %s", sb0);
    c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT_SYSREAL_VERBOSE, buf);

    s->ss_block = old_ss_block;
    s->es_block = old_es_block;
    /* Deliberately not resetting cmd or state */
}

static void midi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    midi_state *s = (midi_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    /* uint8_t rxtx = fields[1].u8; */ /* MIDI doesn't distinguish RX/TX */

    s->ss = start_sample;
    s->es = end_sample;

    enum midi_state new_state = s->state;

    if (byte_val >= 0x80 && byte_val != 0xF7) {
        new_state = midi_get_next_state(s, byte_val);
        if (new_state != MIDI_HANDLE_SYSREALTIME_MSG && s->state != MIDI_IDLE) {
            /* Flush previous data */
            midi_handle_state(di, s, s->state, -1);
        }
        s->ss = start_sample;
        s->es = end_sample;
        if (new_state != MIDI_HANDLE_SYSREALTIME_MSG) {
            s->ss_block = start_sample;
        }
    } else if (s->state == MIDI_IDLE || s->state == MIDI_BUFFER_GARBAGE) {
        s->ss = start_sample;
        s->es = end_sample;
        if (s->state == MIDI_IDLE)
            s->ss_block = start_sample;
        new_state = midi_get_next_state(s, byte_val);
    } else {
        s->ss = start_sample;
        s->es = end_sample;
        new_state = s->state;
    }

    if (new_state != MIDI_HANDLE_SYSREALTIME_MSG)
        s->state = new_state;
    if (new_state == MIDI_BUFFER_GARBAGE) {
        s->status_byte = 0;
        s->explicit_status_byte = 0;
    }

    midi_handle_state(di, s, new_state, byte_val);
}

static void midi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(midi_state)));
    }
    midi_state *s = (midi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(midi_state));
    s->state = MIDI_IDLE;
}

static void midi_start(struct srd_decoder_inst *di)
{
    midi_state *s = (midi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "midi");
}

static void midi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void midi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder midi_c_decoder = {
    .id = "midi_c",
    .name = "MIDI(C)",
    .longname = "Musical Instrument Digital Interface (C)",
    .desc = "Musical Instrument Digital Interface (MIDI) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = midi_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = midi_ann_rows,
    .inputs = midi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = midi_tags,
    .num_tags = 2,
    .reset = midi_reset,
    .start = midi_start,
    .decode = midi_decode,
    .destroy = midi_destroy,
    .decode_upper = midi_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &midi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}