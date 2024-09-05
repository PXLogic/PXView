##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2022 Simon Struck
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


'''
This decoder stacks on top of the 'i2c' decoder and decodes the ST25DV protocol.
'''

import sigrokdecode as srd


class Field:
    def __init__(self, name: str, shift: int, mask: int):
        self.name = name
        self.shift = shift
        self.mask = mask

    def get_value(self, register_value: int) -> int:
        value = register_value & self.mask
        value >>= self.shift
        return value

    def to_string(self, register_value: int) -> str:
        value = self.get_value(register_value)
        return "%s: %d" % (self.name, value)


class Register:
    def __init__(self, short_name: str, long_name: str, length: int, fields: [Field]):
        self.short_name = short_name
        self.long_name = long_name
        self.length = length
        self.fields = fields

    def get_name_strings(self) -> [str]:
        strings = [self.long_name, self.short_name]
        return strings

    def get_long_value_strings(self, register_value: int) -> str:
        strings = []

        for field in self.fields:
            strings.append(field.to_string(register_value))
        else:
            strings.append('%s: %02X' % (self.long_name, register_value))

        return ', '.join(strings)

    def get_short_field_strings(self, register_value: int) -> str:
        strings = []

        for field in self.fields:
            if field.get_value(register_value) != 0:
                strings.append(field.to_string(register_value))
        else:
            strings.append('%s: %02X' % (self.short_name, register_value))

        return ', '.join(strings)

    def get_nonzero_field_strings(self, register_value: int) -> str:
        strings = []

        for field in self.fields:
            if field.get_value(register_value) != 0:
                strings.append(field.name)
        else:
            strings.append('%02X' % register_value)

        return '|'.join(strings)


class Decoder(srd.Decoder):
    api_version = 3
    id = 'st25dv'
    name = 'ST25DV'
    longname = 'ST25DV'
    desc = 'ST25DV NFC EEPROM'
    license = 'mit'
    inputs = ['i2c']
    outputs = ['st25dv']
    tags = ['Embedded/industrial']
    ann_sys = 0
    ann_data = 1
    ann_read = 2
    ann_write = 3
    ann_error = 4
    annotations = (
        ('sys', 'System'),
        ('data', 'Data'),
        ('read', 'Read'),
        ('write', 'Write'),
        ('error', 'Error')
    )
    annotation_rows = (
        ('regs', 'Register access', (0, 1, 2, 3, 4)),
    )
    step = 0
    address = 0
    reg_address = 0
    op = None
    data = []
    reg_start_sample = None
    reg_end_sample = None
    data_start_sample = None
    data_end_sample = None

    registers = {
        0x0000: Register('GPO', 'GPO', 1, [
            Field('RFUSERSTATE', 0, 0x01),
            Field('RFACTIVITY', 1, 0x02),
            Field('RFINTERRUPT', 2, 0x04),
            Field('FIELDCHANGE', 3, 0x08),
            Field('RFPUTMSG', 4, 0x10),
            Field('RFGETMSG', 5, 0x20),
            Field('RFWRITE', 6, 0x40),
            Field('ENABLE', 7, 0x80),
        ]),
        0x0001: Register('ITTIME', 'IT duration', 1, [
            Field('ITTIME_DELAY', 0, 0x03),
        ]),
        0x0002: Register('EH_MODE', 'Energy Harvesting', 1, [
            Field('EH_MODE', 0, 0x01)
        ]),
        0x0003: Register('RF_MNGT', 'RF management', 1, [
            Field('RFDIS', 0, 0x01),
            Field('RFSLEEP', 1, 0x02),
        ]),
        0x0004: Register('RFZ1SS', 'Area 1 security', 1, [
            Field('PWDCTRL', 0, 0x03),
            Field('RWPROT', 2, 0x0C),
        ]),
        0x0005: Register('END1', 'Area 1 end address', 1, []),
        0x0006: Register('RFZ2SS', 'Area 2 security', 1, [
            Field('PWDCTRL', 0, 0x03),
            Field('RWPROT', 2, 0x0C),
        ]),
        0x0007: Register('END2', 'Area 2 end address', 1, []),
        0x0008: Register('RFZ3SS', 'Area 3 security', 1, [
            Field('PWDCTRL', 0, 0x03),
            Field('RWPROT', 2, 0x0C),
        ]),
        0x0009: Register('END3', 'Area 3 end address', 1, []),
        0x000A: Register('RFZ4SS', 'Area 4 security', 1, [
            Field('PWDCTRL', 0, 0x03),
            Field('RWPROT', 2, 0x0C),
        ]),
        0x000B: Register('I2CZSS', 'I2C security', 1, [
            Field('PZ1', 0, 0x03),
            Field('PZ2', 2, 0x0C),
            Field('PZ3', 4, 0x30),
            Field('PZ4', 6, 0xC0),
        ]),
        0x000C: Register('LOCKCCFILE', 'Capability Container lock', 1, [
            Field('BLCK0', 0, 0x01),
            Field('BLCK1', 1, 0x02),
        ]),
        0x000D: Register('MB_MODE', 'Mailbox mode', 1, [
            Field('RW', 0, 0x01),
        ]),
        0x000E: Register('MB_WDG', 'Mailbox Watchdog', 1, [
            Field('DELAY', 0, 0x07),
        ]),
        0x000F: Register('LOCKCFG', 'Configuration lock', 1, [
            Field('B0', 0, 0x01),
        ]),
        0x0010: Register('LOCKDSFID', 'DSFID lock', 1, []),
        0x0011: Register('LOCKAFI', 'AFI lock', 1, []),
        0x0012: Register('DSFID', 'DSFID', 1, []),
        0x0013: Register('AFI', 'AFI', 1, []),
        0x0014: Register('MEM_SIZE', 'Memory size', 1, []),
        0x0017: Register('ICREF', 'ICref', 1, []),
        0x0018: Register('UID', 'UID', 1, []),
        0x0020: Register('ICREV', 'IC revision', 1, []),
        0x0900: Register('I2CPASSWD', 'I2C password', 17, []),
        0x2000: Register('GPO_DYN', 'GPO dynamic', 1, [
            Field('RFUSERSTATE', 0, 0x01),
            Field('RFACTIVITY', 1, 0x02),
            Field('RFINTERRUPT', 2, 0x04),
            Field('FIELDCHANGE', 3, 0x08),
            Field('RFPUTMSG', 4, 0x10),
            Field('RFGETMSG', 5, 0x20),
            Field('RFWRITE', 6, 0x40),
            Field('ENABLE', 7, 0x80),
        ]),
        0x2002: Register('EH_CTRL_DYN', 'Energy Harvesting control dynamic', 1, [
            Field('EH_EN', 0, 0x01),
            Field('EH_ON', 1, 0x02),
            Field('FIELD_ON', 2, 0x04),
            Field('VCC_ON', 3, 0x08),
        ]),
        0x2003: Register('RF_MNGT_DYN', 'RF management dynamic', 1, [
            Field('RFDIS', 0, 0x01),
            Field('RFSLEEP', 1, 0x02),
        ]),
        0x2004: Register('I2C_SSO_DYN', 'I2C secure session opened dynamic', 1, [
            Field('I2CSSO', 0, 0x01),
        ]),
        0x2005: Register('ITSTS_DYN', 'Interrupt status dynamic', 1, [
            Field('RFUSERSTATE', 0, 0x01),
            Field('RFACTIVITY', 1, 0x02),
            Field('RFINTERRUPT', 2, 0x04),
            Field('FIELDFALLING', 3, 0x08),
            Field('FIELDRISING', 4, 0x10),
            Field('RFPUTMSG', 5, 0x20),
            Field('RFGETMSG', 6, 0x40),
            Field('RFWRITE', 7, 0x80),
        ]),
        0x2006: Register('MB_CTRL_DYN', 'Mailbox control dynamic', 1, [
            Field('MBEN', 0, 0x01),
            Field('HOSTPUTMSG', 1, 0x02),
            Field('RFPUTMSG', 2, 0x04),
            Field('STRESERVED', 3, 0x08),
            Field('HOSTMISSMSG', 4, 0x10),
            Field('RFMISSMSG', 5, 0x20),
            Field('HOSTCURRENTMSG', 6, 0x40),
            Field('RFCURRENTMSG', 7, 0x80),
        ]),
        0x2007: Register('MB_LEN_DYN', 'Mailbox message length dynamic', 1, [
            Field('MBLEN', 0, 0xFF),
        ]),
        0x2008: Register('MAILBOX_RAM', 'Mailbox', 256, []),
    }

    def reset(self):
        print('ST25DV DEBUG RESET')
        self.step = 0
        self.address = 0
        self.reg_address = 0
        self.op = None
        self.data = []
        self.reg_start_sample = None
        self.reg_end_sample = None
        self.data_start_sample = None
        self.data_end_sample = None

    def start(self):
        self.register(srd.OUTPUT_ANN)
        pass

    def decode(self, startsample, endsample, data):
        cmd, byte = data
        if cmd == 'BITS':
            return
        self.print_state()
        print('ST25DV DEBUG', data)
        # BEFORE START
        if self.step == 0:
            if cmd == 'START' or cmd == 'START REPEAT':
                self.step = 1
            else:
                self.reset()
        # BEFORE ADDRESS
        elif self.step == 1:
            if cmd == 'ADDRESS WRITE':
                self.address = byte
                self.step = 2
        elif self.step == 2:
            if cmd == 'ACK':
                self.step = 3
            elif cmd == 'NACK':
                self.reset()
        # BEFORE REG MSB
        elif self.step == 3:
            if cmd == 'DATA WRITE':
                self.reg_start_sample = startsample
                self.reg_address = byte
                self.step = 4
        elif self.step == 4:
            if cmd == 'ACK':
                self.step = 5
            elif cmd == 'NACK':
                self.reset()
        # BEFORE REG LSB
        elif self.step == 5:
            if cmd == 'DATA WRITE':
                self.reg_end_sample = endsample
                self.reg_address <<= 8
                self.reg_address |= byte
                self.step = 6
        elif self.step == 6:
            if cmd == 'ACK':
                self.step = 7
            elif cmd == 'NACK':
                self.reset()
        # BEFORE FIRST DATA
        elif self.step == 7:
            if cmd == 'DATA WRITE':
                self.op = 'WRITE'
                self.annotate_register_address(self.reg_start_sample, self.reg_end_sample)
                self.annotate_register_value(startsample, endsample, byte)
                self.step = 8
            elif cmd == 'START REPEAT':
                self.op = 'READ'
                self.annotate_register_address(self.reg_start_sample, self.reg_end_sample)
                self.step = 8
        # BEFORE SECOND DATA
        elif self.step == 8:
            if cmd == 'DATA WRITE':
                self.annotate_register_value(startsample, endsample, byte)
            elif cmd == 'DATA READ':
                self.annotate_register_value(startsample, endsample, byte)
            elif cmd == 'STOP':
                self.reset()
            elif cmd == 'START REPEAT':
                self.reset()
                self.step = 1

        if cmd == 'ADDRESS WRITE' or cmd == 'ADDRESS READ':
            self.annotate_device_address(startsample, endsample, byte)

    def annotate_device_address(self, startsample: int, endsample: int, address: int):
        if address << 1 == 0xA6:
            self.put(startsample, endsample, srd.OUTPUT_ANN, [self.ann_data, ['ST25DV DATA', 'DATA']])
        elif address << 1 == 0xAE:
            self.put(startsample, endsample, srd.OUTPUT_ANN, [self.ann_sys, ['ST25DV SYSTEM', 'SYS']])
        else:
            self.put(startsample, endsample, srd.OUTPUT_ANN,
                     [self.ann_error, ['ST25DV ERROR: Unknown Address', 'ERROR']])

    def annotate_register_address(self, startsample: int, endsample: int):
        if self.op == 'READ':
            op_strings = ['Read', 'R']
            annotation_code = self.ann_read
        else:
            op_strings = ['Write', 'W']
            annotation_code = self.ann_write

        register = self.registers[self.reg_address].get_name_strings()

        address_string = ": %04X: " % self.reg_address

        strings = [
            op_strings[0] + address_string + register[1],
            op_strings[0] + address_string + register[0],
            op_strings[1] + address_string + register[1],
            op_strings[1] + address_string + register[0]
        ]

        self.put(startsample, endsample, srd.OUTPUT_ANN, [annotation_code, strings])

    def annotate_register_value(self, startsample: int, endsample: int, byte: int):
        if self.op == 'READ':
            annotation_code = self.ann_read
        else:
            annotation_code = self.ann_write

        self.data.append(byte)

        register = self.registers[self.reg_address]
        if register is None:
            strings = [
                '0x%02X' % byte,
                '%02X' % byte,
            ]
            self.put(startsample, endsample, srd.OUTPUT_ANN, [annotation_code, strings])
        elif register.length == len(self.data):
            if register.length == 1:
                register_value = self.data.pop(0)
                strings = [
                    register.get_long_value_strings(register_value),
                    register.get_short_field_strings(register_value),
                    register.get_nonzero_field_strings(register_value)
                ]
                self.put(startsample, endsample, srd.OUTPUT_ANN, [annotation_code, strings])
            else:
                strings = [
                    register.long_name + ': ',
                    register.short_name + ': ',
                    register.long_name,
                    register.short_name,
                ]
                for i in range(0, register.length):
                    value_string = "%02X " % self.data.pop(0)
                    strings[0] += value_string
                    strings[1] += value_string

                self.put(self.data_start_sample, endsample, srd.OUTPUT_ANN, [annotation_code, strings])
        elif self.data_start_sample is None:
            self.data_start_sample = startsample

    def print_state(self):
        print("step", self.step, "addr", hex(self.address), "reg", hex(self.reg_address), "op", self.op, end=' ')
        for d in self.data:
            print(hex(d), end=' ')
        print()
