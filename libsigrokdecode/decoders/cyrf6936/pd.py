##
## Copyright (C) 2016 Soenke J. Peters
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## Based on decoders/nrf24l01/pd.py of the libsigrokdecode project,
## copyright (C) 2014 Jens Steinhauser <jens.steinhauser@gmail.com>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program. If not, see <http://www.gnu.org/licenses/>.

import sigrokdecode as srd
from .regdecode import *
from .regs import *

class ChannelError(Exception):
    pass

class Decoder(srd.Decoder):
    api_version = 3
    id = 'cyrf6936'
    name = 'CYRF6936'
    longname = 'Cypress CYRF6936 WirelessUSB(TM) LP 2.4 GHz Radio SoC'
    desc = '2.4GHz transceiver chip.'
    license = 'gplv3+'
    inputs = ['spi']
    outputs = ['cyrf6936']
    tags = ['Embedded/industrial']
    annotations = (
        ('write', 'Write'),
        ('read', 'Read'),
        ('tx-data', 'Payload sent to the device'),
        ('rx-data', 'Payload read from the device'),
        ('state', 'State change'),
        ('warning', 'Warnings'),
        ('wait', 'Wait'),
    )
    ann_write = 0
    ann_read = 1
    ann_tx = 2
    ann_rx = 3
    ann_state = 4
    ann_warn = 5
    ann_wait = 6
    annotation_rows = (
        ('cmd', 'Commands', (ann_write, ann_read, ann_tx, ann_rx)),
        ('warnings', 'Warnings', (ann_warn, ann_state)),
        ('delays', 'Delays', (ann_wait,)),
    )
    options = (
            {'id': 'spi3pin', 'desc': 'SPI 3-pin mode with MOSI/MISO combined as SDAT on the MOSI pin',
                'default': 'no', 'values': ('no', 'yes')},
            {'id': 'delaysplit', 'desc': 'annotate delays (in us) larger than... (0 = off)', 'default': 0},
            {'id': 'invert_mosi', 'desc': 'Invert MOSI', 'default': 'no', 'values': ('yes', 'no')},
            {'id': 'invert_miso', 'desc': 'Invert MISO', 'default': 'no', 'values': ('yes', 'no')},
    )
    binary = (
        ('txpayload', 'Transfer payload'),
        ('rxpayload', 'Receive payload'),
    )

    def __init__(self):
        self.reset()
        self.spi3pin = 0 # 0 = 4-pin SPI with CLK, CSn, MOSI, MISO; 1 = 3-pin SPI with CLK, CSn, SDAT
        self.requirements_met = True
        self.cs_was_released = False
        self.samplerate = None
        self.delaysplit = 0
        self.wait_s = 0
        self.wait_e = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        if self.options['spi3pin'] == 'yes':
            self.spi3pin = 1
        try:
            self.delaysplit = float(self.options['delaysplit'])
        except ValueError:
            self.delaysplit = 0

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def warn(self, pos, msg):
        '''Put a warning message 'msg' at 'pos'.'''
        self.put(pos[0], pos[1], self.out_ann, [self.ann_warn, [msg]])

    def putp(self, pos, ann, msg):
        '''Put an annotation message 'msg' at 'pos'.'''
        self.put(pos[0], pos[1], self.out_ann, [ann, [msg]])

    def putb(self, pos, data):
        self.put(pos[0], pos[1], self.out_binary, data)

    def reset(self):
        '''Resets the decoder after a complete command was decoded.'''
        # 'True' for the first byte after CS went low.
        self.first = True

        # The current command, and the minimum and maximum number
        # of data bytes to follow.
        self.addr = None
        self.dir_wr = 1 # Command direction: 1 = write, 0 = read
        self.inc = 0 # Command increment: 1 = auto address increment, 0 = same address
        self.min = 0
        self.max = 0

        # Used to collect the bytes after the command byte
        # (and the start/end sample number).
        self.mb = []
        self.mb_s = -1
        self.mb_e = -1

    def next(self):
        ''' Sets decoder to the values for the next address.'''
        # The current command, and the minimum and maximum number
        # of data bytes to follow.
        if self.inc:
            self.addr += 1
        self.min = 0
        self.max = 0

        # Used to collect the bytes after the command byte
        # (and the start/end sample number).
        self.mb = []
        self.mb_s = -1
        self.mb_e = -1

    def mosi_bytes(self):
        '''Returns the collected MOSI bytes of a multi byte command.'''
        return [b[0] for b in self.mb]

    def miso_bytes(self):
        '''Returns the collected MISO bytes of a multi byte command.'''
        return [b[1] for b in self.mb]

    def decode_command(self, pos, b):
        '''Decodes the command byte 'b' at position 'pos' and prepares
        the decoding of the following data bytes.'''
        self.addr, self.dir_wr, self.inc = self.parse_command(b)
        if self.addr in regs:
            self.max = regs[self.addr][1]
        else:
            self.warn(pos, 'unknown address/register')

        self.min = 1

        self.mb_s = pos[0]

    def format_command(self, textdata = None):
        '''Returns the label for the current command.'''
        reg = RegDecode.name(self.addr)
        if reg is None:
            reg = hex(self.addr)

        multi = '_inc' if self.inc == 1 else ''

        if self.dir_wr == 1:
            if textdata is None:
                return 'write{}({})'.format(multi, reg)
            else:
                return 'write{}({}, "{}")'.format(multi, reg, textdata)
        else:
            if textdata is None:
                return 'read{}({})'.format(multi, reg)
            else:
                return 'read{}({}) == "{}"'.format(multi, reg, textdata)

    def parse_command(self, b):
        '''Parses the command byte.

        Returns a tuple consisting of:
        - the name of the command / register
        - direction of transaction (read/write)
        - single address or incremental address
        '''
        cmd_addr = (b & 0b111111)
        cmd_dir = (b & (1<<7))>>7  # 1 = write, 0 = read
        cmd_inc = (b & (1<<6))>>6  # 1 = auto address increment, 0 = keep address

        return (cmd_addr, cmd_dir, cmd_inc)

    def convert_mb_data(self, data, always_hex = True):
        '''Converts the data bytes 'data' of a multibyte command to text.
        If 'always_hex' is True, all bytes are decoded as hex codes, otherwise only non
        printable characters are escaped.'''

        prefix = ''
        if always_hex:
            prefix = '0x'
            def escape(b):
                return '{:02X}'.format(b)
        else:
            def escape(b):
                c = chr(b)
                if not str.isprintable(c):
                    return '\\x{:02X}'.format(b)
                return c

        return '%s%s' % (prefix, ''.join([escape(b) for b in data]))

    def finish_command(self, pos):
        '''Decodes the remaining data bytes at position 'pos'.'''

        def slipad(data, w, p = 0):
            d = w - len(data)
            if d < 0:
                d = 0
            return data[0:w] + [ p for i in range(0,d)]

        if self.dir_wr == 1:
            data = self.mosi_bytes()
            if RegDecode.name(self.addr) == 'TX_BUFFER_ADR':
                # slice/pad data to the register width
                bindata = slipad(data, RegDecode.width(self.addr))
                self.putb(pos, [0, bytes(bindata)])
        else:
            data = self.miso_bytes() if (self.spi3pin == 0) else self.mosi_bytes()
            if RegDecode.name(self.addr) == 'RX_BUFFER_ADR':
                # slice/pad data to the register width
                bindata = slipad(data, RegDecode.width(self.addr))
                self.putb(pos, [1, bytes(bindata)])

        decoded = RegDecode.decode(self.addr, data)
        if type(decoded) == tuple:
            textdata, warn = decoded
            if warn == '':
                warn = None
        else:
            textdata = decoded
            warn = None

        self.putp(pos, self.ann_write if (self.dir_wr == 1) else self.ann_read, self.format_command(textdata))
        if not warn is None:
            self.warn(pos, warn)


    def decode(self, ss, es, data):
        if not self.requirements_met:
            return

        ptype, data1, data2 = data

        if ptype == 'CS-CHANGE':
            if data1 is None:
                if data2 is None:
                    self.requirements_met = False
                    raise ChannelError('CS# pin required.')
                elif data2 == 1:
                    self.cs_was_released = True

            if data1 == 0 and data2 == 1:
                # Rising edge, the complete command is transmitted, process
                # the bytes that were send after the command byte.
                if self.addr:
                    # Check if we got the minimum number of data bytes
                    # after the command byte.
                    if len(self.mb) < self.min:
                        self.warn((ss, ss), 'missing data bytes')

                    if (self.mb_s != -1) and len(self.mb) > 0:
                        self.finish_command((self.mb_s, self.mb_e))
                self.wait_s = es

                self.reset()
                self.cs_was_released = True

            if data1 == 1 and data2 == 0:
                # Falling edge of CS# / start of pause
                self.wait_e = ss
                if (self.delaysplit > 0) and (self.wait_e > self.wait_s):
                    dt = ((self.wait_e - self.wait_s) *1000000)/ self.samplerate
                    if (dt >= self.delaysplit):
                        self.putp((self.wait_s, self.wait_e), self.ann_wait, "delay_us({})".format(dt))

        elif ptype == 'DATA' and self.cs_was_released:
            mosi, miso = data1, data2
            pos = (ss, es)

            if mosi is None:
                self.requirements_met = False
                raise ChannelError('A MOSI/SDAT pin is required.')

            if self.options['invert_mosi'] == 'yes':
                mosi = not mosi
            if self.options['invert_miso'] == 'yes':
                miso = not miso

            if self.first:
                self.first = False
                # First MOSI byte is always the command.
                self.addr, self.dir_wr, self.inc = self.parse_command(mosi)
                self.mb_s = pos[0]

                # First MISO byte is discarded
                if ((miso is not None) and not ((miso == 0xff) or (miso == 0x00))):
                    self.warn(pos, 'unrequested data {}'.format(miso))
            else:
                if RegDecode.valid(self.addr):
                    self.max = RegDecode.width(self.addr)
                else:
                    self.max = 0

                if (len(self.mb) > self.max):
                        self.warn(pos, 'excess byte')
                elif ((miso is None) and (self.spi3pin == 0)):
                    self.requirements_met = False
                    raise ChannelError('A MISO pin is required in 4-pin SPI mode.')
                else:
                    # Collect the bytes after the command byte.
                    if self.mb_s == -1:
                        self.mb_s = ss
                    self.mb_e = es
                    self.mb.append((mosi, miso))
                    if (len(self.mb) >= self.max):
                        self.finish_command((self.mb_s, self.mb_e))
                        self.next()

                    if self.addr == 0x0d: # IO_CFG_ADDR
                        old_spi3pin = self.spi3pin
                        if (self.dir_wr == 0) and (self.spi3pin == 0):
                            b = miso
                        else:
                            b = mosi
                        if ((b & 0x02) == 0x02):
                            self.spi3pin = 1
                            msg = "3-pin SPI mode (SDAT)"
                        else:
                            self.spi3pin = 0
                            msg = "4-pin SPI mode (MOSI/MISO)"
                        if self.spi3pin != old_spi3pin:
                            self.put(ss, es, self.out_ann, [self.ann_status, [msg]])

                    if self.inc:
                        self.next()
