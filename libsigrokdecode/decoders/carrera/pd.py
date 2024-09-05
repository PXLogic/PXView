##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2014 Sebastien Bourdelin <sebastien.bourdelin@savoirfairelinux.com>
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
import math

class Decoder(srd.Decoder):
    api_version = 3
    id = 'Carrera'
    name = 'Carrera Digital Decoder'
    longname = 'longname'
    desc = 'was macht der wohl?'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['C Digital']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Data line'},
    )
    optional_channels = ()
    options = (
        { 'id': 'invert', 'desc': 'Signal ist invertiert',
          'default': 'nein', 'values': ('ja', 'nein') },
    )
    annotations = (
        ('controller_0', 'Reglerwort ID 0'), # 0
        ('controller_1', 'Reglerwort ID 1'), # 1
        ('controller_2', 'Reglerwort ID 2'), # 2
        ('controller_3', 'Reglerwort ID 3'), # 3
        ('controller_4', 'Reglerwort ID 4'), # 4
        ('controller_5', 'Reglerwort ID 5'), # 5
        ('controller_sc', 'Reglerwort SC/Ghost'), # 6
        ('controller_prog', 'Programmierwort'), # 7
        ('controller_active', 'Aktivdatenwort'), # 8
        ('bit', 'Bit'), # 9
        ('quittierung', 'Quittierungswort'), # 10
        ('prog_gas', ''),# 11 # Geschwindigkeit programmieren
        ('prog_general', 'Programmierdatenwort'), # 12
        ('prog_bremse', ''), # 13 # Bremswert programmieren
        ('prog_tank', ''), # 14 # Tank programmieren
        ('prog_werte', ''), # 15 # Werte fuer Fahrzeug
        ('prog_tanken', ''), # 16 # Tanken möglich Modus
        ('prog_position', ''), # 17 # Position des Fahrers
        ('prog_finish', ''), # 18 # Rennen beendet
        ('prog_finishline', ''), # 19 # Zieldurchfahrt
        ('prog_fuel', ''), # 20 # Tankstand
        ('prog_jumpstart', ''), # 21 # Frühstart
        ('prog_traffic_light', ''), # 22 # Ampel
        ('prog_lapcount', ''), # 23 # Rundenzahl lower Nibble
        ('prog_reset', ''), # 24 # Reset
        ('prog_pitlaneadapter', ''), # 25 # Pitlandeadapter konfigurieren
        ('prog_pe rformance', ''), # 26 # Performance Messmodus
    )
    annotation_rows = (
        ('word_bit_value', 'Bits', (9,)),
        ('word_controller', 'Reglerwort', (0,1,2,3,4,5,6,)),
        ('active_quit', 'Aktiv-/Quittierungswort', (8, 10,)),
        ('prog_word', 'Programmierdatenwort', (11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,)),
        #('prog_word', 'Programmierdatenwort', (11,)),
    )
    marker = 0

    def init_variables(self):
        self.count = 0
        self.currentMicros = 0
        self.previousMicros = 0
        self.intervalMicros = 0
        self.wordStart = 0
        self.wordEnd = 0
        self.bitStart = 0
        self.dataWord = 1
        self.beginDataWord = 0
        self.endDatatWord = 0
        self.next_could_be_active_data_word = False
        self.pitlane_vorhanden = False

    def __init__(self):
        self.reset()

    def reset(self):
        self.init_variables()

    def metadata(self, key, value):
       if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def get_usec_from_samples(self, samplenum) -> float:
        usec = float(samplenum) * 1000000.0
        usec /= float(self.samplerate)
        return float(usec)

    def get_msec_from_sample(self, samplenum) -> float:
        msec = float(samplenum) * 1000.0
        msec /= float(self.samplerate)
        return float(msec)

    def get_samples_from_usec(self, usec) -> int:
        retval = 1000000.0 / self.samplerate
        retval *= usec
        return int(retval)

    def get_samples_from_msec(self, msec) -> int:
        retval = 1000000.0 / float(self.samplerate)
        retval *= msec * 1000
        return int(retval)

    def get_flipped_value_from_dataword(self, bitsToShift = 0, bitWidth = 1) -> int:
        value = self.get_value_from_dataword(bitsToShift, bitWidth)
        flipped = self.flip_bits(value, bitWidth)
        return int(flipped)

    def get_value_from_dataword(self, bitsToShift = 0, bitWidth = 1) -> int:
        compare_val = int(math.pow(2, bitWidth))
        compare_val -= 1
        retval = (self.dataWord >> bitsToShift) & compare_val
        return int(retval)

    def flip_bits(self, value, bitCount) -> int:
        # https://www.techiedelight.com/reverse-bits-of-given-integer/
        bitCount -= 1
        result = int(0)
        while bitCount >= 0 and value:
            if value & 1:
                result |= (1 << bitCount)

            value >>= 1
            bitCount -= 1

        return int(result)

    def start(self):
        #pass
        self.init_variables()
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def checkBit(self, bit):
        self.currentMicros = self.get_usec_from_samples(self.samplenum)
        self.intervalMicros = self.currentMicros - self.previousMicros
        #self.put(self.samplenum - 100, self.samplenum, self.out_ann, [4, [str(self.currentMicros)]])
        if 75.0 <= self.intervalMicros <= 125.0:
            self.put(self.get_samples_from_usec(self.previousMicros), self.samplenum, self.out_ann, [4, [str(bit)]])
            self.previousMicros = self.currentMicros
            self.dataWord <<= 1 # ein bit shiften
            if bit == 0:
                self.dataWord |= 1

    def print_reglerdatenwort(self):
        regler_id = self.get_value_from_dataword(6, 3)
        regler_str = str(regler_id)
        ta = str(self.get_value_from_dataword())

        if regler_id == 2 or regler_id == 7:
            self.next_could_be_active_data_word = True

        if regler_id == 7:
            regler_id -= 1
            regler_str = "SC"
            pc = str(self.get_value_from_dataword(1))
            nh = str(self.get_value_from_dataword(2))
            fr = str(self.get_value_from_dataword(3))
            tk = str(self.get_value_from_dataword(4))
            kfr = str(self.get_value_from_dataword(5))
            desc_long = "KFR:{} TK:{} FR:{} NH:{} PC:{} TA:{}".format(kfr, tk, fr, nh, pc, ta)
        else:
            # einzelne Werte
            gas = str((self.dataWord >> 1) & 15)
            wt = str(self.get_value_from_dataword(5))
            desc_long = "ID:{} G: {} WT:{} TA:{}".format(regler_str, gas, wt, ta)
        desc_short = "R " + regler_str
        desc = "Regler " + regler_str

        self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [regler_id, [desc_short, desc, desc_long]])

    def print_aktivdatenwort(self):
        ie = str(self.get_value_from_dataword())
        r5 = str(self.get_value_from_dataword(1))
        r4 = str(self.get_value_from_dataword(2))
        r3 = str(self.get_value_from_dataword(3))
        r2 = str(self.get_value_from_dataword(4))
        r1 = str(self.get_value_from_dataword(5))
        r0 = str(self.get_value_from_dataword(6))

        desc_short = "IE:" + ie
        desc_long = "R0:{} R1:{} R2:{} R3:{} R4:{} R5:{} IE:{}".format(r0, r1, r2, r3, r4, r5, ie)

        self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [8, [desc_short, desc_long]])

    def print_quittierungswort(self):
        s7 = str(self.get_value_from_dataword())
        s6 = str(self.get_value_from_dataword(1))
        s5 = str(self.get_value_from_dataword(2))
        s4 = str(self.get_value_from_dataword(3))
        s3 = str(self.get_value_from_dataword(4))
        s2 = str(self.get_value_from_dataword(5))
        s1 = str(self.get_value_from_dataword(6))
        s0 = str(self.get_value_from_dataword(7))

        desc_short = "Q"
        desc = "Quitt."
        desc_long = "S0:{} S1:{} S2:{} S3:{} S4:{} S5:{} S6:{} S7:{}".format(s0, s1, s2, s3, s4, s5, s6, s7)

        self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [10, [desc_short, desc, desc_long]])

    def print_programmierdatenwort(self):
        wert = self.get_flipped_value_from_dataword(8, 4)
        befehl = self.get_flipped_value_from_dataword(3, 5)
        regler = self.get_flipped_value_from_dataword(0, 3)
        #self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [11, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        if befehl == 0: # Geschwindigkeit programmieren
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [11, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 1: # Bremswert programmieren
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [12, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 2: # Tank programmieren
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [13, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 4: # Werte fuer Fahrzeug
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [14, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 5: # Tanken möglich Modus
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [15, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 6: # Position des Fahrers
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [16, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 7: # Rennen beendet
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [17, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 8: # Zieldurchfahrt mit Bestzeit
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [18, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 9: # Zieldurchfahrt
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [19, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 10: # Tankstand
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [20, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 11: # Frühstart
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [21, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 16: # Ampel
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [22, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 17: # Rundenzahl upper Nibble
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [23, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 18: # Rundenzahl lower Nibble
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [23, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 19: # Reset
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [24, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 20: # Pitlandeadapter konfigurieren
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [25, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])
        elif befehl == 21: # Performance Messmodus
            self.put(self.beginDataWord, self.endDatatWord, self.out_ann, [26, ["Befehl: {}, Regler: {}, Wert: {}".format(befehl, regler, wert)]])

        

    def print_bit(self, value):
        self.put(self.bitStart, self.samplenum, self.out_ann, [9, [str(value)]])

    def decode(self):
        invert = self.options['invert'] == 'ja'
        bit = 0
        if invert:
            bit = 1
        while True:
            pin = self.wait({0: 'e'}) # wir warten auf die naechste level-aenderung
            self.currentMicros = self.get_usec_from_samples(self.samplenum)
            self.intervalMicros = self.currentMicros - self.previousMicros
            if self.intervalMicros < 200:
                self.endDatatWord = self.samplenum
            if 75.0 <= self.intervalMicros <= 125.0:
                self.previousMicros = self.currentMicros
                self.dataWord <<= 1
                if pin[0] == bit:
                    self.dataWord |= 1
                    self.print_bit(1)
                else:
                    self.print_bit(0)
                self.bitStart = self.samplenum
            elif self.intervalMicros > 6000.0:
                if self.next_could_be_active_data_word:
                    if 127 < self.dataWord < 256:
                        self.print_aktivdatenwort()
                    elif self.dataWord < 512:
                        self.print_quittierungswort()
                    self.next_could_be_active_data_word = False
                elif self.dataWord < 1024:
                    self.print_reglerdatenwort()
                else:
                    self.print_programmierdatenwort()
                self.dataWord = 1
                self.previousMicros = self.currentMicros
                self.beginDataWord = self.samplenum
                self.bitStart = self.samplenum


"""     def decode(self):
        while True:
            pin = self.wait({0: 'f'})
            if self.wordStart == self.wordEnd:
                self.wordStart = self.samplenum
            self.bitStart = self.samplenum
            self.dataWord = 1
            self.previousMicros = self.get_usec_from_samples(self.samplenum)
            self.checkBit(0)
            self.put(self.get_samples_from_usec(self.currentMicros - 100), self.samplenum, self.out_ann, [4, [str(1)]])

            while True:
                pin = self.wait({0: 'r'})
                self.wordEnd = self.samplenum
                self.checkBit(0)
                self.wait([{0: 'f'}, {'skip': int(self.get_samples_from_msec(2))}])
                if self.matched == (False, True): # hier haben wir 2 ms keine fallende Flanke gehabt
                    # haben wir eine mindestlaenge von 150 us?
                    if self.get_usec_from_samples(self.wordEnd - self.wordStart) > 150.0:
                        self.put(self.wordStart, self.wordEnd, self.out_ann, [2, [str(self.dataWord)]])
                    self.wordStart = self.wordEnd
                    self.dataWord = 1
                    break
                self.checkBit(1) """
                


"""     def decode(self):
        self.put(0, 1000, self.out_ann, [4, ['1']])
        self.put(1000, 2000, self.out_ann, [5, ['0']])
        #waittime = self.get_samples_from_msec(4)
        waittime = self.get_samples_from_usec(4000)
        #print(f"self.samplerate: {self.samplerate} ... 20 * (1000 / self.samplerate): {20 * (1000 / self.samplerate)}")
        #pin = self.wait({'skip': 4 * (1000 / self.samplerate)})
        #pin = self.wait({'skip': 4000})
        pin = self.wait({'skip':  waittime})
        self.put(self.samplenum, self.samplenum + 1000, self.out_ann, [3, [str( waittime)]])
        while True:
            self.wait({0: 'r'})
            self.marker = self.samplenum
            self.wait({0: 'f'})
            elapsed = 1 / float(self.samplerate)
            elapsed *= (self.samplenum - self.marker )
            
            if elapsed == 0 or elapsed >= 1:
                delta_s = '%.1fs' % (elapsed)
            elif elapsed <= 1e-12:
                delta_s = '%.1ffs' % (elapsed * 1e15)
            elif elapsed <= 1e-9:
                delta_s = '%.1fps' % (elapsed * 1e12)
            elif elapsed <= 1e-6:
                delta_s = '%.1fns' % (elapsed * 1e9)
            elif elapsed <= 1e-3:
                delta_s = '%.1fμs' % (elapsed * 1e6)
            else:
                delta_s = '%.1fms' % (elapsed * 1e3)
            self.put(self.marker, self.samplenum, self.out_ann, [0, [str(delta_s), 'test']])
            #self.put(self.marker, self.samplenum, self.out_ann, [3, [str(self.get_usec(self.samplenum)), 'test']])
            #self.wait({0: 'e'})
            ##self.wait()
            #self.count += 1
            #if self.count == 10:
            #    self.put(self.samplenum - 100, self.samplenum, self.out_ann, [0, [str(self.samplenum), 'test', 't']])
            #    self.put(self.samplenum - 300, self.samplenum-100, self.out_ann, [1, [str(self.samplenum), 'test', 't']])
            #    self.put(self.samplenum - 600, self.samplenum-300, self.out_ann, [2, [str(self.samplenum), 'test', 't']])
            #    self.put(self.samplenum - 100, self.samplenum, self.out_ann, [3, [str(self.samplenum), 'test', 't']])
            #    self.count = 0
        #pass """