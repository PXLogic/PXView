##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 James Cordell <james@cordell.org.uk>
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

# This implementation is incomplete. TODO items:
# Link stats and channel packed has been implimented. More packet 
# types needs to be implimented. 
# Needs testing with different CRSF modes
# Be nice to verify the CRC checksums and display if they are correct or not.

import sigrokdecode as srd

RX =0
TX =0

class Decoder(srd.Decoder):
    api_version = 3
    id = 'crsf'
    name = "CRSF"
    longname = "Crossfire rc protocol"
    desc = 'A protocol for radio control systems.'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = []
    tags = ['radio','control', 'RC']
    annotations = (
        ('text-verbose', 'Human-readable text (verbose)'),
        ('text-error', 'Human-readable Error text'),
    )
    annotation_rows = (
        ('normal', 'Normal', (0, 1)),
    )

    sync_bytes = {
            0xEE: ['To Transmitter Module', 'To TX Module'],
            0xEA: ['To Handset', 'To HS'],
            0xC8: ['To Flight Controller', 'To FC' ],
            0xEC: ['To Receiver', 'To RX']
    }

    packet_type = { 
    0x16: ['CRSF_FRAMETYPE_RC_CHANNELS_PACKED'],  #16 channels of 11 bit each is 176 bits
    0x28: ['CRSF_FRAMETYPE_DEVICE_PING'],
    0x29: ['CRSF_FRAMETYPE_DEVICE_INFO'],
    0x2B: ['CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY'],
    0x2C: ['CRSF_FRAMETYPE_PARAMETER_READ'],
    0x2D: ['CRSF_FRAMETYPE_PARAMETER_WRITE'],
    0x32: ['CRSF_FRAMETYPE_COMMAND'],
    0x02: ['GPS'],
    0x07: ['Vario'],
    0x08: ['Battery sensor'],
    0x09: ['Baro altitude'],
    0x10: ['OpenTX sync'],
    0x14: ['LINK_STATISTICS'],
    0x1E: ['Attitude'],
    0x21: ['Flight mode'],
    0x2A: ['Request settings'],
    0x3A: ['Radio'],
    }

    sync_byte = 0
    len_byte = 0
    count = 0
    payload = []
    frame_type = ''

    def __init__(self):
        self.reset()
        self.sync_byte = 0
        self.len_byte = 0
        self.count = 0  # Used to count through the serial bytes.
        self.payload = [] # Holds channel data
        self.frame_type = ''

    def reset(self):
        self.sync_byte = 0
        self.len_byte = 0
        self.count = 0
        self.payload = []
        self.frame_type = ''

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data

        # For now, ignore all UART packets except the actual data packets.
        if ptype != 'DATA':
            return
        serial_byte = pdata[0]

        if self.sync_byte == 0: # Start of packet sync byte
            ann = self.sync_bytes.get(serial_byte,0)
            if ann:
                self.sync_byte = 1
                self.put(ss, es, self.out_ann, [0,ann])
                self.count = 1
                return
            else:
                self.put(ss, es, self.out_ann, [0,['Unknown packet type' + str(serial_byte) ]])
                return
    
        if self.count == 1:
            if serial_byte >= 2 and serial_byte <= 62: # Within range
                self.put(ss, es, self.out_ann, [0,['Num of bytes succeeding:' + str(serial_byte - 2)]]) # len of payload excludes first two bytes
                self.len_byte = serial_byte 
                self.count = 2
                return
            else:
                self.put(ss, es, self.out_ann, [0,['Unknown packet type' + str(serial_byte) ]])
                self.reset()
                return

        if self.count == 2: #Third byte is type
            self.frame_type = self.packet_type.get(serial_byte,0)
            if self.frame_type:
                self.put(ss, es, self.out_ann, [0,self.frame_type])
                self.count = 3
                return
            else:
                self.put(ss, es, self.out_ann, [0,['Unknown packet type' + str(serial_byte) ]])
                self.reset()
                return

        if self.frame_type[0] == 'CRSF_FRAMETYPE_RC_CHANNELS_PACKED': #Accumilate channel data bits
            self.payload = self.payload + pdata[1]
            self.count = self.count + 1
            if self.count == self.len_byte + 2 : # +1 to collect complete payload.
                
                for chan in range(16):
                    chan11bits = ''
                    chan_ss = 0
                    chan_es = 0
                    for bit in range(11):
                        if bit == 0:
                            chan_ss = self.payload[bit + (chan * 11)][1]
                        if bit == 10:
                            chan_es = self.payload[bit + (chan * 11)][2]
                        chan11bits += str(self.payload[bit + (chan * 11)][0])
                    value = int(chan11bits[::-1], 2)  # little edian 'RC' value
                    self.put(chan_ss, chan_es, self.out_ann, [0,['Chan:' + str(chan) + ' Value:' + str(value)]])
                self.put(ss, es, self.out_ann, [0,['Checksum crc8 poly 0xD5.']])
                self.reset()
                return
        elif self.frame_type[0] == 'LINK_STATISTICS':
            self.payload = self.payload + list(pdata)
            self.count = self.count + 1
            if self.count == self.len_byte + 2 : # +1 to collect complete payload.                
                self.put(self.payload[1][0][1], self.payload[1][7][2], self.out_ann, [0,['Uplink RSSI 1: -' + str(self.payload[0]) + 'dB']])
                self.put(self.payload[3][0][1], self.payload[3][7][2], self.out_ann, [0,['Uplink RSSI 2: -' + str(self.payload[2]) + 'dB']])
                self.put(self.payload[5][0][1], self.payload[5][7][2], self.out_ann, [0,['Uplink Link Quality: '+ str(self.payload[4]) ]])
                self.put(self.payload[7][0][1], self.payload[7][7][2], self.out_ann, [0,['Uplink SNR: ' + str(self.payload[6]) + 'dB']])
                self.put(self.payload[9][0][1], self.payload[9][7][2], self.out_ann, [0,['Active Antenna: '+ str(self.payload[8]) ]])
                self.put(self.payload[11][0][1], self.payload[11][7][2], self.out_ann, [0,['RF Mode: '+ str(self.payload[10]) ]])
                self.put(self.payload[13][0][1], self.payload[13][7][2], self.out_ann, [0,['Uplink TX Power: ' + str(self.payload[12]) + ' mW']])
                self.put(self.payload[15][0][1], self.payload[15][7][2], self.out_ann, [0,['Downlink RSSI: -' + str(self.payload[14]) + 'dB']])
                self.put(self.payload[17][0][1], self.payload[17][7][2], self.out_ann, [0,['Downlink Link Quality: ' + str(self.payload[16]) ]])
                self.put(self.payload[19][0][1], self.payload[19][7][2], self.out_ann, [0,['Downlink SNR: ' + str(self.payload[18]) + 'dB']])
                self.put(ss, es, self.out_ann, [0,['Checksum crc8 poly 0xD5.']])
                self.reset()
                return
        else:
            self.put(ss, es, self.out_ann, [0,['Unknown packet type' + str(serial_byte) ]])
            self.reset()

