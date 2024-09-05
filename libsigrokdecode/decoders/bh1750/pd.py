##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2019 Candid Moe <candidmoe@gmail.com>
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
import common.srdhelper as hlp


###############################################################################
# Enumeration classes for device parameters
###############################################################################
class Address:
    """Enumeration of possible slave addresses.

    - Device address is determined by the sensor's ADD0 pin physical connection
      to particular power rail.
    """

    (GND, VCC) = (0x23, 0x5c)


class Register:
    """Enumeration of possible slave register addresses."""

    (
        PWRDOWN, PWRUP, RESET, MTHIGH, MTLOW,  # Operation commands
        MCHIGH, MCHIGH2, MCLOW,     # Continuous measurement modes
        MOHIGH, MOHIGH2, MOLOW,     # One-time measurement modes
    ) = (
            0x00, 0x01, 0x07, 0x40, 0x60,
            0x10, 0x11, 0x13,
            0x20, 0x21, 0x23,
    )


class MTregHighBits:
    """Range of high data bits of the measurement time register."""

    (MIN, MAX) = (0, 2)


class MTregLowBits:
    """Range of low data bits of the measurement time register."""

    (MIN, MAX) = (0, 4)


class Params:
    """Specific parameters."""

    (
        MTREG_TYP,  # Typical value
        ACCURACY_TYP, ACCURACY_MAX, ACCURACY_MIN,   # Count per lux
        UNIT_LIGHT
    ) = (69, 1.20, 1.44, 0.96, "lux")


###############################################################################
# Enumeration classes for annotations
###############################################################################
class AnnAddrs:
    """Enumeration of annotations for addresses."""

    (GND, VCC) = range(2)


class AnnRegs:
    """Enumeration of annotations for registers."""

    (
        PWRDOWN, PWRUP, RESET, MTHIGH, MTLOW,
        MCHIGH, MCHIGH2, MCLOW,
        MOHIGH, MOHIGH2, MOLOW,
        DATA,
    ) = range(AnnAddrs.VCC + 1, (AnnAddrs.VCC + 1) + 12)


class AnnBits:
    """Enumeration of annotations for bits."""

    (RESERVED, DATA) = range(AnnRegs.DATA + 1, (AnnRegs.DATA + 1) + 2)


class AnnInfo:
    """Enumeration of annotations for various strings."""

    (
        WARN, BADADD, CHECK, WRITE, READ,
        SENSE, LIGHT, MTREG, MTIME,
    ) = range(AnnBits.DATA + 1, (AnnBits.DATA + 1) + 9)


###############################################################################
# Parameters mapping
###############################################################################
addr_annots = {  # Convert address value to annotation index
    Address.GND: AnnAddrs.GND,
    Address.VCC: AnnAddrs.VCC,
}

reg_annots = {  # Convert register value to annotation index
    Register.PWRDOWN: AnnRegs.PWRDOWN,
    Register.PWRUP: AnnRegs.PWRUP,
    Register.RESET: AnnRegs.RESET,
    Register.MTHIGH: AnnRegs.MTHIGH,
    Register.MTLOW: AnnRegs.MTLOW,
    Register.MCHIGH: AnnRegs.MCHIGH,
    Register.MCHIGH2: AnnRegs.MCHIGH2,
    Register.MCLOW: AnnRegs.MCLOW,
    Register.MOHIGH: AnnRegs.MOHIGH,
    Register.MOHIGH2: AnnRegs.MOHIGH2,
    Register.MOLOW: AnnRegs.MOLOW,
}

prm_map_accuracy = {  # Convert parameter option to accuracy parameter
    "Typical": Params.ACCURACY_TYP,
    "Minimal": Params.ACCURACY_MIN,
    "Maximal": Params.ACCURACY_MAX,
}

###############################################################################
# Parameters anotations definitions
###############################################################################
addresses = {
    AnnAddrs.GND: ["ADDR grounded", "ADDR_GND", "AG"],
    AnnAddrs.VCC: ["ADDR powered", "ADDR_VCC", "AV"],
}

registers = {
    AnnRegs.PWRDOWN: ["Power down", "Pwr Dwn", "Off", "D"],
    AnnRegs.PWRUP: ["Power up", "Pwr Up", "On", "U"],
    AnnRegs.RESET: ["Reset light register", "Reset light", "Reset",
                    "Rst", "R"],
    AnnRegs.MTHIGH: ["Measurement time high bits", "Mtime Hbits", "MTH", "H"],
    AnnRegs.MTLOW: ["Measurement time low bits", "Mtime Lbits", "MTL", "L"],
    AnnRegs.MCHIGH: ["Continuous measurement high resolution",
                     "Continuous high res", "Cont high", "CH"],
    AnnRegs.MCHIGH2: ["Continuous measurement double high resolution",
                      "Continuous double high res", "Cont double", "CH2"],
    AnnRegs.MCLOW: ["Continuous measurement low resolution",
                    "Continuous low res", "Cont low", "CL"],
    AnnRegs.MOHIGH: ["One time measurement high resolution",
                     "One time high res", "One high", "OH"],
    AnnRegs.MOHIGH2: ["One time measurement double high resolution",
                      "One time double high res", "One double", "OH2"],
    AnnRegs.MOLOW: ["One time measurement low resolution",
                    "One time low res", "One low", "OL"],
    AnnRegs.DATA: ["Illuminance data register", "Illuminance register",
                   "Illuminance", "Light", "L"],
}

bits = {
    AnnBits.RESERVED: ["Reserved", "Rsvd", "R"],
    AnnBits.DATA: ["Measurement time", "MT", "M"],
}

info = {
    AnnInfo.WARN: ["Warnings", "Warn", "W"],
    AnnInfo.BADADD: ["Uknown slave address", "Unknown address", "Uknown",
                     "Unk", "U"],
    AnnInfo.CHECK: ["Slave presence check", "Slave check", "Check",
                    "Chk", "C"],
    AnnInfo.WRITE: ["Write", "Wr", "W"],
    AnnInfo.READ: ["Read", "Rd", "R"],
    AnnInfo.SENSE: ["Sensitivity", "Sense", "S"],
    AnnInfo.LIGHT: ["Ambient light", "Light", "L"],
    AnnInfo.MTREG: ["Measurement time register", "MTreg", "MTR", "R"],
    AnnInfo.MTIME: ["Measurement time", "MTime", "MT", "T"],
}


###############################################################################
# Decoder
###############################################################################
class Decoder(srd.Decoder):
    """Protocol decoder for digital ambient light sensor ``BH1750``."""

    api_version = 3
    id = "bh1750"
    name = "BH1750"
    longname = "Digital ambient light sensor BH1750"
    desc = "Digital 16bit Serial Output Type Ambient Light Sensor IC."
    license = "gplv2+"
    inputs = ["i2c"]
    outputs = ["bh1750"]
    tags = ['Embedded/industrial']

    options = (
        {"id": "radix", "desc": "Number format", "default": "Hex",
         "values": ("Hex", "Dec", "Oct", "Bin")},
        {"id": "params", "desc": "Datasheet parameter used",
         "default": "Typical",
         "values": ("Typical", "Maximal", "Minimal")},
    )

    annotations = hlp.create_annots(
        {
            "addr": addresses,
            "reg": registers,
            "bit": bits,
            "info": info,
        }
    )
    annotation_rows = (
        ("bits", "Bits", (AnnBits.RESERVED, AnnBits.DATA)),
        ("regs", "Registers",
            tuple(range(AnnAddrs.GND, AnnRegs.DATA + 1))),
        ("info", "Info",
            tuple(range(AnnInfo.CHECK, AnnInfo.MTIME + 1))),
        ("warnings", "Warnings", (AnnInfo.WARN, AnnInfo.BADADD)),
    )

    def __init__(self):
        """Initialize decoder."""
        self.reset()

    def reset(self):
        """Reset decoder and initialize instance variables."""
        # Common parameters for I2C sampling
        self.ss = 0         # Start sample
        self.es = 0         # End sample
        self.ssb = 0        # Start sample of an annotation transmission block
        self.write = None   # Flag about recent write action
        self.state = "IDLE"
        # Specific parameters for a device
        self.addr = Address.GND             # Slave address
        self.reg = Register.PWRDOWN         # Processed register
        self.mode = Register.MCHIGH         # Measurement mode
        self.mtreg = Params.MTREG_TYP       # MTreg default value
        self.clear_data()

    def clear_data(self):
        """Clear data cache."""
        self.ssd = 0        # Start sample of an annotation data block
        self.bytes = []     # List of recent processed bytes
        self.bits = []      # List of recent processed byte bits

    def start(self):
        """Actions before the beginning of the decoding."""
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putd(self, sb, eb, data):
        """Span data output across bit range.

        - Because bits are order with MSB first, the output is an annotation
          block from the last sample of the start bit (sb) to the first sample
          of the end bit (eb).
        - The higher bit the lower sample number.
        """
        self.put(self.bits[eb][1], self.bits[sb][2], self.out_ann, data)

    def putb(self, sb, eb=None, ann=AnnBits.RESERVED):
        """Span special bit annotation across bit range bit by bit.

        Arguments
        ---------
        sb : integer
            Number of the annotated start bit counting from 0.
        eb : integer
            Number of the end bit right after the last annotated bit
            counting from 0. If none value is provided, the method uses
            start value increased by 1, so that just the first bit will be
            annotated.
        ann : integer
            Index of the special bit's annotation in the annotations list
            `bits`. Default value is for reserved bit.

        """
        annots = hlp.compose_annot(bits[ann])
        for bit in range(sb, eb or (sb + 1)):
            self.put(self.bits[bit][1], self.bits[bit][2],
                     self.out_ann, [ann, annots])

    def check_addr(self, addr_slave):
        """Check correct slave address."""
        if addr_slave in (Address.GND, Address.VCC):
            return True
        ann = AnnInfo.BADADD
        val = hlp.format_data(self.addr, self.options["radix"])
        annots = hlp.compose_annot(info[ann], ann_value=val)
        self.put(self.ss, self.es, self.out_ann, [ann, annots])
        return False

    def calculate_sensitivity(self):
        """Calculate measurement light sensitivity in lux per count."""
        accuracy = prm_map_accuracy[self.options["params"]]
        sensitivity = 1 / accuracy * Params.MTREG_TYP / self.mtreg  # lux/count
        if self.mode in [Register.MCHIGH2, Register.MOHIGH2]:
            sensitivity /= 2
        return sensitivity

    def calculate_light(self, rawdata):
        """Calculate ambient light.

        Arguments
        ---------
        rawdata : int
            Content of the illuminance data register.

        Returns
        -------
        float
            Ambient light in lux.

        """
        light = rawdata * self.calculate_sensitivity()
        return light

    def collect_data(self, databyte):
        """Collect data byte to a data cache."""
        if self.bytes:
            self.bytes.insert(0, databyte)
        else:
            self.ssd = self.ss
            self.bytes.append(databyte)

    def handle_addr(self):
        """Process slave address."""
        if not self.bytes:
            return
        # Registers row
        self.addr = self.bytes[0]
        ann = addr_annots[self.addr]
        annots = hlp.compose_annot(addresses[ann])
        self.put(self.ss, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def handle_reg(self):
        """Process slave register and call its handler."""
        if not (self.bytes and self.write):
            return
        self.reg = self.bytes[0]
        # Handle measurement time registers
        mask_mthigh = ~((1 << (MTregHighBits.MAX + 1)) - 1)
        if (self.reg & mask_mthigh) == Register.MTHIGH:
            self.handle_mtreg_high()
            return
        mask_mtlow = ~((1 << (MTregLowBits.MAX + 1)) - 1)
        if (self.reg & mask_mtlow) == Register.MTLOW:
            self.handle_mtreg_low()
            return
        # Detect measurement mode registers
        if self.reg in range(Register.MCHIGH, Register.MOLOW + 1):
            self.mode = self.reg
        # Registers row
        ann = reg_annots[self.reg]
        annots = hlp.compose_annot(registers[ann])
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def handle_mtreg_high(self):
        """Process measurement time register with high bits."""
        mask = (1 << (MTregLowBits.MAX + 1)) - 1
        self.mtreg &= mask  # Clear high bits
        mtreg = (self.reg << (MTregLowBits.MAX + 1)) & 0xff
        self.mtreg |= mtreg
        self.reg = Register.MTHIGH
        # Bits row - high bits
        bit_min = MTregHighBits.MIN
        bit_max = MTregHighBits.MAX + 1
        self.putb(bit_min, bit_max, AnnBits.DATA)
        # Registers row
        ann = AnnRegs.MTHIGH
        annots = hlp.compose_annot(registers[ann])
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def handle_mtreg_low(self):
        """Process measurement time register with low bits."""
        mask = (1 << (MTregLowBits.MAX + 1)) - 1
        self.mtreg &= ~mask  # Clear low bits
        mtreg = self.reg & mask
        self.mtreg |= mtreg
        self.reg = Register.MTLOW
        # Bits row - low bits
        bit_min = MTregLowBits.MIN
        bit_max = MTregLowBits.MAX + 1
        self.putb(bit_min, bit_max, AnnBits.DATA)
        # Registers row
        ann = AnnRegs.MTLOW
        annots = hlp.compose_annot(registers[ann])
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def handle_nodata(self):
        """Process transmission without any data."""
        # Info row
        ann = AnnInfo.CHECK
        annots = hlp.compose_annot(info[ann])
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_data(self):
        """Process read data."""
        if self.write:
            # Info row
            if self.reg in [Register.MTHIGH, Register.MTLOW]:
                ann = AnnInfo.MTREG
                val = hlp.format_data(self.mtreg, self.options["radix"])
                annots = hlp.compose_annot(info[ann], ann_value=val)
                self.put(self.ssb, self.es, self.out_ann, [ann, annots])
            if self.reg in range(Register.MCHIGH, Register.MOLOW + 1):
                ann = AnnInfo.SENSE
                val = "{:.2f}".format(self.calculate_sensitivity())
                unit = " {}/cnt".format(Params.UNIT_LIGHT)
                annots = hlp.compose_annot(info[ann], ann_value=val,
                                           ann_unit=unit)
                self.put(self.ssb, self.es, self.out_ann, [ann, annots])
        else:
            regword = (self.bytes[1] << 8) + self.bytes[0]
            # Registers row
            ann = AnnRegs.DATA
            annots = hlp.compose_annot(registers[ann])
            self.put(self.ssd, self.es, self.out_ann, [ann, annots])
            # # Info row
            ann = AnnInfo.LIGHT
            val = "{:.2f}".format(self.calculate_light(regword))
            unit = " {}".format(Params.UNIT_LIGHT)
            annots = hlp.compose_annot(info[ann], ann_value=val, ann_unit=unit)
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
            - Parent decoder ``i2c``stores individual bits in the list from
              the least significant bit (LSB) to the most significant bit
              (MSB) as it is at representing numbers in computers, although I2C
              bus transmits data in oposite order with MSB first.
            """
            self.bits = databyte + self.bits
            return

        # State machine
        if self.state == "IDLE":
            """Wait for an I2C transmission."""
            if cmd != "START":
                return
            self.ssb = self.ss
            self.state = "ADDRESS SLAVE"

        elif self.state == "ADDRESS SLAVE":
            """Wait for a slave address."""
            if cmd in ["ADDRESS WRITE", "ADDRESS READ"]:
                if self.check_addr(databyte):
                    self.collect_data(databyte)
                    self.handle_addr()
                    if cmd == "ADDRESS READ":
                        self.write = False
                    elif cmd == "ADDRESS WRITE":
                        self.write = True
                    self.state = "REGISTER ADDRESS"
                else:
                    self.state = "IDLE"

        elif self.state == "REGISTER ADDRESS":
            """Process slave register."""
            if cmd in ["DATA WRITE", "DATA READ"]:
                self.collect_data(databyte)
                self.handle_reg()
                self.state = "REGISTER DATA"
            elif cmd in ["STOP", "START REPEAT"]:
                """End of transmission without any register and data."""
                self.handle_nodata()
                self.state = "IDLE"

        elif self.state == "REGISTER DATA":
            """Process data of a slave register.
            - Individual command or data can end either with repeated start
              condition or with stop condition.
            """
            if cmd in ["DATA WRITE", "DATA READ"]:
                self.collect_data(databyte)
            elif cmd == "START REPEAT":
                """Output read data and continue in transmission."""
                self.handle_data()
                self.ssb = self.ss
                self.state = "ADDRESS SLAVE"
            elif cmd == "STOP":
                """Output formatted string with register data.
                - This is end of an I2C transmission. Start waiting for another
                  one.
                """
                self.handle_data()
                self.state = "IDLE"
