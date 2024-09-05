##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2019 Libor Gabaj <libor.gabaj@gmail.com>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## Permission is hereby granted, free of charge, to any person obtaining a copy
## of this software and associated documentation files (the "Software"), to deal
## in the Software without restriction, including without limitation the rights
## to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
## copies of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:
##
## The above copyright notice and this permission notice shall be included in all
## copies or substantial portions of the Software.
##
## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
## IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
## FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
## AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
## LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
## OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.

import sigrokdecode as srd
import common.srdhelper as hlp


"""
OUTPUT_PYTHON format:

Packet:
[<ptype>, <pdata>]

<ptype>:
 - "START" (START condition)
 - "COMMAND" (Command)
 - "DATA" (Data)
 - "STOP" (STOP condition)
 - "ACK" (ACK bit)
 - "NACK" (NACK bit)
 - "BITS" (<pdata>: list of data bits and their ss/es numbers)

<pdata> is the data byte associated with the "DATA*" command.
For "START", "STOP", "ACK", and "NACK" <pdata> is None.
"""
###############################################################################
# Channels
###############################################################################
CLK = 0     # Serial clock
DIO = 1     # Data input/output
STB = 2     # Strobe line


###############################################################################
# Enumeration classes for device parameters
###############################################################################
class Bus:
    """Enumeration of possible driver chip types."""

    (WIRE2, WIRE3) = (0, 1)


###############################################################################
# Enumeration classes for annotations
###############################################################################
class AnnProtocol:
    """Enumeration of annotations for protocol states."""

    (START, STOP, ACK, NACK, COMMAND, DATA, BIT) = range(7)


class AnnInfo:
    """Enumeration of annotations for formatted info."""

    (WARN,) = (AnnProtocol.BIT + 1,)


class AnnBinary:
    """Enumeration of annotations for binary info."""

    (DATA,) = (0,)


###############################################################################
# Parameters mapping
###############################################################################
commands = {
    AnnProtocol.START: "START",
    AnnProtocol.STOP: "STOP",
    AnnProtocol.ACK: "ACK",
    AnnProtocol.NACK: "NACK",
    AnnProtocol.COMMAND: "COMMAND",
    AnnProtocol.DATA: "DATA",
    AnnProtocol.BIT: "BITS",
}


###############################################################################
# Parameters anotations definitions
###############################################################################
"""
- The last item of an annotation list is used repeatedly without a value.
- The last two items of an annotation list are used repeatedly without a value.
"""
protocol = {
    AnnProtocol.START: ["Start", "S"],
    AnnProtocol.STOP: ["Stop", "P"],
    AnnProtocol.ACK: ["ACK", "A"],
    AnnProtocol.NACK: ["NACK", "N"],
    AnnProtocol.COMMAND: ["Command", "Cmd", "C"],
    AnnProtocol.DATA: ["Data", "D"],
    AnnProtocol.BIT: ["Bit", "B"],
}

info = {
    AnnInfo.WARN: ["Warnings", "Warn", "W"],
}

binary = {
    AnnBinary.DATA: ["Data", "D"],
}


###############################################################################
# Decoder
###############################################################################
class SamplerateError(Exception):
    """Custom exception."""

    pass


class ChannelError(Exception):
    """Custom exception."""

    pass


class Decoder(srd.Decoder):
    """Protocol decoder for Titan Micro Circuits."""

    api_version = 3
    id = "tmc"
    name = "TMC"
    longname = "Titan Micro Circuit"
    desc = "Bus for TM1636/37/38 7-segment digital tubes."
    license = "gplv2+"
    inputs = ["logic"]
    outputs = ["tmc"]
    tags = ['Embedded/industrial']
    channels = (
        {"id": "clk", "name": "CLK", "desc": "Clock line"},
        {"id": "dio", "name": "DIO", "desc": "Data line"},
    )
    optional_channels = (
        {"id": "stb", "name": "STB", "desc": "Strobe line"},
    )
    options = (
        {"id": "radix", "desc": "Number format", "default": "Hex",
         "values": ("Hex", "Dec", "Oct", "Bin")},
    )
    annotations = hlp.create_annots(
        {
            "prot": protocol,
            "info": info,
         }
    )
    annotation_rows = (
        ("bits", "Bits", (AnnProtocol.BIT,)),
        ("data", "Cmd/Data", tuple(range(
            AnnProtocol.START, AnnProtocol.DATA + 1
            ))),
        ("warnings", "Warnings", (AnnInfo.WARN,)),
    )
    binary = hlp.create_annots({"data": binary})

    def __init__(self):
        """Initialize decoder."""
        self.reset()

    def reset(self):
        """Reset decoder and initialize instance variables."""
        self.ss = self.es = self.ss_byte = self.ss_ack = -1
        self.samplerate = None
        self.pdu_start = None
        self.pdu_bits = 0
        self.bustype = None
        self.bytecount = 0
        self.clear_data()
        self.state = "FIND START"

    def metadata(self, key, value):
        """Pass metadata about the data stream."""
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        """Actions before the beginning of the decoding."""
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_bitrate = self.register(
            srd.OUTPUT_META,
            meta=(int, "Bitrate", "Bitrate from Start bit to Stop bit")
        )

    def putx(self, data):
        """Show data to annotation output across bit range."""
        self.put(self.ss, self.es, self.out_ann, data)

    def putp(self, data):
        """Show data to python output across bit range."""
        self.put(self.ss, self.es, self.out_python, data)

    def putb(self, data):
        """Show data to binary output across bit range."""
        self.put(self.ss, self.es, self.out_binary, data)

    def clear_data(self):
        """Clear data cache."""
        self.bitcount = 0
        self.databyte = 0
        self.bits = []

    def handle_bitrate(self):
        """Calculate bitrate."""
        elapsed = 1 / float(self.samplerate)    # Sample time period
        elapsed *= self.samplenum - self.pdu_start - 1
        if elapsed:
            bitrate = int(1 / elapsed * self.pdu_bits)
            self.put(self.ss_byte, self.samplenum, self.out_bitrate, bitrate)

    def handle_start(self, pins):
        """Process start condition."""
        self.ss, self.es = self.samplenum, self.samplenum
        self.pdu_start = self.samplenum
        self.pdu_bits = 0
        self.bytecount = 0
        cmd = AnnProtocol.START
        self.putp([commands[cmd], None])
        self.putx([cmd, protocol[cmd]])
        self.clear_data()
        self.state = "FIND DATA"

    def handle_data(self, pins):
        """Create name and call corresponding data handler."""
        self.pdu_bits += 1
        if self.bitcount == 0:
            self.ss_byte = self.samplenum
        fn = getattr(self, "handle_data_{}".format(self.bustype))
        fn(pins)

    def handle_stop(self):
        """Create name and call corresponding stop handler."""
        fn = getattr(self, "handle_stop_{}".format(self.bustype))
        fn()

    def handle_data_wire2(self, pins):
        """Process data bits for 2-wire bus.

        Arguments
        ---------
        pins : tuple
            Tuple of bit values (0 or 1) for each channel from the first one.

        Notes
        -----
        - The method is called at rising edge of each clock pulse regardless of
          its purpose or meaning.
        - For acknowledge clock pulse and start/stop pulse the registration of
          this bit is provided in vain just for simplicity of the method.
        - The method stores individual bits and their start/end sample numbers.
        - In the bit list, index 0 represents the recently processed bit, which
          is finally the MSB (LSB-first transmission).
        - The method displays previous bit because its end sample number is
          known just at processing the current bit.

        """
        clk, dio, stb = pins
        self.bits.insert(0, [dio, self.samplenum, self.samplenum])
        # Register end sample of the previous bit and display it
        if self.bitcount > 0:
            self.bits[1][2] = self.samplenum
            # Display previous data bit
            if self.bitcount <= 8:
                annots = [str(self.bits[1][0])]
                self.put(self.bits[1][1], self.bits[1][2], self.out_ann,
                         [AnnProtocol.BIT, annots])
        # Include current data bit to data byte (LSB-first transmission)
        self.bitcount += 1
        if self.bitcount <= 8:
            self.databyte >>= 1
            self.databyte |= (dio << 7)
            return
        # Display data byte
        self.ss, self.es = self.ss_byte, self.samplenum
        cmd = (AnnProtocol.DATA, AnnProtocol.COMMAND)[self.bytecount == 0]
        self.bits = self.bits[-8:]    # Remove superfluous bits (ACK)
        self.bits.reverse()
        self.putp([commands[AnnProtocol.BIT], self.bits])
        self.putp([commands[cmd], self.databyte])
        self.putb([AnnBinary.DATA, bytes([self.databyte])])
        annots = hlp.compose_annot(
            protocol[cmd],
            ann_value=hlp.format_data(self.databyte, self.options["radix"])
        )
        self.putx([cmd, annots])
        self.clear_data()
        self.ss_ack = self.samplenum  # Remember start of ACK bit
        self.bytecount += 1
        self.state = "FIND ACK"

    def handle_ack(self, pins):
        """Process ACK/NACK bit."""
        clk, dio, stb = pins
        self.ss, self.es = self.ss_ack, self.samplenum
        cmd = (AnnProtocol.ACK, AnnProtocol.NACK)[dio]
        self.putp([commands[cmd], None])
        self.putx([cmd, protocol[cmd]])
        self.state = "FIND DATA"

    def handle_stop_wire2(self):
        """Process stop condition for 2-wire bus."""
        self.handle_bitrate()
        # Display stop
        cmd = AnnProtocol.STOP
        self.ss, self.es = self.samplenum, self.samplenum
        self.putp([commands[cmd], None])
        self.putx([cmd, protocol[cmd]])
        self.clear_data()
        self.state = "FIND START"

    def handle_byte_wire3(self):
        """Process data byte after last CLK pulse for 3-wire bus."""
        if not self.bits:
            return
        # Display all bits
        self.bits[0][2] = self.samplenum    # Update end sample of the last bit
        for bit in self.bits:
            annots = [str(bit[0])]
            self.put(bit[1], bit[2], self.out_ann, [AnnProtocol.BIT, annots])
        # Display data byte
        self.ss, self.es = self.ss_byte, self.samplenum
        cmd = (AnnProtocol.DATA, AnnProtocol.COMMAND)[self.bytecount == 0]
        self.bits.reverse()
        self.putp([commands[AnnProtocol.BIT], self.bits])
        self.putp([commands[cmd], self.databyte])
        self.putb([AnnBinary.DATA, bytes([self.databyte])])
        annots = hlp.compose_annot(
            protocol[cmd],
            ann_value=hlp.format_data(self.databyte, self.options["radix"])
        )
        self.putx([cmd, annots])
        self.bytecount += 1

    def handle_data_wire3(self, pins):
        """Process data bits at CLK rising edge for 3-wire bus."""
        clk, dio, stb = pins
        if self.bitcount >= 8:
            self.handle_byte_wire3()
            self.clear_data()
            self.ss_byte = self.samplenum
        self.bits.insert(0, [dio, self.samplenum, self.samplenum])
        self.databyte >>= 1
        self.databyte |= (dio << 7)
        # Register end sample of the previous bit
        if self.bitcount > 0:
            self.bits[1][2] = self.samplenum
        self.bitcount += 1

    def handle_stop_wire3(self):
        """Process stop condition for 3-wire bus."""
        self.handle_bitrate()
        self.handle_byte_wire3()
        # Display stop
        cmd = AnnProtocol.STOP
        self.ss, self.es = self.samplenum, self.samplenum
        self.putp([commands[cmd], None])
        self.putx([cmd, protocol[cmd]])
        self.clear_data()
        self.state = "FIND START"

    def decode(self):
        """Decode samples provided by logic analyzer."""
        if not self.samplerate:
            raise SamplerateError("Cannot decode without samplerate.")
        has_pin = [self.has_channel(ch) for ch in (CLK, DIO)]
        if has_pin != [True, True]:
            raise ChannelError("Both CLK and DIO pins required.")
        while True:
            # State machine
            if self.state == "FIND START":
                # Wait for any of the START conditions:
                # WIRE3: CLK = high, STB = falling
                # WIRE2: CLK = high, DIO = falling
                pins = self.wait([
                    {CLK: 'h', STB: "f"},
                    {CLK: 'l', STB: "f"},
                    {CLK: "h", DIO: "f"},
                ])
                if self.matched[0] or self.matched[1]:
                    self.bustype = "wire3"
                    self.handle_start(pins)
                elif self.matched[2]:
                        self.bustype = "wire2"
                        self.handle_start(pins)
            elif self.state == "FIND DATA":
                # Wait for any of the following conditions:
                #  WIRE3 STOP condition: STB = rising
                #  WIRE2 STOP condition: CLK = high, DIO = rising
                #  Clock pulse: CLK = rising
                pins = self.wait([
                    {STB: "r"},
                    {CLK: "h", DIO: "r"},
                    {CLK: "r"},
                ])
                if self.matched[0] or self.matched[1]:
                    self.handle_stop()
                elif self.matched[2]:
                    self.handle_data(pins)
            elif self.state == "FIND ACK":
                # Wait for an ACK bit
                self.handle_ack(self.wait({CLK: "f"}))
            elif self.state == "FIND STOP":
                # Wait for STOP conditions:
                #  WIRE3 STOP condition: STB = rising
                #  WIRE2 STOP condition: CLK = high, DIO = rising
                pins = self.wait([
                    {STB: "r"},
                    {CLK: "h", DIO: "r"},
                ])
                if self.matched[0] or self.matched[1]:
                    self.handle_stop()
