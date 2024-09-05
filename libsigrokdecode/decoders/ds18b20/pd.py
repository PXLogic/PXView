##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2021 Guy Colin <guy.colin@gmail.com>
## This DS18B20 decoder is made from DS28EA00 decoder
## Original DS28EA00 made by Iztok Jeras
## Copyright (C) 2012 Iztok Jeras <iztok.jeras@gmail.com>
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
import struct

# Dictionary of commands and their names.
command = {
    # ROM commands
    0xf0: 'Search ROM',
    0x33: 'Read ROM',
    0x55: 'Match ROM',
    0xcc: 'Skip ROM',
    0xec: 'Alarm search',

    # Function commands
    0x44: 'Convert temperature',
    0x4e: 'Write scratchpad',
    0xbe: 'Read scratchpad',
    0x48: 'Copy scratchpad',
    0xb8: 'Recall EEPROM',
    0xb4: 'Read power supply'
}

# Dictionary of devices family code (Devices supported by this decoder)
family = {
    0x22: 'DS1822',
    0x3B: 'DS1825',
    0x10: 'DS18S20',
    0x28: 'DS18B20',
    0x42: 'DS28EA00'
}

class Decoder(srd.Decoder):
    api_version = 3
    id = 'ds18b20'
    name = 'DS18B20'
    longname = 'Maxim DS18B20 Programmable Resolution 1-Wire Digital Thermometer'
    desc = '1-Wire digital thermometers.'
    license = 'gplv3+'
    inputs = ['onewire_network']
    outputs = []
    tags = ['IC', 'Sensor']
    annotations = (
        ('text', 'Text'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.trn_beg = 0
        self.trn_end = 0
        self.state = 'INIT'
        self.device = '??'
        
    def reset_scratchpad(self):
        self.scratchpad = [0,0,0,0,0,0,0,0,0]
        self.scratchpad_index = 0
        self.temperature = 0
        self.th = 0
        self.tl = 0
        self.resolution = '??'

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def decode(self, ss, es, data):
        code, val = data

        self.ss, self.es = ss, es
        
        if code == 'RESET/PRESENCE':
            self.putx([0, ['Reset/presence: %s'
                           % ('true' if val else 'false')]])
            self.state = 'INIT'
        elif code == 'ROM':
            # Let's put the ROM number in correct order
            rom_str = ' '.join(format(x, '02X') for x in (val).to_bytes(8, byteorder='little'))
            family_code = int(rom_str[0:2], 16)
            if family_code in family:
                self.device = family[family_code]
            self.putx([0, ['ROM: %s CRC=%s Family:%s=%s' % (rom_str, rom_str[-2:], rom_str[0:2], self.device)]])
            self.state = 'INIT' 
        elif code == 'DATA':
            if self.state == 'INIT':
                if val not in command:
                    self.putx([0, ['Unrecognized command: 0x%02x' % val]])
                    return
                self.putx([0, ['Function command: 0x%02x \'%s\''
                          % (val, command[val])]])
                self.state = command[val].upper()
                if val == 0xbe:              # read scratchpad command received 
                    self.reset_scratchpad()  # reset to be ready to read the scratchpad 9 bytes 
                    # to understand each byte meaning refer to https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf
                    # Byte#0 temperature LSB, #1 MSB, #2 TH, #3 TL, #4 config register, #8 CRC
            elif self.state == 'READ SCRATCHPAD':
                if self.scratchpad_index >= 9:  # Happen only if bus master fails to reset the bus after reading the scratchpad
                    return
                self.scratchpad[ self.scratchpad_index] = val
                self.scratchpad_index +=1
                if self.scratchpad_index == 9:
                    # temperature conversion: as per Maxim datasheet: data is stored as a 16-bit sign-extended two’s complement number
                    # here under calculation successfully tested with positive and negative temperatures
                    if self.device != 'DS18S20':
                        self.temperature = struct.unpack('h', struct.pack('BB', self.scratchpad[0],self.scratchpad[1]))[0] * 0.0625
                    else:
                        self.temperature = struct.unpack('h', struct.pack('BB', self.scratchpad[0],self.scratchpad[1]))[0] * 0.5 
                    # High and low alarm thresholds are stored as a signed char (8 bits) number is the scratchpad 
                    self.th = struct.unpack('b', struct.pack('B', self.scratchpad[2]))[0]
                    self.tl = struct.unpack('b', struct.pack('B', self.scratchpad[3]))[0]
                    if self.scratchpad[4] ==   0b0011111:
                        self.resolution = '9 bits'
                    elif self.scratchpad[4] == 0b0111111:
                        self.resolution = '10 bits'
                    elif self.scratchpad[4] == 0b1011111:
                        self.resolution = '11 bits'
                    elif self.scratchpad[4] == 0b1111111:
                        self.resolution = '12 bits'
                    elif self.device == 'DS18S20':
                        self.resolution = '9 bits'  # DS18S20 is not configurable, fixed 9 bits
                    self.putx([0, ['Temperature: %0.4f°C, TH: %i, TL: %i, Resolution: %s' % (self.temperature, self.th,  self.tl, self.resolution)]])
                else:
                    self.putx([0, ['Scratchpad byte %s= 0x%02x' % ((self.scratchpad_index-1), val)]])
            elif self.state == 'CONVERT TEMPERATURE':
                if val == 0:
                    self.putx([0, ['Temperature conversion status=0 Not yet ready']])
                else:
                    # We will be very lucky if we see the following message one day,
                    # because we don't receive the bit states (from 1wire-link-layer decoder). we receive only bytes. 
                    # so as the bus master is waiting a bit=1. We'll see it only if it's the byte's 8th bit!
                    self.putx([0, ['Temperature conversion status!=0 READY']])
            elif self.state == 'WRITE SCRATCHPAD':   # Bus master MUST write 3 bytes (see satasheet). A bus reset will come after.
                self.putx([0, ['Writing: 0x%02x' % val]])
