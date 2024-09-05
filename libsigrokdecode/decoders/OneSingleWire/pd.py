##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 yayayat
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


class Decoder(srd.Decoder):
    api_version = 3
    id = 'OneSingleWire'
    name = 'OneSingleWire custom bus'
    longname = 'OneSingleWire custom bus used in roboSet'
    desc = 'Bidirectional, half-duplex, asynchronous serial bus.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['OneSingleWire']
    tags = ['Custom']
    channels = (
        {'id': 'osw', 'name': 'OSW', 'desc': 'OSW signal line'},
        {'id': 'strt', 'name': 'Start Pulse',
            'desc': 'OSW device start pulse signal'}
    )

    options = (
        {'id': 'threshold', 'desc': 'Threshold time value (us)', 'default': 8},
    )

    annotations = (
        ('bit', 'Bit'),
        ('byte', 'Byte'),
        ('sample', 'Sample'),
        ('wait', 'Wait'),
        ('pb', 'PB'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0, 3,)),
        ('bytes', 'Bytes', (1, 4)),
        ('samples', 'Samples', (2,)),
    )

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def reset(self):
        pass

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.threshold_samples_num = int(
                self.options['threshold'] * (value / 1000000.0))

    def decode(self):
        self.wait({1: 'r'})
        self.wait({0: 'f'})
        self.bt_block_ss = self.samplenum
        self.by_block_ss = self.samplenum
        i = 0
        decoded_byte = 0
        parity_bit = 0
        while True:
            self.wait({0: 'e'})
            period_range = self.samplenum - self.bt_block_ss
            if(i < 9):
                if (period_range < self.threshold_samples_num):
                    osw = 1
                else:
                    osw = 0
                decoded_byte |= (osw << i)
                parity_bit ^= osw
                if(i == 7):
                    self.put(self.by_block_ss, self.samplenum,
                             self.out_ann, [1, ['Byte: %d' % decoded_byte, '%d' % decoded_byte]])
                elif(i == 8):
                    self.put(self.bt_block_ss, self.samplenum,
                             self.out_ann, [4, ['Parity check: %s' % 'OK' if parity_bit == 0 else 'ERR', 'OK' if parity_bit == 0 else 'ERR']])
                self.put(self.bt_block_ss, self.samplenum,
                         self.out_ann, [0, ['Bit: %d' % osw, '%d' % osw]])
                self.put(self.bt_block_ss, self.samplenum,
                         self.out_ann, [2, ['Samples: %d' % period_range, '%d' % period_range]])
                i += 1
            else:
                self.put(self.bt_block_ss, self.samplenum,
                         self.out_ann, [3, ['Wait', 'w']])
                self.put(self.bt_block_ss, self.samplenum,
                         self.out_ann, [2, ['Samples: %d' % period_range, '%d' % period_range]])
                self.by_block_ss = self.samplenum
                decoded_byte = 0
                parity_bit = 0
                i = 0
            self.bt_block_ss = self.samplenum
