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


###############################################################################
# Enumeration classes for device parameters
###############################################################################
class Command:
    """Enumeration of command registers addresses (bits 6 ~ 7)."""

    (DATA, DISPLAY, ADDRESS) = (0x40, 0x80, 0xC0)


class CommonBits:
    """Enumeration of common bits."""

    (RESERVED,) = (0xff,)


class CommandBits:
    """Range of command bits in a command."""

    (MIN, MAX) = (6, 7)


class DataBits:
    """Data bits in a data command."""

    (RW, ADDR, MODE) = (1, 2, 3)


class DisplayBits:
    """Range of display bits in a display command."""

    (MIN, MAX, SWITCH) = (0, 2, 3)


class AddressBits:
    """Range of address bits in an address command."""

    (MIN, MAX) = (0, 3)


class LedBits:
    """Range of LED color bits in a data."""

    (RED, GREEN) = (0, 1)


class Params:
    """Specific parameters."""

    (DP, LEDOFF, UNKNOWN_CHAR) = (".", ".", "?")


###############################################################################
# Enumeration classes for annotations
###############################################################################
class AnnBits:
    """Enumeration of annotations for bits."""

    (
        RESERVED,
        WRITE, READ,                # Data write, key scan read
        DATA, DISPLAY, ADDRESS,     # Commands
        AUTO, FIXED,                # Addressing
        NORMAL, TEST,               # Mode
        POSITION, DIGIT, LED, KEY,  # Display units
        RED, GREEN,                 # LED colors
        CONTRAST, OFF, ON,          # Display control
    ) = range(19)


class AnnInfo:
    """Enumeration of annotations for various strings."""

    (WARN, DISPLAY, LEDS, KEYS) = range(AnnBits.ON + 1, (AnnBits.ON + 1) + 4)


###############################################################################
# Parameters mapping
###############################################################################
cmd_annot = {  # Convert command value to bits annotation index
    Command.DATA: AnnBits.DATA,
    Command.DISPLAY: AnnBits.DISPLAY,
    Command.ADDRESS: AnnBits.ADDRESS,
}

led_annot = {  # Convert LED code to bits annotation index
    LedBits.RED: AnnBits.RED,
    LedBits.GREEN: AnnBits.GREEN,
}

contrasts = ["1/16", "2/16", "4/16", "10/16",
             "11/16", "12/16", "13/16", "14/16"]

segments = ["a", "b", "c", "d", "e", "f", "g", "dp"]
leds = ["R", "G"]
keybits = ["K3", "K2", "K1", "X"]

switches = {    # Maps key scan bits and nibbles to physical switches
    "K3-KS1": "S1",
    "K3-KS2": "S5",
    "K3-KS3": "S2",
    "K3-KS4": "S6",
    "K3-KS5": "S3",
    "K3-KS6": "S7",
    "K3-KS7": "S4",
    "K3-KS8": "S8",
    "K2-KS1": "S9",
    "K2-KS2": "S13",
    "K2-KS3": "S10",
    "K2-KS4": "S14",
    "K2-KS5": "S11",
    "K2-KS6": "S15",
    "K2-KS7": "S12",
    "K2-KS8": "S16",
    "K1-KS1": "S17",
    "K1-KS2": "S21",
    "K1-KS3": "S18",
    "K1-KS4": "S22",
    "K1-KS5": "S19",
    "K1-KS6": "S23",
    "K1-KS7": "S20",
    "K1-KS8": "S24",
}


fonts = {
  0b0000000: " ",
  0b0100000: "'",
  0b1000000: "-",
  0b0111111: "0",
  0b0000110: "1",
  0b1011011: "2",
  0b1001111: "3",
  0b1100110: "4",
  0b1101101: "5",   # S, s
  0b1111101: "6",
  0b0000111: "7",
  0b1111111: "8",
  0b1101111: "9",
  0b1110111: "A",   # a
  0b1111100: "b",   # B
  0b0111001: "C",   # [, (
  0b1011110: "d",   # D
  0b1111001: "E",   # e
  0b1110001: "F",   # f
  0b1110110: "H",
  0b0110000: "I",
  0b0001110: "J",   # j
  0b0111000: "L",   # l
  0b1010100: "n",   # N
  0b1011100: "o",   # O
  0b1110011: "P",   # p
  0b1010000: "r",   # R
  0b1111000: "t",   # T
  0b0111110: "U",
  0b0001111: "]",   # )
  0b0001000: "_",
  0b1011000: "c",
  0b1110100: "h",
  0b0010000: "i",
  0b0011100: "u",
}

###############################################################################
# Parameters anotations definitions
###############################################################################
bits = {
    AnnBits.RESERVED: ["Reserved", "Rsvd", "R"],
    AnnBits.WRITE: ["Write", "Wrt", "W"],
    AnnBits.READ: ["Read", "Rd", "R"],
    AnnBits.DATA: ["Data command", "Data Cmd", "Data", "D"],
    AnnBits.DISPLAY: ["Display command", "Display Cmd", "Display", "S"],
    AnnBits.ADDRESS: ["Address command", "Address Cmd", "Address",
                      "Addr", "A"],
    AnnBits.AUTO: ["AutoAddr", "Auto", "A"],
    AnnBits.FIXED: ["FixedAddr", "Fix", "F"],
    AnnBits.NORMAL: ["Normal", "Nrm", "N"],
    AnnBits.TEST: ["Test", "Tst", "T"],
    AnnBits.POSITION: ["Position", "Pos", "P"],
    AnnBits.DIGIT: ["Digit", "Dgt", "D"],
    AnnBits.LED: ["LED", "L"],
    AnnBits.KEY: ["Key", "K"],
    AnnBits.RED: ["Red LED", "Red", "R"],
    AnnBits.GREEN: ["Green LED", "Green", "Gr", "G"],
    AnnBits.CONTRAST: ["Contrast", "PWM", "C"],
    AnnBits.OFF: ["OFF", "L"],
    AnnBits.ON: ["ON", "H"],
}

info = {
    AnnInfo.WARN: ["Warnings", "Warn", "W"],
    AnnInfo.DISPLAY: ["Tubes", "T"],
    AnnInfo.LEDS: ["LEDs", "L"],
    AnnInfo.KEYS: ["Keys", "K"],
}


###############################################################################
# Decoder
###############################################################################
class Decoder(srd.Decoder):
    """Protocol decoder for 7-segments digital tubes control ``TM1638``."""

    api_version = 3
    id = "tm1638"
    name = "TM1638"
    longname = "Special circuit for LED driver control"
    desc = "Titan Micro Electronics LED drive control special circuit for \
        driving displays with 7-segments digital tubes, two-color LEDs, \
        and keyboard scanning."
    license = "gplv2+"
    inputs = ["tmc"]
    outputs = ["tm1638"]
    tags = ['Embedded/industrial']

    annotations = hlp.create_annots(
        {
            "bit": bits,
            "info": info,
        }
    )
    annotation_rows = (
        ("bits", "Bits", tuple(
            range(AnnBits.RESERVED, AnnBits.ON + 1)
        )),
        ("display", "Display", (AnnInfo.DISPLAY,)),
        ("leds", "LEDchain", (AnnInfo.LEDS,)),
        ("keys", "Keyboard", (AnnInfo.KEYS,)),
        ("warnings", "Warnings", (AnnInfo.WARN,)),
    )

    def __init__(self):
        """Initialize decoder."""
        self.reset()

    def reset(self):
        """Reset decoder and initialize instance variables."""
        # Common parameters
        self.ss = 0         # Start sample
        self.es = 0         # End sample
        self.ssb = 0        # Start sample of an annotation transmission
        self.bits = []      # List of recent processed byte bits
        self.write = None   # Flag about recent R/W command
        self.state = "IDLE"
        # Specific parameters for a device
        self.auto = None    # Flag about current addressing
        self.position = 0   # Processed address position
        self.clear_data()

    def clear_data(self):
        """Clear data cache."""
        self.display = []   # Buffer for displayed chars
        self.leds = []      # Buffer for displayed LEDs
        self.keys = []      # Buffer for pressed keys

    def start(self):
        """Actions before the beginning of the decoding."""
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putd(self, ss, es, data):
        """Span data output across bit range.

        - Output is an annotation block from the start sample of the first
          bit to the end sample of the last bit.
        """
        self.put(self.bits[ss][1], self.bits[es][2], self.out_ann, data)

    def putr(self, start, end=None):
        """Span reserved bit annotation across bit range bit by bit.

        - Parameters should be considered as a range, so that the end bit
          number is not annotated.
        """
        annots = hlp.compose_annot(bits[AnnBits.RESERVED])
        for bit in range(start, end or (start + 1)):
            self.put(self.bits[bit][1], self.bits[bit][2],
                     self.out_ann, [AnnBits.RESERVED, annots])

    def handle_command(self, data):
        """Detect command and call its handler."""
        mask = 0
        for i in range(CommandBits.MIN, CommandBits.MAX + 1):
            mask |= 1 << i
        cmd = data & mask
        for attr, value in vars(Command).items():
            if not attr.startswith("__") and value == cmd:
                # Bits row - Command bits
                ann = cmd_annot[cmd]
                annots = hlp.compose_annot(bits[ann])
                self.putd(CommandBits.MIN, CommandBits.MAX, [ann, annots])
                # Handler
                fn = getattr(self, "handle_command_{}".format(attr.lower()))
                fn(data & ~mask)

    def handle_command_data(self, data):
        """Process data command."""
        # Bits row - Reserved
        self.putr(DataBits.MODE + 1, CommandBits.MIN)
        # Bits row - Mode bit
        ann = (AnnBits.NORMAL, AnnBits.TEST)[data >> DataBits.MODE & 1]
        annots = hlp.compose_annot(bits[ann])
        self.putd(DataBits.MODE, DataBits.MODE, [ann, annots])
        # Bits row - Addressing bit
        ann = (AnnBits.AUTO, AnnBits.FIXED)[data >> DataBits.ADDR & 1]
        self.auto = (ann == AnnBits.AUTO)
        annots = hlp.compose_annot(bits[ann])
        self.putd(DataBits.ADDR, DataBits.ADDR, [ann, annots])
        # Bits row - Read/Write bit
        self.write = (data >> DataBits.RW & 1 == 0)
        ann = (AnnBits.READ, AnnBits.WRITE)[self.write]
        annots = hlp.compose_annot(bits[ann])
        self.putd(DataBits.RW, DataBits.RW, [ann, annots])
        # Bits row - Prohibited bit
        self.putr(0, DataBits.RW)

    def handle_command_display(self, data):
        """Process display command."""
        # Bits row - Reserved
        self.putr(DisplayBits.SWITCH + 1, CommandBits.MIN)
        # Bits row - Switch bit
        ann = (AnnBits.OFF, AnnBits.ON)[data >> DisplayBits.SWITCH & 1]
        annots = hlp.compose_annot(bits[ann])
        self.putd(DisplayBits.SWITCH, DisplayBits.SWITCH, [ann, annots])
        # Bits row - PWM bits
        mask = 0
        for i in range(DisplayBits.MIN, DisplayBits.MAX + 1):
            mask |= 1 << i
        pwm = contrasts[data & mask]
        ann = AnnBits.CONTRAST
        annots = hlp.compose_annot(bits[ann], ann_value=pwm)
        self.putd(DisplayBits.MIN, DisplayBits.MAX, [ann, annots])

    def handle_command_address(self, data):
        """Process address command."""
        # Bits row - Reserved
        self.putr(AddressBits.MAX + 1, CommandBits.MIN)
        # Bits row - Digit bits
        mask = 0
        for i in range(AddressBits.MIN, AddressBits.MAX + 1):
            mask |= 1 << i
        adr = (data & mask) + 1
        self.position = adr    # Start address
        ann = AnnBits.POSITION
        annots = hlp.compose_annot(bits[ann], ann_value=adr)
        self.putd(AddressBits.MIN, AddressBits.MAX, [ann, annots])

    def handle_data(self, data):
        """Detect display unit and call its handler."""
        if self.write:
            sfx = ("led", "digit")[self.position % 2]
            fn = getattr(self, "handle_data_{}".format(sfx.lower()))
        else:
            sfx = "keyboard"
            fn = getattr(self, "handle_data_{}".format(sfx.lower()))
        fn(data)
        if self.auto:
            self.position += 1     # Automatic address adding

    def handle_data_digit(self, data):
        """Process digital tube data."""
        # Bits row - Active segments bits
        for i in range(8):
            if data >> i & 1:
                annots = [segments[i]]
                self.putd(i, i, [AnnBits.DIGIT, annots])
        # Register digit
        mask = data & ~(1 << 7)
        char = Params.UNKNOWN_CHAR
        dp = ""
        if mask in fonts:
            char = fonts[mask]
            if (data >> 7) & 1:
                dp = Params.DP
        self.display.append(char + dp)

    def handle_data_led(self, data):
        """Process LED data."""
        # Bits row - Active LED
        led = Params.LEDOFF
        for i in range(8):
            if data >> i & 1:
                annots = hlp.compose_annot(bits[led_annot[i]])
                self.putd(i, i, [AnnBits.LED, annots])
                led = leds[i]
        # Register LED
        self.leds.append(led)

    def handle_data_keyboard(self, data):
        """Process key scanning data."""
        # Bits row - Pressed keys
        for bit in range(8):
            if data >> bit & 1:
                key = keybits[bit % 4]
                seg = "KS{}".format(2 * self.position + (bit // 4) + 1)
                keytag = "{}-{}".format(key, seg)
                annots = [keytag]
                self.putd(bit, bit, [AnnBits.KEY, annots])
                self.keys.append(keytag)

    def handle_info(self):
        """Process info rows."""
        # Display row
        if self.display:
            ann = AnnInfo.DISPLAY
            val = "{}".format("".join(self.display))
            annots = hlp.compose_annot(info[ann], ann_value=val)
            self.put(self.ssb, self.es, self.out_ann, [ann, annots])
        # LEDchain row
        if self.leds:
            ann = AnnInfo.LEDS
            val = "{}".format("".join(self.leds))
            annots = hlp.compose_annot(info[ann], ann_value=val)
            self.put(self.ssb, self.es, self.out_ann, [ann, annots])
        # Keyboard row
        if self.keys:
            ann = AnnInfo.KEYS
            val = "{}".format(",".join(
                map(lambda key: switches[key], self.keys)
                ))
            annots = hlp.compose_annot(info[ann], ann_value=val)
            self.put(self.ssb, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def decode(self, ss, es, data):
        """Decode samples provided by parent decoder."""
        cmd, databyte = data
        self.ss, self.es = ss, es

        if cmd == "BITS":
            """Collect packet of bits that belongs to the following command.
            - Packet is in the form of list of bit lists:
                ["BITS", bitlist]
            - Bit list is a list of 3 items list
                [[bitvalue, startsample, endsample], ...]
            - Samples are counted for aquisition sampling frequency.
            - Parent decoder ``tmc``stores individual bits in the list from
              the least significant bit (LSB) to the most significant bit
              (MSB) as it is at representing numbers in computers.
            """
            self.bits = databyte
            return

        # State machine
        if self.state == "IDLE":
            """Wait for new transmission."""
            if cmd != "START":
                return
            self.ssb = self.ss
            self.state = "REGISTER COMMAND"

        elif self.state == "REGISTER COMMAND":
            """Process command register."""
            if cmd == "COMMAND":
                self.handle_command(databyte)
                self.state = "REGISTER DATA"
            elif cmd == "STOP":
                self.state = "IDLE"

        elif self.state == "REGISTER DATA":
            """Process data register."""
            if cmd == "DATA":
                self.handle_data(databyte)
            elif cmd == "STOP":
                self.handle_info()
                self.state = "IDLE"
