##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017-2022 Marek Wodzinski <majek@w7i.pl>

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
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
##

import sigrokdecode as srd
from math import floor, ceil

'''
no OUTPUT_PYTHON and OUTPUT_BINARY:-(
'''


class SamplerateError(Exception):
    pass



# main decoder class
class Decoder(srd.Decoder):
    # api keys
    api_version = 3
    id = 'ccd'
    name = 'CCD'
    longname = 'CCD (Crysler Collision Detection) Data Bus'
    desc = "CCD (Crysler Collision Detection) Data Bus."
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['ccd']
    tags = ['Automotive']
    channels = (
        {'id': 'bus', 'name': 'bus', 'desc': 'CCD bidirectional shared data bus'},
    )
    options = (
        {'id': 'ignoreerrors', 'desc': "Ignore checksum and frame errors", 'default': 'no',
            'values': ('yes', 'no')},
        {'id': 'invert_bus', 'desc': 'Invert bus?', 'default': 'no',
            'values': ('yes', 'no')},
        {'id': 'units', 'desc': 'Show metric/imperial/both/native units', 'default': 'native',
            'values': ('metric', 'imperial', 'both', 'native')},
    )
    annotations = (
        ('bus-bits', 'Bus data bits'),
        ('bus-bytes', 'Bus data bytes'),
        ('idle', 'Bus idle'),
        ('frame-error', 'Frame errors'),
        ('checksum', 'Message checksum errors'),
        ('bus-decoded','Decoded bus message'),
        ('bus-message','Message bytes'),
    )
    annotation_rows = (
        ('a-bus-bits', 'Bus bits', (0,)),
        ('a-idle', 'Idle', (2,)),
        ('a-bus-warnings', 'Bus warnings', (3,4)),
        ('a-bus-data', 'Bus bytes', (1,)),
        ('a-bus-message', 'Message bytes', (6,)),
        ('a-bus-decoded', 'Message decoded', (5,)),
    )
    #binary = (
    #    ('message', 'CCD dump'),
    #)
    # end of api keys


    # Object initialization
    def __init__(self):
        self.reset()

    # API function called to reset decoder state
    def reset(self):
        self.samplerate = None
        self.samplenum = 0
        self.startsample = -1
        self.state = 'WAIT FOR START BIT'
        self.idle = 'IDLE'
        self.idlestart = -1
        self.busystart = -1
        self.oldbus = 1
        self.ccd_message = []
        self.databit = 0
        self.databyte = 0
        self.framestart = -1
        self.waitfotime = -1
        self.waituart = { 'skip': 1 }
        self.waitidle = { 'skip': 1 }
        self.errors = 0
        self.vin = '_________________'

    # API function called before decoding
    def start(self):
        #self.out_python = self.register(srd.OUTPUT_PYTHON)
        #self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_ann = self.register(srd.OUTPUT_ANN)


    # API metadata (to get samplerate)
    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value
            # The width of one UART bit in number of samples.
            # CCD bus speed is 7812.5 bps, so it can be hardcoded here
            self.bit_width = ceil(float(self.samplerate) / 7812.5)


    # emit annotation in CCD decoded bus row
    def ccd_ann(self,annotation):
        self.put(self.busystart, self.samplenum-1, self.out_ann, [5, annotation ])


    # real stuff here
    def decode_ccd_message(self):

        if self.ccd_message[0] == 0x24:
            # speed
            # 4 bytes total
            # 0x24, AA, BB, checksum
            # AA = speed miles/h
            # BB - spedd km/h
            kmh = str(self.ccd_message[2])
            mph = str(self.ccd_message[1])
            self.ccd_ann(['Speed: '+kmh + ' km/h, '+mph+' mph', kmh + 'km/h'])

        elif self.ccd_message[0] == 0xE4:
            # rpm+map
            # 4 bytes total
            # 0xe4, AA, BB, checksum
            # rpm = AA*32 rpm
            # map = BB*0.41 kPa
            rpm = str(self.ccd_message[1]*32)
            mapsensor = str(round(self.ccd_message[2]*0.41))
            self.ccd_ann(['RPM: '+rpm+' rpm, MAP: '+mapsensor+' kPa', 'RPM='+rpm+',MAP='+mapsensor,'R'+rpm+',M'+mapsensor])

        elif self.ccd_message[0] == 0x6D:
            # VIN
            # 4 bytes total
            # 0x6d, AA, BB, checksum
            # one character at a time, VIN[AA] = BB
            pos = self.ccd_message[1]
            char = chr(self.ccd_message[2])
            self.vin = self.vin[:pos-1]+char+self.vin[pos:]
            self.ccd_ann(['VIN character '+str(pos)+': '+char+', VIN: '+self.vin,'VIN['+str(pos)+']:'+char+',VIN:'+self.vin,'VIN['+str(pos)+']:'+char])

        elif self.ccd_message[0] == 0x86:
            # (un)lock doors, alarm
            # 4 bytes total
            # 0x86, AA, BB, checksum
            #  AA:
            #     0x80 - message from DDM
            #     0x81 - message from PDM
            #  BB:
            # CHECK THIS
            if self.ccd_message[1] == 0x80:
                # message from DDM
                # 0x01 - lock doors
                # 0x04 - unlock doors
                # 0 - lock all doors (DDM switch)
                # 2 - unlock all doors (DDM switch)
                self.ccd_ann(['from DDM: ' + hex(self.ccd_message[2]),'DDM:' + hex(self.ccd_message[2])[2:],'DDM:' + hex(self.ccd_message[2])[2:]])
            elif self.ccd_message[1] == 0x81:
                # message from PDM
                # 0x01 - lock driver door (PDM switch)
                # 0x04 - unlock driver door (PDM switch)
                # 0x49 - lock all doors(RKE1)
                # 0x52 - unlock driver door (RKE1)
                # 0x54 - unlock all doors (RKE1)
                # 0x60 - panic button
                # 0x89 - lock all doors (RKE2)
                # 0x92 - unlock driver door (RKE2)
                # 0x94 - unlock all doors (RKE2)
                # 0 - lock all doors
                # 1 - unlock only driver door
                # 2 - unlock all doors
                # 3 - arm
                # 4 - disarm
                # 5 - panic
                # 6 - RKE1
                # 7 - RKE2
                self.ccd_ann(['from PDM: ' + hex(self.ccd_message[2]),'PDM:' + hex(self.ccd_message[2])[2:],'PDM:' + hex(self.ccd_message[2])[2:]])
            else:
                self.ccd_ann(['unknown message from DDM/PDM: ' + hex(self.ccd_message[1]) + ' ' + hex(self.ccd_message[2]),'UNK DDM/PDM:' + hex(self.ccd_message[1])[2:]+ hex(self.ccd_message[2])[2:], 'UNK DDM/PDM:' + hex(self.ccd_message[1])[2:]+ hex(self.ccd_message[2])[2:]])

        elif self.ccd_message[0] == 0x42:
            # TPS/Cruise
            # not tested
            tps = str(self.ccd_message[1])
            cruise = str(self.ccd_message[2])
            self.ccd_ann(['TPS: ' + tps + ', CRUISE: '+cruise,'TPS:'+tps+',CRUISE:'+cruise])

        elif self.ccd_message[0] == 0x35:
            # Ingition switch status bits
            # 4 bytes total
            # 0x35, AA, BB, checksum
            # bits (unused=0):
            # AA:
            # 0 - interior lamps, 1=switched on
            # 1 - US/METRIC STATUS (0- US, 1 - METRIC)
            # 2 - key in accesory position
            # 4 - key in run position
            # 5 - key in start position
            # BB:
            # 1 - high beam
            # 2 - seat belt (1 - fastened)
            # CHECK THIS
            ign = self.ccd_message[1]
            ignstr = "{0:08b}".format(ign) + ' '+str(self.ccd_message[2])
            self.ccd_ann(['Ignition switch: ' + ignstr, 'IGN:'+ignstr, 'IGN:'+ignstr])

        elif self.ccd_message[0] == 0xA4:
            # Instrumental cluster lamps
            # 4 bytes total
            # 0xa4, AA, BB, checksum
            # AA:
            # 0
            # 1 - throttle applied
            # 2 - brake pressed
            # 3 - A/C compressor clutch (1 - idle, 0 - engaged)
            # 4 - overdrive clutch
            # 5 - OD OFF
            # 6 - not in P or N
            # 7
            # BB: always 0x00?
            lamps = self.ccd_message[1]
            lampsstr = "{0:08b}".format(lamps) + ' '+hex(self.ccd_message[2])
            self.ccd_ann(['Instrumental cluster lamps: ' + lampsstr, 'LAMPS:'+lampsstr])

        elif self.ccd_message[0] == 0x8C:
            # Temperatures (not tested)
            # 4 bytes total
            # 0xa4, AA, BB, checksum
            # AA - engine temperature
            # BB - depending on source it may be: ambient/battery/intake temperature
            #
            engtemp = str(self.ccd_message[1]-128)
            battemp = str(self.ccd_message[2]-128)
            self.ccd_ann(['Engine temperature: '+engtemp+', battery temperature: '+battemp,'EngTemp='+engtemp+',BatTemp='+battemp])

        elif self.ccd_message[0] == 0x84:
            # Increment odometer
            # resoluton 0.000125 mi/bit? ~20cm?
            # 4 bytes total
            # 0x84, AA, BB, checksum
            # increment = 256*AA+BB
            self.ccd_ann(['Increment odometer: '+str(256*self.ccd_message[1]+self.ccd_message[2])])

        elif self.ccd_message[0] == 0x7B:
            # Ambient temperature
            # 3 bytes total
            # 0x7b, AA, checksum
            # temp (F) = AA-70
            ambientf=str(self.ccd_message[1]-70)
            ambientc=str(floor((self.ccd_message[1]-70-32)*5/9))
            self.ccd_ann(['Ambient temperature: '+ambientf+' F ('+ambientc+' C)','AmbTemp:'+ambientf])

        elif self.ccd_message[0] == 0x82:
            # Steering wheel keys for radio
            # 5 bytes total
            # 0x82, AA, BB, CC, checksum
            # in my car always 0x20, 0x00, 0x00
            # AA: always 0x20?
            # BB:
            # 0
            # 1
            # 2 - track up
            # 3 - track down
            # 4
            # 5 - volume up
            # 6 - volume down
            # 7
            # CC:
            # 0 - preset
            # 1
            # 2
            # 3
            # 4
            # 5
            # 6
            # 7
            volume=hex(self.ccd_message[1])[2:]+' '+hex(self.ccd_message[2])[2:]+' '+hex(self.ccd_message[3])[2:]
            self.ccd_ann(['Steering wheel volume buttons: '+volume, 'volume: '+volume])

        elif self.ccd_message[0] == 0x8E:
            # Doors
            # 3 bytes total
            # 0x8e, AA, checksum
            # 0 - Left Front (1=open)
            # 1 - Right Front
            # 2 - Left Rear
            # 3 - Right Rear
            # 4 - Liftgate
            # 5
            # 6
            # 7 - parking brake
            doors = self.ccd_message[1]
            doorsdec = ''
            doorsstr = "{0:08b}".format(doors)
            if doors & 0x01:
                doorsdec += ' LeftFront'
            if doors & 0x02:
                doorsdec += ' RightFront'
            if doors & 0x04:
                doorsdec += ' LeftRear'
            if doors & 0x08:
                doorsdec += ' RightRear'
            if doors & 0x10:
                doorsdec += ' Liftgate'
            if doors & 0xe0:
                doorsdec += ' unknown:'+doorsstr
            self.ccd_ann(['Doors:' + doorsdec])

        elif self.ccd_message[0] == 0xFE:
            # PWM pannel lamp dim
            # 3 bytes total
            # 0xfe, AA, checksum
            # AA - pwm value (0-255)
            pwm = floor(self.ccd_message[1]/2.55)
            self.ccd_ann(['Pannel lamp dim: ' + str(pwm) + '%'])

        elif self.ccd_message[0] == 0xEE:
            # trip distance
            # 5 bytes total
            # 0xee, AA, BB, CC, checksum
            # distance = 0xAABBCC/4971 km (or /8000 miles)
            trip = round(((65536*self.ccd_message[1]+256*self.ccd_message[2]+self.ccd_message[3])*128)/4971,1)
            self.ccd_ann(['Trip distance: ' + str(trip) + ' km'])

        elif self.ccd_message[0] == 0x50:
            # airbag lamp
            # 3 bytes total
            # 0x50, AA, checksum
            # AA=0: OK
            # AA=1: PROBLEM
            if self.ccd_message[1] == 0:
                airbag = 'OFF'
            else:
                airbag = 'PROBLEM'
            self.ccd_ann(['Airbag lamp: ' + airbag])

        elif self.ccd_message[0] == 0x25:
            # fuel level
            # 3 bytes total
            # 0x25, AA, checksum
            # fuel level = AA (0-255 scale)
            pwm = floor(self.ccd_message[1]/2.55)
            self.ccd_ann(['Fuel level: ' + str(pwm) + '%'])

        elif self.ccd_message[0] == 0x0c:
            # voltage+temperatures+oil pressure
            # 6 bytes total
            # 0x0c, AA, BB, CC, DD, checksum
            # - battery voltage = AA/8 V
            # - oil pressure = BB/2 psi or BB*3.4473785 kPa (center of gauge is about 300kPa/40psi)
            # - engine temperature = CC - 64 C
            # - battery temperature = DD -64 C
            voltage = str(self.ccd_message[1]/8)
            oil = str(round(self.ccd_message[2]*3.4473785))
            engtemp = str(self.ccd_message[3]-64)
            battemp = str(self.ccd_message[4]-64)
            self.ccd_ann(['Engine temperature: '+engtemp+' C, battery temperature: '+battemp+' C, battery voltage: '+voltage +' V, oil pressure: '+oil+' kPa','EngTemp='+engtemp+',BatTemp='+battemp])

        elif self.ccd_message[0] == 0xda:
            # Instrument Cluster Lamp States (CHECK: new bits)
            # 3 bytes total
            # 0xda, AA, checksum
            # AA:
            # 0 - always 0?
            # 1 - always 0? (trip odo reset state: 1-yes)
            # 2 - always 0? (trip odo reset switch: 1 closed)
            # 3 - always 0? (beep request)
            # 4 - always 0? (cluster alarm)
            # 5 - always 1? (cluster type: 1-metric, 0-us)
            # 6 - check engine lamp MIL (1=lit)
            # 7 - always 0?
            if self.ccd_message[1] & 0x40 == 0:
                mil = 'OFF'
            else:
                mil = 'PROBLEM'
            self.ccd_ann(['Check engine lamp: ' + mil + ' ('+hex(self.ccd_message[1]) + ')'])

        elif self.ccd_message[0] == 0xCE:
            # Odometer
            # 6 bytes total
            # 0xce, AA, BB, CC, DD, checksum
            # odo = 0xAABBCCDD / 4971 km (or /8000 miles)
            odo = (16777216*self.ccd_message[1]+65536*self.ccd_message[2]+256*self.ccd_message[3]+self.ccd_message[4])//4971
            self.ccd_ann(['Odo: ' + str(odo) + ' km'])

        elif self.ccd_message[0] == 0x62:
            # electric doors/mirrors
            # 4 bytes total
            # 0x62, AA, BB, checksum
            # AA: electric windows control (idle state: 1)
            # 0 - left front down
            # 1 - left front up
            # 2 - right front down
            # 3 - right front up
            # 4 - left rear down
            # 5 - left rear up
            # 6 - right rear down
            # 7 - right rear up
            # bits 7-2 = 0 if windows lock activated
            # BB: electric mirrors (idle state = 0)
            # 0 - driver mirror up
            # 1 - driver mirror down
            # 2 - driver mirror left
            # 3 - driver mirror right
            # 4 - passenger mirror up
            # 5 - passenger mirror down
            # 6 - passenger mirror left
            # 7 - passenger mirror right

            # windows
            dmess=''
            if self.ccd_message[1] & 0x01 == 0:
                dmess += " LF_up"
            if self.ccd_message[1] & 0x02 == 0:
                dmess += " LF_down"
            if self.ccd_message[1] & 0xfc == 0:
                dmess += " windows locked"
            else:
                if self.ccd_message[1] & 0x04 == 0:
                    dmess += " RF_down"
                if self.ccd_message[1] & 0x08 == 0:
                    dmess += " RF_up"
                if self.ccd_message[1] & 0x10 == 0:
                    dmess += " LR_down"
                if self.ccd_message[1] & 0x20 == 0:
                    dmess += " LR_up"
                if self.ccd_message[1] & 0x40 == 0:
                    dmess += " RR_down"
                if self.ccd_message[1] & 0x80 == 0:
                    dmess += " RR_up"
            #mirrors
            mmess=''
            if self.ccd_message[2] & 0x01:
                mmess += " Driver_up"
            if self.ccd_message[2] & 0x02:
                mmess += " Driver_down"
            if self.ccd_message[2] & 0x04:
                mmess += " Driver_left"
            if self.ccd_message[2] & 0x08:
                mmess += " Driver_right"
            if self.ccd_message[2] & 0x10:
                mmess += " Pass_up"
            if self.ccd_message[2] & 0x20:
                mmess += " Pass_down"
            if self.ccd_message[2] & 0x40:
                mmess += " Pass_left"
            if self.ccd_message[2] & 0x80:
                mmess += " Pass_right"

            self.ccd_ann(['Windows:'+dmess+', mirrors:'+mmess])

        # 0x23 (35)  - 1 param, 4 bytes, COUNTRY CODE
        #  0x00: USA
        #   AA/BB??
        #    0x01: GULF COAST
        #    0x02: EUROPE
        #    0x03: JAPAN
        #    0x04: MALAYSIA
        #    0x05: INDONESIA
        #    0x06: AUSTRALIA
        #    0x07: ENGLAND
        #    0x08: VENEZUELA
        #    0x09: CANADA
        #    0x0A: UNKNOWN
        # 0x3A (58)  - 1 param, 4 bytes, INSTRUMENT CLUSTER LAMP STATES (AIRBAG)
        #  AA: 0x22, BB: 0
        #  0x10 - AIRBAG (0 - single, 1 - dual)
        #  0x80 - MIC SEATBELT LAMP ST (1 - on, 0 - off)
        #  0x02 - MIC DRIVER STATE/MIC BULB STATE (1 - failed, 0 - ok)
        #  0x01 - MIC DRIVER STATE/MIC BULB STATE (1 - failed, 0 - ok)
        #  0x08 - ACM LAMP STATE (1- on, 0 - off) (ACM=Airbag Control Module)
        #  0x22 - ??
        # 0x44 (68)  - 1 param, 4 bytes, FUEL USED
        # 0x64 (100) - ? param, 5 bytes, ???? TRANS, 0-?, 1-PARK_NEUTRAL,2-?,15-?,23-? (CHECK)
        #  AA:
        #   0xe2 - P or N
        #   0xe0 - R, D, 1, 2
        #  BB: 0xff
        #  CC: 0x01
        # 0x66 - 3 bytes ????
        #  0x08 - VIC service reset?
        #  0x10 - almost everytime
        # 0x7E (126) - 1 param, 3 bytes, HVAC, 0 - AC_REQUEST
        #  0x00 - compressor off
        #  0x01 - engage A/C compressor
        # 0x9A - 3 bytes (overhead console)
        #  bits:
        #   0-3 - display screen showing:
        #         000 0 - ODO, odo since reset
        #         001 1 - AVG ECO, avg fuel economy
        #         010 2 - ECO, current fuel economy
        #         011 3 - DTE, distance to empty
        #         100 4 - ET, estimated/empty time
        #         101 5 - temperature/compass
        #         111 7 - blank
        #   4 - 0
        #   5 - US/M switch pressed
        #   6 - RESET pressed (US/M+STEP)
        #   7 - metric=1/imperial=0 last status
        #
        #  0xa5 10100101 - switch to imperial
        #  0x25 00100101 - switch to metric
        #  0x80 10000000- show odo since reset
        #  0x81 10000001 - show avg fuel economy
        #  0x82 10000010 - show current fuel economy
        #  0x83 10000011 - show dte (distance to empty)
        #  0x84 10000100 - show et (estimated time)
        #  0x85 10000101 - show temp/compass
        #  0x87 10000111 - switch off
        #  0xC0 11000000 - reset
        #  0x20 00100000 - ??? to metric
        #  0xA0 10100000 - ??? to imperial
        #  0x01 00000001 - ???
        #  0x03 00000011 - ???
        #  0x07 00000111 - ???
        #  0x05 00000101 - ???
        #  0x00 00000000 - ???
        #  0x21 00100001 - ??? - to metric
        #  0xC1 11000001 - ??? - reset?

        # 0x9D (157) - ? param, 4 bytes, ???? (CHECK)
        #  0x80
        #   0x01 - memory 1
        #   0x02 - memory 2
        #   0x11 - set memory 1
        #   0x12 - set memory 2
        #  0x81 - ?
        # 0xAC (172) - 1 param, 4 bytes, Body type broadcast AN/DN, VEHICLE INFORMATION
        #  ? ac 08 a2
        # 0xB1 (177) - 1 param, 3 bytes, WARNING
        #  0x01 - KEY IN IGN WARN (1 - on, 0 - off)
        #  0x02 - SEAT BELT WARNING (1 - on, 0 - off)
        #  0x04 - EXTERIOR LAMP WARN (1 - on, 0 - off)
        #  0x10 - OVERSPEED WARNING (1 - on, 0 - off)
        # 0xCC (204) - ? param, 4 bytes, ACCUMULATED MILEAGE, counting sometimes down (CHECK)
        #  AA*256+BB (units?)
        # 0xEC (236) - 1 param, 4 bytes, VEHICLE INFORMATION
        #  AA: 0x00
        #  BB:
        #   Bit 7: N/A (always 1?)
        #   Bit 6: IAT-sensor limp-in (Intake Air Sensor)
        #   Bit 5: N/A (always 1?)
        #   Bit 4:2 Fuel type:
        #    000: CNG
        #    001: NO LEAD
        #    010: LEADED FUEL
        #    011: FLEX FUEL
        #    100: DIESEL
        #   Bit1: N/A (always 1?)
        #   Bit 0: ECT-sensor limp in (Coolant Temperature Sensor)
        # 0xF1 (241) - 1 param, 3 bytes, WARNING
        #  0x10 - BRAKE PRESS WARNING (parking brake lamp lit on instrumental cluster)
        #  0x08 - CRITICAL TEMP WARNING
        #  0x04 - HI TEMP WARNING
        #  0x01 - LOW FUEL WARNING (<13%)
        #  0x?? - LOW OIL WARNING
        # 0xF2 (242) - 1 param, 6 bytes, RESPONSE to B2 message
        #  0xF2 81 00 00 00 73 - sent after 110ms from PDM boot
        #  0xF2 80 00 00 00 72 - sent after 40ms from DDM boot
        # 0xF5 (245) - ? param, 4 bytes, ENGINE LAMP CTRL
        #  AA:
        #   0x00: OFF
        #   0x01: ON
        #   0x02: WASH ON
        #   0x03: ON/BLINK
        #  BB: 0x00
        else:
            # not decoded yet
            message=''
            for byte in self.ccd_message:
                message += hex(byte)[2:] + ' '
            self.ccd_ann(['Unknown CCD message: '+message, 'CCD: '+message , message])

        # log whole message
        message=''
        for byte in self.ccd_message:
            message += hex(byte)[2:] + ' '
        self.put(self.busystart, self.samplenum-1, self.out_ann, [6, [message] ])



    # Main decode function called by API
    def decode(self):
        # check if samplerate is set
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')

        # iterate through sample data (samplenum, channels)
        while True:

            pins = self.wait([{0: 'e'}, self.waituart, self.waitidle])
            bus = int.from_bytes(pins, byteorder='little')

            # invert input if requested
            if self.options['invert_bus'] == 'yes':
                bus = 1-bus

            # IDLE handling
            if self.oldbus != bus:
                # bus changed
                if self.idle == 'BUSY':
                    # for busy only update idlestart
                    self.idlestart = self.samplenum
                else:
                    # for idle: emit annotation and change state
                    self.put(self.idlestart, self.samplenum-1, self.out_ann, [2, ['Idle', 'Id', 'I']])
                    self.idle = 'BUSY'
                    self.idlestart = self.samplenum
                    self.busystart = self.samplenum
                # after every change wait for 10 bits to change from BUSY to IDLE
                self.waitidle = { 'skip': self.bit_width*10 }
            else:
                # bus not changed
                if self.idle == 'BUSY' and bus and self.samplenum-self.idlestart > self.bit_width*10:
                    # idle for more than 10 bits - change state
                    self.put(self.busystart, self.samplenum-1, self.out_ann, [2, ['Busy', 'Bsy', 'B']])
                    self.idle = 'IDLE'
                    self.idlestart = self.samplenum

                    # BUSY ended, time to decode collected bytes

                    # check message checksum
                    chksum = 0
                    for byte in self.ccd_message:
                        chksum += byte
                    chksum -= byte
                    chksum = chksum % 256;
                    if byte != chksum:
                        # emit annotation for bad checksum
                        self.put(self.busystart, self.samplenum-1, self.out_ann, [4, ['Checksum error', 'Bad sum', 'CHK']])
                        self.errors += 1

                    if self.errors == 0 or self.options['ignoreerrors'] == 'yes':
                        # process frame only if no errors
                        self.decode_ccd_message()

                    # clear at the end
                    self.ccd_message = []
                    self.errors = 0

                    # on IDLE wait for next edge
                    self.waitidle = { 0: 'e' }
                elif self.idle == 'IDLE':
                    # nothing changed, still idle, so wait for nearest edge
                    self.waitidle = { 0: 'e' }
                else:
                    # not the end of idle, wait to end of idle time
                    dif=self.bit_width*10-(self.samplenum-self.idlestart)
                    if dif < 1:
                        dif = 1
                    self.waitidle = { 'skip': dif }


            # serial protocol handling
            if self.state == 'WAIT FOR START BIT':
                if self.oldbus and not bus:
                    # initialize byte
                    self.databit = 0
                    self.databyte = 0
                    self.framestart = self.samplenum
                    # set next sampling point
                    self.waitfortime = self.samplenum+ceil(1.5*self.bit_width)
                    self.waituart = { 'skip': ceil(1.5*self.bit_width) }
                    # change state
                    self.state = 'GET DATA BITS'
                    # emit annotation
                    self.put(self.samplenum, self.samplenum+self.bit_width-1, self.out_ann, [0, ['Start bit', 'Start', 'S']])
                else:
                    self.waituart = { 0: 'e' }

            elif self.state == 'GET DATA BITS':
                if self.samplenum >= self.waitfortime:
                    # calculate new byte value
                    self.databyte = self.databyte // 2
                    if bus:
                        self.databyte += 128
                    # annotation: bit
                    self.put(self.samplenum-ceil(self.bit_width/2), self.samplenum+floor(self.bit_width/2)-1, self.out_ann, [0, [str(bus), str(bus), str(bus)]])
                    # set next sample point
                    self.waitfortime = self.samplenum+self.bit_width
                    self.waituart = { 'skip': self.bit_width }
                    # next bit
                    self.databit += 1
                    if self.databit == 8:
                        # full byte acquired, append it to ccd message
                        self.ccd_message.append(self.databyte)
                        # next state
                        self.state = 'GET STOP BIT'
                        # emit byte to annotation
                        self.put(self.framestart+self.bit_width, self.samplenum+floor(self.bit_width/2), self.out_ann, [1, [hex(self.databyte), hex(self.databyte)[2:], hex(self.databyte)[2:] ]])
                else:
                    self.waituart = { 'skip': self.waitfortime-self.samplenum }
            elif self.state == 'GET STOP BIT':
                if self.samplenum >= self.waitfortime:
                    if not bus:
                        # frame error
                        self.errors += 1
                        self.put(self.framestart, self.samplenum+floor(self.bit_width/2), self.out_ann, [3, ['Frame error', 'Fr.ERR', 'FE' ]])
                    # emit annotation for stop bit
                    self.put(self.samplenum-ceil(self.bit_width/2), self.samplenum+floor(self.bit_width/2)-1, self.out_ann, [0, ['Stop bit', 'Stop', 'E']])
                    # change next state
                    self.state = 'WAIT FOR START BIT'
                    # extend idle (a must for bytes like 0xff)
                    self.idlestart = self.samplenum
                    self.waituart = { 0: 'e' }
                else:
                    self.waituart = { 'skip': self.waitfortime-self.samplenum }

            # update old status
            self.oldbus = bus
