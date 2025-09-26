##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2010-2016 Uwe Hermann <uwe@hermann-uwe.de>
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

# TODO: Look into arbitration, collision detection, clock synchronisation, etc.
# TODO: Implement support for inverting SDA/SCL levels (0->1 and 1->0).
# TODO: Implement support for detecting various bus errors.

from collections import deque
import sigrokdecode as srd

'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <pdata>]

<ptype>:
 - 'START' (START condition)
 - 'START REPEAT' (Repeated START condition)
 - 'ADDRESS READ' (Slave address, read)
 - 'ADDRESS WRITE' (Slave address, write)
 - 'DATA READ' (Data, read)
 - 'DATA WRITE' (Data, write)
 - 'STOP' (STOP condition)
 - 'ACK' (ACK bit)
 - 'NACK' (NACK bit)
 - 'BITS' (<pdata>: list of data/address bits and their ss/es numbers)

<pdata> is the data or address byte associated with the 'ADDRESS*' and 'DATA*'
command. Slave addresses do not include bit 0 (the READ/WRITE indication bit).
For example, a slave address field could be 0x51 (instead of 0xa2).
For 'START', 'START REPEAT', 'STOP', 'ACK', and 'NACK' <pdata> is None.
'''

# CMD: [annotation-type-index, long annotation, short annotation]
proto = {
    'START':           [0, 'Start',         'S'],
    'START REPEAT':    [1, 'Start repeat',  'Sr'],
    'STOP':            [2, 'Stop',          'P'],
    'ACK':             [3, 'ACK',           'A'],
    'NACK':            [4, 'NACK',          'N'],
    'BIT':             [5, 'Bit',           'B'],
    'ADDRESS READ':    [6, 'Address read',  'AR'],
    'ADDRESS WRITE':   [7, 'Address write', 'AW'],
    'DATA READ':       [8, 'Data read',     'DR'],
    'DATA WRITE':      [9, 'Data write',    'DW'],
    'PACKET':          [10, 'Packet',       'PK'],
}

SCL = 0
SDA = 1

class Decoder(srd.Decoder):
    api_version = 3
    id = 'i2c'
    name = 'I²C'
    longname = 'Inter-Integrated Circuit'
    desc = 'Two-wire, multi-master, serial bus.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['i2c']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'scl', 'name': 'SCL', 'desc': 'Serial clock line(串行时钟线)'},
        {'id': 'sda', 'name': 'SDA', 'desc': 'Serial data line(串行数据线)'},
    )
    options = (
        {'id': 'address_format', 'desc': 'Displayed slave address format(从地址格式)',
            'default': 'shifted', 'values': ('shifted', 'unshifted')},
        {'id': 'packets_format', 'desc': 'Display packets(数据格式)',
            'default': 'hex', 'values': ('none', 'hex', 'ascii', 'dec', 'bin', 'oct')},
        {'id': 'show_data_point', 'desc': 'Show data point(数据点显示)', 'default': 'yes',
            'values': ('yes', 'no')},
    )
    annotations = (
        ('start', 'Start condition'),
        ('repeat-start', 'Repeat start condition'),
        ('stop', 'Stop condition'),
        ('ack', 'ACK'),
        ('nack', 'NACK'),
        ('bit', 'Data/address bit'),
        ('address-read', 'Address read'),
        ('address-write', 'Address write'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
        ('packet', 'Packet'),
        ('atk-data-point', 'ATK Data point'),   #11
        ('atk-rising-edge', 'ATK Rising edge'), #12
    )
    annotation_rows = (
        ('bits', 'Bits', (5,)),
        ('addr-data', 'Address/data', (0, 1, 2, 3, 4, 6, 7, 8, 9)),
        ('packets', 'Packets', (10,)),
        ('atk-signs', 'ATK signs', (11,12)),
    )
    binary = (
        ('address-read', 'Address read'),
        ('address-write', 'Address write'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
    )

    def __init__(self):
        self.packet_data = deque()
        self.reset()

    def reset(self):
        self.samplerate = None
        self.ss = self.es = self.ss_byte = -1
        self.bitcount = 0
        self.databyte = 0
        self.wr = -1
        self.is_repeat_start = 0
        self.state = 'FIND START'
        self.pdu_start = None
        self.pdu_bits = 0
        self.bits = []
        self.resetPacket()
    
    def resetPacket(self):
        self.packet_data.clear()
        self.packet_str = self.packet_str_short = ''
        self.packet_ss = self.packet_es = self.packet_part_ss = self.address = 0

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_bitrate = self.register(srd.OUTPUT_META,
            meta=(int, 'Bitrate', 'Bitrate from Start bit to Stop bit'))
        self.show_data_point = self.options['show_data_point'] == 'yes'
        format_name = self.options['packets_format']
        if format_name == 'hex':
            self.fmt = '{:02X}'
        elif format_name == 'dec':
            self.fmt = '{:d}'
        elif format_name == 'bin':
            self.fmt = '{:08b}'
        elif format_name == 'oct':
            self.fmt = '{:03o}'
        elif format_name == 'ascii':
            self.fmt = 'ascii'
        else:
            self.fmt = None
        self.packet_data = deque()

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def putp(self, data):
        self.put(self.ss, self.es, self.out_python, data)

    def putb(self, data):
        self.put(self.ss, self.es, self.out_binary, data)

    def putpacket(self, data):
        packet_ss = self.packet_ss
        self.put(packet_ss, self.packet_es, self.out_ann, [proto['PACKET'][0], data])

    def format_data_value(self, v):
        if 32 <= v <= 126:
            return chr(v)
        return "[{:02X}]".format(v)

    def data_array_to_str(self, data_array):
        if self.fmt == "ascii":
            str_array = [self.format_data_value(value) for value in data_array]
            return ''.join(str_array)
        elif self.fmt:
            str_array = [self.fmt.format(value) for value in data_array]
            return ' '.join(str_array)

    def format_data(self, d, cmd, is_address = False):
        if is_address:
            display = "%02X" % d
        elif self.fmt == 'ascii':
            display = self.format_data_value(d)
        elif self.fmt:
            display = self.fmt.format(d)
        else:
            display = '%02x' % d
        return ['%s: %s' % (proto[cmd][1], display),
                '%s: %s' % (proto[cmd][2], display),
                display]
    
    def format_packet(self):
        packet_str = "0x{:02X} {:}: ".format(
            self.address,
            'RD' if self.wr == 0 else 'WR',
        ) + self.data_array_to_str(self.packet_data)

        packet_str_short = packet_str[2:]

        if self.packet_str:
            packet_str = self.packet_str + ' [SR] ' + packet_str
            packet_str_short = self.packet_str_short + ' [SR] ' + packet_str_short

        return packet_str, packet_str_short

    def handle_packet(self, start_repeat=False):
        if self.fmt is None:
            return
        if not len(self.packet_data):
            if not start_repeat:
                self.resetPacket()
            return
        
        packet_str, packet_str_short = self.format_packet()

        if start_repeat:
            self.packet_data.clear()
            self.packet_str = packet_str
            self.packet_str_short = packet_str_short
        else:
            self.putpacket([packet_str, packet_str_short])
            self.resetPacket()

    def handle_start(self, pins):
        self.ss, self.es = self.samplenum, self.samplenum
        self.pdu_start = self.samplenum
        self.pdu_bits = 0
        cmd = 'START REPEAT' if (self.is_repeat_start == 1) else 'START'
        self.handle_packet(self.is_repeat_start)
        self.packet_part_ss = self.samplenum
        if self.is_repeat_start == 0:
            self.packet_ss = self.samplenum
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])
        self.state = 'FIND ADDRESS'
        self.bitcount = self.databyte = 0
        self.is_repeat_start = 1
        self.wr = -1
        self.bits = []

    # Gather 8 bits of data plus the ACK/NACK bit.
    def handle_address_or_data(self, pins):
        scl, sda = pins
        self.pdu_bits += 1
        
        # Address and data are transmitted MSB-first.
        self.databyte <<= 1
        self.databyte |= sda

        # Remember the start of the first data/address bit.
        if self.bitcount == 0:
            self.ss_byte = self.samplenum

        # Store individual bits and their start/end samplenumbers.
        # In the list, index 0 represents the LSB (I²C transmits MSB-first).
        self.bits.insert(0, [sda, self.samplenum, self.samplenum])
        if self.bitcount > 0:
            self.bits[1][2] = self.samplenum

        if self.bitcount == 7:
            self.bitwidth = self.bits[1][2] - self.bits[2][2]
            self.bits[0][2] += self.bitwidth
            
        # Return if we haven't collected all 8 + 1 bits, yet.
        if self.bitcount < 7:
            self.bitcount += 1
            return

        d = self.databyte
        if self.state == 'FIND ADDRESS':
            # The READ/WRITE bit is only in address bytes, not data bytes.
            self.wr = 0 if (self.databyte & 1) else 1
            if self.options['address_format'] == 'shifted':
                d = d >> 1

        bin_class = -1
        if self.state == 'FIND ADDRESS' and self.wr == 1:
            cmd = 'ADDRESS WRITE'
            bin_class = 1
        elif self.state == 'FIND ADDRESS' and self.wr == 0:
            cmd = 'ADDRESS READ'
            bin_class = 0
        elif self.state == 'FIND DATA':
            if self.wr == 1:
                cmd = 'DATA WRITE'
                bin_class = 3
            elif self.wr == 0:
                cmd = 'DATA READ'
                bin_class = 2
            self.packet_data.append(d)

        self.ss, self.es = self.ss_byte, self.samplenum + self.bitwidth
        self.packet_es = self.es

        self.putp(['BITS', self.bits])
        self.putp([cmd, d])

        self.putb([bin_class, bytes([d])])

        for bit in self.bits:
            self.put(bit[1], bit[2], self.out_ann, [5, ['%d' % bit[0]]])
            if self.show_data_point:
                self.put(bit[1], bit[1], self.out_ann,[11, ['%d' % SDA]])
                self.put(bit[1], bit[1], self.out_ann,[12, ['%d' % SCL]])

        if cmd.startswith('ADDRESS'):
            self.ss, self.es = self.samplenum, self.samplenum + self.bitwidth
            w = ['Write', 'Wr', 'W'] if self.wr else ['Read', 'Rd', 'R']
            self.putx([proto[cmd][0], w])
            # 11：sda数据点 12：sdl上升沿
            if self.show_data_point:
                self.put(self.samplenum, self.samplenum, self.out_ann,[11, ['%d' % SDA]])
                self.put(self.samplenum, self.samplenum, self.out_ann,[12, ['%d' % SCL]])
            self.ss, self.es = self.ss_byte, self.samplenum
            self.address = d
            self.putx([proto[cmd][0], self.format_data(d, cmd, is_address=True)])
        else:
            self.putx([proto[cmd][0], self.format_data(d, cmd, is_address=False)])

        self.bitcount = self.databyte = 0
        self.bits = []
        self.state = 'FIND ACK'

    def get_ack(self, pins):
        scl, sda = pins
        self.ss, self.es = self.samplenum, self.samplenum + self.bitwidth
        self.packet_es = self.es
        cmd = 'NACK' if (sda == 1) else 'ACK'
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])
        # There could be multiple data bytes in a row, so either find
        # another data byte or a STOP condition next.
        self.state = 'FIND DATA'

    def handle_stop(self, pins):
        # Meta bitrate
        if self.samplerate:
            # 计算从Start到Stop的时间差：
            elapsed = 1 / float(self.samplerate) * (self.samplenum - self.pdu_start + 1)
            bitrate = int(1 / elapsed * self.pdu_bits)
            self.put(self.ss_byte, self.samplenum, self.out_bitrate, bitrate)

        cmd = 'STOP'
        self.ss = self.es = self.pes = self.packet_es = self.samplenum
        self.handle_packet()
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])

        self.state = 'FIND START'
        self.is_repeat_start = 0
        self.wr = -1
        self.bits = []

    def decode(self):
        self.put(self.ss, self.ss, self.out_ann, [11,["color:#4edc44"]])
        self.put(self.ss, self.ss, self.out_ann, [12,["color:#4edc44"]])
        while True:
            # State machine.
            if self.state == 'FIND START':
                # Wait for a START condition (S): SCL = high, SDA = falling.
                self.handle_start(self.wait({0: 'h', 1: 'f'}))
            elif self.state == 'FIND ADDRESS':
                # Wait for a data bit: SCL = rising.
                self.handle_address_or_data(self.wait({0: 'r'}))
            elif self.state == 'FIND DATA':
                # Wait for any of the following conditions (or combinations):
                #  a) Data sampling of receiver: SCL = rising, and/or
                #  b) START condition (S): SCL = high, SDA = falling, and/or
                #  c) STOP condition (P): SCL = high, SDA = rising
                (scl, sda) = self.wait([{0: 'r'}, {0: 'h', 1: 'f'}, {0: 'h', 1: 'r'}])

                # Check which of the condition(s) matched and handle them.
                if self.matched & (0b1 << 0):
                    self.handle_address_or_data((scl, sda))
                elif self.matched & (0b1 << 1):
                    self.handle_start((scl, sda))
                elif self.matched & (0b1 << 2):
                    self.handle_stop((scl, sda))
            elif self.state == 'FIND ACK':
                # Wait for a data/ack bit: SCL = rising.
                self.get_ack(self.wait({0: 'r'}))
