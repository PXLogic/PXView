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

import sigrokdecode as srd
import binascii
import struct
import traceback
from .handlers import *

# ...
RX = 0
TX = 1

# these reflect the implicit IDs for the 'annotations' variable defined below.
# if you renumber 'annotations', you'll have to change these to match.
ANN_MESSAGE = 0
ANN_ERROR = 1
ANN_BYTES = 2


class Decoder(srd.Decoder):
    api_version = 3
    id = 'boost'
    name = 'Boost'
    longname = 'LEGO Boost'
    desc = 'LEGO Boost Hub and Peripherals.'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = ['boost']
    tags = ['Embedded/industrial']
    options = (
        {'id': 'show_errors', 'desc': 'Show errors?', 'default': 'no', 'values': ('yes', 'no')},
        {'id': 'show_bytes', 'desc': 'Show message bytes?', 'default': 'no', 'values': ('yes', 'no')}
    )

    annotations = (
        ('message', 'Valid messages that pass checksum'),
        ('error', 'Invalid/malformed messages'),
        ('byte', 'Each individual byte'),
    )
    annotation_rows = (
        ('messages', 'Messages', (ANN_MESSAGE,) ),
        ('errors', 'Errors', (ANN_ERROR,) ),
        ('bytes', 'Bytes', (ANN_BYTES,) )
    )


    def __init__(self):
        self.reset()


    def reset(self):
        self.message = [[], []]
        self.ss_block = [None, None]
        self.es_block = [None, None]


    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)


    def putx(self, rxtx, data, ss=None, es=None):
        if(not ss):
            ss = self.ss_block[rxtx]
        if(not es):
            es = self.es_block[rxtx]
        if(data[0] == ANN_ERROR and self.options['show_errors'] == 'no'):
            return
        if(data[0] == ANN_BYTES and self.options['show_bytes'] == 'no'):
            return
        self.put(ss, es, self.out_ann, data)


    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data

        # For now, ignore all UART packets except the actual data packets.
        if ptype != 'DATA':
            return

        # We're only interested in the byte value (not individual bits).
        pdata = pdata[0]

        # If this is the start of a command/reply, remember the Start Sample number.
        if self.message[rxtx] == []:
            self.ss_block[rxtx] = ss

        # Append a new byte to the currently built/parsed command.
        self.message[rxtx].append(pdata)

        self.putx(rxtx, [ANN_BYTES, ['{:02X}'.format(pdata)]], ss=ss, es=es)

        # note our current End Sample number.
        self.es_block[rxtx] = es
        # self.putx(rxtx, [0, [str(binascii.hexlify(bytes(self.message[rxtx])))]])
        
        # will return something if message is complete, False or None if we're waiting on more bytes
        if(self.handle_message(rxtx, self.message[rxtx])):
            self.message[rxtx] = []


    # attempt to find a handler function for this message
    def handle_message(self, rxtx, msg):
        # if we have a handler available for this message type, use it
        try:
            funcname = 'handle_message_{:02X}'.format(msg[0])
            # func = getattr(handlers, funcname)
            func = globals()[funcname]
            # print(funcname, func)
            res = func(msg)
            # print(res)
            if(res):
                self.putx(rxtx, res)
                return True
            return False
        except Exception as e:
            pass
        # no handler found.
        # we don't want to handle this message again, so return True to consume it
        return True

