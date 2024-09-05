##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
## Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
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
from collections import namedtuple

Data = namedtuple('Data', ['ss', 'es', 'val'])

'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <data1>]

<ptype>:
 - 'TRANSFER': <data1> contain a list of Data() namedtuples for each
   byte transferred during this block transfer. Each Data() has
   fields ss, es, and val.

Examples:
 ['TRANSFER', [Data(ss=80, es=96, val=0xff), 
              Data(ss=80, es=96, val=0x3a), ...]]
'''

class ChannelError(Exception):
    pass

ann_bit, ann_data, ann_data_type, ann_warning, ann_transfer = range(5)
class Decoder(srd.Decoder):
    api_version = 3
    id = 'hdlc'
    name = 'HDLC'
    longname = 'High-Level Data Link Control'
    desc = 'High-Level Data Link Control.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['hdlc']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock'},
        {'id': 'data', 'name': 'DATA', 'desc': 'Data in'},
    )
    optional_channels = (
        {'id': 'en', 'name': 'ENABLE', 'desc': 'RX enabled'},
    )
    options = (
        {'id': 'en_polarity', 'desc': 'ENABLE polarity', 'default': 'active-high',
            'values': ('active-low', 'active-high')},
        {'id': 'cpol', 'desc': 'Clock polarity', 'default': 1,
            'values': (0, 1)},
    )
    annotations = (
        ('rx-bit', 'RX bit'),       #0: ann_bit 
        ('data', 'data'),           #1: ann_data
        ('data-type', 'data-type'), #2: ann_data_type
        ('warning', 'Warning'),     #3: ann_warning
        ('transfer', 'transfer'),   #4: ann_transfer
    )
    annotation_rows = (
        ('rx-bits', 'RX bits', (ann_bit,)),
        ('data-vals', 'data', (ann_data,)),
        ('data-types', 'type', (ann_data_type,)),
        ('transfers', 'transfers', (ann_transfer,)),
        ('other', 'Other', (ann_warning,)),
    )
    binary = (
        ('transfer', 'transfer'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.bitcount = 0           # Actual shifted bit number
        self.rxdata = 0             # Received data byte
        self.rxbits = []            # Received bits (tuple, [start, end, value])
        self.rxbytes = []           # Received bytes (tuple, [start, end, value])
        self.prev_bit = None        # Previous bit state
        self.ss_prev_clock = -1     # Previous clock samplenum
        self.ss_prev_false = -1     # samplenum at previous false
        self.flag_found = False     # Start flag found
        self.have_en = None         # Use enable signal
        self.one_count = 0          # Number of consecutive 1
        self.prev_one_count = 0     # Previous number of consecutive 1
        self.abort = None           # Abort packet
        self.flag = None

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)

    #Check if ENABLE is asserted
    def en_asserted(self, en):
        active_low = (self.options['en_polarity'] == 'active-low')
        return (en == 0) if active_low else (en == 1)

    #Computes frame CRC
    def crc16(self, data):        
        crc = 0xFFFF
        for i in range(0, (len(data)-2)):
            crc ^= data[i][2] << 0
            for j in range(0,8):
                if (crc & 0x0001) > 0:
                    crc =(crc >> 1) ^ 0x8408
                else:
                    crc = crc >> 1
        crc = (crc & 0xFFFF) ^ 0xFFFF
        return crc
    
    #Resets state, EN is not asserted
    def reset_state(self):
        self.bitcount = 0           # Actual shifted bit number
        self.rxdata = 0             # Received data byte
        self.rxbits = []            # Received bits (tuple, [start, end, value])
        self.rxbytes = []           # Received bytes (tuple, [start, end, value])
        self.prev_bit = None        # Previous bit state
        self.ss_prev_clock = -1     # Previous clock samplenum
        self.ss_prev_false = -1     # samplenum at previous false
        self.flag_found = False     # Start flag found
        self.one_count = 0          # Number of consecutive 1
        self.abort = None           # Abort packet location
        self.flag = None            # Floag packet location

    #Displays transfer
    def putt(self):
        if len(self.rxbytes) > 4:
            # Transfer valid (Address + control + CRC-16)
            self.put(self.rxbytes[0][0], self.rxbytes[-2][0], self.out_ann, [ann_data_type, ['TRANSFER']])
            self.put(self.rxbytes[-2][0], self.rxbytes[-1][1], self.out_ann, [ann_data_type, ['CRC']])
            transData = []
            for x in self.rxbytes[0:-2]:
                transData.append(Data(ss=x[0], es=x[1], val=x[2]))
            crc = self.crc16(self.rxbytes)
            rxCrc = ((self.rxbytes[-1][2] & 0xFF) << 8) | (self.rxbytes[-2][2] & 0xFF)
            if(crc != rxCrc):
                self.put(self.rxbytes[0][0], self.rxbytes[-1][1], self.out_ann, [ann_warning, ['BAD CRC!']])    
            else:
                #Send to python
                self.put(self.rxbytes[0][0], self.rxbytes[-2][0], self.out_python,
                    ['TRANSFER', transData])
                #Send to binay
                for x in self.rxbytes[0:-2]:
                    self.put(x[0], x[0], self.out_binary, [0, x[2].to_bytes(1, byteorder='big')])
            self.put(self.rxbytes[0][0], self.rxbytes[-1][1], self.out_ann, [ann_transfer, [' '.join(format(x.val, '02X') for x in transData)]])    
            
    #Shifts bit
    def shift_bit(self, data):
        if self.flag_found:
            self.rxdata |= data << self.bitcount
            self.rxbits.append([data, self.samplenum])
            self.bitcount += 1

    #handles bit
    def handle_bit(self, clk, data, en):
        # Displays bit
        if(self.ss_prev_clock != -1):
            self.put(self.ss_prev_clock, self.samplenum, self.out_ann, [ann_bit, ['%d' % self.prev_bit]])
        self.ss_prev_clock = self.samplenum
        self.prev_bit = data

        #Number of bits reached
        if self.bitcount == 8:
            self.put(self.rxbits[0][1], self.samplenum, self.out_ann,
                                [ann_data, ['%02X' % self.rxdata]])
            self.rxbytes.append([self.rxbits[0][1], self.samplenum, self.rxdata])
            self.rxdata = 0
            self.bitcount = 0
            self.rxbits = []

        #Display abort packet
        if self.abort is not None:
            self.put(self.abort[0], self.samplenum, self.out_ann, [ann_data_type, ['ABORT']])
            self.abort = None
        #Same for flag
        if self.flag is not None:
            self.put(self.flag[0], self.samplenum, self.out_ann, [ann_data_type, ['FLAG']])
            self.flag = None
            # Display previous transfer (if relevant)
            self.putt()
            self.rxbytes = []

        #Count number of 1
        if data == 1:
            if self.one_count < 5:
                self.shift_bit(data)
            self.one_count += 1
        else:
            if self.one_count == 6:
                # Found flag
                self.flag_found = True
                self.flag = [self.ss_prev_false, self.samplenum]                
                self.rxdata = 0
                self.bitcount = 0
                self.rxbits = []
            elif self.one_count > 6:
                #Abort
                self.abort = [self.ss_prev_false, self.samplenum]
                self.flag_found = False
                self.rxdata = 0
                self.bitcount = 0
                self.rxbits = []
            #Skip the 0 if 5 one in a row
            elif self.one_count < 5:
                self.shift_bit(data)
            
            self.prev_one_count = self.one_count
            self.one_count = 0
            self.ss_prev_false = self.samplenum

    #Find clock edges
    def find_clk_edge(self, clk, data, en, first):
        # We only care about samples if CS# is asserted.
        if self.have_en and not self.en_asserted(en):
            self.reset_state()
            return
        # Ignore sample if the clock pin hasn't changed.
        if first or not self.matched[0]:
            return

        #Check clock edge type
        if self.options['cpol'] != clk:
            return

        self.handle_bit(clk, data, en)

    def decode(self):
        # The CLK input is mandatory. Other signals are (individually)
        # optional. Yet either MISO or MOSI (or both) must be provided.
        # Tell stacked decoders when we don't have a CS# signal.
        if not self.has_channel(0) and not self.has_channel(1):
            raise ChannelError('Both clock and data pins required.')
        self.have_en = self.has_channel(2)
       
        if not self.have_en:
            self.put(0, 0, self.out_python, ['CS-CHANGE', None, None])

        # We want all CLK changes. We want all CS changes if CS is used.
        # Map 'have_cs' from boolean to an integer index. This simplifies
        # evaluation in other locations.
        wait_cond = [{0: 'e'}]
        if self.have_en:
            self.have_en = len(wait_cond)
            wait_cond.append({3: 'e'})

        # "Pixel compatibility" with the v2 implementation. Grab and
        # process the very first sample before checking for edges. The
        # previous implementation did this by seeding old values with
        # None, which led to an immediate "change" in comparison.
        (clk, data, en) = self.wait({})
        self.find_clk_edge(clk, data, en, True)

        while True:
            (clk, data, en) = self.wait(wait_cond)
            self.find_clk_edge(clk, data, en, False)
