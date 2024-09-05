##
## Copyright (C) 2023 Rikka0w0 <929513338@qq.com>
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

class ChannelError(Exception):
    pass

class Decoder(srd.Decoder):
    api_version = 3
    id = 'delta-sigma'
    name = 'Delta-Sigma'
    longname = 'Delta-Sigma Decoder'
    desc = 'Clocked.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Util']
    channels = (
        {'id': 'dat', 'name': 'DAT', 'desc': 'Data'},
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock'},
    )
    optional_channels = (
    )
    options = (
        {'id': 'clock_mode', 'desc': 'Clock Mode', 'default': 'normal',
          'values': ('normal', 'manchester')},
        {'id': 'filter_type', 'desc': 'Filter type', 'default': 'sinc3',
         'values': ('sinc_fast', 'sinc1', 'sinc2', 'sinc3')},
        {'id': 'osr', 'desc': 'Oversampling Factor', 'default': 4},
        {'id': 'shift', 'desc': 'Right shift the result by', 'default': 0},
        {'id': 'scale', 'desc': 'Code-Actual scaler', 'default': 1.0},
    )
    annotations = (
        ('bit-stream', 'Bit Stream'),
        ('filtered', 'Filtered'),
        ('converted', 'Converted'),
    )
    annotation_rows = (
        ('bit-streams', 'Bit Stream', (0,)),
        ('filtereds', 'Filtered', (1,)),
        ('converteds', 'Converted', (2,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.last_samplenum = None
        self.current_dat = None
        self.last_filternum = 0

        self.sinc_DELTA1 = 0
        self.sinc_CN1 = 0
        self.sinc_CN2 = 0
        self.sinc_DN0 = 0
        self.sinc_DN1 = 0
        self.sinc_DN3 = 0
        self.sinc_DN5 = 0
        self.sinc_CNTR = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.clock_mode = self.options['clock_mode']
        self.filter_type = self.options['filter_type']
        self.osr = self.options['osr']
        self.shift = self.options['shift']
        self.scale = self.options['scale']

    def metadata(self, key, value):
       if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def put_result(self, code):
        code = code >> self.shift;

        self.put(self.last_filternum, self.samplenum, self.out_ann,
                [1, ['%d' % (code)]])
        self.put(self.last_filternum, self.samplenum, self.out_ann,
                [2, ['%d' % (code * self.scale)]])

    def run_sinc1(self, dat):
        if dat > 0:
            sinc_DELTA1 = self.sinc_DELTA1 + 1
        else:
            sinc_DELTA1 = self.sinc_DELTA1 - 1

        self.sinc_CNTR = self.sinc_CNTR + 1
        if self.sinc_CNTR == self.osr:
            self.sinc_CNTR = 0
            sinc_DN0 = self.sinc_DELTA1
            sinc_DN1 = self.sinc_DN0
            sinc_CN3 = self.sinc_DN0 - self.sinc_DN1

            # Update state variables
            self.sinc_DN0 = sinc_DN0
            self.sinc_DN1 = sinc_DN1

            self.put_result(sinc_CN3)

            self.last_filternum = self.samplenum

        # Update state variables
        self.sinc_DELTA1 = sinc_DELTA1

    def run_sinc2(self, dat):
        if dat > 0:
            sinc_DELTA1 = self.sinc_DELTA1 + 1
        else:
            sinc_DELTA1 = self.sinc_DELTA1 - 1

        sinc_CN1 = self.sinc_CN1 + self.sinc_DELTA1

        self.sinc_CNTR = self.sinc_CNTR + 1
        if self.sinc_CNTR == self.osr:
            self.sinc_CNTR = 0
            sinc_DN0 = self.sinc_CN1
            sinc_DN1 = self.sinc_DN0
            sinc_CN3 = self.sinc_DN0 - self.sinc_DN1
            sinc_CN4 = sinc_CN3 - self.sinc_DN3

            # Update state variables
            self.sinc_DN0 = sinc_DN0
            self.sinc_DN1 = sinc_DN1
            self.sinc_DN3 = sinc_CN3

            self.put_result(sinc_CN4)

            self.last_filternum = self.samplenum

        # Update state variables
        self.sinc_DELTA1 = sinc_DELTA1
        self.sinc_CN1 = sinc_CN1

    def run_sinc3(self, dat):
        if dat > 0:
            sinc_DELTA1 = self.sinc_DELTA1 + 1
        else:
            sinc_DELTA1 = self.sinc_DELTA1 - 1

        sinc_CN1 = self.sinc_CN1 + self.sinc_DELTA1
        sinc_CN2 = self.sinc_CN1 + self.sinc_CN2

        self.sinc_CNTR = self.sinc_CNTR + 1
        if self.sinc_CNTR == self.osr:
            self.sinc_CNTR = 0
            sinc_DN0 = self.sinc_CN2
            sinc_DN1 = self.sinc_DN0
            sinc_CN3 = self.sinc_DN0 - self.sinc_DN1
            sinc_CN4 = sinc_CN3 - self.sinc_DN3
            sinc_CN5 = sinc_CN4 - self.sinc_DN5

            # Update state variables
            self.sinc_DN0 = sinc_DN0
            self.sinc_DN1 = sinc_DN1
            self.sinc_DN3 = sinc_CN3
            self.sinc_DN5 = sinc_CN4

            self.put_result(sinc_CN5)

            self.last_filternum = self.samplenum

        # Update state variables
        self.sinc_DELTA1 = sinc_DELTA1
        self.sinc_CN1 = sinc_CN1
        self.sinc_CN2 = sinc_CN2

    def find_clk_edge(self, clk, dat):
        # Ignore sample if the clock pin hasn't changed.
        if not self.last_samplenum:
            self.last_samplenum = 0
            self.put(0, self.samplenum, self.out_ann,
                    [0, ['X']])
        else:
            self.put(self.last_samplenum, self.samplenum, self.out_ann,
                    [0, ['%d' % (self.current_dat)]])

        if self.filter_type == 'sinc1':
            self.run_sinc1(dat)
        elif self.filter_type == 'sinc2':
            self.run_sinc2(dat)
        elif self.filter_type == 'sinc3':
            self.run_sinc3(dat)


        self.last_samplenum = self.samplenum
        self.current_dat = dat

    def decode(self):
        # The DAT input is mandatory. Other signals are optional.
        if not self.has_channel(0):
            raise ChannelError('DAT not found.')
        if not self.has_channel(1):
            raise ChannelError('CLK not found.')

        # Trigger on rising edges
        wait_cond = [{1: 'r'}]

        while True:
            (dat, clk) = self.wait(wait_cond)
            self.find_clk_edge(clk, dat)
