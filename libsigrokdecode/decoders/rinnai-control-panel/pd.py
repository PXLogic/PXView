##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017 Kevin Redon <kingkevin@cuvoodoo.info>
## Copyright (C) 2020 Arik Yavilevich <arik_sigrok@yavilevich.com>
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

class SamplerateError(Exception):
    pass

ANN_ID_BIT = 0
ANN_ID_WARNINGS = 1
ANN_ID_RESET = 2
ANN_ID_BYTE = 3
ANN_ID_PACKET = 4

SYMBOL_DURATION_US = 600

SYMBOL_SHORT_PERIOD_RATIO_MIN = 0.15
SYMBOL_SHORT_PERIOD_RATIO_MAX = 0.35
SYMBOL_LONG_PERIOD_RATIO_MIN = 0.65
SYMBOL_LONG_PERIOD_RATIO_MAX = 0.85

RESET_RATIO_MIN = 1
RESET_RATIO_MAX = 2

class Decoder(srd.Decoder):
    api_version = 3
    id = 'rinnai-control-panel'
    name = 'Rinnai Control Panel'
    longname = 'Rinnai control panel internal pulse length encoding protocol'
    desc = 'Bidirectional, half-duplex, asynchronous serial bus.' # ?
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['rinnai']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Pulse length signal line'},
    )
    options = (
        {'id': 'invert', 'desc': 'Invert bits',
            'default': 'no', 'values': ('yes', 'no')},
        {'id': 'bit_numbering', 'desc': 'Bit numbering, first',
            'default': 'lsb', 'values': ('lsb', 'msb')},
    )
    annotations = (
        ('bit', 'Bit'),
        ('warning', 'Warning'),
        ('reset', 'Reset'),
        ('byte', 'Byte'),
        ('packet', 'Packet'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0, 2)),
        ('warnings', 'Warnings', (1,)),
        ('bytes', 'Bytes', (3,)),
        ('packets', 'Packets', (4,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.state = 'INITIAL'
        self.fall = 0
        self.rise = 0
        self.bits_reset()
        self.bytes_reset()

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.invert = (self.options['invert'] == 'yes')
        self.bit_numbering_lsb_first = (self.options['bit_numbering'] == 'lsb')
        self.fall = 0
        self.rise = 0
        self.bits_reset()

    def metadata(self, key, value):
        if key != srd.SRD_CONF_SAMPLERATE:
            return
        self.samplerate = value

    def bit_append(self, start, end, bit):
        # render bit
        self.put(start, end, self.out_ann, [ANN_ID_BIT, [str(bit)]])
        # manage bytes
        if self.byte_start == -1:
            self.byte_start = start
        if self.bit_numbering_lsb_first:
            self.byte += (bit << self.bit_count)
        else:
            self.byte = 2 * self.byte + bit
        self.bit_count += 1
        if self.bit_count == 8:
            self.byte_append(self.byte_start, end, self.byte)

    def bits_reset(self):
        self.bit_count = 0
        self.byte = 0
        self.byte_start = -1

    def byte_append(self, start, end, byte):
        self.put(start, end, self.out_ann, [ANN_ID_BYTE, ['%02x' % byte]])
        self.put(start, end, self.out_python, [ANN_ID_BYTE, [byte]])
        self.bits_reset()
        if self.packet_start == -1:
            self.packet_start = start
        self.bytes.append(byte)

    def bytes_flush(self, end):
        if len(self.bytes) > 0:
            self.put(self.packet_start, end, self.out_ann, [ANN_ID_PACKET, [','.join(['%02x' % byte for byte in self.bytes])]])
        self.bytes_reset()

    def bytes_reset(self):
        self.bytes = []
        self.packet_start = -1

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        ##self.checks()
        while True:
            # State machine.
            if self.state == 'INITIAL': # Unknown initial state.
                # Wait until we reach the idle low state.
                self.wait({0: 'l'})
                self.fall = self.samplenum
                self.state = 'IDLE'
            elif self.state == 'IDLE': # Idle high state.
                # Wait for rising edge.
                self.wait({0: 'r'})
                self.rise = self.samplenum
                # Get time since last falling edge.
                time = ((self.rise - self.fall) / self.samplerate) * 1000000.0
                ##if self.rise > 0:
                ##    self.put_fr([1, ['Recovery time not long enough'
                ##        'Recovery too short',
                ##        ]])
                # A reset pulse or slot can start on a rising edge.
                #self.put(self.fall, self.samplenum, self.out_ann, [ANN_ID_WARNINGS, ['Idle: %d' % time]])
                self.state = 'PRE'
            elif self.state == 'PRE':
                # Wait for falling edge.
                self.wait({0: 'f'})
                self.fall = self.samplenum
                # Get time since last rising edge.
                time = ((self.fall - self.rise) / self.samplerate) * 1000000.0
                # render time
                if time > RESET_RATIO_MIN * SYMBOL_DURATION_US and time < RESET_RATIO_MAX * SYMBOL_DURATION_US:
                    self.put(self.rise, self.samplenum, self.out_ann, [ANN_ID_RESET, ['Reset: %d' % time]])
                    self.state = 'SYMBOL'
                    self.bytes_flush(self.samplenum)
                else:
                    self.put(self.rise, self.samplenum, self.out_ann, [ANN_ID_WARNINGS, ['Bad pre: %d' % time]])
                    self.state = 'IDLE'
                    self.bytes_flush(self.samplenum)
            elif self.state == 'SYMBOL': # symbol
                # Wait for rising edge.
                self.wait({0: 'r'})
                self.rise = self.samplenum
                # Wait for falling edge.
                self.wait({0: 'f'})
                # get time slices
                timeA = ((self.rise - self.fall) / self.samplerate) * 1000000.0
                timeB = ((self.samplenum - self.rise) / self.samplerate) * 1000000.0
                # set output and next state
                if timeA > SYMBOL_SHORT_PERIOD_RATIO_MIN * SYMBOL_DURATION_US and timeA < SYMBOL_SHORT_PERIOD_RATIO_MAX * SYMBOL_DURATION_US and timeB > SYMBOL_LONG_PERIOD_RATIO_MIN * SYMBOL_DURATION_US and timeB < SYMBOL_LONG_PERIOD_RATIO_MAX * SYMBOL_DURATION_US:
                    self.bit_append(self.fall, self.samplenum, 0 if self.invert else 1)
                elif timeB > SYMBOL_SHORT_PERIOD_RATIO_MIN * SYMBOL_DURATION_US and timeB < SYMBOL_SHORT_PERIOD_RATIO_MAX * SYMBOL_DURATION_US and timeA > SYMBOL_LONG_PERIOD_RATIO_MIN * SYMBOL_DURATION_US and timeA < SYMBOL_LONG_PERIOD_RATIO_MAX * SYMBOL_DURATION_US:
                    self.bit_append(self.fall, self.samplenum, 1 if self.invert else 0)
                elif timeB > RESET_RATIO_MIN * SYMBOL_DURATION_US and timeB < RESET_RATIO_MAX * SYMBOL_DURATION_US: # not a symbol but an idle zone and a new reset
                    self.bits_reset()
                    self.put(self.rise, self.samplenum, self.out_ann, [ANN_ID_RESET, ['Reset: %d' % time]])
                    self.bytes_flush(self.fall)
                else:
                    self.bits_reset()
                    self.put(self.fall, self.samplenum, self.out_ann, [ANN_ID_WARNINGS, ['Bad Bit: %d,%d' % (timeA, timeB)]])
                    self.state = 'IDLE' # start over
                    self.bytes_flush(self.fall)
                self.fall = self.samplenum # update state

    ##def put_message(self, data):
    ##    self.put(0, 0, self.out_ann, data)

    ##def put_py_fs(self, data):
    ##    self.put(self.fall, self.samplenum, self.out_python, data)

    ##def put_fs(self, data):
    ##    self.put(self.fall, self.samplenum, self.out_ann, data)

    ##def put_fr(self, data):
    ##    self.put(self.fall, self.rise, self.out_ann, data)

    ##def put_py_rs(self, data):
    ##    self.put(self.rise, self.samplenum, self.out_python, data)

    ##def put_rs(self, data):
    ##    self.put(self.rise, self.samplenum, self.out_ann, data)

    ##def checks(self):
        # Check if samplerate is appropriate.
        ##if self.options['overdrive'] == 'yes':
        ##    if self.samplerate < 2000000:
        ##        self.put_message([1, ['Sampling rate is too low. Must be above ' +
        ##                       '2MHz for proper overdrive mode decoding.']])
        ##    elif self.samplerate < 5000000:
        ##        self.put_message([1, ['Sampling rate is suggested to be above 5MHz ' +
        ##                       'for proper overdrive mode decoding.']])
        ##else:
        ##    if self.samplerate < 400000:
        ##        self.put_message([1, ['Sampling rate is too low. Must be above ' +
        ##                       '400kHz for proper normal mode decoding.']])
        ##    elif self.samplerate < 1000000:
        ##        self.put_message([1, ['Sampling rate is suggested to be above ' +
        ##                       '1MHz for proper normal mode decoding.']])
