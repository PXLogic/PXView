##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
## Copyright (C) 2013 Matt Ranostay <mranostay@gmail.com>
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

import re
import sigrokdecode as srd
from common.srdhelper import bcd2int


regs = (
    'Seconds', 'Minutes', 'Hours', 'Day', 'Date', 'Month', 'Year',
    'Control', 'RAM',
)

bits = (
    'Clock halt', 'Seconds', 'Reserved', 'Minutes', '12/24 hours', 'AM/PM',
    'Hours', 'Day', 'Date', 'Month', 'Year', 'OUT', 'SQWE', 'RS', 'RAM',
)

MPU6050_I2C_ADDRESS = 0x68

def regs_and_bits():
    l = [('reg-' + r.lower(), r + ' register') for r in regs]
    l += [('bit-' + re.sub('\/| ', '-', b).lower(), b + ' bit') for b in bits]
    return tuple(l)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'mpu6050'
    name = 'MPU6050'
    longname = 'InvenSense MPU6050'
    desc = 'Accelerometer module protocol.'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = ['mpu6050']
    tags = ['Gyroscope']
    annotations =  regs_and_bits() + (
        ('data-out', 'Read data'),
        ('write-datetime', 'Write date/time'),
        ('reg-read', 'Register read'),
        ('reg-write', 'Register write'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('bits', 'Bits', tuple(range(9, 24))),
        ('regs', 'Registers', tuple(range(9))),
        ('accel-gyro', 'AccelXYZ/Temp/GyroXYZ', (24, 25, 26, 27)),
        ('warnings', 'Warnings', (28,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        
        self.acc_x = -1
        self.acc_y = -1
        self.acc_z = -1

        self.temp = -1

        self.gyro_x = -1
        self.gyro_y = -1
        self.gyro_z = -1
        self.bits = []

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def putd(self, bit1, bit2, data):
        self.put(self.bits[bit1][1], self.bits[bit2][2], self.out_ann, data)

    def putr(self, bit):
        self.put(self.bits[bit][1], self.bits[bit][2], self.out_ann,
                 [11, ['Reserved bit', 'Reserved', 'Rsvd', 'R']])



    def handle_reg_0x1b(self, b): # GYRO_CONFIG register
        self.putd(7, 0, [0, ['GYRO_CONFIG', 'GYR', 'G']])
        
        XG_ST = YG_ST = ZG_ST = 'self test'
        XG_ST += ' on' if (b & (1 << 7)) else ' off'
        YG_ST += ' on' if (b & (1 << 6)) else ' off'
        ZG_ST += ' on' if (b & (1 << 5)) else ' off'

        FS_SEL = ''
        if (b & (0b11 << 3) == 0b00000):
            FS_SEL = '± 250 °/s'
        elif (b & (0b11 << 3) == 0b01000):
            FS_SEL = '± 500 °/s'
        elif (b & (0b11 << 3) == 0b10000):
            FS_SEL = '± 1000 °/s'
        elif (b & (0b11 << 3) == 0b11000):
            FS_SEL = '± 2000 °/s'

        self.putd(7, 7, [9, ['XG_ST: %s' % XG_ST, 'XG_', 'X']])
        self.putd(6, 6, [9, ['YG_ST: %s' % YG_ST, 'YG_', 'Y']])
        self.putd(5, 5, [9, ['ZG_ST: %s' % ZG_ST, 'ZG_', 'Z']])
        self.putd(4, 3, [9, ['FS_SEL: %s' % FS_SEL, 'FS', 'F']])
        self.putd(2, 0, [9, ['-', '-', '-']])

    def handle_reg_0x1c(self, b): # ACCEL_CONFIG
        self.putd(7, 0, [0, ['ACCEL_CONFIG', 'ACC', 'A']])
        XA_ST = YA_ST = ZA_ST = 'self test'
        XA_ST += ' on' if (b & (1 << 7)) else ' off'
        YA_ST += ' on' if (b & (1 << 6)) else ' off'
        ZA_ST += ' on' if (b & (1 << 5)) else ' off'

        AFS_SEL = ''
        if (b & (0b11 << 3) == 0b00000):
            AFS_SEL = '± 2g'
        elif (b & (0b11 << 3) == 0b01000):
            AFS_SEL = '± 4g'
        elif (b & (0b11 << 3) == 0b10000):
            AFS_SEL = '± 8g'
        elif (b & (0b11 << 3) == 0b11000):
            AFS_SEL = '± 16g'

        self.putd(7, 7, [9, ['XA_ST: %s' % XA_ST, 'XA_', 'X']])
        self.putd(6, 6, [9, ['YA_ST: %s' % YA_ST, 'YA_', 'Y']])
        self.putd(5, 5, [9, ['ZA_ST: %s' % ZA_ST, 'ZA_', 'Z']])
        self.putd(4, 3, [9, ['AFS_SEL: %s' % AFS_SEL, 'AFS', 'A']])
        self.putd(2, 0, [9, ['-', '-', '-']])

    def handle_reg_0x38(self, b): # INT_ENABLE register
        self.putd(7, 0, [0, ['INT_ENABLE', 'INT', 'I']])

        self.putd(7, 5, [9, ['-', '-', '-']])
        self.putd(4, 4, [9, ['FIFO_OFLOW_EN', 'FIF', 'F']])
        self.putd(3, 3, [9, ['I2C_MST_INT', 'I2C', 'I']])
        self.putd(2, 1, [9, ['-', '-', '-']])
        self.putd(0, 0, [9, ['DATA_RDY_INT', 'DAT', 'D']])

    def handle_reg_0x75(self, b): # WHO_AM_I register
        self.putd(7, 0, [0, ['WHO_AM_I', 'WHO', 'W']])

        self.putd(6, 1, [9, ['WHO_AM_I', 'WHO', 'W']])
        self.putd(7, 7, [9, ['-', '-', '-']])
        self.putd(0, 0, [9, ['-', '-', '-']])

    def handle_reg_0x6b(self, b): # PWR_MGMT_1
        self.putd(7, 0, [0, ['PWR_MGMT_1', 'PWR', 'P']])

        self.putd(7, 7, [9, ['DEVICE_RESET', 'DEV', 'D']])
        self.putd(6, 6, [9, ['SLEEP', 'SLE', 'S']])
        self.putd(5, 5, [9, ['CYCLE', 'CYC', 'C']])
        self.putd(4, 4, [9, ['-', '-', '-']])
        self.putd(3, 3, [9, ['TEMP_DIS', 'TEM', 'T']])
        self.putd(2, 0, [9, ['CLKSEL', 'CLK', 'C']])

    def handle_reg_data(self, b):
        if self.reg == 0x3b:
            self.acc_x = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['ACCEL_XOUT[15:8]', 'ACX', 'A']])

        elif self.reg == 0x3c:
            self.acc_x += b / 1000000
            self.putd(7, 0, [0, ['ACCEL_XOUT[7:0]', 'ACX', 'A']])
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['ACCEL_XOUT: %d' % self.acc_x]])

        elif self.reg == 0x3d:
            self.acc_y = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['ACCEL_YOUT[15:8]', 'ACY', 'A']])

        elif self.reg == 0x3e:
            self.acc_y += b / 1000000
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['ACCEL_YOUT: %d' % self.acc_y]])
            self.putd(7, 0, [0, ['ACCEL_YOUT[7:0]', 'ACY', 'A']])

        elif self.reg == 0x3f:
            self.acc_z = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['ACCEL_ZOUT[15:8]', 'ACZ', 'A']])

        elif self.reg == 0x40:
            self.acc_z += b / 1000000
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['ACCEL_ZOUT: %d' % self.acc_z]])
            self.putd(7, 0, [0, ['ACCEL_ZOUT[7:0]', 'ACZ', 'A']])

        elif self.reg == 0x41:
            self.temp = b << 8
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['TEMP_OUT[15:8]', 'TMP', 'T']])

        elif self.reg == 0x42:
            self.temp += b
            # temp in degrees C
            self.temp = self.temp / 340 + 36.53
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['TEMP: %.1f°C' % self.temp]])
            self.putd(7, 0, [0, ['TEMP_OUT[7:0]', 'TMP', 'T']])

        elif self.reg == 0x43:
            self.gyro_x = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['GYRO_XOUT[15:8]', 'GYX', 'G']])

        elif self.reg == 0x44:
            self.gyro_x += b / 1000000
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['GYRO_XOUT: %d' % self.gyro_x]])
            self.putd(7, 0, [0, ['GYRO_XOUT[7:0]', 'GYX', 'G']])

        elif self.reg == 0x45:
            self.gyro_y = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['GYRO_YOUT[15:8]', 'GYY', 'G']])

        elif self.reg == 0x46:
            self.gyro_y += b / 1000000
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['GYRO_YOUT: %d' % self.gyro_y]])
            self.putd(7, 0, [0, ['GYRO_YOUT[7:0]', 'GYY', 'G']])

        elif self.reg == 0x47:
            self.gyro_z = b
            self.ss_block = self.ss
            self.putd(7, 0, [0, ['GYRO_ZOUT[15:8]', 'GYZ', 'G']])

        elif self.reg == 0x48:
            self.gyro_z += b / 1000000
            self.put(self.ss_block, self.es, self.out_ann,
                 [25, ['GYRO_ZOUT: %d' % self.gyro_z]])
            self.putd(7, 0, [0, ['GYRO_ZOUT[7:0]', 'GYZ', 'G']])

    def handle_reg(self, b):
        r = self.reg
        print('handle_reg_0x%02x \n' % r)
        fn = getattr(self, 'handle_reg_0x%02x' % r, None)
        if fn:
            fn(b)
        elif 0x3b <= r <= 0x48:
            # handle accel/temp/gyro values
            self.handle_reg_data(b)
            self.reg += 1

    def is_correct_chip(self, addr):
        print('is_correct_chip ', hex(addr))
        if addr == MPU6050_I2C_ADDRESS:
            return True
        self.put(self.ss_block, self.es, self.out_ann,
                 [28, ['Ignoring non-MPU6050 data (slave 0x%02X)' % addr]])
        return False

    def decode(self, ss, es, data):
        cmd, databyte = data

        # Collect the 'BITS' packet, then return. The next packet is
        # guaranteed to belong to these bits we just stored.
        if cmd == 'BITS':
            self.bits = databyte
            print(self.state, cmd, [[hex(item) for item in group] for group in databyte])
            return

        # Store the start/end samples of this I²C packet.
        self.ss, self.es = ss, es

        databyte_hex = None
        if databyte:
            databyte_hex = hex(databyte)

        print(self.state, cmd, databyte_hex)

        # State machine.
        if self.state == 'IDLE':
            # Wait for an I²C START condition.
            if cmd != 'START':
                return
            self.state = 'GET SLAVE ADDR'

        elif self.state == 'GET SLAVE ADDR':
            if cmd != 'ADDRESS WRITE' and cmd != 'ADDRESS READ':
                return
            if not self.is_correct_chip(databyte):
                self.state = 'IDLE'
                return
            self.state = 'GET REG ADDR'

        elif self.state == 'GET REG ADDR':
            if cmd == 'START REPEAT':
                self.state = 'GET SLAVE ADDR'
            elif cmd == 'STOP':
                self.state = 'IDLE'
            elif cmd == 'DATA WRITE':
                self.reg = databyte
                self.state = 'WRITE REGS'
            elif cmd == 'DATA READ':
                self.handle_reg(databyte)

        elif self.state == 'WRITE REGS':
            if cmd == 'STOP':
                self.state = 'IDLE'
                return
            elif cmd == 'START REPEAT':
                self.state = 'GET SLAVE ADDR'
                return
            if databyte is not None:
                self.handle_reg(databyte)