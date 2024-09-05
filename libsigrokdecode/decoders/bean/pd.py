##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2019 Trufanov Alex <trufan@mail.ru>
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

## 1 SOF (1 bit) (start of frame)
## 2-5 PRI (4 bits) (priority of bits)
## 6-9 ML (4 bits) (message length, length of message and number of bytes, [ID + DATA] (3-13)
## 10-17 DST-ID (8 bits) (the id showing the communication destination)
## 18-25 MES-ID (8 bits) (is an area showing message contents)
## 26-113 DATA (8 to 88 bits) (1 to 11 bytes of data, variable length, could be as small as bits 26-33, reskewing the bit number slots for the remaining bit groups)
## 114-121 CRC (8 bits) (error check code remainder from polynomial)
## 122-129 EOM (8 bits) (end of message and controls time to prepare response for sending)
## 130-132 RSP (2 bits) (represents the response area)
## 133-138 EOF (6 bits) (shows the end of the frame)

##    PRI
##    |                           DATA (variable                   RSP
## SOF|    ML   DID      MID      | length)      CRC      EOM      |  EOF
##  | |    |    |        |        |              |        |        |  |
##  x_xxxx_xxxx_xxxxxxxx_xxxxxxxx_[8 to 88 bits]_xxxxxxxx_xxxxxxxx_xx_xxxxxx 


##    PRI+ML	 DST-ID   MES-ID   DATA 1   DATA 2   CRC     EOM 
##    04	 	 FE		  AB	   A1	    80	     E9	     7E

##  1000001100 111110110 1010101110100001100000100111010010111111   with bit stuff
##   00000100 11111110 10101011 10100001 10000000 11101001 01111110  remove bit stuff



import sigrokdecode as srd
from .lists import *

def pinlabels(bit_count):
    return 'Data%i' % (bit_count - 2)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'bean'
    name = 'BEAN'
    longname = 'BEAN is a Toyota Body Electronics Area Network'
    desc = 'BEAN is a Toyota Body Electronics Area Network.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Data line', 'default': 0},
    )
    annotations = (
        ('bit-0', 'Bit 0'),
        ('bit-1', 'Bit 1'),
        ('bite_ann', 'Bite_ann'),
        ('byte', 'Byte'),
        ('frame', 'Frame'),
        ('message', 'Message'),
        ('pulse_width', 'Pulse width'),
        ('debug', 'Debug'),
        ('all byte', 'All byte'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0, 1,)),
        ('bits_ann', 'Bits_ann', (2,)),
        ('bytes', 'Bytes', (3,)),
        ('frames', 'Frames', (4, 5,)),
        ('pulse_widths', 'Pulse_widths', (6,)),
        ('command', 'Command', (7,)),
        ('All byte', 'All byte', (8,)),
    )
    options = (
        {'id': 'bit_annotations', 'desc': 'Bit annotations', 'default': 'none', 
            'values': ('none', 'yes')},
        {'id': 'pulse_len', 'desc': 'Pulse length', 'default': 'none',
            'values': ('none', 'yes')},
        {'id': 'command', 'desc': 'Command', 'default': 'yes', 
            'values': ('none', 'yes')},
        {'id': 'all byte', 'desc': 'All byte', 'default': 'yes', 
            'values': ('none', 'yes')},
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplenumber_last = None
        self.pulses = []
        self.bits = []
        self.bits_ann = []
        self.byte = []
        self.byte_ann =[]
        self.labels = []
        self.byte_all =[]
        self.bit_count = 0
        self.ss = None
        self.es = None
        self.state = 'IDLE'
        self.temp = 0
        self.SOF = 0
        self.EOM = 0
        self.stuff = 0
        self.draw = 0
        self.noresp = 0

    def reset_frame(self):
        #self.samplenumber_last = None
        self.pulses = []
        self.bits = []
        self.bits_ann = []
        self.byte = []
        self.byte_ann =[]
        self.labels = []
        self.byte_all =[]
        self.bit_count = 0
        #self.ss = None
        #self.es = None
        self.state = 'IDLE'
        self.temp = 0
        self.SOF = 0
        self.EOM = 0
        self.stuff = 0
        self.draw = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        #self.model = self.options['remote']

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def decode(self):
        b = 0
        frame_count = 0
        while True:
            (self.pin,) = self.wait({0: 'e'})
            i = 0
            if not self.samplenumber_last: # Set counters to start of signal.
                self.samplenumber_last = self.samplenum
                self.pin_last = self.pin
                self.ss = self.samplenum
                continue
            self.es = self.samplenum
            puls = self.es - self.ss
            if self.options['pulse_len'] == 'yes' :     self.put(self.ss, self.es, self.out_ann, [6, [' %i' % puls]]) # Write decoded bit.
            count = puls//(100)
            if (puls > 150 and puls <650) :
                temp_ss = self.ss
                while i < (count-1):
                    if self.stuff == 0:
                        if self.SOF == 0:
                            self.SOF = 1
                            temp_es = temp_ss + puls//count
                            self.bits_ann.append(['SOF', self.ss, temp_es]) # Save states and times.
                            temp_ss = temp_es
                        else :
                            temp_es = temp_ss + puls//count
                            self.bits.append([self.pin_last, temp_ss, temp_es]) # Save states and times.
                            temp_ss = temp_es
                    else:
                       self.stuff = 0
                       temp_es = temp_ss + puls//count
                       self.bits_ann.append(['Stuff', temp_ss, temp_es]) # Save states and times.
                       temp_ss = temp_es
                    i += 1
                    if (i == 4 and count == 5):
                        self.stuff = 1
                self.bits.append([self.pin_last, temp_ss, self.es]) # Save states and times.
               #if self.EOM == 1 :
               #     self.byte_ann.append(['RSP', self.bits[-3][1], self.bits[-1][2], 0])
               #     self.draw = 1
                if count == 6:
                    self.EOM = 1
                    frame_count += 1
            elif puls <=150 :
                if self.SOF == 0:
                    self.SOF = 1
                    self.bits_ann.append(['SOF', self.ss, self.es]) # Save states and times.
                elif self.stuff == 1:
                    self.bits_ann.append(['Stuff', self.ss, self.es]) # Save states and times.
                    self.stuff = 0
                else :
                    self.bits.append([self.pin_last, self.ss, self.es]) # Save states and times.
                if self.EOM == 1 :
                    self.byte_ann.append(['RSP', self.bits[-2][1], self.bits[-1][2], 0])
                    self.draw = 1
            elif puls >= 650 :
                if (self.EOM == 1) :
                    self.bits.append([self.pin_last, self.bits[-1][2], self.bits[-1][2]+(self.bits[-1][2] - self.bits[-1][1])]) # Save states and times.
                    self.draw = 1
                    self.noresp = 1
                #else :
                self.SOF = 0
                self.stuff = 0
                
            self.ss = self.samplenum
            self.pin_last = self.pin
            if self.draw == 1 :
                frame_pri = 0
                for i in range(0,4):
                    if self.bits[i][0] == 1:
                        frame_pri = frame_pri<<1
                        frame_pri = frame_pri|1
                    else :
                        frame_pri = frame_pri<<1
                frame_length = 0
                for i in range(4,8):
                    if self.bits[i][0] == 1:
                        frame_length = frame_length<<1
                        frame_length = frame_length|1
                    else :
                        frame_length = frame_length<<1
                b = 0
                sdata = ''
                allbyte = ''
                if (frame_length+3 <= len(self.bits)//8) :
                    for i in range(0,frame_length+3):
                        for j in range(0,8):
                            if self.bits[i*8+j][0] == 1:
                                b = b<<1
                                b = b|1
                            else :
                                b = b<<1
                        s = hex(b)[2:]
                        self.byte.append([s.upper(), self.bits[i*8][1], self.bits[i*8+j][2]])
                        allbyte = allbyte + " " + self.byte[-1][0]
                        if i == 0 : 
                            self.byte_ann.append(['PRI: ' + hex(frame_pri), self.bits[0][1], self.bits[3][2], 0])
                            self.byte_ann.append(['ML: ' + hex(frame_length), self.bits[4][1], self.bits[7][2], 0])
                        elif i == 1 : self.byte_ann.append(['DST-ID', self.bits[i*8][1], self.bits[i*8+j][2], 1])
                        elif i == 2 :
                            self.byte_ann.append(['MES-ID', self.bits[i*8][1], self.bits[i*8+j][2], 1])
                            sdata = sdata + self.byte[-1][0]
                        elif i == frame_length+1 :
                            self.byte_ann.append(['CRC', self.bits[i*8][1], self.bits[i*8+j][2], 0])
                            self.byte_all.append([allbyte, self.bits[0][1], self.bits[i*8+j+8][2]])
                        elif i == frame_length+2 :
                            self.byte_ann.append(['EOM', self.bits[i*8][1], self.bits[i*8+j][2], 0])
                        else : 
                            self.byte_ann.append([pinlabels(i), self.bits[i*8][1], self.bits[i*8+j][2], 1])
                            sdata = sdata + self.byte[-1][0]
                        b = 0
                    x = command.get(sdata, 'Unknown')
                    if (self.options['command'] == 'yes' and x != 'Unknown') : self.put(self.byte[-(frame_length+1)][1], self.byte[-3][2], self.out_ann, [7, [x]]) # Write decoded bit.
                    for i in range(0,len(self.bits)) :
                        self.put(self.bits[i][1], self.bits[i][2], self.out_ann, [self.bits[i][0], [' %i' % self.bits[i][0]]]) # Write decoded bit.
                    if self.options['bit_annotations'] == 'yes' : 
                        for i in range(0,len(self.bits_ann)) :
                            self.put(self.bits_ann[i][1], self.bits_ann[i][2], self.out_ann, [2, [self.bits_ann[i][0]]]) # Write decoded bit annotations.
                    if self.options['all byte'] == 'yes' : 
                        for i in range(0,len(self.byte_all)) :
                            self.put(self.byte_all[i][1], self.byte_all[i][2], self.out_ann, [8, [self.byte_all[i][0]]]) # Write decoded all byte annotations for analiz.
                    for i in range(0,len(self.byte)) :
                        self.put(self.byte[i][1], self.byte[i][2], self.out_ann, [3, [self.byte[i][0]]]) # Write decoded byte.
                    for i in range(0,len(self.byte_ann)) :
                        if self.byte_ann[i][3] == 1 : self.put(self.byte_ann[i][1], self.byte_ann[i][2], self.out_ann, [4, [ self.byte_ann[i][0]]]) # Write byte annotation.
                        else : self.put(self.byte_ann[i][1], self.byte_ann[i][2], self.out_ann, [5, [ self.byte_ann[i][0]]]) # Write byte annotation.
                    if self.noresp :
                        self.reset_frame()
                    else : self.reset()
                else : 
                    self.reset()

