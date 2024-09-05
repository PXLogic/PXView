##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2012-2013 Uwe Hermann <uwe@hermann-uwe.de>
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

import binascii
import struct
import traceback
import sys

# these reflect the implicit IDs for the 'annotations' variable defined in the Decoder in pd.py
ANN_MESSAGE = 0
ANN_ERROR = 1
ANN_BYTES = 2

LEGO_COLORS = {0:'Black', 1:'Pink', 2:'Purple', 3:'Blue', 4:'LightBlue', 5:'Cyan', 6:'Green', 7:'Yellow', 8:'Orange', 9:'Red', 10:'White'} # 255:'None'
LEGO_SENSOR_MODES = {0:'ColorOnly', 1:'CoarseDist', 4:'FineDist', 6:'RGB', 8:'Color+Dist', 9:'Luminosity'}

def legoColor(colorCode):
    if(colorCode in LEGO_COLORS):
        return LEGO_COLORS[colorCode]
    return 'None'

def legoCDSensorMode(modeCode):
    if(modeCode in LEGO_SENSOR_MODES):
        return LEGO_SENSOR_MODES[modeCode]
    return 'Unk {:02X}'.format(modeCode)

def valid_checksum(msg):
    b = 0xFF
    for c in bytearray(msg):
        b ^= c
    return not b


# decorator that allows us to specify an expected message length, so we don't have to explicitly check in every message handler
def expectedLength(expected_len):
    def new_funcEL(func):
        def wrapper(*args, **kwargs):
            msg = args[0]
            if(len(msg) < expected_len):
                return False
            return func(*args, **kwargs)
        return wrapper
    return new_funcEL


# decorator that lets us check a message for a valid checksum before handling it
def validateChecksum(func):
    def new_funcVC(*args, **kwargs):
        msg = args[0]
        if(not valid_checksum(msg)):
            return failed_checksum(msg, name=func.__name__)
        return func(*args, **kwargs)
    return new_funcVC


def failed_checksum(msg, name=None):
    if(name == None):
        name = sys._getframe(1).f_code.co_name
    messageStr = str(binascii.hexlify(bytes(msg)))
    return [ANN_ERROR, ['Failed checksum: {}({})'.format(name, messageStr)]]


# Color/Distance sensor -- possibly sensor mode indicator?
@expectedLength(3)
@validateChecksum
def handle_message_46(msg):
    sensorMode = legoCDSensorMode(msg[1])
    outMessage = ['C/D Sensor: Mode={} ({:02X})'.format(sensorMode, msg[1]),
                  'SensorMode={}'.format(sensorMode),
                  sensorMode]
    return [ANN_MESSAGE, outMessage]



# Message sent from the Hub to the Motor every ~100ms prompting the motor to send its current status
# def handle_message_02(self, rxtx, msg):
#     expected_len = 1
#     return [ANN_MESSAGE, ['Status Request', 'St Req', 'SR']]


# CD sensor mode 01 response
# C1 0A 34
@expectedLength(3)
@validateChecksum
def handle_message_C1(msg):
    outMessage = ['Distance: {} inches'.format(msg[1]),
                  'Dist: {} in'.format(msg[1]),
                  '{}in'.format(msg[1])]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 02 response
# D2 00 00 00 00 2D
@expectedLength(6)
@validateChecksum
def handle_message_D2(msg):
    outMessage = [str(binascii.hexlify(bytes(msg)))]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 03 response -- distance >= 1 inch
# C3 00 3C
@expectedLength(3)
@validateChecksum
def handle_message_C3(msg):
    outMessage = ['CD Mode 03: 1/{} inch?'.format(msg[1])]
    return [ANN_MESSAGE, outMessage]
 
# CD sensor mode 04 response -- distance < 1 inch
# C4 00 3B
@expectedLength(3)
@validateChecksum
def handle_message_C4(msg):
    outMessage = ['CD Mode 04: 1/{} inch?'.format(msg[1])]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 05 response
# C5 FF 5C
# C5 03 39 -- sketch ended?
@expectedLength(3)
@validateChecksum
def handle_message_C5(msg):
    outMessage = ['CD Mode 05: {:02X}'.format(msg[1])]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 07 response
# CF 00 00 30
@expectedLength(3)
@validateChecksum
def handle_message_CF(msg):
    outMessage = ['CD Mode 07: {:02X} {:02X}'.format(msg[1]. msg[2])]
    return [ANN_MESSAGE, outMessage]

 
# CD sensor mode 0A response
# E2 74 04 3D 04 3E 04 40 04 6D 04 A8 04 B6 04 
@expectedLength(15)
@validateChecksum
def handle_message_E2(msg):
    outMessage = ['CD Mode 0A: ' + str(binascii.hexlify(bytes(msg)))]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 09
# Color/Distance sensor -- luminosity measurement mode
@expectedLength(6)
@validateChecksum
def handle_message_D1(msg):
    outMessage = ['CD Mode D1: ' + str(binascii.hexlify(bytes(msg)))]
    lum = struct.unpack('<H', bytearray(msg[1:3]))[0]
    outMessage = ['Luminosity={}'.format(lum),
                  'Lum={}'.format(lum)]
    return [ANN_MESSAGE, outMessage]


# CD sensor mode 06
# Color/Distance sensor -- R/G/B values
@expectedLength(10)
@validateChecksum
def handle_message_DE(msg):
    red, green, blue, unk = struct.unpack('<HHHH', bytearray(msg[1:9]))

    outMessage = ['Red={} Green={} Blue={}'.format(red,green,blue),
                  'R={} G={} B={}'.format(red,green,blue),
                  '#{:02X}{:02X}{:02X}'.format(red,green,blue)]
    return [ANN_MESSAGE, outMessage]


# Color/Distance sensor -- sensor mode change order
@expectedLength(4)
@validateChecksum
def handle_message_43(msg):
    sensorMode = legoCDSensorMode(msg[1])
    outMessage = ['C/D Sensor Mode Change: Mode={} ({:02X})'.format(sensorMode, msg[1]),
                  'SensorModeChange={}'.format(sensorMode),
                  sensorMode]

    return [ANN_MESSAGE, outMessage]


# Motor initialization marker?
# The Hub sends this message to the Motor, then the Motor echoes it back to the Hub
# 0x54 22 00 10 20 B9
@expectedLength(6)
def handle_message_54(msg):
    expected_msg = b'\x54\x22\x00\x10\x20\xB9'
    if(msg != expected_msg):
        return [ANN_ERROR, ['Malformed Motor Initialization message']]
    return [ANN_MESSAGE, ['Motor Initialization', 'Motor Init', 'MI']]


@expectedLength(3)
@validateChecksum
def handle_message_C0(msg):
    color = legoColor(msg[1])
    outMessage = ['Color/Distance Sensor: Color={}'.format(color),
                  'C/D Sensor: Color={}'.format(color),
                  'Color={}'.format(color),
                  color]
    return [ANN_MESSAGE, outMessage]


@expectedLength(6)
@validateChecksum
def handle_message_D0(msg):
    color = legoColor(msg[1])
    coarseDist = msg[2]
    status = msg[3]
    fineDist = msg[4]
    outMessage = ['Color/Distance Sensor: Status={:02X} CoarseDist={:<3d} FineDist={:<3d} Color={}'.format(status, coarseDist, fineDist, color),
                  'C/D Sensor: Status={:02X} CDist={:<3d} FDist={:<3d} Color={}'.format(status, coarseDist, fineDist, color),
                  'C/D: Stat={:02X} cD={} fD={} C={}'.format(status, coarseDist, fineDist, color),
                  'c={} f={} C={}'.format(coarseDist, fineDist, color)]
    return [ANN_MESSAGE, outMessage]


# Message from the Motor to the Hub giving its current status.  Sent either in response to a Status Request message, or self-initiated whenever the motor's status (speed, angle) have changed.
@expectedLength(10)
@validateChecksum
def handle_message_D8(msg):
    speed, angleDist = struct.unpack('<bi', bytearray(msg[1:6]))
    angle = angleDist % 360
    outMessage = ['Motor Status: Speed={:<3d} Angle={:03d} AngleDist={}'.format(speed, angle, angleDist),
                  'Motor: Speed={:<3d} Angle={:03d}'.format(speed, angle),
                  'Speed={} Angle={}'.format(speed, angle),
                  '{},{}'.format(speed, angle)]
    return [ANN_MESSAGE, outMessage]

