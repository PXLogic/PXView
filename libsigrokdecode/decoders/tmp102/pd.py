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
class GeneralCall:
    """Enumeration of parameters for general call on I2C bus."""

    (ADDRESS, WRITE, RESET) = (0x00, 0x04, 0x06)


class Address:
    """Enumeration of possible slave addresses.

    - Device address is determined by a pin, which the sensor's ADD0 pin is
      physically connected to.
    """

    (GND, VCC, SDA, SCL) = (0x48, 0x49, 0x4a, 0x4b)


class Register:
    """Enumeration of possible slave register addresses."""

    (TEMP, CONF, TLOW, THIGH) = range(4)


class ConfigBits:
    """Enumeration of bits in the configuration register."""

    (
        # Byte 2 (LSB)
        EM,     # Extended mode
        AL,     # Alert
        CR0,    # Conversion rate
        CR1,
        # Byte 1 (MSB)
        SD,     # Shutdown mode
        TM,     # Thermostat mode
        POL,    # Polarity
        F0,     # Fault queue
        F1,
        R0,     # Converter resolution
        R1,
        OS,     # One-shot conversion
    ) = range(4, 16)


class TempBits:
    """Enumeration of specific temperature register bits."""

    (EM, RESERVED) = (0, 1)


class Params:
    """Typical slave parameters."""

    (CUSTOM, POWERUP) = (0, 0x60a0)


###############################################################################
# Enumeration classes for annotations
###############################################################################
class AnnAddrs:
    """Enumeration of annotations for addresses."""

    (GC, GND, VCC, SDA, SCL) = range(5)


class AnnRegs:
    """Enumeration of annotations for registers."""

    (
        RESET, CONF, TEMP, TLOW, THIGH,
    ) = range(AnnAddrs.SCL + 1, (AnnAddrs.SCL + 1) + 5)


class AnnBits:
    """Enumeration of annotations for configuration bits."""

    (
        RESERVED, DATA,
        EM, AL, CR0, SD, TM, POL, F0, R0, OS,
    ) = range(AnnRegs.THIGH + 1, (AnnRegs.THIGH + 1) + 11)


class AnnInfo:
    """Enumeration of annotations for formatted info."""

    (
        WARN, BADADD, GRST, CHECK, WRITE, READ, SELECT, CUSTOM, PWRUP,
        CONF, TEMP, TLOW, THIGH,
    ) = range(AnnBits.OS + 1, (AnnBits.OS + 1) + 13)


###############################################################################
# Parameters mapping
###############################################################################
addr_annots = {  # Convert value to annotation index
    GeneralCall.ADDRESS: AnnAddrs.GC,
    Address.GND: AnnAddrs.GND,
    Address.VCC: AnnAddrs.VCC,
    Address.SDA: AnnAddrs.SDA,
    Address.SCL: AnnAddrs.SCL,
}

reg_annots_gc = {  # Convert general call register value to annotation index
    GeneralCall.RESET: AnnRegs.RESET,
}

reg_annots = {  # Convert device register value to annotation index
    Register.CONF: AnnRegs.CONF,
    Register.TEMP: AnnRegs.TEMP,
    Register.TLOW: AnnRegs.TLOW,
    Register.THIGH: AnnRegs.THIGH,
}

prm_annots = {  # Convert device parameter value to annotation index
    Params.CUSTOM: AnnInfo.CUSTOM,
    Params.POWERUP: AnnInfo.PWRUP,
}

rates = {
    0b00: "0.25",
    0b01: "1",
    0b10: "4",
    0b11: "8",
}

faults = {
    0b00: "1",
    0b01: "2",
    0b10: "4",
    0b11: "6",
}

resolutions = {
    0b11: "12",
}

temp_units = {  # Convert temperature scale option to measurement unit
    "Celsius": "°C",
    "Fahrenheit": "°F",
    "Kelvin": "K",
}


###############################################################################
# Parameters anotations definitions
###############################################################################
addresses = {
    AnnAddrs.GC: ["General call", "GEN_CALL", "GC", "G"],
    AnnAddrs.GND: ["ADD0 grounded", "ADD0_GND", "AG"],
    AnnAddrs.VCC: ["ADD0 powered", "ADD0_VCC", "AV"],
    AnnAddrs.SDA: ["ADD0 to SDA", "ADD0_SDA", "AD"],
    AnnAddrs.SCL: ["ADD0 to SCL", "ADD0_SSCL", "AC"],
}

registers = {
    AnnRegs.RESET: ["Reset register", "Reset", "Rst", "R"],
    AnnRegs.CONF: ["Configuration register", "Configuration", "Conf",
                   "Cfg", "C"],
    AnnRegs.TEMP: ["Temperature register", "Temperature", "Temp", "T"],
    AnnRegs.TLOW: ["Low alert register", "Low alert", "Tlow", "L"],
    AnnRegs.THIGH: ["High alert register", "High alert", "Thigh", "H"],
}

bits = {
    AnnBits.RESERVED: ["Reserved", "Rsvd", "R"],
    AnnBits.DATA: ["Data", "D"],
    AnnBits.EM: ["Extended mode", "EM", "E"],
    AnnBits.AL: ["Alert", "AL", "A"],
    AnnBits.CR0: ["Conversion rate", "Rate", "R"],
    AnnBits.SD: ["Shutdown mode", "Shutdown", "Shtd", "SD", "S"],
    AnnBits.TM: ["Thermostat mode", "Thermostat", "TMode", "TM", "T"],
    AnnBits.POL: ["Polarity", "Pol", "P"],
    AnnBits.F0: ["Consecutive faults", "Faults", "Flts", "F"],
    AnnBits.R0: ["Converter resolution", "Resolution", "Res", "R"],
    AnnBits.OS: ["One-shot conversion", "Oneshot", "OS", "O"],
}

info = {
    AnnInfo.WARN: ["Warnings", "Warn", "W"],
    AnnInfo.BADADD: ["Uknown slave address", "Unknown address", "Uknown",
                     "Unk", "U"],
    AnnInfo.GRST: ["General reset", "GenReset", "GRST", "Rst", "R"],
    AnnInfo.CHECK: ["Slave presence check", "Slave check", "Check",
                    "Chk", "C"],
    AnnInfo.WRITE: ["Write", "Wr", "W"],
    AnnInfo.READ: ["Read", "Rd", "R"],
    AnnInfo.SELECT: ["Select", "Sel", "S"],
    AnnInfo.CUSTOM: ["Custom", "Cst", "C"],
    AnnInfo.PWRUP: ["Power-up reset", "PwrReset", "Pwr", "P"],
    AnnInfo.CONF: ["Configuration", "Conf", "Cfg", "C"],
    AnnInfo.TEMP: ["Measured temperature", "Temperature", "Temp", "T"],
    AnnInfo.TLOW: ["Low temperature limit", "Low limit", "Low", "L"],
    AnnInfo.THIGH: ["High temperature limit", "High limit", "High", "H"],
}


###############################################################################
# Decoder
###############################################################################
class Decoder(srd.Decoder):
    """Protocol decoder for digital temperature sensor ``TMP102``."""

    api_version = 3
    id = "tmp102"
    name = "TMP102"
    longname = "Digital temperature sensor TMP102"
    desc = "Low power digital temperature sensor."
    license = "gplv2+"
    inputs = ["i2c"]
    outputs = ["tmp102"]
    tags = ['Embedded/industrial']

    options = (
        {"id": "radix", "desc": "Number format", "default": "Hex",
         "values": ("Hex", "Dec", "Oct", "Bin")},
        {"id": "units", "desc": "Temperature unit", "default": "Celsius",
         "values": ("Celsius", "Fahrenheit", "Kelvin")},
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
        ("bits", "Bits", tuple(range(AnnBits.RESERVED, AnnBits.OS + 1))),
        ("regs", "Registers", tuple(range(AnnAddrs.GC, AnnRegs.THIGH + 1))),
        ("info", "Info", tuple(range(AnnInfo.GRST, AnnInfo.THIGH + 1))),
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
        self.write = True   # Flag about recent write action (default write)
        self.state = "IDLE"
        # Specific parameters for a device
        self.addr = Address.GND     # Slave address (default ADD0 grounded)
        self.reg = Register.TEMP    # Processed slave register (default temp)
        self.em = False             # Flag about extended mode (default Normal)
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

    def check_addr(self, addr_slave, check_gencall=False):
        """Check correct slave address or general call."""
        if addr_slave in (
            Address.GND,
            Address.VCC,
            Address.SDA,
            Address.SCL,
        ) or not check_gencall or addr_slave == GeneralCall.ADDRESS:
            return True
        ann = AnnInfo.BADADD
        val = hlp.format_data(self.addr, self.options["radix"])
        annots = hlp.compose_annot(info[ann], ann_value=val)
        self.put(self.ss, self.es, self.out_ann, [ann, annots])
        return False

    def calculate_temperature(self, rawdata):
        """Calculate and convert temperature.

        Arguments
        ---------
        rawdata : int
            Content of the temperature, TLOW, or THIGH register.

        Returns
        -------
        tuple: float, string
            Temperature and unit in a scale determined by corresponding decoder
            option.

        """
        if rawdata & (1 << TempBits.EM):
            self.em = True
        # Extended mode (13-bit resolution)
        if self.em:
            rawdata >>= 3
            if rawdata > 0x0fff:
                rawdata |= 0xe000  # 2s complement
        # Normal mode (12-bit resolution)
        else:
            rawdata >>= 4
            if rawdata > 0x07ff:
                rawdata |= 0xf000  # 2s complement
        temperature = rawdata / 16  # Celsius
        if self.options["units"] == "Fahrenheit":
            temperature *= 9 / 5
            temperature += 32
        elif self.options["units"] == "Kelvin":
            temperature += 273.15
        # Measurement unit
        unit = " {}".format(temp_units[self.options["units"]])
        return temperature, unit

    def collect_data(self, databyte):
        """Collect data byte to a data cache."""
        if self.bytes:
            self.bytes.insert(0, databyte)
        else:
            self.ssd = self.ss
            self.bytes.append(databyte)

    def format_rw(self):
        """Format read/write action."""
        act = (AnnInfo.READ, AnnInfo.WRITE)[self.write]
        return info[act]

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
        """Process slave register."""
        if not self.bytes:
            return
        self.reg = self.bytes[0]
        if self.addr == GeneralCall.ADDRESS:
            ann = reg_annots_gc[self.reg]
            act = None
        else:
            ann = reg_annots[self.reg]
            act = info[AnnInfo.SELECT]
        annots = hlp.compose_annot(registers[ann], ann_action=act)
        self.put(self.ss, self.es, self.out_ann, [ann, annots])
        self.clear_data()

    def handle_nodata(self):
        """Process transmission without any data."""
        # Info row
        ann = AnnInfo.CHECK
        annots = hlp.compose_annot(info[ann])
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_data(self):
        """Create name and call corresponding data register handler."""
        fn = getattr(self, "handle_datareg_{:#04x}".format(self.reg))
        dataword = ((self.bytes[1] << 8) + self.bytes[0]) if (self.bytes) \
            else None
        fn(dataword)
        self.clear_data()

    def handle_datareg_0x06(self, dataword):
        """Process general reset register."""
        # Info row
        ann = AnnInfo.GRST
        annots = hlp.compose_annot(info[ann])
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_datareg_0x01(self, dataword):
        """Process configuration register."""
        # Bits row - OS bit - one-shot measurement
        os = dataword >> ConfigBits.OS & 1
        os_l = ("dis", "en")[os] + "abled"
        os_s = os_l[0].upper()
        ann = AnnBits.OS
        annots = hlp.compose_annot(bits[ann], [os, os_l, os_s])
        self.putd(ConfigBits.OS, ConfigBits.OS, [ann, annots])
        # Bits row - R0/R1 bits - converter resolution
        res = resolutions[dataword >> ConfigBits.R0 & 0b11]
        ann = AnnBits.R0
        val = "{}".format(res)
        annots = hlp.compose_annot(bits[ann], ann_value=val, ann_unit="bit")
        self.putd(ConfigBits.R0, ConfigBits.R1, [ann, annots])
        # Bits row - F0/F1 bits - fault queue
        flt = faults[dataword >> ConfigBits.F0 & 0b11]
        ann = AnnBits.F0
        val = "{}".format(flt)
        annots = hlp.compose_annot(bits[ann], ann_value=val)
        self.putd(ConfigBits.F0, ConfigBits.F1, [ann, annots])
        # Bits row - POL bit - polarity, alert active
        pol = dataword >> ConfigBits.POL & 1
        pol_l = ("low", "high")[pol]
        pol_s = pol_l[0].upper()
        ann = AnnBits.POL
        annots = hlp.compose_annot(bits[ann], ann_value=[pol, pol_l, pol_s])
        self.putd(ConfigBits.POL, ConfigBits.POL, [ann, annots])
        # Bits row - TM bit - thermostat mode
        tm = dataword >> ConfigBits.TM & 1
        tm_l = ("comparator", "interrupt")[tm]
        tm_s = tm_l[0].upper()
        ann = AnnBits.TM
        annots = hlp.compose_annot(bits[ann], ann_value=[tm, tm_l, tm_s])
        self.putd(ConfigBits.TM, ConfigBits.TM, [ann, annots])
        # Bits row - SD bit - shutdown mode
        sd = dataword >> ConfigBits.SD & 1
        sd_l = ("dis", "en")[sd] + "abled"
        sd_s = sd_l[0].upper()
        ann = AnnBits.SD
        annots = hlp.compose_annot(bits[ann], ann_value=[sd, sd_l, sd_s])
        self.putd(ConfigBits.SD, ConfigBits.SD, [ann, annots])
        # Bits row - CR0/CR1 bits - conversion rate
        rate = rates[dataword >> ConfigBits.CR0 & 0b11]
        ann = AnnBits.CR0
        annots = hlp.compose_annot(bits[ann], ann_value=rate, ann_unit="Hz")
        self.putd(ConfigBits.CR0, ConfigBits.CR1, [ann, annots])
        # Bits row - AL bit - alert
        al = dataword >> ConfigBits.AL & 1
        al_l = ("", "in")[al ^ pol] + "active"
        al_s = al_l[0].upper()
        ann = AnnBits.AL
        annots = hlp.compose_annot(bits[ann], ann_value=[al, al_l, al_s])
        self.putd(ConfigBits.AL, ConfigBits.AL, [ann, annots])
        # Bits row - EM bit - extended mode
        em = dataword >> ConfigBits.EM & 1
        self.em = bool(em)
        em_l = ("dis", "en")[em] + "abled"
        em_s = em_l[0].upper()
        ann = AnnBits.EM
        annots = hlp.compose_annot(bits[ann], ann_value=[em, em_l, em_s])
        self.putd(ConfigBits.EM, ConfigBits.EM, [ann, annots])
        # Bits row - reserved bits
        for i in range(ConfigBits.EM - 1, -1, -1):
            self.putb(i)
        # Registers row
        ann = AnnRegs.CONF
        val = hlp.format_data(dataword, self.options["radix"])
        annots = hlp.compose_annot(registers[ann], ann_value=val)
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        # Info row
        ann = AnnInfo.CONF
        val = info[prm_annots[
                (Params.CUSTOM, dataword)[dataword == Params.POWERUP]]]
        act = self.format_rw()
        annots = hlp.compose_annot(info[ann], ann_value=val, ann_action=act)
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_datareg_0x00(self, dataword):
        """Process temperature register."""
        temp, unit = self.calculate_temperature(dataword)
        # Bits row - EM bit - extended mode
        em = int(self.em)
        em_l = ("dis", "en")[self.em] + "abled"
        em_s = em_l[0].upper()
        ann = AnnBits.EM
        annots = hlp.compose_annot(bits[ann], [em, em_l, em_s])
        self.putd(TempBits.EM, TempBits.EM, [ann, annots])
        # Bits row - reserved bits
        res_bits = (3, 2)[self.em]
        bit_min = TempBits.RESERVED
        bit_max = bit_min + res_bits
        self.putb(bit_min, bit_max)
        # Bits row - data bits
        data_bits = 8 * len(self.bytes) - 1 - res_bits
        bit_min = bit_max
        bit_max = bit_min + data_bits
        self.putb(bit_min, bit_max, AnnBits.DATA)
        # Registers row
        ann = AnnRegs.TEMP
        val = hlp.format_data(dataword, self.options["radix"])
        annots = hlp.compose_annot(registers[ann], ann_value=val)
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        # Info row
        ann = AnnInfo.TEMP
        annots = hlp.compose_annot(info[ann], ann_value=temp, ann_unit=unit)
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_datareg_0x02(self, dataword):
        """Process TLOW register."""
        temp, unit = self.calculate_temperature(dataword)
        # Registers row
        ann = AnnRegs.TLOW
        val = hlp.format_data(dataword, self.options["radix"])
        annots = hlp.compose_annot(registers[ann], ann_value=val)
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        # Info row
        ann = AnnInfo.TLOW
        act = self.format_rw()
        annots = hlp.compose_annot(info[ann], ann_value=temp, ann_unit=unit,
                                   ann_action=act)
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

    def handle_datareg_0x03(self, dataword):
        """Process THIGH register."""
        temp, unit = self.calculate_temperature(dataword)
        # Registers row
        ann = AnnRegs.THIGH
        val = hlp.format_data(dataword, self.options["radix"])
        annots = hlp.compose_annot(registers[ann], ann_value=val)
        self.put(self.ssd, self.es, self.out_ann, [ann, annots])
        # Info row
        ann = AnnInfo.THIGH
        act = self.format_rw()
        annots = hlp.compose_annot(info[ann], ann_value=temp, ann_unit=unit,
                                   ann_action=act)
        self.put(self.ssb, self.es, self.out_ann, [ann, annots])

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
                if self.check_addr(databyte, check_gencall=True):
                    self.collect_data(databyte)
                    self.handle_addr()
                    if cmd == "ADDRESS READ":
                        self.write = False
                        self.state = "REGISTER DATA"
                    elif cmd == "ADDRESS WRITE":
                        self.write = True
                        self.state = "REGISTER ADDRESS"
                else:
                    self.state = "IDLE"

        elif self.state == "REGISTER ADDRESS":
            """Process slave register"""
            if cmd in ["DATA WRITE", "DATA READ"]:
                self.collect_data(databyte)
                self.handle_reg()
                self.state = "REGISTER DATA"
            elif cmd in ["STOP", "START REPEAT"]:
                """Output end of transmission without any register and data."""
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
                self.state = "ADDRESS SLAVE"
            elif cmd == "STOP":
                """Output formatted string with register data.
                - This is end of an I2C transmission. Start waiting for another
                  one.
                """
                self.handle_data()
                self.state = "IDLE"
