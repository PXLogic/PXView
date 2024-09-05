##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2022 Sergey Spivak <sespivak@yandex.ru>
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

from itertools import islice
from collections import deque
import sigrokdecode as srd


class Ann:
    HEADER, DATASIZE, CHECKSUM, \
    ANSWER, COMMAND, DATA_RX, DATA_TX,  \
    PACKET_RX, PACKET_TX, WARN = range(10)


RX = 0
TX = 1

PACKETSIZE_MAX = 64
PACKETSIZE_MIN = 4


class BufPos:
    HEADER = 1
    DATA_SIZE = 2
    DATA_TYPE = 3
    DATA_START = 4


def byte_to_hex(byte):
    return '{:02X}'.format(byte)


def bytes_to_hex(data):
    data = ['{:02X}'.format(byte) for byte in data]
    return ' '.join(data)


class Decoder(srd.Decoder):
    api_version = 3
    id = 'streletz'
    name = 'Streletz'
    longname = 'Streletz RS232 (Serial bus)'
    desc = 'Serial bus for guard system Streletz'
    license = 'mit'
    inputs = ['uart']
    outputs = ['streletz']
    tags = ['Embedded/industrial']
    options = (
        {'id': 'header_tx', 'desc': 'Request header', 'default': 217},
        {'id': 'header_rx', 'desc': 'Response header', 'default': 157},
    )
    annotations = (
        ('head', 'Header'),
        ('datasize', 'Data Size'),
        ('checksum', 'Checksum'),
        ('answer', 'Answer'),
        ('command', 'Command'),
        ('rx-data', 'RX Data'),
        ('tx-data', 'TX Data'),
        ('rx-packet', 'RX packet'),
        ('tx-packet', 'TX packet'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('framing', 'Framing', (Ann.HEADER, Ann.DATASIZE, Ann.CHECKSUM)),
        ('data', 'Data', (Ann.ANSWER, Ann.COMMAND, Ann.DATA_RX, Ann.DATA_TX,)),
        ('warnings', 'Warnings', (Ann.WARN,)),
        ('packets', 'Packets', (Ann.PACKET_RX, Ann.PACKET_TX)),
    )

    def __init__(self):
        self.out_py = None
        self.out_ann = None
        self.msg_complete = None
        self.failed = None
        self.checksum = 0
        self.accum_bytes = deque(maxlen=PACKETSIZE_MAX)
        self.rxtx = 0
        self.packet_size = None
        self.packet_ss = None
        self.packet_es = None
        self.data_ss = None
        self.data_es = None
        self.buf_pos = 0
        self.header = [None, None]
        self.reset()

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.header = self.options['header_rx'], self.options['header_tx']

    def reset(self):
        self.checksum = 0
        self.accum_bytes.clear()
        self.rxtx = 0
        self.packet_size = None
        self.packet_ss = None
        self.packet_es = None
        self.data_ss = None
        self.data_es = None
        self.buf_pos = 0

    def putg(self, ss, es, data):
        """Put a graphical annotation."""
        self.put(ss, es, self.out_ann, data)

    def handle_byte(self, ss, es, byte, rxtx):
        """UART data bits were seen. Store them, validity is yet unknown."""

        if self.buf_pos > 0:
            if self.rxtx == rxtx:
                self.buf_pos += 1
                self.accum_bytes.append(byte)
                self.checksum ^= byte
            else:
                self.reset()

        # wait header
        if self.buf_pos < BufPos.HEADER:
            if byte == self.header[rxtx]:
                self.buf_pos = BufPos.HEADER
                self.rxtx = rxtx
                self.accum_bytes.append(byte)
                self.packet_ss = ss
                self.checksum = byte
                self.putg(ss, es, [Ann.HEADER, ['HEAD: 0x' + byte_to_hex(byte),
                                                'HEAD', 'H']])

        else:
            if self.buf_pos == BufPos.DATA_SIZE:
                # data size
                self.packet_size = PACKETSIZE_MIN + byte
                if self.packet_size > self.accum_bytes.maxlen:
                    self.putg(ss, es, [Ann.WARN, ['Wrong DS: 0x' + byte_to_hex(byte),
                                                  'WDS']])
                    self.reset()
                    return
                else:
                    self.putg(ss, es, [Ann.DATASIZE, ['DS: 0x' + byte_to_hex(byte),
                                                      'DS']])
            elif self.buf_pos == BufPos.DATA_TYPE:
                # data type - command or answer
                if rxtx == TX:
                    self.putg(ss, es, [Ann.COMMAND, ['CMD: 0x' + byte_to_hex(byte), 'CMD']])
                else:
                    self.putg(ss, es, [Ann.ANSWER, ['ANS: 0x' + byte_to_hex(byte), 'ANS']])

            elif self.buf_pos == BufPos.DATA_START:
                # start of data
                self.data_ss = ss

            if self.buf_pos == self.packet_size - 1:
                # End of data block
                self.data_es = es

            if self.buf_pos == self.packet_size:
                # Checksum bytes: end of packet
                cs_str = 'CS: 0x' + byte_to_hex(byte)
                rxtx_str = 'TX' if rxtx == TX else 'RX'
                packet_str = bytes_to_hex(self.accum_bytes)
                self.packet_es = es
                if self.checksum == 0:
                    # Correct checksum received
                    self.putg(ss, es, [Ann.CHECKSUM, ['CS: 0x' + byte_to_hex(byte), 'CS']])
                    packet_ann = Ann.PACKET_TX if rxtx == TX else Ann.PACKET_RX
                    self.putg(self.packet_ss, self.packet_es,
                              [packet_ann, ['{} PACKET: {}'.format(rxtx_str, packet_str),
                                            rxtx_str + ' PACKET',
                                            rxtx_str + 'P']])
                    if self.packet_size > PACKETSIZE_MIN:
                        data_str = bytes_to_hex(islice(self.accum_bytes, BufPos.DATA_START-1, self.packet_size-1))
                        self.putg(self.data_ss, self.data_es,
                                  [Ann.DATA_TX if rxtx == TX else Ann.DATA_RX, [
                                      rxtx_str + ' DATA: ' + data_str,
                                      rxtx_str + 'DATA',
                                      rxtx_str + 'D', 'D']])
                else:
                    # Wrong checksum received
                    self.putg(self.packet_ss, self.packet_es,
                              [Ann.WARN, ['Err {} PACKET: {}'.format(rxtx_str, packet_str),
                                          'Err ' + rxtx_str + ' PACKET',
                                          rxtx_str + 'P', 'EP']])

                self.reset()
                return

    def decode(self, ss, es, data):
        # Analyze DATA bits only
        ptype, rxtx, pdata = data
        if ptype == 'FRAME':
            data_value, frame_valid = pdata
            self.handle_byte(ss, es, data_value, rxtx)
