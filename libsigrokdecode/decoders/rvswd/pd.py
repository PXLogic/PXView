##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 perigoso
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

# Helper class to help wrangle bits
class bit_list:
    # Bits are stored as a list of dicts, each dict containing the following
    # keys:
    # - bit: the bit value (0 or 1), or -1 if the bit is invalid
    # - start: the sample number at which the bit started
    # - end: the sample number at which the bit ended

    def __init__(self, endian_msb=True):
        self.reset()

    def __len__(self):
        return len(self._bits)

    def __iter__(self):
        return iter(self._bits)

    def __getitem__(self, key):
        if isinstance(key, slice):
            return self._bits[key.start : key.stop : key.step]
        elif isinstance(key, int) and key >= 0 and key < len(self._bits):
            return self._bits[key]

    def push(self, bit):
        self._curr_bit["bit"] = bit

    def terminate(self, sample):
        self._curr_bit["end"] = sample
        if self._curr_bit["bit"] != -1 and self._curr_bit["start"] != -1:
            self._bits.append(self._curr_bit)
        self._curr_bit = {"bit": -1, "start": sample, "end": -1}

    def reset(self):
        self._curr_bit = {"bit": -1, "start": -1, "end": -1}
        self._bits = []


# Helper class to help list channels and later reference them by name
# Channels are stored as a list of dicts, each dict containing the following
# keys:
# - id: the channel ID
# - name: the channel name
# - desc: the channel description
# the class is initialized with a list of dicts with the above keys
# channel_list(ch_list)
class channel_list:
    def __init__(self, ch_list):
        self._channels = tuple(ch_list)
        self._lookup = {c["id"]: i for i, c in enumerate(self._channels)}

    def index(self, id):
        return self._lookup[id]

    @property
    def list(self):
        return self._channels


# Helper class to help deal with annotations
# Annotations are stored as a list of dicts, each dict containing the following
# keys:
# - id: the annotation ID
# - desc: the annotation description
# - names: list of annotation names, in order of size, from largest to smallest
#          python format strings with a single {data} placeholder are allowed for later formatting
# - row: the annotation row ID
# Annotation rows are stored as a list of dicts, each dict containing the following
# keys:
# - id: the annotation row ID
# - desc: the annotation row description
# the class is initialized with a list of annotation row dicts and a list of annotation dicts with the above keys
# annotation_list(ann_rows, ann_list)
class annotation_list:
    def __init__(self, ann_rows, ann_list):
        self._ann_rows = tuple(ann_rows)
        self._ann_list = tuple(ann_list)
        self._lookup = {c["id"]: i for i, c in enumerate(self._ann_list)}

    def index(self, id):
        return self._lookup[id]

    def __getitem__(self, id):
        if isinstance(id, int):
            return self._ann_list[id]
        else:
            return self._ann_list[self.index(id)]

    def fnames(self, id, data):
        return [name.format(data=data) for name in self[self.index(id)]["names"]]

    @property
    def list(self):
        return tuple((ann["id"], ann["desc"]) for ann in self._ann_list)

    @property
    def row_list(self):
        rows = []
        for row in self._ann_rows:
            anns = []
            for ann in self._ann_list:
                if ann["row"] == row["id"]:
                    anns.append(self.index(ann["id"]))
            rows.append((row["id"], row["desc"], tuple(anns)))
        return tuple(rows)


CHANNELS = channel_list(
    (
        {"id": "clk", "name": "CLK", "desc": "Serial clock line"},
        {"id": "dio", "name": "DIO", "desc": "Serial data line"},
    )
)

ANNOTATIONS = annotation_list(
    (
        {"id": "addr-data", "desc": "Address/data"},
        {"id": "bits", "desc": "Bits"},
    ),
    (
        {"id": "start", "desc": "Start condition", "names": ["START", "S"], "row": "addr-data"},
        {"id": "stop", "desc": "Stop condition", "names": ["STOP", "P"], "row": "addr-data"},
        {
            "id": "address-host",
            "desc": "Address host",
            "names": ["ADDR HOST 0x{data:02x}", "AH 0x{data:02x}", "{data:02x}"],
            "row": "addr-data",
        },
        {
            "id": "address-target",
            "desc": "Address target",
            "names": ["ADDR TARGET 0x{data:02x}", "AT 0x{data:02x}", "{data:02x}"],
            "row": "addr-data",
        },
        {
            "id": "data-host",
            "desc": "Data host",
            "names": ["DATA HOST 0x{data:08x}", "DH 0x{data:08x}", "{data:08x}"],
            "row": "addr-data",
        },
        {
            "id": "data-target",
            "desc": "Data target",
            "names": ["DATA TARGET 0x{data:08x}", "DT 0x{data:08x}", "{data:08x}"],
            "row": "addr-data",
        },
        {
            "id": "parity-host",
            "desc": "Parity host",
            "names": ["PARITY HOST 0x{data:01x}", "PH 0x{data:01x}", "{data:01x}"],
            "row": "addr-data",
        },
        {
            "id": "parity-target",
            "desc": "Parity target",
            "names": ["PARITY TARGET 0x{data:01x}", "PT 0x{data:01x}", "{data:01x}"],
            "row": "addr-data",
        },
        {
            "id": "operation",
            "desc": "Operation",
            "names": ["OPERATION 0x{data:01x}", "OP 0x{data:01x}", "{data:01x}"],
            "row": "addr-data",
        },
        {
            "id": "status",
            "desc": "Status",
            "names": ["STATUS 0x{data:01x}", "ST 0x{data:01x}", "{data:01x}"],
            "row": "addr-data",
        },
        {"id": "bit", "desc": "Bit", "names": ["BIT {data[0]}: {data[1]:b}", "{data[1]:b}"], "row": "bits"},
    ),
)


class Decoder(srd.Decoder):
    api_version = 3
    id = "rvswd"
    name = "RVSWD"
    longname = "RISC-V Serial Wire Debug (WCH)"
    desc = "WCH RISC-V Serial Wire Debug protocol."
    license = "gplv2+"
    inputs = ["logic"]
    outputs = []
    tags = ["Debug/trace"]
    channels = CHANNELS.list
    annotations = ANNOTATIONS.list
    annotation_rows = ANNOTATIONS.row_list
    binary = ()

    def __init__(self):
        self.reset()

    def reset(self):
        self.bits = bit_list()
        self.in_packet = False

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_annotation = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)

    def put_annotation(self, start_sample, end_sample, annotation_id, data=None):
        self.put(
            start_sample,
            end_sample,
            self.out_annotation,
            [ANNOTATIONS.index(annotation_id), ANNOTATIONS.fnames(annotation_id, data)],
        )

    def handle_start_condition(self):
        self.bits.reset()
        self.put_annotation(self.samplenum, self.samplenum, "start")
        self.in_packet = True

    def handle_stop_condition(self):
        self.put_annotation(self.samplenum, self.samplenum, "stop")
        self.in_packet = False

    def handle_bit(self, pins):
        clk, dio = pins

        if clk == 0:
            self.bits.terminate(self.samplenum)
        else:
            self.bits.push(dio)

    def process_packet(self):
        if len(self.bits) == 52:
            self.process_short_packet()
        elif len(self.bits) == 84:
            self.process_long_packet()
        else:
            print("invalid packet length: {}".format(len(self.bits)))

    def process_short_packet(self):
        for i, b in enumerate(self.bits):
            self.put_annotation(b["start"], b["end"], "bit", [i, b["bit"]])

        # short format
        self.put_annotation_bits(self.bits[0:7], "address-host")
        self.put_annotation_bits(self.bits[7:8], "operation")
        self.put_annotation_bits(self.bits[8:9], "parity-host")
        self.put_annotation_bits(self.bits[14:46], "data-target")
        self.put_annotation_bits(self.bits[46:47], "parity-target")

    def process_long_packet(self):
        for i, b in enumerate(self.bits):
            self.put_annotation(b["start"], b["end"], "bit", [i, b["bit"]])

        self.put_annotation_bits(self.bits[0 : 7], "address-host")
        self.put_annotation_bits(self.bits[7 : 39], "data-host")
        self.put_annotation_bits(self.bits[39 : 41], "operation")
        self.put_annotation_bits(self.bits[41 : 42], "parity-host")
        self.put_annotation_bits(self.bits[42 : 49], "address-target")
        self.put_annotation_bits(self.bits[49 : 81], "data-target")
        self.put_annotation_bits(self.bits[81 : 83], "status")
        self.put_annotation_bits(self.bits[83 : 84], "parity-target")

    def put_annotation_bits(self, bits, annotation_id):
        if len(bits) == 0:
            return

        end_index = len(bits) - 1
        start_sample = bits[0]["start"]
        end_sample = bits[end_index]["end"]

        data = 0
        for bit in bits:
            data <<= 1
            data |= bit["bit"]

        self.put_annotation(start_sample, end_sample, annotation_id, data)

    def decode(self):
        clk = CHANNELS.index("clk")
        dio = CHANNELS.index("dio")

        while True:
            # State machine.
            if not self.in_packet:
                # Wait for a START condition (S): CLK = high, DIO = falling.
                pins = self.wait({clk: "h", dio: "f"})
                self.handle_start_condition()
            else:
                # Wait for any of the following conditions (or combinations):
                #  a) Data sampling of receiver: CLK = rising/falling, and/or
                #  c) STOP condition (P): CLK = high (not rising or falling), DIO = rising
                pins = self.wait([{clk: "r"}, {clk: "f"}, {clk: "h", dio: "r"}])

                if self.matched == (False, False, True):
                    self.process_packet()
                    self.handle_stop_condition()
                else:
                    self.handle_bit(pins)
