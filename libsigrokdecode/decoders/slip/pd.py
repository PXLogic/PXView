##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017 Hattori, Hiroki <seagull.kamome@gmail.com>
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

RX = 0
TX = 1
RX_JUNK = 2
TX_JUNK = 3

class Decoder(srd.Decoder):
    api_version = 3
    id = 'slip'
    name = 'slip'
    longname = 'SLIP packet decoder'
    desc = 'SLIP packet decoder.'
    license = 'BSD3'
    inputs = ['uart']
    outputs = ['slip']
    tags = ['Embedded']
    annotations = (
        ('rx', 'RX packet.'),
        ('tx', 'TX packet.'),
        ('rx-err', 'RX Garbages.'),
        ('tx-err', 'TX Garbages.'),
    )
    annotation_rows = (
        ('rxs', 'RX', (0,2)),
        ('txs', 'TX', (1,3))
    )


    def __init__(self):
        self.cmd = ['', '']
        self.datavalues = [[], []]
        self.inside_packet = [False, False]
        self.ss_block = [None, None]
        self.es_block = [None, None]
        self.dir = ['RX:', 'TX:']
        self.escaping = [False, False]

    def reset(self):
        self.__init__(self)

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)

    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data

        if ptype != 'DATA':
            return

        pdata = pdata[0]

        if self.escaping[rxtx]:
            if pdata == 0xdc:
                pdata = 0xc0
            elif pdata == 0xdd:
                pdata = 0xdb
            self.escaping[rxtx] = False
            self.cmd[rxtx] += "{:02X} ".format(pdata)
            self.datavalues[rxtx].append(pdata)

        elif pdata == 0xc0:
            if self.inside_packet[rxtx]: # It's the end of packet.
                self.put(self.ss_block[rxtx], es, self.out_ann, [rxtx, [self.dir[rxtx] + self.cmd[rxtx]]])
                self.put(self.ss_block[rxtx], es, self.out_python, [rxtx, self.datavalues[rxtx]])
                self.ss_block[rxtx] = None
                self.inside_packet[rxtx] = False
            else: # It's the begin of packet.
                if self.ss_block[rxtx] is not None:
                    self.put(self.ss_block[rxtx], self.es_block[rxtx], self.out_ann, [rxtx + 2, ['JUNK ' + self.dir[rxtx] + self.cmd[rxtx]]])
                    self.put(self.ss_block[rxtx], es, self.out_python, [rxtx + 2, self.datavalues[rxtx]])
                self.ss_block[rxtx] = ss
                self.inside_packet[rxtx] = True

            self.cmd[rxtx] = ''
            self.datavalues[rxtx] = []

        elif pdata == 0xdb and self.inside_packet[rxtx]:
            self.escaping[rxtx] = True

        else:
            self.cmd[rxtx] += "{:02X} ".format(pdata)
            self.datavalues[rxtx].append([pdata])
            if self.ss_block[rxtx] is None:
                self.ss_block[rxtx] = ss

        self.es_block[rxtx] = es

