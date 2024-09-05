##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2018 Juan Carlos Galvez Villegas.
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 3 of the License, or
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
from functools import reduce

housecodes = ['M','E','C','K','O','G','A','I','N','F','D','L','P','H','B','J']
        
class SamplerateError(Exception):
    pass

def decodeUnitCommand(byte1, byte2):
      cmd = ''
      unit = ''
      if byte2 & 0B1:
          unit = ''
          if byte2 & 0B10000:
              if byte2 & 0B1000:
                  cmd = 'DIM'
              else:
                  cmd = 'BRIGHT'
          else:
              if byte2 & 0B1000:
                  cmd = 'ALLON'
              else:
                  cmd = 'ALLOFF'
      else:
          unit = ((byte2 >> 3) | ((byte2 << 1) & 0B100) | ((byte1 >> 2) & 0B1000)) + 1
          if byte2 & 0B100:
              cmd = "OFF"
          else:
              cmd = "ON";
            
      return unit, cmd

class Decoder(srd.Decoder):
    api_version = 3
    id = 'X10 RF Decoding'
    name = 'X10 RF'
    longname = 'X10 radio frequency protocol decoder'
    desc = 'Decoding of X10 radio frequency protocol'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = ['x10rf']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'din', 'name': 'DIN', 'desc': 'DIN data line'},
    )
    annotations = (
        ('bit', 'Bit'),
        ('byte', 'Byte'),
        ('code', 'Code'),
        ('debug', 'Debug'),
        ('timing', 'Timing'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0,)),
        ('bytes', 'Byte (Reversed bits)', (1,)),
        ('codes', 'Code', (2,)),
        ('debugs', 'Debug', (3,)),
        ('timings', 'Timing', (4,)),
    )

    options = (
        { 'id': 'debug', 'desc': 'Show debug', 'default': 'no', 'values': ('yes', 'no') },
        { 'id': 'timing', 'desc': 'Show timing', 'default': 'no', 'values': ('yes', 'no') },
    )
            
    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.code_start_samplenum = None
        self.byte_start_samplenum = None
        self.last_rising_samplenum = 0
        self.last_falling_samplenum = 0
        self.bits_code = []
        self.bits_code_byte = []

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def decode_data(self, code_start_samplenum, samplenum):
            bitsreversed = reduce(lambda a, b: (a << 1) | b, self.bits_code[::-1])
            commandinv = (bitsreversed >> 24) & 0xff
            command = (bitsreversed >> 16) & 0xff
            houseinv = (bitsreversed >> 8) & 0xff
            house = bitsreversed & 0xff
            
            unit, cmd = decodeUnitCommand(house, command)
            
            if house == (~houseinv & 0xff) and command == (~commandinv & 0xff):
                self.put(code_start_samplenum, samplenum, self.out_ann,
                     [2, ['%s-%s %s' % (housecodes[house & 0x0f], unit, cmd)]])
                if self.options['debug'] == 'yes':
                    bits = reduce(lambda a, b: (a << 1) | b, self.bits_code)
                    self.put(code_start_samplenum, samplenum, self.out_ann,
                        [3, ['Received: #%06X  - Inverted: #%06X - Decoded: H=#%02x Hinv=#%02X U=#%02X Uinv=#%02X'
                        % (bits, bitsreversed, house, houseinv, command, commandinv)]])
            else:
                self.put(self.code_start_samplenum, samplenum, self.out_ann,
                  [2, ['Invalid data: %s - %s - %s - %s' % (bin(house)[2:].zfill(8),
                      bin(houseinv)[2:].zfill(8), bin(command)[2:].zfill(8),
                      bin(commandinv)[2:].zfill(8))]])
            self.bits_code = []
            self.code_start_samplenum = None

    def decode_byte(self, code_start_samplenum, samplenum):
            bits = reduce(lambda a, b: (a << 1) | b, self.bits_code_byte[::-1])
            
            self.put(code_start_samplenum, samplenum, self.out_ann,
                [1, ['0x%02X' % (bits)]])

            self.bits_code_byte = []
            self.byte_start_samplenum = None

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')

        lastnum = 0
        while True:
            # Wait for state change
            (pin,) = self.wait({0: 'e'})
            
            # if Rising edge
            if pin and self.last_rising_samplenum and self.last_falling_samplenum:
                uptime = (self.last_falling_samplenum - self.last_rising_samplenum) / self.samplerate
                downtime = (self.samplenum - self.last_falling_samplenum) / self.samplerate
                cycletime = uptime + downtime

                # Preamble is UP between 8 and 12 ms and DOWN between 3.5 and 4.5 ms
                if uptime >= 0.008 and uptime <= 0.012 \
                    and downtime >= 0.003 and downtime <= 0.005:
                    self.put(self.last_rising_samplenum, self.samplenum, self.out_ann,
                       [1, ['Preamble']])
                    self.code_start_samplenum = self.samplenum
                    self.byte_start_samplenum = self.samplenum
                elif self.code_start_samplenum:
                    # Between 0.9 and 1.5 mS is a Zero
                    if cycletime >= 0.0008 and cycletime <= 0.0015:                
                        bit_ = 0
                    # Between 1.8 and 2.6 mS is a One
                    elif cycletime >= 0.0018 and cycletime <= 0.0026:                      
                        bit_ = 1
                    else: # timing is invalid. Start over again
                        self.put(self.last_rising_samplenum, self.samplenum, self.out_ann,
                             [0, ['Invalid timing']])
                        self.code_start_samplenum = None
                        self.bits_code = []
                        self.bits_code_byte = []

                    if self.code_start_samplenum:
                        self.put(self.last_rising_samplenum, self.samplenum, self.out_ann,
                             [0, ['%d' % (bit_)]])

                        self.bits_code.append(bit_)
                        self.bits_code_byte.append(bit_)
                                            
                        if len(self.bits_code_byte) == 8:
                            self.decode_byte(self.byte_start_samplenum, self.samplenum)
                                                    
                        if len(self.bits_code) == 32:
                            self.decode_data(self.code_start_samplenum, self.samplenum)
                        
                        if not self.byte_start_samplenum:
                            self.byte_start_samplenum = self.samplenum

            if self.options['timing'] == 'yes':
                self.put(lastnum, self.samplenum, self.out_ann,
                    [4, ['%.2f ms' %
                     (((self.samplenum - lastnum) / self.samplerate) * 1000.0)]])
                
            lastnum = self.samplenum
            if pin:
                self.last_rising_samplenum = self.samplenum
            else:
                self.last_falling_samplenum = self.samplenum
