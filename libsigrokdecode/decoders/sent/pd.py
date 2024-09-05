##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 enp6s0
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


##
## SAE J2716 (SENT) decoder
## Main protocol decoder class
##


import sigrokdecode as srd
from .crc4 import *

class Decoder(srd.Decoder):
    api_version = 3
    id = 'sae_j2716_sent'
    name = 'SAE J2716'
    longname = 'Single Edge Nibble Transmission'
    desc = 'One-wire automotive sensor bus'
    license = 'mit'
    inputs = ['logic']
    outputs = ['j2716']
    tags = ['automotive']
    channels = (
        {'id': 'sent', 'name': 'Data Line', 'desc': 'SENT data line'},
    )
    optional_channels = ()
    options = (
        {'id': 'data_nibbles_count', 'desc': 'Number of data nibbles', 'default': 6},
        {'id': 'tick_time', 'desc': 'Tick time (us)', 'default': 3.0},
        {'id': 'use_spc', 'desc': 'SPC (Short PWM Code)', 'default': 'No', 'values': ('Yes', 'No')},
        {'id': 'crc_method', 'desc': 'CRC method', 'default': 'J2716 Recommended', 'values': ('J2716 Recommended', 'J2716 Legacy', 'Infineon')},
        {'id': 'data_output_format', 'desc': 'Data output format', 'default': 'Hexadecimal', 'values': ('Hexadecimal', 'Decimal', 'Binary')},
    )
    annotations = (
        ('unknown', 'Unknown'),
        ('calibration', 'Calibration pulse'),
        ('status', 'Status nibble'),
        ('data', 'Data nibble'),
        ('crc', 'CRC nibble'),
        ('spc-trigger', 'SPC trigger pulse'),
        ('spc-end', 'SPC end pulse'),
        ('packet', 'SENT packet'),
        ('invalid-packet', 'Invalid SENT packet'),
        ('debug-generic', 'Generic debug message'),
    )
    annotation_rows = (
        ('datarow', 'Data', (2,3,4)),
        ('pulse', 'Pulses', (1,5,6)),
        ('warnings', 'Warnings', (0,)),
        ('packets', 'Packets', (7,8)),
        ('debug', 'Debug', (9,)),
    )

    def __init__(self, **kwargs):
        self.reset()

    def reset(self):
        # SENT packet holder, to hold pulses that makes up
        # a SENT packet for analysis
        self.packetHolder = []

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        # Register output annotations
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_binary = self.register(srd.OUTPUT_BINARY)

        # Number of data nibbles
        self.dataNibblesCount = int(self.options['data_nibbles_count'])
        assert self.dataNibblesCount >= 1, 'Data nibbles count must be positive'

        # SENT tick time
        self.tickTime = float(self.options['tick_time'])
        assert self.tickTime >= 1, 'Tick time must be positive and at least 1uS'

        # Use SPC?
        self.spc = bool(self.options['use_spc'] == 'Yes')

        # Protocol name (SENT alone or SENT/SPC)
        self.protoname = 'SENT' if not self.spc else 'SENT/SPC'

        # Data output format (hexadecimal, binary, or decimal?)
        self.dataOutFormat = str(self.options['data_output_format']).lower()

        # CRC method
        self.crcMethod = str(self.options['crc_method']).lower()

        # Debug mode?
        # Currently, this is an internal-only variable used during
        # development for verbose outputs and stuff. As this protocol
        # decoder is still mostly a WIP, I'm leaving this in for now
        self.debug = False

    def validCRC(self, decoded):
        '''
        Find out whether the packet has a valid CRC or not. Takes
        in a list of decoded values, with CRC as the last item
        '''
        if type(decoded) != list: return False, -1, -1
        if len(decoded) <= 2: return False, -2, -2

        data = decoded[1:-1] # skip status nibble
        crc = decoded[-1]

        # Find expected checksum (depending on mode)
        if(self.crcMethod == 'j2716 recommended'):
            expectedCRC = crc4(data)
        elif(self.crcMethod == 'j2716 legacy'):
            expectedCRC = crc4(data, legacy = True)
        elif(self.crcMethod == 'infineon'):
            # Infineon method seems to also consider the status
            # nibble for some reason (see: TLE4998 user manual)
            expectedCRC = crc4_infineon(decoded[:-1])

        return (crc == expectedCRC), self.formatData(expectedCRC), self.formatData(crc)

    def formatData(self, data):
        '''
        Function to format data (for display/output purposes) as a string
        of a certain type, defined by the data output format setting
        '''
        if(self.dataOutFormat == 'decimal'):
            return str(data)
        elif(self.dataOutFormat == 'hexadecimal'):
            return '0x{:#X}'.format(data)[2:]
        elif(self.dataOutFormat == 'binary'):
            return str('{:#b}'.format(data)[2:]).rjust(4, '0')
        else:
            return '?'

    def analyzePacket(self, pulses):
        '''
        This subroutine analyzes complete SENT frames
        '''
        firstSample = pulses[0][0] # first falling edge sample number of first pulse
        lastSample = pulses[-1][1] # rising edge sample number of last pulse (end/pause)

        # Total number of pulses in here
        pulseCount = len(pulses)

        # Ignore end/gap pulses
        pulseCount = pulseCount - 1

        # List of ALL stuff to export (not just data carrying nibbles) so we can do
        # annotations and Python exports
        export = []

        # Expected number of pulses: # of data nibbles + status + crc + calibration
        #                            (+ 1 if SPC - first pulse isn't actually a nibble)
        expectedPulseCount = self.dataNibblesCount + 3
        if(self.spc):
            expectedPulseCount += 1

        # Check for pulse count validity - if not, return and end this now
        if(pulseCount != expectedPulseCount):
            self.put(firstSample, lastSample, self.out_ann, [7, ['Malformed ' + str(self.protoname) + ' packet (' + str(pulseCount) + ' pulses, expecting ' + str(expectedPulseCount) + ')']])
            return

        # Set per-tick time and frame status to invalid for now
        tickTime = -1
        frameValid = False

        # Container for all data-carrying pulses
        decoded = []

        # Quick stuffed function to decode nibbles
        def decodeNibble(ticks):
            if(ticks >= 12 and ticks <= 27):
                actualValue = ticks - 12
                decoded.append(actualValue)
                return self.formatData(actualValue), actualValue
            else:
                return '?', 0

        # For each pulse, process it...
        for pulseNum, pulse in enumerate(pulses):
            fall, rise, end, what = pulse

            # If SPC, first pulse should be the SPC trigger pulse
            # todo: parse this (SPC has multiple modes)
            if(pulseNum == 0 and self.spc):
                export.append({
                    'type' : 'spc_trigger',
                    'samples' : {
                        'fall' : fall,
                        'rise' : rise,
                        'end' : end
                    }
                })
                self.put(fall, end, self.out_ann, [5, ['SPC trigger']])
                continue

            # If this is the last pulse, it's probably either a
            # SPC end pulse, or a pause period between nibbles
            if(pulseNum == len(pulses) - 1):
                if(self.spc):
                    # Note: SPC end pulse is only util rise!
                    export.append({
                        'type' : 'spc_end',
                        'samples' : {
                            'fall' : fall,
                            'rise' : rise,
                            'end' : end
                        }
                    })
                    self.put(fall, rise, self.out_ann, [6, ['SPC end']])
                    continue
                else:
                    # A pause pulse maybe? I don't have the traces
                    # to try handling this yet, so it's a TODO
                    continue

            # Now, if SPC, pulse number is going to be -1, as we don't
            # count the sync...
            if(self.spc):
                pulseNum -= 1

            # What is the nibble size?
            nibbleSize = int(end - fall)

            # If we have already encountered a tick time, we can calculate
            # number of ticks for this nibble, specifically
            pulseTicks = -1
            if(tickTime > 0):
                pulseTicks = round(nibbleSize / tickTime)

            # BEGIN actual handling
            if(pulseNum == 0):
                # Calibration/sync pulse (should be 56 ticks)
                tickTime = (end - fall) / 56

                export.append({
                    'type' : 'calibration',
                    'samples' : {
                        'fall' : fall,
                        'rise' : rise,
                        'end' : end
                    },
                    'tick' : tickTime
                })

                self.put(fall, end, self.out_ann, [1, ['Calibration (tick: ' + str(round(tickTime, 4)) + ' samples)']])
            elif(pulseNum == 1):
                # Status nibble
                data, rawdata = decodeNibble(pulseTicks)
                export.append({
                    'type' : 'status',
                    'samples' : {
                        'fall' : fall,
                        'rise' : rise,
                        'end' : end
                    },
                    'data' : data,
                    'raw' : rawdata
                })
                self.put(fall, end, self.out_ann, [2, ['Status: ' + str(data), str(data)]])
            elif(pulseNum >= 2 and pulseNum < 2 + self.dataNibblesCount):
                # Data nibble
                data, rawdata = decodeNibble(pulseTicks)
                export.append({
                    'type' : 'data',
                    'samples' : {
                        'fall' : fall,
                        'rise' : rise,
                        'end' : end
                    },
                    'data' : data,
                    'raw' : rawdata
                })
                self.put(fall, end, self.out_ann, [3, ['Data: ' + str(data), str(data)]])
            elif(pulseNum == 2 + self.dataNibblesCount):
                # CRC
                data, rawdata = decodeNibble(pulseTicks)
                export.append({
                    'type' : 'crc',
                    'samples' : {
                        'fall' : fall,
                        'rise' : rise,
                        'end' : end
                    },
                    'data' : data,
                    'raw' : rawdata
                })
                self.put(fall, end, self.out_ann, [4, ['CRC: ' + str(data), str(data)]])

                # Calculate CRC over the data list, it should be valid...
                frameValid, expectedCRC, actualCRC = self.validCRC(decoded)
            else:
                # Unknown?
                self.put(fall, end, self.out_ann, [0, ['Unknown']])

        if(frameValid):
            self.put(firstSample, lastSample, self.out_ann, [7, [str(self.protoname) + ' frame of length ' + str(pulseCount)]])
        else:
            self.put(firstSample, lastSample, self.out_ann, [8, ['Invalid ' + str(self.protoname) + ' frame of length ' + str(pulseCount) + ' (CRC error: expected ' + str(expectedCRC) + ', got ' + str(actualCRC) + ')']])

        # Export to Python
        self.put(firstSample, lastSample, self.out_python, {
            'type' : 'packet', 
            'data' : export, 
            'samples' : {'begin' : firstSample, 'end' : lastSample}, 
            'crc' : {
                'valid' : frameValid, 
                'actual' : actualCRC, 
                'expected' : expectedCRC, 
                'method' : self.crcMethod
            },
            'format' : self.dataOutFormat,
        })

    def analyzePulse(self, pulse):
        '''
        This subroutine does the analyzing of pulses, given raw data that decode()
        has given us. It'll try to combine multiple SENT pulses into a single
        SENT frame, which will then be sent up to another analyzer to try and
        make sense of the whole packet (frame)
        '''
        fall, rise, end, what = pulse

        # Append pulse to packet holder
        self.packetHolder.append(pulse)

        # If we get a break, we should close any dangling SENT packet as incomplete,
        # and then move on...
        if(what == 'break'):
            self.analyzePacket(self.packetHolder)
            self.packetHolder = []

    def decode(self):

        # Sample rate is needed so we can translate sample
        # numbers into uS, which is a defined value for SENT
        if not self.samplerate:
            raise SamplerateError('Cannot decode without sample rate')

        # Calculate maximum pulse width in samples (anything above this will be ignored)
        # this is a hardcoded value, and as SENT maximum is ~56 ticks, it should work
        # with some margin to spare
        maxPulseWidthTicks = 100 
        self.maxPulseWidthSamples = int(((10 ** 6) * (maxPulseWidthTicks * self.tickTime)) / self.samplerate)

        # Find first falling edge
        pins = self.wait({0: 'f'})
        fall = self.samplenum

        while True:

            # There should be a rising edge here...
            pins = self.wait({0: 'r'})
            rise = self.samplenum

            # Keep finding falling edges
            pins = self.wait({0: 'f'})
            end = self.samplenum

            # At this point, we have the rise-fall-rise pattern
            # which is indicative of a pulse. We should now check
            # if its length does not exceed the maximum pulse width,
            # and if so, pass this pulse along to the pulse analyzer 
            # subroutine to try and make sense of it
            if(end - fall <= self.maxPulseWidthSamples):
                self.analyzePulse((fall, rise, end, 'pulse'))
            else:
                # Indicative of a break, we should let the analyzer
                # know as well, so it can 'terminate'
                self.analyzePulse((fall, rise, end, 'break'))

            # Move on by setting falling edge sample to next falling edge sample
            fall = end
