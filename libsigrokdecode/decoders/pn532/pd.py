##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2018 Stephan Thiele <stephan.thiele@mailbox.org>

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
from collections import deque, namedtuple
from .list import *

PREAMBLE=[0x00]
START_PACKET=[0x00,0xFF]
POSTAMBLE=[0x00]
START_FRAME = PREAMBLE + START_PACKET
END_FRAME = POSTAMBLE


'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <pdata>]

<ptype>:
- 'DATA': <pdata> contain the byte and ss, es data.

'''



class State:
    START_FRAME       = "START_FRAME"
    LENGTH            = "LENGHT"
    TFI               = "TFI"
    DATA              = "DATA"
    CHECKSUM          = "CHECKSUM"
    END_FRAME         = "END_FRAME"


class FrameType:
    HOST_TO_PN532   = 0
    PN532_TO_HOST   = 1
    ACK             = 2
    NACK            = 3
    ERROR           = 4


frame2string = (
    ['Host to PN532','H2C'],
    ['PN532 to Host','C2H'],
    ['Acknoledge', 'ACK'],
    ['Not Acknoledge', 'NACK'],
    ['Application Error', 'Error']
)
ByteData = namedtuple('ByteData', 'ss es data')


class Decoder(srd.Decoder):
    api_version = 3
    id = 'pn532'
    name = 'PN532'
    longname = 'PN532 nfc transceiver'
    desc = 'PN532 chip command decoder'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = ['ISO14443']
    tags = ['Automotive']
    options = (
        {'id': 'preamble', 'desc': 'Preamble byte', 'default': 0x00},
        {'id': 'postamble', 'desc': 'Postamble byte', 'default': 0x00},
        {'id': 'start frame', 'desc': 'Postamble byte', 'default': 0x00},
        {'id': 'format', 'desc': 'Data format', 'default': 'hex', 'values': ('ascii', 'dec', 'hex', 'oct', 'bin')},
    )
    annotations = (
        ('start', ' Start frame'),          #0
        ('len', 'Data lenght'),             #1
        ('lcs', 'Data lenght checksum'),    #2
        ('tfi', 'Frame identifier'),        #3
        ('data', 'Packet data'),            #4
        ('dcs', 'Data checksum'),           #5
        ('end', 'Postable'),                #6
        ('error', 'Error description'),     #7
        ('frame', 'frame type'),            #8
        ('cmd', 'Command'),                 #9
        ('preamble', 'Preamble'),            #10
        ('instruction', 'Instruction')      #11
    )
    annotation_rows = (
        ('data_vals', 'Data', (0, 1, 2, 3, 4, 5, 6, 10, 11)),
        ('frame_type', 'Frame type',(8, )),
        ('commands', 'Commands',(9, )),
        ('errors', 'Errors', (7,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.start_frame = deque(maxlen=len(START_FRAME))
        self.data_lenght = []
        self.data_packet = []
        self.frame_type = None
        self.data_size = None
        self.tfi = None
        self.start_byte = None
        self.state = State.START_FRAME

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)

    def put_at(self, ss, es, data):
        self.put(ss, es, self.out_ann, data)

    def put_at_byte(self, byte, data):
        self.put(byte.ss, byte.es, self.out_ann, data)

    def put_at_bytes(self, bytes, data):
        self.put(bytes[0].ss, bytes[-1].es, self.out_ann, data)

    def format_value(self, v):
        # Format value 'v' according to configured options.
        # Reflects the user selected kind of representation, as well as
        # the number of data bits in the UART frames.

        fmt = self.options['format']

        # Assume "is printable" for values from 32 to including 126,
        # below 32 is "control" and thus not printable, above 127 is
        # "not ASCII" in its strict sense, 127 (DEL) is not printable,
        # fall back to hex representation for non-printables.
        if fmt == 'ascii':
            if v in range(32, 126 + 1):
                return chr(v)
            return "[{:02X}]".format(v)

        # Mere number to text conversion without prefix and padding
        # for the "decimal" output format.
        if fmt == 'dec':
            return "{:d}".format(v)

        # Padding with leading zeroes for hex/oct/bin formats, but
        # without a prefix for density -- since the format is user
        # specified, there is no ambiguity.
        if fmt == 'hex':
            digits = 2
            fmtchar = "X"
        elif fmt == 'oct':
            digits = 3
            fmtchar = "o"
        elif fmt == 'bin':
            digits = 8
            fmtchar = "b"
        else:
            fmtchar = None
        if fmtchar is not None:
            fmt = "{{:0{:d}{:s}}}".format(digits, fmtchar)
            return fmt.format(v)

        return None


    def checksum(self, bytes, checksum):
        packet = map(lambda x: x.data, bytes)
        return (sum(packet) + checksum) & 0xFF == 0

    def change_state(self, new_state):
        self.state = new_state

    def handle_command_default(self, ss, es):
        if self.tfi == 0xD4:
            cmd = self.data_packet[0].data
            command = miscellaneous.get(cmd, None) or rf_communication.get(cmd, None) or initiator.get(cmd, None) or target.get(cmd, None)
            if command:
                self.put_at(ss, es, [9, command])
        elif self.tfi == 0xD5:
            pass
        else:
            err = errors.get(self.tfi, None)



    def handle_start_frame(self, byte):
        # memorize only the size of the START_FRAME (defautl to 3 bytes) until a start frame arrives
        self.start_frame.append(byte)

        # Check the pattern "START_FRAME" is present
        if [x.data for x in self.start_frame] == START_FRAME:
            self.start_byte = self.start_frame[0]
            self.put_at_byte(self.start_frame.popleft(), [10, ['Preamble', 'PR']])
            self.put_at_bytes(self.start_frame, [0, ['Start Frame', 'Start', 'S']])
            self.change_state(State.LENGTH)

    def handle_lenght(self, byte):
        self.data_lenght.append(byte)

        if len(self.data_lenght) < 2:   #TODO
            return

        sequence = [x.data for x in self.data_lenght]

        if sequence == [0x00, 0xFF]:            #Check if frame is an "acknoledge"
            self.frame_type = FrameType.ACK
            self.change_state(State.END_FRAME)
        elif sequence == [0xFF, 0x00]:          #Check if frame is "not acknoledge"
            self.frame_type = FrameType.NACK
            self.change_state(State.END_FRAME)
        elif sequence == [0xFF, 0xFF]:          #Check if frame is "extended"
            pass
        else:
            self.data_size = self.data_lenght[0].data - 1
            self.put_at_byte(self.data_lenght[0], [1, [f'Data Lenght: {self.format_value(self.data_size)}', f'Lenght: {self.format_value(self.data_size)}', 'LEN']])
            if self.checksum(self.data_lenght, 0x00):
                self.put_at_byte(self.data_lenght[1], [2, ['Data Lenght Checksum: OK', 'Checksum: OK', 'LCS']])
                self.change_state(State.TFI)
            else:
                self.put_at_byte(self.data_lenght[1], [7, ['Checksum Error', 'Error', 'E']])
                self.change_state(State.END_FRAME)

    def handle_tfi(self, byte):
        if byte.data == 0xD4:
            self.frame_type = FrameType.HOST_TO_PN532
            self.change_state(State.DATA)
        elif byte.data == 0xD5:
            self.frame_type = FrameType.PN532_TO_HOST
            self.change_state(State.DATA)
        elif byte.data == 0x7F:
            self.frame_type = FrameType.ERROR
            self.change_state(State.CHECKSUM)
        else:
            self.frame_type = FrameType.ERROR

        self.tfi = byte.data
        self.put_at_byte(byte, [3, [f'Frame identifier: {self.format_value(byte.data)}', f'TFI: {self.format_value(byte.data)}', 'TFI']])


    def handle_data(self, byte):
        self.data_packet.append(byte)

        if len(self.data_packet) < self.data_size:
            return

        #TODO decode data packet

        data_packet = [self.format_value(x.data) for x in self.data_packet]
        self.put_at_byte(self.data_packet[0], [4, [f"Command: {data_packet[0]}", f"{data_packet[0]}"]])
        if len(data_packet) > 1:
            self.put_at_bytes(self.data_packet[1:], [4, [f"Data: {' '.join(data_packet[1:])}", f"{' '.join(data_packet[1:])}"]])
        self.change_state(State.CHECKSUM)

    def handle_checksum(self, byte):
        if self.checksum(self.data_packet, byte.data + self.tfi):
            self.put_at_byte(byte, [5, ['Data Checksum: OK', 'DCS']])
        else:
            self.put_at_byte(byte, [5, ['Data Checksum', 'DCS']])
            self.put_at_byte(byte, [7, ['Checksum Error', 'Error', 'E']])

        self.change_state(State.END_FRAME)

    def handle_end_frame(self, byte):
        self.put_at(self.start_byte.ss, byte.es, [8, frame2string[self.frame_type]])

        self.put_at_byte(byte,[6,['Postamble', 'PO']])
        if self.frame_type not in [FrameType.ACK, FrameType.NACK, FrameType.ERROR]:
            handler = getattr(self, 'handle_command_%s' % self.data_packet[0].data, self.handle_command_default)
            handler(self.start_byte.ss, byte.es)

        self.start_frame.clear()
        self.data_lenght = []
        self.data_packet = []
        self.frame_type = None
        self.data_size = None
        self.tfi = None
        self.start_byte = None

        self.change_state(State.START_FRAME)




    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data

        # Ignore all UART packets except the actual data packets
        if ptype != 'DATA':
            return


        handler = getattr(self, 'handle_%s' % self.state.lower())
        handler(ByteData(ss, es, pdata[0]))