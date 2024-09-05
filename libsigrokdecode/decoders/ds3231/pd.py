##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2020 Neil McKechnie <neilmck999@gmail.com>
## Based broadly on work by the following for DS1307
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
from itertools import chain

days_of_week = (
    'Sunday', 'Monday', 'Tuesday', 'Wednesday',
    'Thursday', 'Friday', 'Saturday',
)

regs = ('Date/Time', 'Alarm1', 'Alarm2', 'Control/Status', 'Ageing', 'Temperature') # 0-5

bits = ('Date/Time', 'Alarm1', 'Alarm2', 'Control/Status', 'Ageing', 'Temperature', 'Reserved') # 6-12

rates = ('1Hz', '1.024kHz', '4.096kHz', '8.192kHz')

DS3231_I2C_ADDRESS = 0x68

def regs_and_bits():
    l = [('reg-' + r.lower(), r + ' register') for r in regs]
    l += [('bit-' + re.sub('\/| ', '-', b).lower(), b + ' bit') for b in bits]
    return tuple(l)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'ds3231'
    name = 'DS3231'
    longname = 'Maxim DS3231'
    desc = 'Maxim DS3231 realtime clock module protocol.'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    options = (
        {'id': 'day0', 'desc': 'First day of week', 'default': 'Sunday',
            'values': days_of_week},
    )
    tags = ['Clock/timing', 'IC']
    annotations =  regs_and_bits() + (
        ('reg-set', 'Set register'), # 13
        ('read-datetime', 'Read date/time'), # 14
        ('write-datetime', 'Write date/time'), # 15
        ('read-alarm', 'Read alarm'), # 16
        ('write-alarm', 'Write alarm'), # 17
        ('read-temperature', 'Read temperature'), # 18
        ('warning', 'Warning'), #19
    )
    annotation_rows = (
        ('regs', 'Registers', (0, 1, 2, 3, 4, 5, 13)),
        ('bits', 'Bits', tuple(range(6, 13))),
        ('date-time', 'Date/time/temperature', tuple(range(14, 19))),
        ('warnings', 'Warnings', (19,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        self.hours = -1
        self.minutes = -1
        self.seconds = -1
        self.days = -1
        self.date = -1
        self.months = -1
        self.years = -1
        self.reg = 0
        self.temperature1 = 0
        self.temperature2 = 0
        self.bits = []
        self.ss_block = -1
        self.asecond = ''
        self.aminute = ''
        self.ahour = ''
        self.adaydate = ''
        self.aampm = ''

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.dayoffset = days_of_week.index(self.options['day0']);

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def putd(self, bit1, bit2, data):
        self.put(self.bits[bit1][1], self.bits[bit2][2], self.out_ann, data)

    def putr(self, bit):
        self.put(self.bits[bit][1], self.bits[bit][2], self.out_ann,
                 [12, ['Reserved', 'Rsvd', 'R']])
                 
    def ordinal(self, n):
        n = n % 10
        return 'st' if n==1 else 'nd' if n==2 else 'rd' if n==3 else 'th'

    def handle_reg_0x00(self, b): # Seconds (0-59) 
        self.putd(7, 0, [0, ['Seconds', 'Sec', 'S']])
        self.putr(7)
        s = self.seconds = bcd2int(b & 0x7f)
        self.putd(6, 0, [6, ['Second: %d' % s, 'Sec: %d' % s, 'S: %d' % s, 'S']])

    def handle_reg_0x01(self, b): # Minutes (0-59)
        self.putd(7, 0, [0, ['Minutes', 'Min', 'M']])
        self.putr(7)
        m = self.minutes = bcd2int(b & 0x7f)
        self.putd(6, 0, [6, ['Minute: %d' % m, 'Min: %d' % m, 'M: %d' % m, 'M']])

    def handle_reg_0x02(self, b): # Hours (1-12+AM/PM or 0-23)
        self.putd(7, 0, [0, ['Hours', 'H']])
        self.putr(7)
        ampm_mode = True if (b & (1 << 6)) else False
        if ampm_mode:
            self.putd(6, 6, [6, ['12-hour mode', '12h mode', '12h']])
            a = 'PM' if (b & (1 << 5)) else 'AM'
            self.putd(5, 5, [6, [a, a[0]]])
            h = self.hours = bcd2int(b & 0x1f)
            self.putd(4, 0, [6, ['Hour: %d' % h, 'H: %d' % h, 'H']])
        else:
            self.putd(6, 6, [6, ['24-hour mode', '24h mode', '24h']])
            h = self.hours = bcd2int(b & 0x3f)
            self.putd(5, 0, [6, ['Hour: %d' % h, 'H: %d' % h, 'H']])

    def handle_reg_0x03(self, b): # Day / day of week (1-7)
        self.putd(7, 0, [0, ['Day of week', 'Day', 'D']])
        for i in (7, 6, 5, 4, 3):
            self.putr(i)
        w = self.days = bcd2int(b & 0x07)
        ws = days_of_week[(self.days + self.dayoffset) % 7]
        self.putd(2, 0, [6, ['Weekday: %s' % ws, 'WD: %s' % ws, 'WD', 'W']])

    def handle_reg_0x04(self, b): # Date (1-31)
        self.putd(7, 0, [0, ['Date', 'D']])
        for i in (7, 6):
            self.putr(i)
        d = self.date = bcd2int(b & 0x3f)
        self.putd(5, 0, [6, ['Date: %d%s' % (d, self.ordinal(d)), 'D: %d' % d, 'D']])

    def handle_reg_0x05(self, b): # Month (1-12)
        self.putd(7, 0, [0, ['Month', 'Mon', 'M']])
        for i in (7, 6, 5):
            self.putr(i)
        m = self.months = bcd2int(b & 0x1f)
        self.putd(4, 0, [6, ['Month: %d' % m, 'Mon: %d' % m, 'M: %d' % m, 'M']])

    def handle_reg_0x06(self, b): # Year (0-99)
        self.putd(7, 0, [0, ['Year', 'Y']])
        y = self.years = bcd2int(b & 0xff)
        self.years += 2000
        self.putd(7, 0, [6, ['Year: %d' % y, 'Y: %d' % y, 'Y']])
        
    def handle_reg_0x07(self, b): # Alarm 1 seconds
        self.putd(7, 0, [1, ['Alarm 1 Seconds', 'A1Secs']])
        self.handle_alarm_seconds(7, b)

    def handle_reg_0x08(self, b): # Alarm 1 Minutes
        self.putd(7, 0, [1, ['Alarm 1 Minutes', 'A1Mins']])
        self.handle_alarm_minutes(7, b)

    def handle_reg_0x09(self, b): # Alarm 1 Hour
        self.putd(7, 0, [1, ['Alarm 1 Hour', 'A1Hour', 'A1Hr']])
        self.handle_alarm_hour(7, b)

    def handle_reg_0x0a(self, b): # Alarm 1 Day/Date
        self.putd(7, 0, [1, ['Alarm 1 Day/Date', 'A1Day']])
        self.handle_alarm_daydate(7, b)
            
    def handle_reg_0x0b(self, b): # Alarm 2 Minutes
        self.putd(7, 0, [2, ['Alarm 2 Minutes', 'A2Mins']])
        self.handle_alarm_minutes(8, b)

    def handle_reg_0x0c(self, b): # Alarm 2 Hour
        self.putd(7, 0, [2, ['Alarm 2 Hour', 'A2Hour', 'A2Hr']])
        self.handle_alarm_hour(8, b)

    def handle_reg_0x0d(self, b): # Alarm 2 Day/Date
        self.putd(7, 0, [2, ['Alarm 2 Day/Date', 'A2Day']])
        self.handle_alarm_daydate(8, b)
           
    def handle_reg_0x0e(self, b): # Control Register
        self.putd(7, 0, [3, ['Control Register', 'Ctrl', 'C']])
        eosc = 'OFF' if (b & (1 << 7)) else 'ON'
        self.putd(7, 7, [9, ['Oscillator %s' % eosc, 'EOSC %s' % eosc]])
        bbsqw = 'ON' if (b & (1 << 6)) else 'OFF'
        self.putd(6, 6, [9, ['Battery Backed Square Wave %s' % bbsqw, 'BBSQW %s' % bbsqw]])
        conv = 'ON' if (b & (1 << 5)) else 'OFF'
        self.putd(5, 5, [9, ['Convert Temperature %s' % conv, 'CONV %s' % conv]])
        rs = rates[(b >> 3) & 3];
        self.putd(4, 3, [9, ['Rate Select: %s' % rs, rs]])
        intcn = 'ON' if (b & (1 << 2)) else 'OFF'
        self.putd(2, 2, [9, ['Interrupt Control %s' % intcn, 'INTCN %s' % intcn]])
        a2ie = 'ENABLED' if (b & (1 << 1)) else 'DISABLED'
        self.putd(1, 1, [9, ['Alarm 2 Interrupt %s' % a2ie, 'A2IE %s' % a2ie[0:3]]])
        a1ie = 'ENABLED' if (b & (1 << 0)) else 'DISABLED'
        self.putd(0, 0, [9, ['Alarm 1 Interrupt %s' % a1ie, 'A1IE %s' % a1ie[0:3]]])

    def handle_reg_0x0f(self, b): # Control /Status Register
        self.putd(7, 0, [3, ['Control/Status Register', 'Stat', 'S']])
        osf = 'STOPPED' if (b & (1 << 7)) else 'RUNNING'
        self.putd(7, 7, [9, ['Oscillator %s' % osf, 'OSF %s' % osf, osf[0]]])
        for i in (6, 5, 4):
            self.putr(i)
        en32khz = 'ENABLED' if (b & (1 << 3)) else 'DISABLED'
        self.putd(3, 3, [9, ['32kHz Output %s' % en32khz, 'EN32kHz %s' % en32khz[0:3]]])
        bsy = 'ON' if (b & (1 << 2)) else 'OFF'
        self.putd(2, 2, [9, ['Busy %s' % bsy, 'BSY %s' % bsy]])
        a2f = 'ELAPSED' if (b & (1 << 1)) else 'CLEAR'
        self.putd(1, 1, [9, ['Alarm 2 %s' % a2f, 'A2F %s' % a2f]])
        a1f = 'ELAPSED' if (b & (1 << 0)) else 'CLEAR'
        self.putd(0, 0, [9, ['Alarm 1 %s' % a1f, 'A1F %s' % a1f]])

    def handle_reg_0x10(self, b): # Ageing offset
        self.putd(7, 0, [4, ['Ageing Register', 'Ageing', 'A']])
        offset =  -((b ^ 0xff) + 1) if (b & (1 << 7)) else b
        selt.putd(7, 0, [10, ['Offset=%d' % offset, offset]])
        
    def handle_reg_0x11(self, b): # temperature register 1
        self.putd(7, 0, [5, ['Temperature Register 1', 'Temp1', 'T']])
        if (b & (1<<7)):    # allow for twos complement negative 
            sign = '-' 
            self.temperature1 = -((b ^ 0xff) + 1)
        else:
            sign = '+'
            self.temperature1 = b
        self.putd(7, 7, [11, ['Sign: %s' % sign, '%s' % sign]])
        self.putd(6, 0, [11, ['Temperature: %d' % (b & 0x7f), 'Temp: %d' % (b & 0x7f)]])

    def handle_reg_0x12(self, b): # temperature register 2
        self.putd(7, 0, [5, ['Temperature Register 2', 'Temp2', 'T']])
        self.temperature2 = b / 256
        self.putd(7, 6, [11, ['Temperature fraction: %.2f' % (b/256), 'Frac: %.2f' % (b/256)]])
        for i in (5, 4, 3, 2, 1, 0):
            self.putr(i)

    def output_datetime(self, cls, rw):
        if self.ss_block < 0:
            return
        d = '%s, %02d.%02d.%4d %02d:%02d:%02d' % (
            days_of_week[(self.days+self.dayoffset) % 7], self.date, self.months,
            self.years, self.hours, self.minutes, self.seconds)
        self.put(self.ss_block, self.ss, self.out_ann,
                 [cls, ['%s date/time: %s' % (rw, d), '%s' % d]])
                 
    def output_temperature(self, cls, integer):
        t = self.temperature1
        if integer:
            self.put(self.ss_block, self.ss, self.out_ann,
                [cls, ['Read temperature: %d \u00b0C' % t]])
        else:
            t += self.temperature2  # fractional part
            self.put(self.ss_block, self.ss, self.out_ann,
                [cls, ['Read temperature: %.2f \u00b0C' % t]])
               
    def output_alarm(self, cls, rw, alm):
        s = '%s %s:%s:%s%s' % (self.adaydate, self.ahour, self.aminute, 
            '00' if alm == 2 else self.asecond, self.aampm)
        if s == '* **:**:**':
            s += ' (every second)'
        self.put(self.ss_block, self.ss, self.out_ann, 
            [cls, ['%s Alarm %d: %s' % (rw, alm, s), 'A%d: %s' % (alm, s)]])

    def handle_alarm_daydate(self, cls, b):
        ignore = b & (1 << 7)
        dydt = b & (1 << 6)
        self.putd(6, 6, [8, ['Day Mode' if (dydt) else 'Date Mode', 'Day' if (dydt) else 'Date']])
        if dydt:
            mm = 'Ignore day' if ignore else 'Alarm on day match'
            self.putd(7, 7, [cls, [mm, mm[0:1]]])
            ws = days_of_week[((b & 0x3f) + self.dayoffset) % 7]
            self.putd(5, 0, [cls, ['Day=%s' % ws]])
            self.adaydate = '*' if ignore else ws
        else:
            mm = 'Ignore date' if b & (1 << 7) else 'Alarm on date match'
            self.putd(7, 7, [cls, [mm, mm[0:1]]])
            d = bcd2int(b % 0x3f)
            self.putd(5, 0, [cls, ['Date=%d%s of month' % (d, self.ordinal(d)), '%d%s' % (d, self.ordinal(d))]]) 
            self.adaydate = '*' if ignore else '%d%s of month' % (d, self.ordinal(d))
            
    def handle_alarm_hour(self, cls, b):
        ignore = b & (1 << 7)
        if ignore:
            mm = 'Ignore hour' 
            self.ahour = '**'
        else:
            mm = 'Alarm on hour match'
        self.putd(7, 7, [cls, [mm, mm[0:1]]])
        ampm_mode = True if (b & (1 << 6)) else False
        if ampm_mode:
            ampm = 'pm' if (b % (1 << 5)) else 'am'
            self.putd(6, 6, [cls, [ampm, ampm[0]]])
            h = bcd2int(b & 0x1f)
            self.putd(5, 0, [cls, ['Hour=%d%s' % (h, ampm), 'Hr=%d%s' % (h, ampm)]])
            if not ignore:
                self.aampm = ampm
                self.ahour = '%02d' % h
        else:
            h = bcd2int(b & 0x3f)
            self.putd(6, 0, [cls, ['Hour=%d' % h, 'Hr=%d' % h]])
            self.aampm = ''
            if not ignore:
                self.ahour = '%02d' % h
    
    def handle_alarm_minutes(self, cls, b):
        ignore = b & (1 << 7)
        mm = 'Ignore minute' if ignore else 'Alarm on minute match'
        self.putd(7, 7, [cls, [mm, mm[0:1]]])
        m = bcd2int(b & 0x7f)
        self.putd(6, 0, [cls, ['Minutes=%d' % m, 'Min=%d' % m]])
        self.aminute = '*' if ignore else '%02d' % m

    def handle_alarm_seconds(self, cls, b):
        s = bcd2int(b & 0x7f)
        if b & (1 << 7):
            mm = 'Ignore second' 
            self.asecond = '**'
        else:
            mm = 'Alarm on second match'
            self.asecond = '%02d' % s
        self.putd(7, 7, [cls, [mm, mm[0:1]]])
        self.putd(6, 0, [cls, ['Seconds=%d' % s, 'Sec=%d' % s]])
    
    def handle_reg(self, b):
        if self.reg in range(0, 0x13):
            r = self.reg
            fn = getattr(self, 'handle_reg_0x%02x' % r)
            fn(b)
        # Honor address auto-increment feature of the DS3231. When the
        # address reaches 0x12, it will wrap around to address 0.
        self.reg += 1
        if self.reg > 0x12:
            self.reg = 0

    def is_correct_chip(self, addr):
        if addr == DS3231_I2C_ADDRESS:
            return True
        return False

    def decode(self, ss, es, data):
        cmd, databyte = data

        # Collect the 'BITS' packet, then return. The next packet is
        # guaranteed to belong to these bits we just stored.
        if cmd == 'BITS':
            self.bits = databyte
            return
            
        if cmd == 'STOP':
            # End or abort, go back to idle
            self.state = 'IDLE'
            self.ss_block = -1
            return
        elif cmd == 'START' or cmd == 'START REPEAT':
            # Whatever state, wait for address
            self.state = 'GET SLAVE ADDR'
            return

        # Store the start/end samples of this I²C packet.
        self.ss, self.es = ss, es

        # State machine.
        if self.state == 'GET SLAVE ADDR':
            # Wait for an address write operation.
            if not self.is_correct_chip(databyte):
                self.state = 'IDLE'
                return
            if cmd == 'ADDRESS WRITE':
                self.state = 'SET REG ADDR'
            elif cmd == 'ADDRESS READ':
                self.state = 'READ RTC REGS'
        elif self.state == 'SET REG ADDR':
            # Wait for a data write (master selects the slave register).
            if cmd == 'DATA WRITE':
                self.reg = databyte
                self.putx([13, ['Select register %d' % self.reg, 'Reg=%d' % self.reg, 
                    'R=%d' % self.reg]])
                self.state = 'WRITE RTC REGS'
            elif cmd == 'NACK':
                self.state = 'IDLE' # end of write
        elif self.state == 'WRITE RTC REGS':
            # Get data bytes until a NACK condition occurs (end of data)
            if cmd == 'DATA WRITE':
                # Check for possible starts of sequences
                if self.reg in (0, 7, 11): 
                    self.ss_block = self.ss
                # Process the data byte
                self.handle_reg(databyte)
            elif cmd == 'ACK':
                # Check for end of sequence
                if self.ss_block >= 0:
                    if self.reg == 7:
                        self.output_datetime(15, 'Written')
                    elif self.reg == 11: # alarm 1
                        self.output_alarm(17, 'Written', 1)
                    elif self.reg == 14: # alarm 2
                        self.output_alarm(17, 'Written', 2)
        elif self.state == 'READ RTC REGS':
            if cmd == 'DATA READ':
                # Check for possible starts of sequences
                if self.reg in (0, 7, 11, 17): 
                    self.ss_block = self.ss
                # Process the data byte
                self.handle_reg(databyte)
            elif cmd == 'NACK':  # End of sequence of data reads
                if self.ss_block >= 0:
                    if self.reg == 7:
                        self.output_datetime(14, 'Read')
                    elif self.reg == 11: # alarm 1 
                        self.output_alarm(16, 'Read', 1)
                    elif self.reg == 14: # alarm 2
                        self.output_alarm(16, 'Read', 2);
                    elif self.reg == 18: # temperature first byte read only
                        self.output_temperature(18, True)   
                    elif self.reg == 0: # temperature both bytes read
                        self.output_temperature(18, False)
