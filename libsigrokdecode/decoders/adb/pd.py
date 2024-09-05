## Copyright (C) 2022 Jun Wako <wakojun@gmail.com>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## Permission is hereby granted, free of charge, to any person obtaining a copy
## of this software and associated documentation files (the "Software"), to deal
## in the Software without restriction, including without limitation the rights
## to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
## copies of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:
##
## The above copyright notice and this permission notice shall be included in all
## copies or substantial portions of the Software.
##
## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
## IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
## FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
## AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
## LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
## OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.

import sigrokdecode as srd

class Decoder(srd.Decoder):
    # https://sigrok.org/wiki/Protocol_decoder_API#Decoder_registration
    api_version = 3
    id = 'adb'
    name = 'ADB'
    longname = 'Apple Desktop Bus'
    desc = 'Decode command and data of Apple Desktop Bus protocol.'
    license = 'mit'
    inputs = ['logic']
    outputs = []
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Data line'},
    )
    options = ()
    tags = ['PC']
    annotations = (
        ('lo', 'Low'),                  # 0
        ('hi', 'High'),                 # 1
        ('attn', 'Attention'),          # 2
        ('greset', 'Global Reset'),     # 3
        ('bit', 'Bit'),                 # 4
        ('data', 'Data'),               # 5
        ('start','Start'),              # 6
        ('stop','Stop'),                # 7
        ('srq','Service Request'),      # 8
        ('reset', 'Reset'),             # 9
        ('flush', 'Flush'),             # 10
        ('listen', 'Listen'),           # 11
        ('talk', 'Talk'),               # 12
        ('unknown', 'Unknown'),         # 13
    )
    annotation_rows = (
        ('cells', 'Cells', (0,1,2,3,8)),
        ('bits', 'Bits', (4,6,7)),
        ('bytes', 'Bytes', (5,9,10,11,12,13)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        low = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def to_us(self, sample):
        return (sample / (self.samplerate / 1000000))

    def putl(self, ss, es):
        self.put(ss, es, self.out_ann, [0, ['%d' % self.to_us(es - ss)]])

    def puth(self, ss, es):
        self.put(ss, es, self.out_ann, [1, ['%d' % self.to_us(es - ss)]])

    def puta(self, ss, es):
        self.put(ss, es, self.out_ann, [2, ['Attn:%d' % self.to_us(es - ss), 'Attn', 'A']])

    def putr(self, ss, es):
        self.put(ss, es, self.out_ann, [3, ['Reset:%d' % self.to_us(es - ss), 'Rst', 'R']])

    def putb(self, ss, es, b):
        self.put(ss, es, self.out_ann, [4, ['%X' % b]])

    def putD(self, ss, es, D):
        self.put(ss, es, self.out_ann, [5, ['%02X' % D]])

    def putC(self, ss, es, C):
        addr = (C >> 4) & 0x0f
        cmd = C & 0x0f
        reg = C & 0x03
        if (cmd == 0):
            self.put(ss, es, self.out_ann, [9, ['Reset:%02X' % C, 'RST', 'R']])
        elif (cmd == 1):
            self.put(ss, es, self.out_ann, [10, ['Flush:%02X' % C, 'FLS', 'F']])
        elif ((cmd & 0x0c) == 0x08):
            self.put(ss, es, self.out_ann, [11, ['Listen($%X,r%d) %02X' % (addr, reg, C), 'L:%X:%d' % (addr, reg), 'L']])
        elif ((cmd & 0x0c) == 0x0c):
            self.put(ss, es, self.out_ann, [12, ['Talk($%X,r%d) %02X' % (addr, reg, C), 'T:%X:%d' % (addr, reg), 'T']])
        else:
            self.put(ss, es, self.out_ann, [13, ['Unknown:%02X' % C, 'Unk', 'U']])


    def putS(self, ss, es):
        self.put(ss, es, self.out_ann, [6, ['Start(1)', 'S1', 'S']])

    def putT(self, ss, es):
        self.put(ss, es, self.out_ann, [7, ['Stop(0)', 'T0', 'T']])

    def putQ(self, ss, es):
        self.put(ss, es, self.out_ann, [8, ['SRQ:%d' % self.to_us(es - ss), 'SRQ', 'Q']])

    def decode(self):
        '''
        Bit cell
        --------
                      ___
            bit1: |__|   |
                       __
            bit0: |___|  |
                  \   \  `--- cell_e
                   \   +----- low_e
                    +-------- cell_s

        '''
        byte = 0
        bit_count = 0
        attention = 0
        self.wait({0: 'f'})
        cell_s = self.samplenum
        while True:
            # low
            self.wait({0: 'r'})
            low_e = self.samplenum
            low = self.to_us(low_e - cell_s)
            if low < 100:
                # cell-low
                self.putl(cell_s, low_e)

                if bit_count % 8 == 0:
                    byte_s = cell_s
            elif low > 1500:
                # global reset
                self.putr(cell_s, low_e)
            elif low > 500:
                # attention(560-1040us)
                self.puta(cell_s, low_e)
                attention = 1
            else:
                # 100 <= low <= 500
                # SRQ(140-260us) after command
                self.putQ(cell_s, low_e)

            # high
            self.wait({0: 'f'})
            cell_e = self.samplenum
            high = self.to_us(cell_e - low_e)
            cell = self.to_us(cell_e - cell_s)
            if high < 100:
                # cell-high
                self.puth(low_e, cell_e)

                if cell <= 130:
                    # bit-cell  0:___-- 1:__---
                    bit_count += 1
                    if bit_count == 0:
                        # start-bit(1) __---
                        self.putS(cell_s, cell_e)
                    else:
                        if low > high:
                            # bit0
                            self.putb(cell_s, cell_e, 0)
                            byte = ((byte << 1) & 0xff) | 0
                        else:
                            # bit 1
                            self.putb(cell_s, cell_e, 1)
                            byte = ((byte << 1) & 0xff) | 1

                    if bit_count and bit_count % 8 == 0:
                        # byte
                        if attention == 1:
                            # comand
                            self.putC(byte_s, cell_e, byte)
                            attention = 0
                            bit_count = -1
                        else:
                            # data
                            self.putD(byte_s, cell_e, byte)
                else:
                    if low < 100:
                        # stop-bit(0)  ___--
                        self.putT(cell_s, cell_e)
                    else:
                        # start-bit(1) after attention  _____---
                        self.putS(low_e, cell_e)
                        bit_count = 0
            else:
                # high >= 100
                if self.to_us(low_e - cell_s) < 100:
                    # low < 100
                    # ___-----
                    # stop-bit(0)
                    self.putT(cell_s, low_e)

            cell_s = cell_e
