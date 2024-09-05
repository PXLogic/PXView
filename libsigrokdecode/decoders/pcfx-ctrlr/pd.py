##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 David Shadoff <david.shadoff@gmail.com>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, see <http://www.gnu.org/licenses/>.
##

import sigrokdecode as srd
from collections import deque

class SamplerateError(Exception):
    pass

#
# Extract a subgroup of bits from a 32-bit stream as a
# binary value, for display and/or calculation purposes
#
def get_bitfield(self, start_bit, field_size):
    end_bit = start_bit + field_size - 1
    value = 0
    for iter in range (0, field_size):
        value = value + (self.bits_value[start_bit + iter] << iter)
    return value

#
# From a binary value, display a value in an annotation channel,
# based on a bit's start/end timing, and whether the bit is 'on'
# used by the joypad section, to display individual buttons
#
def putbit(self, annot_type, value, bitnum, dispval):
    if (value & (1 << bitnum)):
        self.put(self.bits_start[bitnum], self.bits_end[bitnum], self.out_ann,
                 [annot_type, [dispval]])


class Decoder(srd.Decoder):
    api_version = 3
    id = 'pcfx_cntrlr'
    name = 'PCFX Cntrlr'
    longname = 'PCFX Controller'
    desc = 'Controller protocol for NEC PC-FX videogame console'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Retro computing']
    channels = (
        {'id': 'trigger', 'name': 'TRG', 'desc': 'Trigger'},
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock'},
        {'id': 'data', 'name': 'DATA', 'desc': 'Data'},
    )
    optional_channels = (
        {'id': 'dir', 'name': 'DIR', 'desc': 'Data Direction'},
    )
    annotations = (
        ('start', 'Start'),
        ('reset', 'Reset'),
        ('bit', 'Bit'),
        ('outbits', 'Outbound Bits'),
        ('byte', 'Byte'),
        ('word', 'Word'),
        ('ctrldata', 'Controller Data'),
        ('ctrlpad', 'Joypad Controller'),
        ('ctrltap', 'Multitap Controller'),
        ('ctrlmouse', 'Mouse Controller'),
        ('ctrlunkn', 'Unknown Controller'),
        ('warning', 'Warnings'),
    )
    annotation_rows = (
        ('starts', 'Start', (0,1,)),
        ('bits', 'Bits', (2,3,)),
        ('bytes', 'Bytes', (4,)),
        ('words', 'Words', (5,)),
        ('controller', 'Controller', (6,7,8,9,10,)),
        ('warnings', 'Warnings', (11,)),
    )
    options = (
        { 'id': 'bitvals', 'desc': 'Show bit values', 'default': 'electrical', 'values': ('electrical', 'internal') },
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.last_samplenum = None
        self.state = 'FIND START'    # starting state for state machine
        self.startsamplenum = None   # used for trigger pulse
        self.triggertype = 0         # whether trigger is normal or multitap reset
        self.startbit = None         # temporary value for start sample of bit
        self.bitvalue = None         # actual electrical value of bit
        self.dispbit = None          # value of bit to be displayed
                                     # (based on electrical/internal choice - internal is inverted)
        self.bitstring = None        # displayable string for bit

        self.have_direction = None   # whether the optional Direction channel is in use
        self.dir = None              # whether data is inbound/outbound (based on Direction bchannel)

        self.bitcount = 0            # counter within 32-bit data array
        self.bits_value = list()     # array for 32 bits of data in a read cycle
        self.bits_start = list()     #   array of corresponding start sample nums (for display)
        self.bits_end = list()       #   array of corresponding end sample nums (for display)


    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

#    def get_trigger(self):

#    def get_trigger_end(self):

#    def get_bit(self):

#    def get_bit_end(self):

#    def decode(self, ss, es, data):
    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        self.have_direction = self.has_channel(3) # Optional OE channel

        while True:
            if self.state == 'FIND START':
                # Wait for falling transition on channel 0 (trigger), which starts a read cycle
                self.wait({0: 'f'})
                self.state = 'CHECK RESET'
                self.triggertype = 0
                self.startsamplenum = self.samplenum

            elif self.state == 'CHECK RESET':
                # Wait for either of the two following conditions:
                # a) clock line (channel 1) goes low while trigger (channel 0) is low: (RESET joypad counter)
                # b) trigger line (channel 0) goes high (completion of trigger)
                (trg, clk, dataval, self.dir) = self.wait([{0: 'l', 1: 'f'}, {0: 'r'}])
                if (self.matched[0]):
                    self.triggertype = 1   # this is a RESET joypad counter event; continue to search for trigger end

                if (self.matched[0]):
                    self.state = 'START BIT'
                    self.bitcount = 0
                    self.bits_start.clear()
                    self.bits_value.clear()
                    self.bits_end.clear()
                    self.startbit = self.samplenum
                    if self.triggertype == 0:
                        self.put(self.startsamplenum, self.samplenum, self.out_ann,
                                 [0, ['Trigger', 'Trig', 'T']])
                    else:
                        self.put(self.startsamplenum, self.samplenum, self.out_ann,
                                 [1, ['Reset Joy Count', 'Reset', 'R']])
                        
            elif self.state == 'START BIT':
                (trg, clk, dataval, self.dir) = self.wait([{1: 'f'}, {0: 'f'}])
                if (self.matched[0]):
                    self.bitvalue = dataval
                    self.bits_start.append(self.startbit)
                    self.bits_value.append((1 - self.bitvalue))  # internal value is inverted
                    self.state = "END BIT"
#                else   framing error

            elif self.state == 'END BIT':
                self.wait([{1: 'r'}, {0: 'f'}])
                if (self.matched[0]):

                    self.bits_end.append(self.samplenum)

                    self.dispbit = self.bitvalue if (self.options['bitvals'] == 'electrical') else (1 - self.bitvalue)

                    self.bitstring = '0' if (self.dispbit == 0) else '1'

                    if (self.have_direction) and (self.dir == 1):
                        self.display_annot = 3
                    else:
                        self.display_annot = 2

                    self.put(self.startbit, self.samplenum, self.out_ann,
                             [self.display_annot, [self.bitstring]])

                self.startbit = self.samplenum   # next bit's start
                self.bitcount = self.bitcount + 1

                if self.bitcount != 32:
                    self.state = 'START BIT'
                else:
                    # do summary protocols here
                    # byte:
                    for byteseq in range (0, 4):
                        fieldsize = 8
                        startbit  = byteseq * fieldsize
                        endbit = startbit + 7
                        value = get_bitfield(self, startbit, fieldsize)

                        dispval = '0x%2.2X' % value
                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [4, [dispval]])

                    # word:
                    fieldsize = 32
                    startbit = 0
                    value = get_bitfield(self, startbit, fieldsize)

                    dispval = '0x%8.8X' % value
                    self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                             [5, [dispval]])

                    self.state = 'FIND START'

                    # controller:
                    fieldsize = 4
                    startbit = 28
                    value = get_bitfield(self, startbit, fieldsize)
                    if value == 15:
                        dispval = 'Joypad'
                        outannot = 7
                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])

                        fieldsize = 16
                        startbit = 0
                        value = get_bitfield(self, startbit, fieldsize)
                        outannot = 6

                        putbit(self, outannot, value, 0,  'I')
                        putbit(self, outannot, value, 1,  'II')
                        putbit(self, outannot, value, 2,  'III')
                        putbit(self, outannot, value, 3,  'IV')
                        putbit(self, outannot, value, 4,  'V')
                        putbit(self, outannot, value, 5,  'VI')
                        putbit(self, outannot, value, 6,  'Sel')
                        putbit(self, outannot, value, 7,  'Run')
                        putbit(self, outannot, value, 8,  'Up')
                        putbit(self, outannot, value, 9,  'Right')
                        putbit(self, outannot, value, 10, 'Down')
                        putbit(self, outannot, value, 11, 'Left')
                        putbit(self, outannot, value, 12, 'Mode 1')
                        putbit(self, outannot, value, 14, 'Mode 2')


                    elif value == 14:
                        dispval = 'Multitap'
                        outannot = 8
                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])

                    elif value == 13:
                        dispval = 'Mouse'
                        outannot = 9
                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])

                        fieldsize = 8
                        startbit = 0
                        value = get_bitfield(self, startbit, fieldsize)
                        outannot = 6
                        if (value & 0x80):
                            value = 0 - value
                            dispval = 'Y=%d (Up)' % value
                        else:
                            dispval = 'Y=%d (Down)' % value

                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])

                        fieldsize = 8
                        startbit = 8
                        value = get_bitfield(self, startbit, fieldsize)
                        outannot = 6
                        if (value & 0x80):
                            value = 0 - value
                            dispval = 'X=%d (Left)' % value
                        else:
                            dispval = 'X=%d (Right)' % value

                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])

                        fieldsize = 1
                        startbit = 16
                        value = get_bitfield(self, startbit, fieldsize)
                        outannot = 6
                        if (value == 1):
                            dispval = 'Left'

                            self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                     [outannot, [dispval]])

                        fieldsize = 1
                        startbit = 17
                        value = get_bitfield(self, startbit, fieldsize)
                        outannot = 6
                        if (value == 1):
                            dispval = 'Right'

                            self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                     [outannot, [dispval]])

                    else:
                        dispval = '(%d)' % value
#                        dispval = ['Unknown (%d)' % value, 'Unknown', '(%d)' % value ]
                        outannot = 10
                        self.put(self.bits_start[startbit], self.bits_end[startbit+fieldsize-1], self.out_ann,
                                 [outannot, [dispval]])


            self.last_samplenum = self.samplenum
