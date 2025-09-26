##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
## Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
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
[<ptype>, <data1>, <data2>]

<ptype>:
 - 'DATA': <data1> contains the MOSI data, <data2> contains the MISO data.
   The data is _usually_ 8 bits (but can also be fewer or more bits).
   Both data items are Python numbers (not strings), or None if the respective
   channel was not supplied.
 - 'BITS': <data1>/<data2> contain a list of bit values in this MOSI/MISO data
   item, and for each of those also their respective start-/endsample numbers.
 - 'CS-CHANGE': <data1> is the old CS# pin value, <data2> is the new value.
   Both data items are Python numbers (0/1), not strings. At the beginning of
   the decoding a packet is generated with <data1> = None and <data2> being the
   initial state of the CS# pin or None if the chip select pin is not supplied.
 - 'TRANSFER': <data1>/<data2> contain a list of Data() namedtuples for each
   byte transferred during this block of CS# asserted time. Each Data() has
   fields ss, es, and val.

Examples:
 ['CS-CHANGE', None, 1]
 ['CS-CHANGE', 1, 0]
 ['DATA', 0xff, 0x3a]
 ['BITS', [[1, 80, 82], [1, 83, 84], [1, 85, 86], [1, 87, 88],
           [1, 89, 90], [1, 91, 92], [1, 93, 94], [1, 95, 96]],
          [[0, 80, 82], [1, 83, 84], [0, 85, 86], [1, 87, 88],
           [1, 89, 90], [1, 91, 92], [0, 93, 94], [0, 95, 96]]]
 ['DATA', 0x65, 0x00]
 ['DATA', 0xa8, None]
 ['DATA', None, 0x55]
 ['CS-CHANGE', 0, 1]
 ['TRANSFER', [Data(ss=80, es=96, val=0xff), ...],
              [Data(ss=80, es=96, val=0x3a), ...]]
'''

# Key: (CPOL, CPHA). Value: SPI mode.
# Clock polarity (CPOL) = 0/1: Clock is low/high when inactive.
# Clock phase (CPHA) = 0/1: Data is valid on the leading/trailing clock edge.
spi_mode = {
    (0, 0): 0, # Mode 0
    (0, 1): 1, # Mode 1
    (1, 0): 2, # Mode 2
    (1, 1): 3, # Mode 3
}

class ChannelError(Exception):
    pass
CLK = 0
MISO = 1
MOSI = 2
CS = 3

class Decoder(srd.Decoder):
    api_version = 3
    id = 'spi'
    name = 'SPI'
    longname = 'Serial Peripheral Interface'
    desc = 'Full-duplex, synchronous, serial bus.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['spi']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock(串行时钟)'},
    )
    optional_channels = (
        {'id': 'miso', 'name': 'MISO', 'desc': 'Master in, slave out(主入从出)'},
        {'id': 'mosi', 'name': 'MOSI', 'desc': 'Master out, slave in(主出从入)'},
        {'id': 'cs', 'name': 'CS#', 'desc': 'Chip-select(片选信号)'},
    )
    options = (
        {'id': 'cs_polarity', 'desc': 'CS# polarity(片选极性)', 'default': 'active-low',
            'values': ('active-low', 'active-high')},
        {'id': 'cpol', 'desc': 'Clock polarity(时钟极性)', 'default': 0,
            'values': (0, 1)},
        {'id': 'cpha', 'desc': 'Clock phase(时钟相位)', 'default': 0,
            'values': (0, 1)},
        {'id': 'bitorder', 'desc': 'Bit order(位序)',
            'default': 'msb-first', 'values': ('msb-first', 'lsb-first')},
        {'id': 'wordsize', 'desc': 'Word size(字长)', 'default': 8},
        {'id': 'format', 'desc': 'Data format(数据格式)', 'default': 'hex',
            'values': ('ascii', 'dec', 'hex', 'oct', 'bin')},
        {'id': 'show_data_point', 'desc': 'Show data point(数据点显示)', 'default': 'yes',
            'values': ('yes', 'no')},
    )
    annotations = (
        ('miso-data', 'MISO data'),
        ('mosi-data', 'MOSI data'),
        ('miso-bit', 'MISO bit'),
        ('mosi-bit', 'MOSI bit'),
        ('warning', 'Warning'),
        ('miso-transfer', 'MISO transfer'),
        ('mosi-transfer', 'MOSI transfer'),
        ('atk-data-point', 'ATK Data point'),   #7
        ('atk-rising-edge', 'ATK Rising edge'), #8
        ('atk-falling-edge', 'ATK Falling edge'), #9
    )
    annotation_rows = (
        ('miso-bits', 'MISO bits', (2,)),
        ('miso-data-vals', 'MISO data', (0,)),
        ('miso-transfers', 'MISO transfers', (5,)),
        ('mosi-bits', 'MOSI bits', (3,)),
        ('mosi-data-vals', 'MOSI data', (1,)),
        ('mosi-transfers', 'MOSI transfers', (6,)),
        ('other', 'Other', (4,)),
        ('atk-signs', 'ATK signs', (7,8,9)),
    )
    binary = (
        ('miso', 'MISO'),
        ('mosi', 'MOSI'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.bitcount = 0
        self.misodata = self.mosidata = 0
        self.misobits = []
        self.mosibits = []
        self.misobytes = []
        self.mosibytes = []
        self.ss_block = -1
        self.ss_transfer = -1
        self.cs_was_deasserted = False
        self.have_cs = self.have_miso = self.have_mosi = None

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_bitrate = self.register(srd.OUTPUT_META,
                meta=(int, 'Bitrate', 'Bitrate during transfers'))
        self.bw = (self.options['wordsize'] + 7) // 8
        self.show_data_point = self.options['show_data_point'] == 'yes'

    def metadata(self, key, value):
       if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def putw(self, data):
        self.put(self.ss_block, self.samplenum, self.out_ann, data)

    def putdata(self):
        # Pass MISO and MOSI bits and then data to the next PD up the stack.
        so = self.misodata if self.have_miso else None
        si = self.mosidata if self.have_mosi else None
        so_bits = self.misobits if self.have_miso else None
        si_bits = self.mosibits if self.have_mosi else None

        if self.have_miso:
            ss, es = self.misobits[-1][1], self.misobits[0][2]
            bdata = so.to_bytes(self.bw, byteorder='big')
            self.put(ss, es, self.out_binary, [0, bdata])
        if self.have_mosi:
            ss, es = self.mosibits[-1][1], self.mosibits[0][2]
            bdata = si.to_bytes(self.bw, byteorder='big')
            self.put(ss, es, self.out_binary, [1, bdata])

        self.put(ss, es, self.out_python, ['BITS', si_bits, so_bits])
        self.put(ss, es, self.out_python, ['DATA', si, so])

        if self.have_miso:
            self.misobytes.append(Data(ss=ss, es=es, val=so))
        if self.have_mosi:
            self.mosibytes.append(Data(ss=ss, es=es, val=si))

        # Bit annotations.
        if self.have_miso:
            for bit in self.misobits:
                self.put(bit[1], bit[2], self.out_ann, [2, ['%d' % bit[0]]])
        if self.have_mosi:
            for bit in self.mosibits:
                self.put(bit[1], bit[2], self.out_ann, [3, ['%d' % bit[0]]])

        # Dataword annotations.
        # 数据字注释
        if self.have_miso:
            self.miso_format = self.options['format']
            if self.miso_format == 'hex':     
                self.miso_data = "%02x"%(self.misodata)
            elif self.miso_format == 'dec':
                self.miso_data = f"{self.misodata}"
            elif self.miso_format == 'oct':
                self.miso_data = "%03o"%(self.misodata)
            elif self.miso_format == 'bin':
                self.miso_data = f"{self.misodata:08b}"
            elif self.miso_format == 'ascii':
                self.miso_data = chr(self.misodata) if 32 <= self.misodata <= 126 else f"{self.misodata:02X}"
            self.put(ss, es, self.out_ann, [0, [self.miso_data]])
        if self.have_mosi:
            self.mosi_format = self.options['format']
            if self.mosi_format == 'hex':     
                self.mosi_data = "%02x"%(self.mosidata)
            elif self.mosi_format == 'dec':
                self.mosi_data = f"{self.mosidata}"
            elif self.mosi_format == 'oct':
                self.mosi_data = "%03o"%(self.mosidata)
            elif self.mosi_format == 'bin':
                self.mosi_data = f"{self.mosidata:08b}"
            elif self.mosi_format == 'ascii':
                self.mosi_data = chr(self.mosidata) if 32 <= self.mosidata <= 126 else f"{self.mosidata:02X}"
            self.put(ss, es, self.out_ann, [1, [self.mosi_data]])
            # self.put(ss, es, self.out_ann, [1, [self.mosidata]])

    def reset_decoder_state(self):
        self.misodata = 0 if self.have_miso else None
        self.mosidata = 0 if self.have_mosi else None
        self.misobits = [] if self.have_miso else None
        self.mosibits = [] if self.have_mosi else None
        self.bitcount = 0

    def cs_asserted(self, cs):
        active_low = (self.options['cs_polarity'] == 'active-low')
        return (cs == 0) if active_low else (cs == 1)

    def handle_bit(self, miso, mosi, clk, cs):
        # If this is the first bit of a dataword, save its sample number.
        if self.bitcount == 0:
            self.ss_block = self.samplenum
            self.cs_was_deasserted = \
                not self.cs_asserted(cs) if self.have_cs else False

        ws = self.options['wordsize']
        bo = self.options['bitorder']

        # Receive MISO bit into our shift register.
        if self.have_miso:
            if bo == 'msb-first':   # 高位优先
                self.misodata |= miso << (ws - 1 - self.bitcount)
            else:                   # 低位优先
                self.misodata |= miso << self.bitcount

        # Receive MOSI bit into our shift register.
        if self.have_mosi:
            if bo == 'msb-first':   # 高位优先
                self.mosidata |= mosi << (ws - 1 - self.bitcount)
            else:                   # 低位优先
                self.mosidata |= mosi << self.bitcount

        # Guesstimate the endsample for this bit (can be overridden below).
        es = self.samplenum
        if self.bitcount > 0:
            if self.have_miso:
                es += self.samplenum - self.misobits[0][1]
            elif self.have_mosi:
                es += self.samplenum - self.mosibits[0][1]

        if self.have_miso:
            self.misobits.insert(0, [miso, self.samplenum, es])
        if self.have_mosi:
            self.mosibits.insert(0, [mosi, self.samplenum, es])

        if self.bitcount > 0 and self.have_miso:
            self.misobits[1][2] = self.samplenum
        if self.bitcount > 0 and self.have_mosi:
            self.mosibits[1][2] = self.samplenum

        self.bitcount += 1

        # Continue to receive if not enough bits were received, yet.
        if self.bitcount != ws:
            return

        self.putdata()

        # Meta bitrate.
        if self.samplerate:
            elapsed = 1 / float(self.samplerate)
            elapsed *= (self.samplenum - self.ss_block + 1)
            bitrate = int(1 / elapsed * ws)
            self.put(self.ss_block, self.samplenum, self.out_bitrate, bitrate)

        if self.have_cs and self.cs_was_deasserted:
            self.putw([4, ['CS# was deasserted during this data word!']])

        self.reset_decoder_state()
    
    # Format the bytes according to the selected format
    def format_data(self, bytes_list, format_type):
        if format_type == 'hex':
            return ' '.join(f"{x.val:02X}" for x in bytes_list)
        elif format_type == 'dec':
            return ' '.join(f"{x.val:d}" for x in bytes_list)
        elif format_type == 'oct':
            return ' '.join(f"{x.val:03o}" for x in bytes_list)
        elif format_type == 'bin':
            return ' '.join(f"{x.val:08b}" for x in bytes_list)
        elif format_type == 'ascii':
            return ''.join(chr(x.val) if 32 <= x.val <= 126 else f'\\x{x.val:02X}' for x in bytes_list)
        return ''
    
    def find_clk_edge(self, miso, mosi, clk, cs, first):
        if self.have_cs and (first or (self.matched & (0b1 << self.have_cs))):
            # Send all CS# pin value changes.
            oldcs = None if first else 1 - cs
            self.put(self.samplenum, self.samplenum, self.out_python,
                     ['CS-CHANGE', oldcs, cs])

            if self.cs_asserted(cs):
                self.ss_transfer = self.samplenum
                self.misobytes = []
                self.mosibytes = []
            elif self.ss_transfer != -1:
                fmt = self.options['format']
                if self.have_miso:
                    formatted_miso = self.format_data(self.misobytes, fmt)
                    self.put(self.ss_transfer, self.samplenum,self.out_ann,
                            [5, [formatted_miso]])
                    # self.put(self.ss_transfer, self.samplenum, self.out_ann,
                    #     [5, [' '.join(format(x.val, '02X') for x in self.misobytes)]])
                if self.have_mosi:
                    formatted_mosi = self.format_data(self.mosibytes, fmt)
                    self.put(self.ss_transfer, self.samplenum, self.out_ann,
                            [6, [formatted_mosi]])
                    # self.put(self.ss_transfer, self.samplenum, self.out_ann,
                    #     [6, [' '.join(format(x.val, '02X') for x in self.mosibytes)]])
                self.put(self.ss_transfer, self.samplenum, self.out_python,
                    ['TRANSFER', self.mosibytes, self.misobytes])

            # Reset decoder state when CS# changes (and the CS# pin is used).
            self.reset_decoder_state()

        # We only care about samples if CS# is asserted.
        if self.have_cs and not self.cs_asserted(cs):
            return

        # Ignore sample if the clock pin hasn't changed.
        if first or (not self.matched & (0b1 << 0)):
            return

        # Sample data on rising/falling clock edge (depends on mode).
        mode = spi_mode[self.options['cpol'], self.options['cpha']]
        # if mode == 0 and clk == 0:   # Sample on rising clock edge
        #     return
        # elif mode == 1 and clk == 1: # Sample on falling clock edge
        #     return
        # elif mode == 2 and clk == 1: # Sample on falling clock edge
        #     return
        # elif mode == 3 and clk == 0: # Sample on rising clock edge
        #     return
        
        # 上升沿采样
        if (mode == 0 and clk == 1) or (mode == 3 and clk == 1):
            if self.show_data_point:
                self.put(self.samplenum, self.samplenum, self.out_ann,[8,['%d'% CLK]])
                self.put(self.samplenum, self.samplenum, self.out_ann,[7,['%d'% MISO]])
                self.put(self.samplenum, self.samplenum, self.out_ann,[7,['%d'% MOSI]])
        # 下降沿采样
        elif (mode == 1 and clk == 0) or (mode == 2 and clk == 0):
            if self.show_data_point:
                self.put(self.samplenum, self.samplenum, self.out_ann,[9,['%d'% CLK]])
                self.put(self.samplenum, self.samplenum, self.out_ann,[7,['%d'% MISO]])
                self.put(self.samplenum, self.samplenum, self.out_ann,[7,['%d'% MOSI]])
        else:
            return
        
        # Found the correct clock edge, now get the SPI bit(s).
        self.handle_bit(miso, mosi, clk, cs)
    
    def decode(self):
        self.put(self.samplenum, self.samplenum, self.out_ann, [7,["color:#F32FDC"]])
        self.put(self.samplenum, self.samplenum, self.out_ann, [8,["color:#F32FDC"]])
        self.put(self.samplenum, self.samplenum, self.out_ann, [9,["color:#F32FDC"]])

        # The CLK input is mandatory. Other signals are (individually)
        # optional. Yet either MISO or MOSI (or both) must be provided.
        # Tell stacked decoders when we don't have a CS# signal.
        if not self.has_channel(0):
            raise ChannelError('Either MISO or MOSI (or both) pins required.')
        self.have_miso = self.has_channel(1)
        self.have_mosi = self.has_channel(2)
        if not self.have_miso and not self.have_mosi:
            raise ChannelError('Either MISO or MOSI (or both) pins required.')
        self.have_cs = self.has_channel(3)
        if not self.have_cs:
            self.put(0, 0, self.out_python, ['CS-CHANGE', None, None])
        
        # We want all CLK changes. We want all CS changes if CS is used.
        # Map 'have_cs' from boolean to an integer index. This simplifies
        # evaluation in other locations.
        wait_cond = [{0: 'e'}]
        if self.have_cs:
            self.have_cs = len(wait_cond)
            wait_cond.append({3: 'e'})

        # "Pixel compatibility" with the v2 implementation. Grab and
        # process the very first sample before checking for edges. The
        # previous implementation did this by seeding old values with
        # None, which led to an immediate "change" in comparison.
        (clk, miso, mosi, cs) = self.wait({})
        self.find_clk_edge(miso, mosi, clk, cs, True)

        while True:
            (clk, miso, mosi, cs) = self.wait(wait_cond)
            self.find_clk_edge(miso, mosi, clk, cs, False)
