##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2021 strange_v <strange.rand@gmail.com>
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
from collections import deque

class SamplerateError(Exception):
    pass

def get_rpm(t):
    rpm = 1000 / (t * 1000)  * 60
    return '%d' % rpm

class Decoder(srd.Decoder):
    api_version = 3
    id = 'rpm'
    name = 'RPM'
    longname = 'Revolutions per minute'
    desc = 'Calculate the number of turns in one minute.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Util']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Data line'},
    )
    annotations = (
        ('rpm', 'RPM'),
    )
    annotation_rows = (
        ('rpms', 'RPM', (0,)),
    )
    options = (
        { 'id': 'num_pulses', 'desc': 'Number of pulses per revolution', 'default': 2 },
        { 'id': 'edge', 'desc': 'Edges to check', 'values': ('rising', 'falling'), 'default': 'falling' },
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.last_samplenum = None
        self.edge_num = 0
        self.last_t = 0

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.edge = self.options['edge']
        self.num_pulses = self.options['num_pulses']

    def wait_for_edge(self):
        if self.edge == 'rising':
            return self.wait({0: 'r'})
        elif self.edge == 'falling':
            return self.wait({0: 'f'})
        else:
            return self.wait({0: 'e'})

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        while True:
            if not self.last_samplenum:
                self.wait_for_edge()
                self.last_samplenum = self.samplenum
                continue

            self.wait_for_edge()
            self.edge_num += 1
                
            samples = self.samplenum - self.last_samplenum
            t = samples / self.samplerate
            self.last_t += t

            if t >= 0.5:
                self.edge_num = 0
                self.last_t = 0
                self.last_samplenum = self.samplenum
                continue

            if self.edge_num == self.num_pulses:
                self.edge_num = 0
                self.last_t = 0
            
                self.put(self.last_samplenum, self.samplenum, self.out_ann, [0, [get_rpm(t)]])
                self.last_samplenum = self.samplenum