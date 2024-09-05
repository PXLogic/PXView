##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 Libor Tomsik <libor@tomsik.eu>
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

PREAMBLE_MASTER = 0b101100011100010101  # 18bits
PREAMBLE_SLAVE  = 0b101010100011100011  # 18bits
PREAMBLE_LENGTH = 18
PREAMBLE_MASK = 0
for i in range(0, 18):
    PREAMBLE_MASK += (1 << i)

F_codes = ["PD 2B",
           "PD 4B",
           "PD 8B",
           "PD 16B",
           "PD 32B",
           "reserved",
           "reserved",
           "reserved",
           "Master transfer",  # Master to  Master
           "General event",  # Master
           "reserved",
           "reserved",
           "MD",  #(0x0C) Selected device
           "Group event",  # Master
           "Single event",  # Master
           "Device status"]  # Master or monitor


def bits_to_bytes(binary_string):
    # print(f"bs: {binary_string} len: {len(binary_string)}")
    byte_array = bytearray()
    for i in range(0, len(binary_string), 8):
        byte = int(binary_string[i:i + 8], 2)
        byte_array.append(byte)
    return byte_array


crc_polynome = '11100101'


# Returns XOR of 'a' and 'b'
# (both of same length)
def xor(a, b):
    # initialize result
    result = []

    # Traverse all bits, if bits are
    # same, then XOR is 0, else 1
    for i in range(1, len(b)):
        if a[i] == b[i]:
            result.append('0')
        else:
            result.append('1')

    return ''.join(result)


# Performs Modulo-2 division
def mod2div(dividend, divisor):
    # Number of bits to be XORed at a time.
    pick = len(divisor)

    # Slicing the dividend to appropriate
    # length for particular step
    tmp = dividend[0: pick]

    while pick < len(dividend):

        if tmp[0] == '1':

            # replace the dividend by the result
            # of XOR and pull 1 bit down
            tmp = xor(divisor, tmp) + dividend[pick]

        else:  # If leftmost bit is '0'
            # If the leftmost bit of the dividend (or the
            # part used in each step) is 0, the step cannot
            # use the regular divisor; we need to use an
            # all-0s divisor.
            tmp = xor('0' * pick, tmp) + dividend[pick]

        # increment pick to move further
        pick += 1

    # For the last n bits, we have to carry it out
    # normally as increased value of pick will cause
    # Index Out of Bounds.
    if tmp[0] == '1':
        tmp = xor(divisor, tmp)
    else:
        tmp = xor('0' * pick, tmp)

    checkword = tmp
    return checkword


def parity(data):
    res = 0
    for i in data:
        res = res ^ int(i, 2)
    return str(res)


def invert(data):
    res = ""
    for i in data:
        res = res + str(1 ^ int(i, 2))
    return res


def encode_data(data, key):
    l_key = len(key)
    # Appends n-1 zeroes at end of data
    appended_data = data + '0' * (l_key - 1)
    remainder = mod2div(appended_data, key)
    remainder = invert(remainder + parity(data + remainder))
    return remainder


def check_check_sequence(mvb_frame):
    calculated_check = encode_data(mvb_frame[:-8], crc_polynome)
    received_check = mvb_frame[-8:]
    return calculated_check == received_check


class Decoder(srd.Decoder):
    api_version = 3
    id = 'mvb'
    name = 'MVB'
    longname = 'Multifunction Vehicle Bus'
    desc = 'Multifunction Vehicle Bus Manchester II with custom preamble.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    channels = (
        {'id': 'mvb', 'name': 'MVB', 'desc': 'TTL from RS485'},
    )
    optional_channels = ()
    tags = ['Frame']
    annotations = (
        ('master_preamble', 'Master preamble'),
        ('slave_preamble', 'Slave preamble'),
        ('master_data', 'Master data'),
        ('f_code', 'Function code'),
        ('slave_data', 'Slave data'),
        ('crc', 'CRC'),
        ('crc_error', 'CRC Error'),
        ('bit', 'Bit'),
        ('addr', 'Address'),
    )
    MASTER_PREAMBLE, SLAVE_PREAMBLE, MASTER_DATA, FUNCTION_CODE, SLAVE_DATA, CRC, CRC_ERROR, BITS, ADDRESS = range(len(annotations))
    annotation_rows = (
        ('bits', 'Bits', (MASTER_PREAMBLE, SLAVE_PREAMBLE, BITS,)),
        ('crcs', 'Check sequence', (CRC,)),
        ('ma-sl-data', 'Data', (MASTER_DATA, SLAVE_DATA)),
        ('f_codes', 'Function code', (FUNCTION_CODE, ADDRESS)),
        ('errors', 'Decoding errors', (CRC_ERROR,))
    )

    matching_header_ticks = 0
    received_master_header = False
    received_slave_header = False
    last_tick = 0
    isEvenTick = True  # 0 is taken as even number
    decoded_buffer = ""
    frame_data_begin = 0
    mvb_samples_per_bit = 2
    sample_begin = 0
    sample_end = 0

    def __init__(self, **kwargs):
        self.state = 'FIND START'
        # And various other variable initializations...

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def reset(self):
        self.matching_header_ticks = 0
        self.last_tick = 0
        self.reset_frame()

    def process_slave_frame(self, decoded_manchester):
        ret = len(decoded_manchester)
        if ret == 24:
            data_segment_sample_begin = self.frame_data_begin
            data_segment_sample_end = data_segment_sample_begin + 16 * self.mvb_samples_per_bit
            check_ok = check_check_sequence(decoded_manchester)
            if check_ok:
                self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC, [f"{hex(int(decoded_manchester[-8:], 2))}"]])
                sub_frame_data = bits_to_bytes(decoded_manchester[:-8])
                data = ''.join([hex(b)[2:].zfill(2) for b in sub_frame_data])
                self.put(data_segment_sample_begin, data_segment_sample_end, self.out_ann, [self.SLAVE_DATA, [f"0x{data}"]])
                return sub_frame_data
            else:
                self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC_ERROR, [f"CRC Error"]])
                return bytearray()
            
        if ret == 40:
            data_segment_sample_begin = self.frame_data_begin
            data_segment_sample_end = data_segment_sample_begin + 32 * self.mvb_samples_per_bit
            check_ok = check_check_sequence(decoded_manchester)
            if check_ok:
                self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC, [f"{hex(int(decoded_manchester[-8:], 2))}"]])
                sub_frame_data = bits_to_bytes(decoded_manchester[:-8])
                data = ''.join([hex(b)[2:].zfill(2) for b in sub_frame_data])
                self.put(data_segment_sample_begin, data_segment_sample_end, self.out_ann, [self.SLAVE_DATA, [f"0x{data}"]])
                return sub_frame_data
            else:
                self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC_ERROR, [f"CRC Error"]])
                return bytearray()
            
        else:
            data_segment_length = 64 + 8  # 64 bits data and 8 bits crc
            segments = int(len(decoded_manchester) / data_segment_length)
            # print(f"8B Segments: {segments}")
            slave_data = bytearray()
            for i in range(0, segments):
                data_segment_sample_begin = self.frame_data_begin + i * data_segment_length * self.mvb_samples_per_bit
                data_segment_sample_end = data_segment_sample_begin + 64 * self.mvb_samples_per_bit
                sub_sequence = decoded_manchester[i * data_segment_length:(i + 1) * data_segment_length]
                if check_check_sequence(sub_sequence):
                    self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC, [f"{hex(int(sub_sequence[-8:], 2))}"]])
                    slave_data += bits_to_bytes(sub_sequence[:-8])
                else:
                    self.put(data_segment_sample_end, data_segment_sample_end + 8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC_ERROR, [f"CRC Error"]])
                    return bytearray()

            data = ''.join([hex(b)[2:].zfill(2) for b in slave_data])
            self.put(self.frame_data_begin, self.frame_data_begin + len(decoded_manchester) * self.mvb_samples_per_bit, self.out_ann, [self.SLAVE_DATA, [f"0x{data}"]])
            return slave_data

    def process_master_frame(self, decoded_manchester):
        # print(f"Master frame:{decoded_manchester}")
        check_ok = check_check_sequence(decoded_manchester)
        addr_begin = self.frame_data_begin + 16 * self.mvb_samples_per_bit
        if check_ok:
            self.put(addr_begin, addr_begin +  8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC, [f"{hex(int(decoded_manchester[-8:], 2))}"]])
            # Extract the flag (first 4 bits)
            flag_bits = decoded_manchester[:4]
            flag = int(flag_bits, 2)

            # Extract the address (next 12 bits)
            address_bits = decoded_manchester[4:16]
            address = int(address_bits, 2)

            # Print the results
            # print(f"Master:{len(frame) - len(master_frame) - len(frame_end)} {frame} d:{decoded_manchester} f:{F_codes[flag]} a:{address}")
            return [flag, address]
        else:
            self.put(addr_begin, addr_begin +  8 * self.mvb_samples_per_bit, self.out_ann, [self.CRC_ERROR, [f"CRC Error"]])
            return [0, 0]

    def reset_frame(self):
        self.isEvenTick = True
        self.decoded_buffer = ""
        self.received_master_header = False
        self.received_slave_header = False

    def process_tick(self, tick_value):
        # print(int(tick_value), end="")
        # return
        if not self.received_master_header and not self.received_slave_header:
            self.matching_header_ticks = ((self.matching_header_ticks << 1) + int(tick_value)) & PREAMBLE_MASK
            if not self.received_master_header and not self.received_slave_header:
                if self.matching_header_ticks == PREAMBLE_MASTER:
                    self.received_master_header = True
                    self.matching_header_ticks = 0
                    self.put(self.samplenum - int((PREAMBLE_LENGTH+1) * self.mvb_samples_per_bit / 2), self.sample_end, self.out_ann, [self.MASTER_PREAMBLE, ['Master p']])
                    self.frame_data_begin = self.sample_end
                if self.matching_header_ticks == PREAMBLE_SLAVE:
                    self.received_slave_header = True
                    self.matching_header_ticks = 0
                    self.put(self.samplenum - int((PREAMBLE_LENGTH+1) * self.mvb_samples_per_bit / 2), self.sample_end, self.out_ann, [self.SLAVE_PREAMBLE, ['Slave p']])
                    self.frame_data_begin = self.sample_end
            return True

        if self.received_master_header or self.received_slave_header:
            # print(f" master or slave ready pt:{self.last_tick} t:{tick_value}")
            if not self.isEvenTick:
                bit_begin = self.sample_end - self.mvb_samples_per_bit
                if self.last_tick == 0 and tick_value == 1:
                    self.decoded_buffer += "0"
                    self.put(bit_begin, self.sample_end, self.out_ann, [self.BITS, [f"0"]])
                elif self.last_tick == 1 and tick_value == 0:
                    self.decoded_buffer += "1"
                    self.put(bit_begin, self.sample_end, self.out_ann, [self.BITS, [f"1"]])
                else:
                    # print(f"End of decoding 00 buffer:{self.decoded_buffer}")
                    if self.received_master_header:
                        master_data = self.process_master_frame(self.decoded_buffer)
                        # print(f"Master f_code:{F_codes[master_data[0]]} address:{master_data[1]}")
                        addr_begin = self.frame_data_begin + 4 * self.mvb_samples_per_bit
                        self.put(self.frame_data_begin, addr_begin, self.out_ann, [self.MASTER_DATA, [hex(master_data[0])]])
                        self.put(addr_begin, addr_begin + 12 * self.mvb_samples_per_bit, self.out_ann, [self.MASTER_DATA, [hex(master_data[1])]])
                        self.put(self.frame_data_begin, self.frame_data_begin + 4 * self.mvb_samples_per_bit, self.out_ann, [self.FUNCTION_CODE, [f"{F_codes[master_data[0]]}"]])
                        if master_data[0] in {0, 1, 2, 3, 4}:
                            self.put(addr_begin, addr_begin + 12 * self.mvb_samples_per_bit, self.out_ann, [self.ADDRESS, [f"{master_data[1]}"]])
                        self.reset_frame()
                        return False
                    if self.received_slave_header:
                        self.process_slave_frame(self.decoded_buffer)
                        self.reset_frame()
                        return False
                    self.reset_frame()
            self.isEvenTick = not self.isEvenTick
            self.last_tick = tick_value
        return True  # frame not yet found

    def decode(self):
        MVB_CLOCK_RATE = 3e6
        samples_per_tick = int(self.samplerate / MVB_CLOCK_RATE)
        self.mvb_samples_per_bit = 2 * samples_per_tick
        # print(f"Sample rate:{self.samplerate} {self.mvb_samples_per_bit}")
        self.wait({0: 'f'})
        notch_begin = self.samplenum
        phase = False
        while True:
            self.wait({0: 'e'})
            notch_length = self.samplenum - notch_begin
            notch_length_mvb = int(round(notch_length / samples_per_tick))
            # print(f"{1 if phase else 0} notch duration:{notch_length} {notch_length_mvb}")
            if notch_length_mvb < 4:
                for i in range(notch_length_mvb):
                    self.sample_begin = notch_begin + i * samples_per_tick
                    self.sample_end = notch_begin + (i+1) * samples_per_tick
                    self.process_tick(1 if phase else 0)
            else:
                self.process_tick(1)
                self.reset_frame()

            phase = not phase
            notch_begin = self.samplenum