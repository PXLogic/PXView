## SENT SAE J2716 decoder
##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2014/2024 Volker Oth [0xdeadbeef]
##   (based on my Javascript version from 2014)
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
## Comments:
## - Based on the SAE 2716 from 2010
## - Configuration bit is displayed but not evaluated to format enhanced serial data differently

import sigrokdecode as srd
from collections import namedtuple

class SamplerateError(Exception):
    pass

class ChannelError(Exception):
    pass

# DATA_BIT:        Bit2 of status & communication nibble is the serial data bit
DATA_BIT = 4
# START_BIT:       Bit3 of status & communication nibble is the serial start bit
START_BIT = 8

# 4bit CRC calculation
CRC4_INIT = 5
crc4Table = [0,13,7,10,14,3,9,4,1,12,6,11,15,2,8,5]

# 6bit CRC calculation
CRC6_INIT = 0x15
CRC6_POLY = 0x59

crc6Table = [ 0,25,50,43,61,36,15,22,35,58,17, 8,30, 7,44,53,
             31, 6,45,52,34,59,16, 9,60,37,14,23, 1,24,51,42,
             62,39,12,21, 3,26,49,40,29, 4,47,54,32,57,18,11,
             33,56,19,10,28, 5,46,55, 2,27,48,41,63,38,13,20 ]

ann_tick, ann_cal, ann_sc, ann_data, ann_crc, ann_pause, ann_serial_start, ann_serial_data, ann_serial_id, ann_serial_config, ann_serial_frame, ann_serial_sync, ann_serial_crc, ann_warning = range(14)

class Decoder(srd.Decoder):

    api_version = 3
    id = 'sent'
    name = 'SENT'
    longname = 'Single Edge Nibble Transmission'
    desc = 'Single line, one-directional, nibble based protocol'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['sent']
    tags = ['Embedded/automotive']

    channels = (
        {'id': 'data', 'type': 0, 'name': 'data', 'desc': 'Nibble Data'},
    )

    optional_channels = ()

    options = (
        {'id': 'dataSize', 'desc': 'Number of data nibbles', 'default': 6,
            'values': (1,2,3,4,5,6,7,8), 'idn':'dec_sent_opt_datasize'},
        {'id': 'tickPer', 'desc': 'Clock period (µs)', 'default': 3,
            'values': tuple(range(2,101,1)), 'idn':'dec_sent_opt_tickper'},
        {'id': 'tickTol', 'desc': 'Tick tolerance (%)', 'default': 5,
            'values': tuple(range(0,21,1)), 'idn':'dec_sent_opt_ticktol'},
        {'id': 'pausePulse', 'desc': 'Pause Pulse','default': 'on',
            'values': ('off', 'on'), 'idn':'dec_sent_opt_pausepulse'},
        {'id': 'crcMode', 'desc': 'CRC Mode', 'default': 'recommended',
            'values': ('legacy', 'recommended'), 'idn':'dec_sent_opt_crcmode'},
        {'id': 'serialMode', 'desc': 'Serial Decoding', 'default': 'off',
            'values': ('short', 'enhanced', 'off'), 'idn':'dec_sent_opt_serialmode'},
    )
    
    annotations = (                        #  Implicitly assigned annotation type ID
        ('pulse', 'Pulse length'),         #  0 ann_tick
        ('cal', 'Calibration Pulse'),      #  1 ann_cal
        ('sc', 'Status&Comm Nibble'),      #  2 ann_sc
        ('data', 'Nibble Data'),           #  3 ann_data
        ('crc', 'CRC Nibble'),             #  4 ann_crc
        ('pause', 'Pause Pulse'),          #  5 ann_pause
        ('start', 'Start of Frame' ),      #  6 ann_serial_start
        ('serData', 'Serial Data'),        #  7 ann_serial_data
        ('serID', 'Serial ID'),            #  8 ann_serial_id
        ('serCfg', 'Serial Config'),       #  9 ann_serial_config
        ('serFrame', 'Serial Frame Bit'),  # 10 ann_serial_frame
        ('serSync', 'Serial Sync Bit'),    # 11 ann_serial_sync
        ('serCrc', 'Serial CRC'),          # 12 ann_serial_crc
        ('warning', 'Warning'),          # 13 ann_warning
    )
    annotation_rows = (
        ('sent', 'SENT fast messages', (ann_cal,ann_sc,ann_data,ann_crc,ann_pause)),
        ('pulses', 'Pulse lengths', (ann_tick,)),
        ('serial', 'SENT slow messages', (ann_serial_start,ann_serial_data,ann_serial_crc)),
        ('serialID', 'Enhanced slow message IDs', (ann_serial_id,ann_serial_config,ann_serial_frame,ann_serial_sync)),
        ('warnings', 'Warnings', (ann_warning,)),
    )

    # Number of SENT ticks allowed for a pause pulse (SAE J2716 defines 768, +1 to be more relaxed)
    ppTicks = 769

    # Table used for displaying enhanced serial data
    strData = ['HI', 'MED', 'LO']

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.last_samplenum = None

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def metadata(self, key, value):
       if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def decode(self):
        # One input is mandatory. 
        if not self.has_channel(0):
            raise ChannelError('SENT pin required.')
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')
        
        self.state = 'SYNC_ST'                      # message deoding state: start searching for calibration
        self.serialState = 'SER_SYNC_ST'            # serial decoding state: start searching for sync bit/pattern
        self.pausePulse = 0 if self.options['pausePulse']=='off' else 1
        self.pauseTmp = self.pausePulse             # temporary enable bit for pause pulse
        self.statusNibble = -1                      # value of status nibble that contains the serial information
        self.serialCtr = 0                          # counter for serial messages
        self.pulseCtr = 0                           # message pulse counter
        self.serialMode = self.options['serialMode'] # serial mode
        self.tickPer = self.options['tickPer']      # clock/tick period
        self.dataSize = self.options['dataSize'] +2 # +2 for SC and CRC nibbles
        self.crcMode = 0 if self.options['crcMode']=='legacy' else 1
        self.crcPos = self.dataSize
        if self.pauseTmp != 0:
            self.dataSize += 1
        # Note: samplerate is the number of samples per second
        # Since tickPer etc. are given in microseconds, samplerate/1e6 is the number of samples per microsecond
        
        # calculate min/max CAL size from given tick period and allowed deviation
        self.calPeriod = round(56*self.tickPer*self.samplerate/1000000.0)
        self.maxPausePulse = round(self.ppTicks*self.tickPer*self.samplerate/1000000.0)
        self.minCalTicks = round(self.calPeriod * (1.0-self.options['tickTol']/100.0))
        self.maxCalTicks = round(self.calPeriod * (1.0+self.options['tickTol']/100.0))
        self.wait({0: 'f'}) # search for first falling edge
        
        calStart = -1
        calEnd = -1

        while True:
            self.last_samplenum = self.samplenum
            self.wait({0: 'f'}) # search for first falling edge
            storeCal = 0
            nibble = -1
            period = self.samplenum - self.last_samplenum # period between active edges
            fault = 'OK'
            serialFault = 'SER_OK'
            if self.state == 'SYNC_ST':
                nibble = -1
                # Synchronization State
                if period <= self.maxCalTicks:
                    if period >= self.minCalTicks:
                        # Either a pause pulse (CAL will be next) or CAL pulse
                        self.pauseTmp = 2 # possible pause pulse
                        self.pulseCtr = 0
                        self.state = 'DECODE_ST'
                        storeCal = 1
                    else:
                        # Continue searching for CAL
                        if self.pulseCtr == self.dataSize:
                            fault = 'TOO_MANY_PULSES'
                            self.state = 'RESYNC_ST'
                        else:
                            self.pulseCtr+=1
                else:
                    # This could be pause pulse
                    if (self.pauseTmp == 0) or (period > self.maxPausePulse):
                        # timeout
                        fault = 'PULSE_TOO_LONG'
                        self.state = 'RESYNC_ST'
                    else:
                        self.pauseTmp = 0 # forbid another pause pulse
            else: 
                # decode state
                if period > self.maxPausePulse:
                    # timeout
                    fault = 'PULSE_TOO_LONG'
                    self.state = 'RESYNC_ST'
                elif (self.pauseTmp != 1) and (period >= self.minCalTicks) and (period <= self.maxCalTicks):
                    # CAL pulse
                    storeCal = 1
                    if self.pulseCtr == self.dataSize:
                        # check clock shift
                        if abs(period-self.calPeriod)*64 > self.calPeriod:
                            fault = 'CLOCK_SHIFT'
                        else:
                            # message was received -> serial decoding
                            if self.serialMode =='short':
                                # short message format
                                if (self.statusNibble & START_BIT) != 0:
                                    # start bit detected
                                    if self.serialState == 'SER_DECODE_ST':
                                        # serialFault = 'SER_TOO_FEW_BITS'
                                        self.put(serialNibbleStart, calStart, self.out_ann, [ann_warning, ['SER_TOO_FEW_BITS']])
                                    self.serialCtr = 0
                                    self.serialState = 'SER_DECODE_ST'
                                    self.put(calStart, calEnd, self.out_ann, [ann_serial_start, ['Start short message', 'Start short', 'Start', 'ST']])
                                    serialNibbleStart = calEnd
                                    # Init CRC
                                    serialCrc = CRC4_INIT
                                    serialNibble = 1 if (self.statusNibble & DATA_BIT) != 0 else 0
                                elif self.serialState == 'SER_DECODE_ST':
                                    serialNibble <<= 1
                                    serialNibble |= 1 if ((self.statusNibble & DATA_BIT) != 0) else 0
                                    self.serialCtr+=1
                                    if (self.serialCtr == 3) or (self.serialCtr == 7) or (self.serialCtr == 11):
                                        serialCrc = crc4Table[serialCrc] ^ serialNibble
                                        dataIndex = round(((self.serialCtr+1)/4)-1)
                                        self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_serial_data, ['DATA_%d 0x%01X' % (dataIndex,serialNibble), '0x%01X' % serialNibble, '%01X' % serialNibble]])
                                        serialNibbleStart = self.last_samplenum
                                        serialNibble = 0
                                    elif self.serialCtr == 15:
                                        self.serialState = 'SER_SYNC_ST'
                                        # check CRC
                                        if self.crcMode != 0:
                                            serialCrc = crc4Table[serialCrc]
                                        if (serialCrc & 0xf) != (serialNibble & 0x0f):
                                            # serialFault = 'SER_CRC_ERROR'
                                            self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_warning, ['SER_CRC_ERROR (0x%01X/0x%01X)' % (serialNibble,serialCrc), 'SER_CRC_ERROR', 'CRC']])
                                        # message decoded
                                        self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_serial_crc, ['CRC 0x%01X' % serialNibble, 'CRC %01X' % serialNibble]])
                                if serialFault != 'SER_OK':
                                    self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_warning, [serialFault]])
                            elif self.serialMode == 'enhanced':
                                # enhanced message format
                                serialBit3 = (self.statusNibble >> 3) & 1
                                if self.serialState == 'SER_SYNC_ST':
                                    if serialBit3 == 0:
                                        self.serialState = 'SER_SYNCED_ST' # look for start pattern
                                else:
                                    if self.serialState == 'SER_SYNCED_ST':
                                        self.serialCtr = 0
                                        self.serialState = 'SER_PATTERN_ST'
                                        serialNibble = 0
                                        serialNibbleID = 0
                                        serialCrc = CRC6_INIT
                                        if serialBit3 != 0:
                                            self.put(calStart, calEnd, self.out_ann, [ann_serial_start, ['Start enhanced message', 'Start enhanced', 'Start', 'ST']])
                                        serialNibbleStart = calEnd
                                    serialBit2 = (self.statusNibble >> 2) & 1
                                    serialNibble <<= 1
                                    serialNibble |= serialBit2
                                    self.serialCtr+=1
                                    if self.serialCtr < 7:
                                        if serialBit3 != 0:
                                            if self.serialCtr == 6:
                                                serialCrcRx = serialNibble
                                                serialNibble = 0
                                                self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_serial_crc, ['CRC 0x%02X' % serialCrcRx, 'CRC %02X' % serialCrcRx]])
                                                self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_serial_sync, ['SYNC Pattern', 'SYNC']])
                                                self.crcStart = serialNibbleStart
                                                self.crcEnd = self.last_samplenum
                                        else:
                                            self.serialState = 'SER_SYNCED_ST' # look for start pattern
                                    else:
                                        # update crc
                                        serialCrc <<= 1
                                        if (serialCrc & 64) != 0:
                                            serialCrc ^= CRC6_POLY
                                        serialCrc ^= serialBit2
                                        serialCrc <<= 1
                                        if (serialCrc & 64) != 0:
                                            serialCrc ^= CRC6_POLY
                                        serialCrc ^= serialBit3
                                        # handling of different bits
                                        if self.serialCtr in [7,13,18]:
                                                # check zero bits
                                                if serialBit3 == 0:
                                                    if self.serialCtr == 18:
                                                        # message received
                                                        self.serialState = 'SER_SYNCED_ST'
                                                        serialCrc = crc6Table[serialCrc] # add 6 zero bits to CRC
                                                        if serialCrcRx != serialCrc:
                                                            # serialFault = 'SER_CRC_ERROR'
                                                            self.put(self.crcStart, self.crcEnd, self.out_ann, [ann_warning, ['SER_CRC_ERROR (0x%01X/0x%01X)' % (serialCrcRx,serialCrc), 'SER_CRC_ERROR', 'CRC']])
                                                    else:
                                                        self.serialState = 'SER_DECODE_ST'
                                                else:
                                                    self.serialState = 'SER_SYNC_ST'
                                                    # serialFault = 'SER_FRAME_BIT_ERROR'
                                                    self.put(calStart, self.last_samplenum, self.out_ann, [ann_warning, ['SER_FRAME_BIT_ERROR', 'FRAME_ERROR', 'F']])
                                                self.put(calStart, self.last_samplenum, self.out_ann, [ann_serial_frame, ['Frame %d' % serialBit3, 'F %d' % serialBit3]])
                                        elif self.serialCtr == 8:
                                                # configuration bit
                                                self.put(calStart, self.last_samplenum, self.out_ann, [ann_serial_config, ['Config %d' % serialBit3, 'C %d' % serialBit3]])
                                        else:
                                            # update ID
                                            serialNibbleID <<= 1
                                            serialNibbleID |= serialBit3
                                        # packet output
                                        if self.serialCtr in [7,11,15]:
                                            serialNibbleStart = calStart
                                        elif self.serialCtr == 9:
                                            serialNibbleIDStart = calStart
                                        elif self.serialCtr in [12,17]:
                                            idString = 'ID_HI ' if self.serialCtr==12 else 'ID_LO '
                                            self.put(serialNibbleIDStart, self.last_samplenum, self.out_ann, [ann_serial_id, ['%s0x%01X' % (idString,serialNibbleID), '%s%01X' % (idString,serialNibbleID)]])
                                            serialNibbleID = 0
                                        elif self.serialCtr in [10,14,18]:
                                            if self.serialCtr == 14:
                                                serialNibbleIDStart = calStart
                                            dataString = 'DATA_'+self.strData[round((self.serialCtr-10)/4)]
                                            self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_serial_data, ['%s 0x%01X' % (dataString,serialNibble), '%s %01X' % (dataString,serialNibble)]])
                                            serialNibble = 0
                                if serialFault != 'SER_OK':
                                    self.put(serialNibbleStart, self.last_samplenum, self.out_ann, [ann_warning, [serialFault]])
                    else:
                        if self.pauseTmp != 2:
                            fault = 'TOO_FEW_PULSES' # CAL or pause pulse before all bits received
                            self.pauseTmp = 2 # could be a pause pulse
                        else:
                            self.pauseTmp = 0 # CAL pulse after pause pulse
                    self.pulseCtr = 0
                else:
                    # normal pulse (or pause pulse)
                    self.pauseTmp = 0
                    if self.pulseCtr == self.dataSize:
                        fault = 'TOO_MANY_PULSES' # CAL or pause pulse before all bits received
                        self.state = 'RESYNC_ST'
                    else:
                        self.pulseCtr += 1
                        # decode nibble
                        nibble = round(period / self.tickPeriod)
                        if nibble < 12:
                            fault = 'DATA_TOO_SMALL'
                            self.state = 'RESYNC_ST'
                            nibble = -1
                        elif (self.pausePulse == 0) or (self.pulseCtr < self.dataSize):
                            if nibble > 27:
                                if period > self.maxCalTicks:
                                    fault = 'PULSE_TOO_LONG' # Longer than a CAL pulse 
                                else:
                                    fault = 'DATA_TOO_LARGE' # Shorter than a CAL pulse, but too long for a nibble
                                    nibble = -1
                                self.state = 'RESYNC_ST'
                            else:
                                # store nibble
                                nibble -= 12
                                if self.pulseCtr == 1:
                                    self.statusNibble = nibble
                                    crc = CRC4_INIT
                                else:
                                    tmp = crc4Table[crc]
                                    if self.pulseCtr == self.crcPos:
                                        self.pauseTmp = 1  # expect pause pulse next (if enabled)
                                        crcRx = nibble
                                        if self.crcMode != 0:
                                            crc = tmp
                                        if crc != crcRx:
                                            fault = 'CRC_ERROR'
                                    else:
                                        crc = tmp ^ nibble

            perMicroSec = period*1000000/self.samplerate
            # Display CAL
            if storeCal != 0:
                self.calPeriod = period
                self.tickPeriod = round(self.calPeriod / 56)
                self.maxPausePulse = round(self.calPeriod * self.ppTicks / 56)
                calStart = self.last_samplenum
                calEnd = self.samplenum

            self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_tick, ['%4.1fµs' % perMicroSec, '%3.0f' % perMicroSec]])
            # Display nibbles
            if (storeCal == 0) and (nibble != -1):
                if self.pulseCtr==1:
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_sc, ['Status&Communication 0x%01X' % nibble, 'Status&Comm 0x%01X' % nibble, 'SC 0x%01X' % nibble, 'SC %01X' % nibble]])
                elif self.pulseCtr==self.crcPos:
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_crc, ['Checksum 0x%01X' % nibble, 'CRC 0x%01X' % nibble, 'CRC %01X' % nibble]])
                elif (self.pausePulse!=0) and (self.pulseCtr==self.dataSize):
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_pause, ['Pause Pulse', 'Pause', 'P']])
                else:
                    dataIndex = round(self.pulseCtr-2)
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_data, ['DATA_%d 0x%01X' % (dataIndex,nibble), '0x%01X' % nibble, '%01X' % nibble]])
            # Display faults
            if fault != 'OK':
                if fault == 'CRC_ERROR':
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_warning, ['CRC_ERROR (0x%01X/0x%01X)' % (crcRx,crc), 'CRC_ERROR (%01X/%01X)' % (crcRx,crc), 'CRC_ERROR', 'CRC']])
                else: 
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_warning, [fault, 'Fault', 'F']])
                if self.serialState == 'SEC_DECODE_ST':
                    self.put(self.last_samplenum, self.samplenum, self.out_ann, [ann_warning, ['TOO_FEW_BITS', 'Fault', 'F']])
                self.serialState = 'SER_SYNC_ST'
            # Add CAL to packet view
            if storeCal != 0:
                self.put(self.last_samplenum, self.samplenum , self.out_ann, [ann_cal, ['Calibration Pulse', 'Calibration', 'CAL', 'C']])
            if self.state == 'RESYNC_ST':
                self.pulseCtr = 0
                self.state = 'SYNC_ST'
                self.pauseTmp = 1
            if self.pausePulse == 0: # if pause pulse is disabled, ignore temporary expectations
                self.pauseTmp = 0
