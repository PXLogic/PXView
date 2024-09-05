##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2019 darell tan
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
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
##


import sigrokdecode as srd

class Decoder(srd.Decoder):
    api_version = 3
    id       = 'swi'
    name     = 'SWI'
    longname = 'Toy Decoder'
    desc     = 'A very simple decoder'
    license  = 'gplv2+'
    inputs   = ['logic']
    outputs  = []
    tags     = ['Clock/timing', 'Util']
    channels = (
        {'id': 'swi', 'name': 'SWI', 'desc': 'SWI channel'},
    )
    options = (
    )
    annotations = (
        ('baud_rate', 'Bauds'),
        ('bits', 'Bits'),
        ('bytes', 'Bytes'),
        ('err', 'Errors'),
        ('mean', 'Means'),
        ('pbytes', 'Byte'),
        ('nmbr', 'Number'),
    )
    annotation_rows = (
        ('bauds', 'Timing', (0,)),
        ('bits_a', 'Bits', (1,)),
        ('data', 'Words', (2,)),
        ('errors', 'Errors', (3,)),
        ('meanings', 'Meaning', (4,)),
        ('meanings_data', 'Data', (5,)),
        ('numbs', 'Numbers', (6,)),
    )


    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None

        self.strt = 0
        self.halfRate = 0

        self.pastNs = [0]
        self.pastVs = ["1"]

    ## word = [startN (0), endN (1), type_int (2), data_int (3), bit_string (4), inverted (5)]
        self.pastWords = []
        self.lastHdrIdx = 0
        self.packetClass = -1
        self.recieveData = 0

    ## packet = [startN (0), endN (1), recieve (2), first_two_bytes (3), last_byte (4), packetClass (5), recieve (6)]
        self.pastPackets = []
        self.readPacketSeq = 0
        self.polling = 0

        self.readOdcNumber = 0

        self.pastBits = ""
        self.pastUidData = []
        self.startUidByte = 0
        self.bitsIdx = 0
        self.enumIdx = 0


    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def save_log(self, v, n):
        self.pastNs.append(n)
        self.pastVs.append(v)

    def parse_enumerate(self, word):
        if word[4][8:] == "0000":
            self.put(word[0], word[1], self.out_ann, [4, ["Enum Start", "Enum"]])
            self.pastBits = ""
            self.bitsIdx = 0
            self.enumIdx = 0
        elif word[4][8:] == "0110":
            self.put(word[0], word[1], self.out_ann, [4, ["Request 0s?", "0s?"]])
        elif word[4][8:] == "0111":
            self.put(word[0], word[1], self.out_ann, [4, ["Request 1s?", "1s?"]])
        elif word[4][8:] == "0100":
            self.put(word[0], word[1], self.out_ann, [4, ["Sel 0", "0"]])
            self.pastBits += "0"
            self.bitsIdx += 1
            if self.startUidByte == 0:
                self.startUidByte = word[0]
        elif word[4][8:] == "0101":
            self.put(word[0], word[1], self.out_ann, [4, ["Sel 1", "1"]])
            self.pastBits += "1"
            self.bitsIdx += 1
            if self.startUidByte == 0:
                self.startUidByte = word[0]
        else:
            self.put(word[0], word[1], self.out_ann, [3, ["Error", "E"]])

        if self.bitsIdx == 8:
            data_parsed = int(self.pastBits, 2)
            start_of_byte = self.startUidByte
            self.startUidByte = 0
            self.put(start_of_byte, word[1], self.out_ann, [5, [hex(data_parsed)]])
            self.pastBits = ""
            self.bitsIdx = 0
            self.enumIdx += 1
            if self.enumIdx == 12:
                uid = data_parsed
                for e_idx in range (1,12):
                    uid += self.pastUidData[-e_idx][1] << (e_idx*8)
                self.put(self.pastUidData[-11][0], word[1], self.out_ann, [6, ["UID: " + hex(uid), hex(uid)]])
                self.enumIdx = 0
            self.pastUidData.append([start_of_byte, data_parsed])


    def parse_broadcast(self, word):
        if word[3] == 0: # Initial
            self.put(word[0], word[1], self.out_ann, [4, ["Initialize", "Init"]])
        elif word[4][2:8] == "000011": # Enumerate/Select
            self.parse_enumerate(word)
        elif word[4][2:8] == "000010": # Start of Packet Word HEADER
            bits_parsed = int(word[4][8:], 2)
            if bits_parsed != 0:
                self.put(word[0], word[1], self.out_ann, [3, ["Unknown Packet Header", "E"]])
            else:
                self.put(word[0], word[1], self.out_ann, [4, ["Packer Header", "Hdr"]])
                self.lastHdrIdx = 0
                self.packetClass = -1
                self.recieveData = 0
                self.pastBits = ""
        elif word[4][2:8] == "000101": # Packet class Word
            bits_parsed = int(word[4][8:], 2)
            if self.packetClass == -1:
                self.packetClass = bits_parsed
            else:
                self.put(word[0], word[1], self.out_ann, [3, ["Packet Class Word Without Header", "E"]])

            if bits_parsed == 0:
                self.put(word[0], word[1], self.out_ann, [4, ["Packet Class 0", "C0"]])
                #self.put(word[0], word[1], self.out_ann, [3, ["Packet Class 0", "C0"]])
            elif bits_parsed == 1:
                self.put(word[0], word[1], self.out_ann, [4, ["Packet Class 1", "C1"]])
            else:
                self.put(word[0], word[1], self.out_ann, [3, ["Unknown Packet Class", "E"]])
        elif word[4][2:4] == "01": # Broadcast selected device byte 1
            self.pastBits = word[4][4:]
        elif word[4][2:4] == "10": # Broadcast selected device byte 2
            selected_device = int(self.pastBits + word[4][4:], 2)
            self.put(self.pastWords[-1][0], word[1], self.out_ann, [4, ["Selected Device #" + str(selected_device), "Sel #" + str(selected_device)]])
            self.pastBits = ""
        elif word[4][2:] == "0011000001": # Start ECCE commands
            self.put(word[0], word[1], self.out_ann, [4, ["Start ECCE"]])
        elif word[4][2:] == "0000010000": # Start ECCE commands
            self.put(word[0], word[1], self.out_ann, [4, ["Start ECCE"]])
        elif word[4][2:] == "0000000010": # ECCE wrong
            self.put(word[0], word[1], self.out_ann, [4, ["Wrong!", "W"]])
        elif word[4][2:] == "0000000011": # ECCE correct
            self.put(word[0], word[1], self.out_ann, [4, ["Correct!", "C"]])
        else:
            self.put(word[0], word[1], self.out_ann, [3, ["Unimplemented", "E"]])


    def parse_unicast(self, word):
        if self.packetClass < 0 or self.packetClass > 1:
            self.put(word[0], word[1], self.out_ann, [3, ["Unknown Packet Class", "E"]])

        if self.lastHdrIdx == 2:
            self.pastBits = word[4][4:]
            word_type = int(word[4][2:4], 2)
            if word_type != 1:
                self.put(word[0], word[1], self.out_ann, [3, ["Wrong Word Type", "E"]])
        elif self.lastHdrIdx == 3:
            self.pastBits += word[4][4:]
            two_bytes_data = int(self.pastBits, 2)
            self.put(self.pastWords[-1][0], word[1], self.out_ann, [4, [hex(two_bytes_data)]])
            word_type = int(word[4][2:4], 2)
            if word_type == 3:
                self.recieveData = 1
        elif self.lastHdrIdx == 4:
            one_byte_data = int(word[4][4:], 2)
            if self.recieveData == 0:
                data_text = "T"
            else:
                data_text = "R"
            self.put(word[0], word[1], self.out_ann, [4, [data_text + ": " + hex(one_byte_data)]])
            word_type = int(word[4][2:4], 2)
            if (self.recieveData == 0 and word_type != 0) or (self.recieveData == 1 and word_type != 3):
                self.put(word[0], word[1], self.out_ann, [3, ["Word type " + hex(word_type) + " does not match action: " + data_text]])
    ## packet = [startN (0), endN (1), recieve (2), first_two_bytes (3), last_byte (4), packetClass (5), recieve (6)]
            two_bytes_data = int(self.pastBits, 2)
            self.pastBits = ""
            sent_packet = [self.pastWords[-4][0], word[1], self.recieveData, two_bytes_data, one_byte_data, self.packetClass, self.recieveData]
            self.parse_packet(sent_packet)
            self.pastPackets.append(sent_packet)
        else:
            self.put(word[0], word[1], self.out_ann, [3, ["Wrong Packet Length", "E"]])
        #self.put(word[0], word[1], self.out_ann, [3, ["Error", "E"]])


    def parse_packet(self, packet):
        if packet[5] == 0:
            self.parse_packet_p0(packet)
            return

        if self.readPacketSeq == 1 and (packet[3] & 0x10 == 0x10) and (packet[3] & ~0x10 <= 0x7):
            self.put(self.pastPackets[-2][0], packet[1], self.out_ann, [5, ["Read: " + hex(packet[4]), "R: " + hex(packet[4])]])
            self.readPacketSeq = 0
            self.readOdcNumber += 1
            if self.readOdcNumber % 4 == 0:
                odc_read = (packet[4] << 24) | (self.pastPackets[-5][4] << 16) | (self.pastPackets[-10][4] << 8) | self.pastPackets[-15][4]
                #self.put(self.pastPackets[-19][0], packet[1], self.out_ann, [6, ["ODC: " + hex(odc_read), hex(odc_read)]])
                if self.readOdcNumber <= 48 and self.readOdcNumber % 24 == 0:
                    odc_full_read = 0 # packet[4] << (23 * 8)
                    for i in range(23):
                        temp_val = self.pastPackets[-5-5*i][4]
                        if i > 2:
                            odc_full_read = odc_full_read | (temp_val << ((22-i)*8))
                        elif i == 2:
                            odc_full_read = odc_full_read | ((temp_val & 7) << ((22-i)*8))
                    self.put(self.pastPackets[-119][0], packet[1], self.out_ann, [6, ["Sig: " + hex(odc_full_read), hex(odc_full_read)]])
            elif self.readOdcNumber == (48 + 17):
                odc_full_read = (packet[4] & 7) << (16*8)
                for i in range(16):
                    temp_val = self.pastPackets[-5-5*i][4]
                    odc_full_read = odc_full_read | (temp_val << ((15-i)*8))
                self.put(self.pastPackets[-84][0], self.pastPackets[-5][1], self.out_ann, [6, ["Msg: " + hex(odc_full_read), hex(odc_full_read)]])
            elif self.readOdcNumber == (48 + 18):
                odc_full_read = (packet[4] << 8) | (self.pastPackets[-5][4] & (~7))
                self.put(self.pastPackets[-9][0], packet[1], self.out_ann, [6, ["Rnd: " + hex(odc_full_read), hex(odc_full_read)]])
        elif packet[3] == 0x274:
            return
        elif packet[3] == 0x272:
            if self.pastPackets[-1][3] == 0x272 and self.pastPackets[-2][3] == 0x274 and self.pastPackets[-3][3] == 0x272:
                read_addr = ((self.pastPackets[-1][4] & 0x7f) << 3) | self.pastPackets[-2][4]
                self.put(self.pastPackets[-3][0], self.pastPackets[-1][0], self.out_ann, [5, ["Request: " + hex(read_addr), "?: " + hex(read_addr)]])
                self.readPacketSeq = 1
        elif (packet[3] & 0x40 == 0x40 or packet[3] & 0x30 == 0x30 or packet[3] & 0x10 == 0x10) and self.polling == 0:
            self.parse_packet_ecce(packet)
        elif (packet[3] == 0xf or packet[3] == 0x18) and packet[6] == 1: # Unknown polling mechanism, hardiwre
            return
        else:
            self.put(packet[0], packet[1], self.out_ann, [3, ["Unknown Packet Sequence P1", "E"]])


    def parse_packet_p0(self, packet):
        if self.readOdcNumber != 0:
            #self.put(self.pastPackets[-10][0], self.pastPackets[-1][1], self.out_ann, [6, ["[Ignored]", "[N]"]])
            self.readOdcNumber = 0

        if packet[3] == 0x20:
            return
        elif packet[3] == 0x21:
            selected_device = "You're: " + str((packet[4] << 8) | self.pastPackets[-1][4])
            self.put(self.pastPackets[-1][0], packet[1], self.out_ann, [5, [selected_device]])
        elif (packet[3] & 0x40 == 0x40) and packet[3] & ~0x40 < 10:
            uid_idx = packet[3] & ~0x40
            uid_value = packet[4]
            self.put(packet[0], packet[1], self.out_ann, [5, ["UID[" + str(uid_idx) + "]: " + hex(uid_value), "U" + str(uid_idx) + ": " + hex(uid_value)]])
            if uid_idx == 9:
                uid_total_value = uid_value << (8*9)
                for i in range(1,10):
                    uid_total_value += self.pastPackets[-i][4] << (8*(9-i))
                self.put(self.pastPackets[-9][0], packet[1], self.out_ann, [6, ["UID: " + hex(uid_total_value)]])
        elif (packet[3] == 0xf or packet[3] == 0x18) and packet[6] == 0: # Unknown polling mechanism, hardiwre
            if packet[3] == 0xf:
                self.polling = 1
                self.put(self.pastPackets[-1][0], packet[1], self.out_ann, [5, ["Poll 1"]])
            else:
                self.polling = 0
                self.put(self.pastPackets[-1][0], packet[1], self.out_ann, [5, ["Poll 2"]])
        else:
            self.put(packet[0], packet[1], self.out_ann, [3, ["Unknown Packet Sequence P0", "E"]])


    def parse_packet_ecce(self, packet):
        ecce_types = [("C", 0x40), ("Z", 0x30), ("X", 0x10)]
        for e_t in ecce_types:
            if packet[3] & e_t[1] != e_t[1]:
                continue
            byte_idx = packet[3] & ~e_t[1]
            if byte_idx > 0xf and byte_idx != 0x300:
                self.put(packet[0], packet[1], self.out_ann, [3, ["Not an ECCE packet", "E"]])
                return
            self.put(packet[0], packet[1], self.out_ann, [5, [e_t[0] + ": " + hex(packet[4])]])
            if byte_idx == 0x300:
                self.put(packet[0], packet[1], self.out_ann, [6, [e_t[0] + ": " + hex(packet[4])]])
            elif byte_idx % 4 == 3:
                challenge_data = (packet[4] << 24) | (self.pastPackets[-1][4] << 16) | (self.pastPackets[-2][4] << 8) | self.pastPackets[-3][4]
                self.put(self.pastPackets[-3][0], packet[1], self.out_ann, [6, [e_t[0] + ": " + hex(challenge_data), hex(challenge_data)]])
            break

    def wait_edge(self):
        # look for an edge, and capture both value and sample number
        v, = self.wait({0: 'e'})
        n = self.samplenum
        return (v, n)

    def calculate_bauds(self, sampleN, prevSampleN):
        t = (sampleN - prevSampleN) / self.samplerate
        bauds = int(round(t / 4.47e-6));
        if bauds % 2 == 0:
            bauds = bauds // 2
            if self.halfRate < 1 and (bauds == 1):
                self.halfRate = 1
                self.put(self.pastNs[-1]-100, sampleN-100, self.out_ann, [2, ["<HALF_BAUD>"]])
        return bauds

    def calculate_bit(self, baud, invert):
        return int((baud == 3 and not invert) or (baud == 1 and invert))

    def decode(self):
        if self.samplerate is None:
            raise Exception('Cannot decode without samplerate.')

        while True:
            (v, n) = self.wait_edge()

            if self.strt == 1:
                self.put(self.pastNs[-2], n, self.out_ann, [2, ["[START]"]])
                self.strt = 0

            bauds = self.calculate_bauds(n, self.pastNs[-1])
            # check correct space
            if bauds != 1 and bauds != 3:
                if int(self.pastVs[-1]) != 1:
                    if bauds < 3:
                        self.put(self.pastNs[-1], n, self.out_ann, [3, ["Error"]])
                        self.put(self.pastNs[-1], n, self.out_ann, [2, ["[ACK]"]])
                    else:
                        self.strt = 1
                self.save_log(v, n)
                continue

            # check that waited 5 bauds
            bauds_prev = self.calculate_bauds(self.pastNs[-1], self.pastNs[-2])
            if bauds_prev < 5 or self.pastVs[-1]:
                if int(self.pastVs[-1]) != 1:
                    #self.put(self.pastNs[-1], n, self.out_ann, [3, ["Error"]])
                    self.put(self.pastNs[-1], n, self.out_ann, [2, ["[ACK]"]])
                self.save_log(v, n)
                continue

            start_bauds = bauds
            start_prevN = self.pastNs[-1]
            start_n = n

            data = [(self.pastNs[-1], bauds)] * 13
            self.save_log(v, n)

            # Start new cycle of wait
            for i in range(1, 13):
                (v, n) = self.wait_edge()
                bauds = self.calculate_bauds(n, self.pastNs[-1])
                if bauds == 1:
                    self.put(self.pastNs[-1], n, self.out_ann, [0, ['B1']])
                elif bauds == 3:
                    self.put(self.pastNs[-1], n, self.out_ann, [0, ['B3']])
                else:
                    if bauds_prev < 11:
                        self.put(self.pastNs[-2], self.pastNs[-1], self.out_ann, [2, ["[S]"]])
                    else:
                        self.put(self.pastNs[-2], self.pastNs[-1], self.out_ann, [2, ["[L]"]])
                    self.save_log(v, n)
                    break

                data[i] = (self.pastNs[-1], bauds)
                self.save_log(v, n)

            if bauds != 1 and bauds != 3:
                continue

            self.put(start_prevN - 25, start_prevN, self.out_ann, [0, ['START']])
            if start_bauds == 1:
                self.put(start_prevN, start_n, self.out_ann, [0, ['B1']])
            elif start_bauds == 3:
                self.put(start_prevN, start_n, self.out_ann, [0, ['B3']])

            ### FULL 12 bits (Word)
            inv = data[12][1] == 3
            bit_char = str(self.calculate_bit(data[0][1], False))
            self.put(data[0][0], data[1][0], self.out_ann, [1, [bit_char]])
            bit_string = bit_char
            bit_char = str(self.calculate_bit(data[1][1], False))
            self.put(data[1][0], data[2][0], self.out_ann, [1, [bit_char]])
            bit_string += bit_char
            for j in range(2, 12):
                bit_char = str(self.calculate_bit(data[j][1], inv))
                self.put(data[j][0], data[j+1][0], self.out_ann, [1, [bit_char]])
                bit_string += bit_char
            bit_char = str(self.calculate_bit(data[12][1], False))
            self.put(data[12][0], n, self.out_ann, [1, [bit_char]])

            ## Training data
            word_type = int(bit_string[:2], 2)
            if word_type == 1:
                self.put(data[0][0], data[2][0], self.out_ann, [2, ["Unicast", "U"]])
            elif word_type == 2:
                self.put(data[0][0], data[2][0], self.out_ann, [2, ["Broadcast", "B"]])
            else:
                self.put(data[0][0], data[12][0], self.out_ann, [3, ["Error"]])
            invert_txt = "N"
            if inv:
                invert_txt = "Inv"
            self.put(data[12][0], n, self.out_ann, [2, [invert_txt]])

            ## Data
            data_sent = int(bit_string[2:], 2)
            self.put(data[2][0], data[12][0], self.out_ann, [2, [hex(data_sent)]])

            curr_word = (data[0][0], data[12][0], word_type, data_sent, bit_string, inv)
            self.lastHdrIdx += 1

            ### PARSE BITS
            if word_type == 2:
                self.parse_broadcast(curr_word)
            elif word_type == 1:
                self.parse_unicast(curr_word)

            self.pastWords.append(curr_word)
