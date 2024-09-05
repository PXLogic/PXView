##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 ottojo
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

from typing import List

import sigrokdecode as srd

regs = {
    0x0000: 'NOP',
    0x0001: 'ERRFL',
    0x0003: 'PROG',
    0x0016: 'ZPOSM',
    0x0017: 'ZPOSL',
    0x0018: 'SETTINGS1',
    0x0019: 'SETTINGS2',
    0x3FFC: 'DIAAGC',
    0x3FFD: 'MAG',
    0x3FFE: 'ANGLEUNC',
    0x3FFF: 'ANGLECOM',
}


class Decoder(srd.Decoder):
    api_version = 3
    id = 'as5047'
    name = 'as5047'
    longname = 'as5047'
    desc = 'as5047'
    license = 'JSON'
    inputs = ['spi']
    outputs = []
    tags = ['Embedded/industrial']
    annotations = (
        ('commandframe', 'command frame'),  # 0
        ('readdataframe', 'read data frame'),  # 1
        ('writedataframe', 'write data frame'),  # 2
        ('registerread', 'register read'),  # 3
        ('registerwrite', 'register write'),  # 4
        ('warning', 'warning'),  # 5
        ('field', 'field'),  # 6
    )
    annotation_rows = (  # row id, row name, annotation class indices
        ('fields', 'fields', (6,)),
        ('frames', 'frames', (0, 1, 2)),
        ('transactions', 'transactions', (3, 4)),
        ('warnings', 'warnings', (5,))
    )

    def __init__(self):
        self.state = 'init'
        self.transaction_start = 0
        self.current_reg = 0
        self.reset()

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def reset(self):
        self.transaction_start = 0
        self.current_reg = 0
        self.state = 'init'

    def putx(self, start, end, data):
        self.put(start, end, self.out_ann, data)

    def putWarning(self, start, end, desc: List[str]):
        self.putx(start, end, [5, desc])

    def decode(self, ss, es, data):
        ptype = data[0]
        if ptype == 'CS-CHANGE':
            # TODO
            # self.reset()
            return

        # Don't care about anything else.
        if ptype != 'DATA':
            return
        mosi, miso = data[1:]

        if self.state == 'init' or self.state == 'write':
            if bin(mosi).count('1') % 2 != 0:
                self.putWarning(ss, es, ["mosi parity"])

        if self.state == 'init':
            self.transaction_start = ss
            reg = mosi & 0b0011111111111111
            self.current_reg = reg
            reg_desc = regs.get(reg, f"unknown")

            if mosi & 0b0100000000000000 != 0:
                self.state = 'read'
                self.putx(ss, es, [0, [f"read from {reg_desc} (0x{reg:04x})", f"read {reg_desc}", reg_desc]])
            else:
                self.state = 'write'
                self.putx(ss, es, [0, [f"write to {reg_desc} (0x{reg:04x})", f"write {reg_desc}", reg_desc]])
            self.transaction_start = ss
        else:
            if bin(miso).count('1') % 2 != 0:
                self.putWarning(ss, es, ["miso parity"])
            reg_desc = regs.get(self.current_reg, f"unknown")
            if self.state == 'read':
                if miso & 0b0100000000000000:
                    self.putWarning(ss, es, ["error flag set", "EF"])
                if bin(miso).count('1') % 2 != 0:
                    self.putWarning(ss, es, ["miso parity"])
                data = miso & 0b0011111111111111
                self.putx(ss, es, [1, [f"read data frame: 0x{data:04x}", f"read 0x{data:04x}", f"0x{data:04x}"]])
                self.putx(self.transaction_start, es,
                          [3, [f"Read 0x{data:04x} from {reg_desc}", f"{reg_desc}:0x {data}"]])
            if self.state == 'write':
                data = mosi & 0b0011111111111111
                self.putx(ss, es, [2, [f"write data frame: 0x{data:04x}", f"write 0x{data:04x}", f"0x{data:04x}"]])
                self.putx(self.transaction_start, es,
                          [4, [f"Write 0x{data:04x} to {reg_desc}", f"0x{data} -> {reg_desc}"]])

            self.state = 'init'
