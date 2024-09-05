##
# This file is part of the libsigrokdecode project.
##
# Copyright (C) 2015 Paul Evans <leonerd@leonerd.org.uk>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
##
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
##
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
##

from string import ascii_letters
import sigrokdecode as srd


def _decode_intensity(val):
    intensity = val & 0x0f
    if intensity == 0:
        return 'min'
    elif intensity == 15:
        return 'max'
    else:
        return intensity


def _decode_configuration(val):
    S = "false" if val & 0b1 else "true"
    B = "fast" if val & 0b100 else "slow"
    E = "enabled" if val & 0b1000 else "disabled"
    T = "true" if val & 0b10000 else "false"
    R = "true" if val & 0b10000 else "false"
    I = "per digit" if val & 0b10000 else "global"
    return "Shutdown: {S}, Blink rate: {B}, Blink: {E}, Reset blink: {T}, Clear data: {R}, Intensity control: {I}".format(**locals())


def _decode_digit_type(val):
    if(val == 0xff):
        return "All 14-seg"
    elif(val == 0x0):
        return "All 16/7-seg"
    dig0 = "Dig 0: {}".format('14-seg' if val & 0b1 else '16/7-seg')
    dig1 = "Dig 1: {}".format('14-seg' if val & 0b10 else '16/7-seg')
    dig2 = "Dig 2: {}".format('14-seg' if val & 0b100 else '16/7-seg')
    dig3 = "Dig 3: {}".format('14-seg' if val & 0b1000 else '16/7-seg')
    dig4 = "Dig 4: {}".format('14-seg' if val & 0b10000 else '16/7-seg')
    dig5 = "Dig 5: {}".format('14-seg' if val & 0b100000 else '16/7-seg')
    dig6 = "Dig 6: {}".format('14-seg' if val & 0b1000000 else '16/7-seg')
    dig7 = "Dig 7: {}".format('14-seg' if val & 0b10000000 else '16/7-seg')
    return "{dig0}, {dig1}, {dig2}, {dig3}, {dig4}, {dig5}, {dig6}, {dig7}".format(**locals())


def _decode_digit(val):
    return "'{}'".format(chr(val)) if chr(val) in ascii_letters else '0x{:02x}'.format(val)


registers = {
    0x00: ['No-op', lambda _: ''],
    0x01: ['Decode Mode', lambda v: '0b{:08b}'.format(v)],
    0x02: ['Global Intensity', _decode_intensity],
    0x03: ['Scan limit', lambda v: 1 + v],
    0x04: ["Configuration", _decode_configuration],
    0x05: ["GPIO Data", lambda v: "P0: {v&0b1}, P1: {v&0b10}, P2: {v&0b100}, P3: {v&0b1000}, P4: {v&0b10000}, ".format(**locals())],
    0x06: ["Port Configuration", "not done"],
    0x07: ['Display test', lambda v: 'on' if v else 'off'],
    0x08: ["KEY_A Mask", lambda v: '0b{:08b}'.format(v)],
    0x09: ["KEY_B Mask", lambda v: '0b{:08b}'.format(v)],
    0x0A: ["KEY_C Mask", lambda v: '0b{:08b}'.format(v)],
    0x0B: ["KEY_D Mask", lambda v: '0b{:08b}'.format(v)],
    0x0C: ["Digit Type", _decode_digit_type],
    0x0D: ["", "not done"],  # don't write
    0x0E: ["", "not done"],  # don't write
    0x0F: ["", "not done"],  # don't write
    0x10: ["Intensity 10", lambda v: "0: {_decode_intensity(v&0xff)}, 1: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x11: ["Intensity 32", lambda v: "2: {_decode_intensity(v&0xff)}, 3: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x12: ["Intensity 54", lambda v: "4: {_decode_intensity(v&0xff)}, 5: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x13: ["Intensity 76", lambda v: "6: {_decode_intensity(v&0xff)}, 7: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x14: ["Intensity 10a (7 Segment Only)", lambda v: "0: {_decode_intensity(v&0xff)}, 1: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x15: ["Intensity 32a (7 Segment Only)", lambda v: "2: {_decode_intensity(v&0xff)}, 3: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x16: ["Intensity 54a (7 Segment Only)", lambda v: "4: {_decode_intensity(v&0xff)}, 5: {_decode_intensity(v&0xff00)}".format(**locals())],
    0x17: ["Intensity 76a (7 Segment Only)", lambda v: "6: {_decode_intensity(v&0xff)}, 7: {_decode_intensity(v&0xff00)}".format(**locals())],

    0x20: ["Digit 0 Plane P0", _decode_digit],
    0x21: ["Digit 1 Plane P0", _decode_digit],
    0x22: ["Digit 2 Plane P0", _decode_digit],
    0x23: ["Digit 3 Plane P0", _decode_digit],
    0x24: ["Digit 4 Plane P0", _decode_digit],
    0x25: ["Digit 5 Plane P0", _decode_digit],
    0x26: ["Digit 6 Plane P0", _decode_digit],
    0x27: ["Digit 7 Plane P0", _decode_digit],
    0x28: ["Digit 0a Plane P0 (7 Segment Only)", _decode_digit],
    0x29: ["Digit 1a Plane P0 (7 Segment Only)", _decode_digit],
    0x2A: ["Digit 2a Plane P0 (7 Segment Only)", _decode_digit],
    0x2B: ["Digit 3a Plane P0 (7 Segment Only)", _decode_digit],
    0x2C: ["Digit 4a Plane P0 (7 Segment Only)", _decode_digit],
    0x2D: ["Digit 5a Plane P0 (7 Segment Only)", _decode_digit],
    0x2E: ["Digit 6a Plane P0 (7 Segment Only)", _decode_digit],
    0x2F: ["Digit 7a Plane P0 (7 Segment Only)", _decode_digit],

    0x40: ["Digit 0 Plane P1", _decode_digit],
    0x41: ["Digit 1 Plane P1", _decode_digit],
    0x42: ["Digit 2 Plane P1", _decode_digit],
    0x43: ["Digit 3 Plane P1", _decode_digit],
    0x44: ["Digit 4 Plane P1", _decode_digit],
    0x45: ["Digit 5 Plane P1", _decode_digit],
    0x46: ["Digit 6 Plane P1", _decode_digit],
    0x47: ["Digit 7 Plane P1", _decode_digit],
    0x48: ["Digit 0a Plane P1 (7 Segment Only)", _decode_digit],
    0x49: ["Digit 1a Plane P1 (7 Segment Only)", _decode_digit],
    0x4A: ["Digit 2a Plane P1 (7 Segment Only)", _decode_digit],
    0x4B: ["Digit 3a Plane P1 (7 Segment Only)", _decode_digit],
    0x4C: ["Digit 4a Plane P1 (7 Segment Only)", _decode_digit],
    0x4D: ["Digit 5a Plane P1 (7 Segment Only)", _decode_digit],
    0x4E: ["Digit 6a Plane P1 (7 Segment Only)", _decode_digit],
    0x4F: ["Digit 7a Plane P1 (7 Segment Only)", _decode_digit],

    0x60: ["Digit 0 Both Planes", _decode_digit],
    0x61: ["Digit 1 Both Planes", _decode_digit],
    0x62: ["Digit 2 Both Planes", _decode_digit],
    0x63: ["Digit 3 Both Planes", _decode_digit],
    0x64: ["Digit 4 Both Planes", _decode_digit],
    0x65: ["Digit 5 Both Planes", _decode_digit],
    0x66: ["Digit 6 Both Planes", _decode_digit],
    0x67: ["Digit 7 Both Planes", _decode_digit],
    0x68: ["Digit 0a Both Planes (7 Segment Only)", _decode_digit],
    0x69: ["Digit 1a Both Planes (7 Segment Only)", _decode_digit],
    0x6A: ["Digit 2a Both Planes (7 Segment Only)", _decode_digit],
    0x6B: ["Digit 3a Both Planes (7 Segment Only)", _decode_digit],
    0x6C: ["Digit 4a Both Planes (7 Segment Only)", _decode_digit],
    0x6D: ["Digit 5a Both Planes (7 Segment Only)", _decode_digit],
    0x6E: ["Digit 6a Both Planes (7 Segment Only)", _decode_digit],
    0x6F: ["Digit 7a Both Planes (7 Segment Only)", _decode_digit],

    0x88: ["Key_A Debounced", "not done"],
    0x89: ["Key_B Debounced", "not done"],
    0x8A: ["Key_C Debounced", "not done"],
    0x8B: ["Key_D Debounced", "not done"],
    0x8C: ["Key_A Pressed", "not done"],
    0x8D: ["Key_A Pressed", "not done"],
    0x8E: ["Key_A Pressed", "not done"],
    0x8F: ["Key_A Pressed", "not done"]
}

ann_reg, ann_digit, ann_warning = range(3)


class Decoder(srd.Decoder):
    api_version = 3
    id = 'max6954'
    name = 'MAX6954'
    longname = 'Maxim MAX6954'
    desc = 'Maxim MAX6954 LED display driver.'
    license = 'gplv2+'
    inputs = ['spi']
    outputs = []
    tags = ['Display']
    annotations = (
        ('register', 'Register write'),
        ('digit', 'Digit displayed'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('commands', 'Commands', (ann_reg, ann_digit)),
        ('warnings', 'Warnings', (ann_warning,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        pass

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.pos = 0
        self.cs_start = 0

    def putreg(self, ss, es, reg, value):
        self.put(ss, es, self.out_ann, [ann_reg, ['%s: %s' % (reg, value)]])

    def putwarn(self, ss, es, message):
        self.put(ss, es, self.out_ann, [ann_warning, [message]])

    def decode(self, ss, es, data):
        ptype, mosi, _ = data

        if ptype == 'DATA':
            if not self.cs_asserted:
                return

            if self.pos == 0:
                self.addr = mosi
                self.addr_start = ss
            elif self.pos == 1:
                if self.addr in registers:
                    name, decoder = registers[self.addr]
                    self.putreg(self.addr_start, es, name, decoder(mosi))
                else:
                    self.putwarn(self.addr_start, es,
                                 'Unknown register %02X' % (self.addr))

            self.pos += 1
        elif ptype == 'CS-CHANGE':
            self.cs_asserted = mosi
            if self.cs_asserted:
                self.pos = 0
                self.cs_start = ss
            else:
                if self.pos == 1:
                    # Don't warn if pos=0 so that CS# glitches don't appear
                    # as spurious warnings.
                    self.putwarn(self.cs_start, es, 'Short write')
                elif self.pos > 2:
                    self.putwarn(self.cs_start, es, 'Overlong write')
