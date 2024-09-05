# Copyright (c) 2021 National Motor Freight Traffic Association Inc.
# Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
import struct
from functools import reduce
from typing import Union, Any

import sigrokdecode as srd

J1708_BAUD = 9600
MIN_BUS_ACCESS_BIT_TIMES = 12


class SimpleUartFsm:
    class State:
        WaitForStartBit = 'WAIT_FOR_STARTBIT'
        WaitForData     = 'WAIT_FOR_DATA'
        WaitForStopBit  = 'WAIT_FOR_STOPBIT'
        Error           = 'ERROR'

    def transit(self, target_state):
        if not self._transition_allowed(target_state):
            return False
        self.state = target_state
        return True

    def _transition_allowed(self, target_state):
        if target_state == SimpleUartFsm.State.Error:
            return True
        return target_state in self.allowed_state[self.state]

    def reset(self):
        self.state = SimpleUartFsm.State.WaitForStartBit

    def __init__(self):
        a = dict()
        a[SimpleUartFsm.State.WaitForStartBit] = (SimpleUartFsm.State.WaitForData,)
        a[SimpleUartFsm.State.WaitForData]     = (SimpleUartFsm.State.WaitForStopBit,)
        a[SimpleUartFsm.State.WaitForStopBit]  = (SimpleUartFsm.State.WaitForStartBit,)
        a[SimpleUartFsm.State.Error]           = (SimpleUartFsm.State.WaitForStartBit,)
        self.allowed_state = a

        self.state = None
        self.reset()


class Decoder(srd.Decoder):
    uart_fsm: SimpleUartFsm
    message_break: int  # configurable maximum delay between bytes before making a new message
    samplerate: float
    bit_width: Union[float, Any]  # The width of one UART bit in number of samples.
    data: bytearray
    data_block: Any  # The data object of the current block being processed
    type_block: Any  # The type of the current block being processed
    startsample_block: Union[int, Any]  # The sample start of the current block being processed
    endsample_block: Union[int, Any]  # The sample end of the current block being processed
    prev_stopbit_endsample: int  # The sample end of the last observed STOPBIT
    last_valid_message_stopbit_endsample: int  # The sample end of the last STOPBIT of the last message
    first_startbit_startsample: int  # The sample start of the first STARTBIT of the last message
    out_ann: Any
    out_bin: Any
    api_version = 3
    id = 'j1708'
    name = 'J1708'
    longname = 'J1708'
    desc = 'J1708'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = []
    tags = ['Automotive']
    options = (
        {'id': 'message_break', 'desc': 'Delay (in bit times) for message break', 'default': 2, 'values': (2, 10, 12)},
    )
    annotations = (
        ('datum', 'A J1708 message'),
        ('info', 'Protocol info'),
        ('error', 'Protocol violation or error'),
        ('inline_error', 'Protocol violation or error'),
        ('delay', 'Inter-message Delay [bit times]'),
        ('bus_access', 'Bus Access time violation [bit times]')
    )

    ANNOTATION_DATA = 0
    ANNOTATION_FIELDS = 1
    ANNOTATION_ERROR = 2
    ANNOTATION_INLINE_ERROR = 3
    ANNOTATION_MESSAGE_DELAYS = 4
    ANNOTATION_BUS_ACCESS = 5

    annotation_rows = (
        ('fields', 'RX Fields', (ANNOTATION_FIELDS,)),
        ('data', 'RX Data', (ANNOTATION_DATA, ANNOTATION_INLINE_ERROR)),
        ('errors', 'RX Errors', (ANNOTATION_ERROR, ANNOTATION_BUS_ACCESS,)),
        ('delays', 'RX Message Delays', (ANNOTATION_MESSAGE_DELAYS,)),
    )

    BINARY_MID = 0
    BINARY_PAYLOAD = 1
    BINARY_CRC = 2

    binary = (
        ('mid', 'J1708 MID'),
        ('payload', 'J1708 Payload'),
        ('crc', 'J1708 Checksum'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.uart_fsm = SimpleUartFsm()
        self.data = bytearray()
        self.startsample_block = None
        self.endsample_block = None
        self.do_message_ready()
        self.do_message_break_ready()

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_bin = self.register(srd.OUTPUT_BINARY)
        self.message_break = self.options['message_break']

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value
            self.bit_width = float(self.samplerate) / float(J1708_BAUD)

    # from https://github.com/TruckHacking/py-hv-networks/blob/master/hv_networks/J1708Driver.py
    @staticmethod
    def calculate_checksum(msg):
        val = ~reduce(lambda x, y: (x + y) & 0xFF, list(msg)) + 1
        return struct.pack('B', val & 0xFF)

    def is_checksum_valid(self):
        mid_and_payload = self.data[0:-1]
        if len(mid_and_payload) == 0:
            return False
        test_checksum = Decoder.calculate_checksum(mid_and_payload)
        checksum = bytes(self.data[-1:])
        return checksum == test_checksum

    def do_message_break_ready(self):
        self.last_valid_message_stopbit_endsample = 0
        return

    def is_message_break_armed(self):
        return self.last_valid_message_stopbit_endsample != 0

    def get_current_inter_message_delay(self):
        return (self.startsample_block - self.last_valid_message_stopbit_endsample) / self.bit_width

    def do_flush_message_break_measurement(self):
        if not self.is_message_break_armed():
            return

        current_delay = self.get_current_inter_message_delay()
        delay_bits = "{:05.1F}".format(current_delay)
        self.put(self.last_valid_message_stopbit_endsample, self.startsample_block, self.out_ann,
                 [Decoder.ANNOTATION_MESSAGE_DELAYS, ['%s' % delay_bits]])

        if current_delay < MIN_BUS_ACCESS_BIT_TIMES:
            self.put(self.last_valid_message_stopbit_endsample, self.startsample_block, self.out_ann,
                     [Decoder.ANNOTATION_BUS_ACCESS, ['%s' % delay_bits]])
        return

    def do_flush(self):
        # called from handle_wait_for_start when >= message_break delay is detected
        if len(self.data) == 0:
            return

        # arm message delay measurement
        self.last_valid_message_stopbit_endsample = self.prev_stopbit_endsample
        if not self.is_checksum_valid():
            data_print = self.data[0:-1].hex() + "(" + self.data[-1:].hex() + ")"
            self.put(self.first_startbit_startsample,
                     self.prev_stopbit_endsample, self.out_ann,
                     [Decoder.ANNOTATION_INLINE_ERROR, [data_print]])
            self.put(int(self.prev_stopbit_endsample - self.bit_width * 10),
                     self.prev_stopbit_endsample, self.out_ann,
                     [Decoder.ANNOTATION_ERROR, ['Checksum', 'CRC']])
        else:
            data_print = self.data[0:-1].hex()
            mid_print = hex(self.data[0])
            payload_print = self.data[1:-1].hex()
            checksum_print = self.data[-1:].hex()
            self.put(self.first_startbit_startsample, self.prev_stopbit_endsample, self.out_ann,
                     [Decoder.ANNOTATION_DATA, [data_print]])

            startsample = self.first_startbit_startsample
            endsample = int(self.first_startbit_startsample + self.bit_width * 10)
            self.put(startsample, endsample, self.out_ann,
                     [Decoder.ANNOTATION_FIELDS, ['MID: ' + mid_print, mid_print, 'MID']])
            self.put(startsample, endsample, self.out_bin,
                     [Decoder.BINARY_MID, bytes(self.data[0:1])])

            startsample = endsample
            endsample = int(self.prev_stopbit_endsample - self.bit_width * 10)
            self.put(startsample, endsample, self.out_ann,
                     [Decoder.ANNOTATION_FIELDS, ['Payload: ' + payload_print, payload_print, 'Payload']])
            self.put(startsample, endsample, self.out_bin,
                     [Decoder.BINARY_PAYLOAD, bytes(self.data[1:-1])])

            startsample = endsample
            endsample = self.prev_stopbit_endsample
            self.put(startsample, endsample, self.out_ann,
                     [Decoder.ANNOTATION_FIELDS, ['CRC: ' + checksum_print, checksum_print, 'CRC']])
            self.put(startsample, endsample, self.out_bin,
                     [Decoder.BINARY_CRC, bytes(self.data[-1:])])
        return

    def do_message_ready(self):
        self.uart_fsm.transit(SimpleUartFsm.State.WaitForStartBit)
        self.prev_stopbit_endsample = 0
        self.first_startbit_startsample = 0
        self.data = bytearray()
        return

    def do_invalid_uart_event(self, ptype):
        self.put(self.startsample_block, self.endsample_block, self.out_ann,
                 [Decoder.ANNOTATION_ERROR, ['Unexpected: ' + ptype + ' in ' + self.uart_fsm.state,
                                             ptype + ' in ' + self.uart_fsm.state, 'Unexpected']])
        self.uart_fsm.transit(SimpleUartFsm.State.Error)
        self.do_message_ready()
        return

    def get_current_inter_character_delay(self):
        return (self.startsample_block - self.prev_stopbit_endsample) / self.bit_width

    def maybe_flush_message(self):
        if int(self.get_current_inter_character_delay()) > self.message_break:
            self.do_flush()
            self.do_message_ready()
        return

    def handle_wait_for_startbit(self):
        if self.type_block == 'STARTBIT':
            if self.first_startbit_startsample == 0:
                self.first_startbit_startsample = self.startsample_block
            else:
                self.maybe_flush_message()
            self.uart_fsm.transit(SimpleUartFsm.State.WaitForData)
        elif self.type_block == 'IDLE':
            self.maybe_flush_message()
        else:
            self.do_invalid_uart_event(self.type_block)

        if self.type_block == 'STARTBIT' and \
                self.is_message_break_armed() and \
                int(self.get_current_inter_message_delay()) > self.message_break:
            self.do_flush_message_break_measurement()
            self.do_message_break_ready()
            self.first_startbit_startsample = self.startsample_block

        return

    def handle_wait_for_data(self):
        if self.type_block == 'DATA':
            self.data.append(self.data_block[0])
            self.uart_fsm.transit(SimpleUartFsm.State.WaitForStopBit)
        else:
            self.do_invalid_uart_event(self.type_block)
        return

    def handle_wait_for_stopbit(self):
        if self.type_block == 'STOPBIT':
            self.prev_stopbit_endsample = self.endsample_block
            self.uart_fsm.transit(SimpleUartFsm.State.WaitForStartBit)
        else:
            self.do_invalid_uart_event(self.type_block)
        return

    def decode(self, ss, es, data):
        """
        Short J1708 overview:
         - Message begins when there has been a message break delay (either 2, 10, or 12 bit-
         times depending on your read of the specification. Max inter-character, idle time and
         minimum bus access time respectively.), configurable for this decoder by the
         'message_break' option
         - break is always followed by MID
         - MID is followed by an unlimited number of payload bytes and a final checksum byte. The
         specification recommends no longer than a 21 byte payload.
        """
        ptype, rxtx, pdata = data

        if rxtx != 0:
            return  # drop all TX uart traffic, J1708 is a shared medium (and we want a simpler decoder)

        self.type_block = ptype
        if self.type_block == 'FRAME':
            return  # ignore b/c J1708 has real-time constraints for framing which this decoder implements
        if self.type_block == 'BREAK':
            return  # ignore b/c J1708 has 0x00 bytes and they sometimes get mislabeled as BREAK by UART decoder
        if self.type_block == 'INVALID STOPBIT':
            return  # same reason for INVALID_STOP_BIT

        self.startsample_block, self.endsample_block = ss, es
        self.data_block = pdata

        handler = getattr(self, 'handle_%s' % self.uart_fsm.state.lower())
        handler()
        return
