##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017 Ryan "Izzy" Bales <izzy84075@gmail.com>
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
# Recoil laser tag IR protocol decoder

import sigrokdecode as srd

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
    api_version = 3
    id = 'afsk'
    name = 'AFSK'
    longname = 'Audio Frequency Shift Keying'
    desc = 'Audio Frequency Shift Keying'
    license = 'unknown'
    inputs = ['logic']
    outputs = ['afsk_bits']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'afsk', 'name': 'afsk', 'desc': 'AFSK stream'},
    )
    options = (
        {'id': 'markfreq', 'desc': 'Mark(1) Frequency', 'default': 2000},
        {'id': 'spacefreq', 'desc': 'Space(0) Frequency', 'default': 4000},
        {'id': 'marginpct', 'desc': 'Error margin %', 'default': 40},
    )
    annotations = (
        ('bit-raw', 'Raw Bit'),
        ('bit-error', 'Unknown half-cycle'),
        ('bit-phase', 'Phase error'),

    )
    annotation_rows = (
        ('raw-bits', 'Raw Bits', (0,)),
        ('errors', 'Errors', (1, 2,)),
    )
    
    def putbitraw(self):
        self.put(self.twoedgesagosample, self.currentedgesample, self.out_python,
                ['BIT', self.lastbit])
        self.put(self.twoedgesagosample, self.currentedgesample, self.out_ann,
                [0, ['%d' % self.lastbit]])
    
    def puterror(self):
        self.put(self.oneedgeagosample, self.currentedgesample, self.out_python,
                ['ERROR', 'INVALID'])
        self.put(self.oneedgeagosample, self.currentedgesample, self.out_ann,
                [1, ['Error: Invalid cycle', 'Error', 'Err', 'E']])
    
    def putphaseerror(self):
        self.put(self.oneedgeagosample, self.currentedgesample, self.out_python,
                ['ERROR', 'PHASE'])
        self.put(self.oneedgeagosample, self.currentedgesample, self.out_ann,
                [2, ['Phase error: Resyncing', 'Phase error', 'Phase', 'P']])
    
    def __init__(self):
        self.state = 'IDLE'
        self.lastbit = None
        self.twoedgesagosample = self.oneedgeagosample = self.currentedgesample = self.lastcycletype = self.cycletype = self.count = 0

    def reset(self):
        self.__init__(self)
    
    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.markfreq = self.options['markfreq']
        self.spacefreq = self.options['spacefreq']
        self.marginpct = self.options['marginpct']
        
        self.markhalfcycle = int(self.samplerate * ((1 / self.markfreq) / 2)) - 1
        self.markmargin = int(self.markhalfcycle * (self.marginpct * 0.01))
        self.spacehalfcycle = int(self.samplerate * ((1 / self.spacefreq) / 2)) - 1
        self.spacemargin = int(self.spacehalfcycle * (self.marginpct * 0.01))
    
    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value
    
    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        
        while True:
            #Save the most recent edge sample for the next length
            self.twoedgesagosample = self.oneedgeagosample
            self.oneedgeagosample = self.currentedgesample
            #Save the last cycle type
            self.lastcycletype = self.cycletype
            
            #Wait for any edge
            (self.audio,) = self.wait({0: 'e'})
            #self.log(5, 'Found edge at sample ' + self.samplenum)
            #print('Found edge at', self.samplenum, '!')
            
            #Save the new edge
            self.currentedgesample = self.samplenum
            
            length = self.currentedgesample - self.oneedgeagosample
            #print('Length since last edge', length, '.')
            
            if length in range(self.spacehalfcycle - self.spacemargin, self.spacehalfcycle + self.spacemargin + 1):
                self.cycletype = 'SPACE'
            elif length in range(self.markhalfcycle - self.markmargin, self.markhalfcycle + self.markmargin + 1):
                self.cycletype = 'MARK'
            else:
                #Invalid half-cycle length. Clean up!
                self.cycletype = 'ERROR'
                print('L', length)
                print('M', self.markhalfcycle)
                print('S', self.spacehalfcycle)
                print('MM', self.markmargin)
                print('SM', self.spacemargin)
            
            if self.cycletype == 'SPACE' and self.lastcycletype == 'SPACE':
                #Two unprocessed SPACE half-cycles
                self.lastbit = 0
                self.putbitraw()
                
                self.cycletype = 'PROCESSED'
            elif self.cycletype == 'MARK' and self.lastcycletype == 'MARK':
                #Two unprocessed MARK half-cycles
                self.lastbit = 1
                self.putbitraw()
                
                self.cycletype = 'PROCESSED'
            elif self.cycletype == 'ERROR':
                #This cycle was an error, pass it onward so that things can clean up
                self.lastbit = 2
                self.puterror()
            elif (self.cycletype == 'SPACE' and self.lastcycletype == 'MARK') or (self.cycletype == 'MARK' and self.lastcycletype == 'SPACE'):
                self.lastbit = 2
                self.putphaseerror()
                print('L', length)
                print('M', self.markhalfcycle)
                print('S', self.spacehalfcycle)
                print('MM', self.markmargin)
                print('SM', self.spacemargin)
            else:
                #Nothing to do with this half-cycle
                self.lastbit = 2
                