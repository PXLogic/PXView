#
# This file is part of the libsigrokdecode project.
#
# Copyright (C) 2020 Thomas Hoffmann <th.hoffmann@mailbox.org>
# Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#

import re
import sigrokdecode as srd

# FIXME: reduce annotation types
anb_DA, anb_RES, anb_SL, anb_CB, anb_DC, anb_PG, \
    anb_CO, anb_mux, anb_par, anb_LAST = range(10)

bits = (
    'Display Addressing', 'Reserved', 'Start Line', 'Continuation',
    'Data / Command', 'Page', 'Column', 'mux', 'Parameter', 'Last'
)

(ann_LC, ann_HC, ann_DM, ann_SCA, ann_SPA, ann_SFB, ann_RHS, ann_LHS, ann_VRHS,
    ann_VLHS, ann_SS, ann_AS, ann_DSL, ann_SCC, ann_SCPU, ann_MC0TS0,
    ann_MCFFTS0, ann_SVSA, ann_DOR, ann_DOI, ann_ND, ann_ID, ann_SMR,
    ann_DOFF, ann_DON, ann_PSA, ann_CSU, ann_CSD, ann_SVO, ann_DCR, ann_ZI,
    ann_SPP, ann_SCPI, ann_SVD, ann_NOP, ann_GR, ann_DA, ann_CB,
    ann_LAST) = range(anb_LAST+1, anb_LAST+40)

cmds2 = {
    #       ann name              annID       cmd txt                                                       param?
    0x00: ('LowerColStart',       ann_LC,     ['Set Lower Column Start Address', 'Set L Col Start', 'LC'],  0),
    0x10: ('HigherColStart',      ann_HC,     ['Set Higher Column Start Address', 'Set H Col Start', 'HC'], 0),
    0x20: ('DisplayMode',         ann_DM,     ['Set Display Mode', 'Set Dsp Md', 'DM'],                     1),
    0x21: ('SetColAddress',       ann_SCA,    ['Set Column Address', 'Set Col Adr', 'CA'],                  1),
    0x22: ('SetPageAddress',      ann_SPA,    ['Set Page Address', 'Set Pg Adr', 'PA'],                     1),
    0x23: ('SetFadeoutBlinking',  ann_SFB,    ['Set Fade-out and Blinking', 'Set FO Blnk', 'FB'],           1), 
    0x26: ('RightHorScroll',      ann_RHS,    ['Right horizontal scroll', 'Right hor scr', 'RHS'],          1), 
    0x27: ('LeftHorScroll',       ann_LHS,    ['Left horizontal scroll', 'Left hor scr', 'LHS'],            1), 
    0x29: ('VertRightHorScroll',  ann_VRHS,   ['Vertical and right horizontal scroll', 
                                               'Vert right hor scr', 'VRHS'],                               1), 
    0x2A: ('VertLeftHorScroll',   ann_VLHS,   ['Vertical and left horizontal scroll', 
                                               'Vert left hor scr', 'VLHS'],                                1), 
    0x2E: ('StopScrolling',       ann_SS,     ['Stop scrolling', 'Stsc'],                                   0), 
    0x2F: ('ActivateScrolling',   ann_AS,     ['Activate scrolling' , 'Acsc'],                              0), 
    0x40: ('DisplayStartLine',    ann_DSL,    ['Display start line', 'DSL',],                               0), 
    0x81: ('SetContrast',         ann_SCC,     ['Set contrast control', 'Set Ctr', 'SC'],                   1),
    0x8D: ('SetChargePump',       ann_SCPU,   ['Set charge pump', 'Set Ch pmp', 'SP'],                      1),
    0xA0: ('MapCol0ToSeg0',       ann_MC0TS0, ['Map col addr0 to seg0', 'Map C0 to S0', 'M00'],             0),
    0xA1: ('MapCol127toSeg0',     ann_MCFFTS0,['Map col addr7f to seg0', 'Map C7f to S0', 'M7f0'],          0),
    0xA3: ('SetVertScrollArea',   ann_SVSA,   ['Set vertical scroll area', 'Set vert scr ar', 'SVSA'],      1),    
    0xA4: ('DisplayOnResume',     ann_DOR,    ['Display on, resume to RAM', 'Dis on, res RAM', 'D1R'],      0),
    0xA5: ('DisplayOnIgnore',     ann_DOI,    ['Display on, ignore RAM', 'Dis on, ign RAM', 'D1I'],         0),
    0xA6: ('NormalDisplay',       ann_ND,     ['Normal display', 'Norm disp', 'DN'],                        0),
    0xA7: ('InverseDisplay',      ann_ID,     ['Inverse display', 'Inv disp', 'DI'],                        0),
    0xA8: ('SetMultiplexRatio',   ann_SMR,    ['Set multiplex ratio', 'Set MUX rat', 'MUX'],                1),
    0xAE: ('DisplayOff',          ann_DOFF,   ['Display OFF', 'Dis OFF', 'DO'],                             0),
    0xAF: ('DisplayOn',           ann_DON,    ['Display ON', 'Dis ON', 'D1'],                               0),
    0xB0: ('PgStartAddr',         ann_PSA,    ['Page start address', 'Pg start', 'PS'],                     0),
    0xC0: ('ComScanUp',           ann_CSU,    ['COM scan 0 to mux', 'C scan upw', 'SCU'],                   0),
    0xC8: ('ComScanDown',         ann_CSD,    ['COM scan mux to 0', 'C scan dwd', 'SCD'],                   0),
    0xD3: ('Set Vertical Offset', ann_SVO,    ['Set vertical offset', 'Set vert ofs', 'VO'],                1),
    0xD5: ('DisplayClockRatio',   ann_DCR,    ['Display clock ratio', 'Clock ratio', 'CR'],                 1),
    0xD6: ('ZoomIn',              ann_ZI,     ['Set zoom-in', 'Zoom in', 'ZI'],                             1),
    0xD9: ('PrechargePeriod',     ann_SPP,    ['Set precharge period', 'Pre chrg', 'PC'],                   1),
    0xDA: ('SetCOMPins',          ann_SCPI,   ['Set COM pins', 'COM pins', 'CP'],                           1),
    0xDB: ('SetVcomhDeselect',    ann_SVD,    ['Set Vcomh deselect', 'Vcomh desel', 'VD'],                  1),
    0xE3: ('NOP',                 ann_NOP,    ['No operation', 'NOP'],                                      0),
    'data': ('GDDRAM',            ann_GR,     ['Data write',], 0),      # data write
    'dev':  ('DeviceAddress',    ann_DA,     ['Device address',], 0), # 0x3C or 0x3D
    'cbyte': ('ControlByte',     ann_CB,     ['Control byte',], 0),    # 0x80 (Command follows) or 0x40 (data)
    'last':  ('LastCmd',         ann_LAST,   ['Last',], 0)          # unused: marks end of cmds
}

anbl_block=ann_LAST+1
anw_warn=anbl_block+1

SSD1306_I2C_ADDRESS = 0x3C
SSD1306_I2C_ADDRESS_2 = 0x3D

def bits_and_cmds():
    l = [('bit_' + re.sub('\\/| ', '_', b).lower(), b + ' bit') for b in bits]
    #order dictionary entries by ann_
    sl=sorted(cmds2.items(), key=lambda x: x[1][1])
    l += [('cmd_' + k[1][0].lower(), k[1][0] + ' command') for k in sl]
    return tuple(l)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'ssd1306'
    name = 'SSD1306'
    longname = 'Solomon 1306'
    desc = 'Solomon SSD1306 OLED controller protocol.'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['Display', 'IC']
    annotations =  bits_and_cmds() + (
        ('write_block', 'Write block'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('bits', 'Bits', tuple(range(anb_LAST+1))),
        ('cmds', 'Commands', tuple(range(anb_LAST+1, ann_LAST+1))),
        ('blockdata', 'Block Data', (anbl_block, )),
        ('warnings', 'Warnings', (anw_warn,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        self.substate = 'COMMAND'
        self.prevreg = -1
        self.bits = []
        self.sscmd = 0
        self.blockstring = ''

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putd(self, bit1, bit2, data):
        self.put(self.bits[bit1][1], self.bits[bit2][2], self.out_ann, data)

    def putr(self, bit):
        self.put(self.bits[bit][1], self.bits[bit][2], self.out_ann,
                 [anb_RES, ['Reserved bit NC', 'Res NC', 'NC', 'N']])

    def put0(self, bit):
        self.put(self.bits[bit][1], self.bits[bit][2], self.out_ann,
                 [anb_RES, ['fixed 0', '0',]])

    def put1(self, bit):
        self.put(self.bits[bit][1], self.bits[bit][2], self.out_ann,
                 [anb_RES, ['fixed 1', '1',]])

    def handle_par_0x00(self, param): # low col start addr
        #bit output:
        for i in range(4, 8): self.put0(i)
        self.putd(3, 0, [cmds2[self.prevreg][1], 
            ['Lwr col start addr= %d' % param & 0xf,
             'lo col %d' % param & 0xf, 'LCS']])
        #cmd output: none
        #block output
        self.blockstring += '= %d' % param & 0xf

    def handle_par_0x10(self, param): # high col start addr
        #bit output:
        for i in range(5, 8): self.put0(i)
        self.put1(4)
        self.putd(3, 0, [cmds2[self.prevreg][1], 
            ['Hghr col start addr= %d' % param & 0xf,
             'hi col %d' % param & 0xf, 'HCS']])
        #cmd output: none
        #block output
        self.blockstring += '= %d' % param & 0xf       

    def handle_par_0x20(self, param): # set display mode
        am = ['hor. addr.', 'vert. addr.', 'page addr.', 'invalid']
        am2 = ['HA', 'VA', 'PA', 'IV']
        #bit output:
        for i in range(2,8):
            self.putr(i)
        self.putd(1, 0, [anb_DA, ['mode: %s' % am2[param & 3], 'DM']])
        #cmd output:
        self.putd(7, 0, [cmds2[self.prevreg][1], 
            ['Display mode: %s' % am[param & 3],
             'mode: %s' % am2[param & 3], 'DM']])
        #block output:
        self.blockstring += ': %s' % am[param & 3]
        self.substate = 'COMMAND'

    def handle_par_0x21(self, param): # A B - set column address
        if self.substate == 'PARAMETER':
            sc = param & 0x7f
            res = ' (reset)' if sc == 0 else ''
            #bit output:
            self.putr(7)
            self.putd(6, 0, [anb_CO, ['col %d' % sc, 'CO']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Start column: %d%s' % (sc,res), 
                 'St Col: %d' % sc, 'SC']])
            #block output:
            self.blockstring += ' from %d%s ' % (sc, res)
            self.substate = 'PARAMETER2'
        elif self.substate == 'PARAMETER2':
            ec = param & 0x7f
            res = ' (reset)' if ec == 0x7f else ''
            #bit output:
            self.putr(7)
            self.putd(6, 0, [anb_CO, ['col %d' % ec, 'CO']])
            #cmd output: 
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['End column: %d%s' % (ec,res), 'End Col: %d' % ec, 'EC']])
            #block output
            self.blockstring += 'to %d%s' % (ec,res)
            self.substate = 'COMMAND'
            
    def handle_par_0x22(self, param): # A B - set page address
        if self.substate == 'PARAMETER':
            sp = param & 0x7
            res = ' (reset)' if sp == 0 else ''
            #bit output:
            for i in range (3,8):
                self.putr(i)
            self.putd(2, 0, [anb_PG, ['page %d' % sp, 'PG']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Start Page: %d%s' % (sp,res), 'St Pg: %d' % sp, 'SP']])
            #block output:
            self.blockstring += ' from %d%s ' % (sp, res)
            self.substate = 'PARAMETER2'
        elif self.substate == 'PARAMETER2':
            ep = param & 0x7
            res = ' (reset)' if ep == 0x7 else ''
            #bit output:
            for i in range (3,8):
                self.putr(i)
            self.putd(2, 0, [anb_PG, ['page %d' % ep, 'PG']])    
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['End Page: %d%s' % (ep,res), 'End Pg: %d' % ep, 'EP']])
            #block output
            self.blockstring += 'to %d%s' % (ep,res)
            self.substate = 'COMMAND'

    def handle_par_0x23(self, param): # set fade-out and blinking
        bf = ['no FA / blnk', 'invalid', 'fade-out', 'blink'][(param >> 4) & 3]
        bf2 = ['no', 'IV', 'FO', 'BL'][(param >> 4) & 3]
        fo = ((param & 0xf) << 3) + 8
        #bit output:
        self.putr(7)
        self.putr(6)
        self.putd(5, 4, [anb_par, [bf2, 'BF']])
        self.putd(3, 0, [anb_par, ['fr: %d' % fo, 'FS']])
        #cmd output:
        self.putd(7, 0, [cmds2[self.prevreg][1], ['%s (%d frames)' % (bf, fo),
        '%s (%d fr)' % (bf2, fo), 'BF']])
        #block output:
        self.blockstring += ': %s, %d frames' % (bf, fo)
        self.substate = 'COMMAND'

    def handle_par_0x26(self, param): # set right hor. scrolling
        if self.substate == 'PARAMETER':
            for i in range(8):
                self.put0(i)
            self.substate = 'PARAMETER2'
        elif self.substate == 'PARAMETER2':
            for i in range(3, 8):
                self.putr(i)
            #B[2:0] start page
            sp = param & 0x7
            self.putd(2, 0, [anb_PG, ['page %d' % sp, 'PG']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Start Page: %d' % sp, 'St Pg: %d' % sp, 'SP']])
            #block output:
            self.blockstring += ' from page %d, ' % sp
            self.substate = 'PARAMETER3'
        elif self.substate == 'PARAMETER3':
            for i in range(3, 8):
                self.putr(i)
            #C[2:0] time interval
            iv = (5, 64, 128, 256, 3, 4, 25, 2)[param & 0x7]
            self.putd(2, 0, [anb_PG, ['intvl %d' % iv, 'IV']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Scroll Interval: %d' % iv, 'Scr Iv: %d' % iv, 'IV']])
            #block output
            self.blockstring += 'time interval %d, ' % iv
            self.substate = 'PARAMETER4'
        elif self.substate == 'PARAMETER4':
            for i in range(3, 8):
                self.putr(i)
            #D[2:0] end page
            ep = param & 0x7
            self.putd(2, 0, [anb_PG, ['page %d' % ep, 'PG']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['End Page: %d' % ep, 'End Pg: %d' % ep, 'EP']])
            #block output
            self.blockstring += 'to page %d' % ep
            self.substate = 'PARAMETER5'
        elif self.substate == 'PARAMETER5':
            for i in range(8):
                self.put0(i)
            self.substate = 'PARAMETER6'
        elif self.substate == 'PARAMETER6':
            for i in range(8):
                self.put1(i)
            self.substate = 'COMMAND'

    def handle_par_0x27(self, param): # set left hor. scrolling
        self.handle_par_0x26(param)

    def handle_par_0x29(self, param): # set vert and right hor scrolling
        if self.substate == 'PARAMETER':
            for i in range(8):
                self.put0(i)
            self.substate = 'PARAMETER2'
        elif self.substate == 'PARAMETER2':
            for i in range(3, 8):
                self.putr(i)
            #B[2:0] start page
            sp = param & 0x7
            self.putd(2, 0, [anb_PG, ['page %d' % sp, 'PG']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Start Page: %d' % sp, 'St Pg: %d' % sp, 'SP']])
            #block output:
            self.blockstring += ' from page %d, ' % sp
            self.substate = 'PARAMETER3'
        elif self.substate == 'PARAMETER3':
            for i in range(3, 8):
                self.putr(i)
            #C[2:0] time interval
            iv = (5, 64, 128, 256, 3, 4, 25, 2)[param & 0x7]
            self.putd(2, 0, [anb_PG, ['intvl %d' % iv, 'IV']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Scroll Interval: %d' % iv, 'Scr Iv: %d' % iv, 'IV']])
            #block output
            self.blockstring += 'time interval %d, ' % iv
            self.substate = 'PARAMETER4'
        elif self.substate == 'PARAMETER4':
            for i in range(3, 8):
                self.putr(i)
            #D[2:0] end page
            ep = param & 0x7
            self.putd(2, 0, [anb_PG, ['page %d' % ep, 'PG']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['End Page: %d' % ep, 'End Pg: %d' % ep, 'EP']])
            #block output
            self.blockstring += 'to page %d' % ep
            self.substate = 'PARAMETER5'
        elif self.substate == 'PARAMETER5':
            for i in (6, 7):
                self.putr(i)
            #E[5:0] - vertical scrolling offset
            vso = param & 0x3f
            self.putd(2, 0, [anb_PG, ['scr ofs %d' % vso, 'VO']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Vert Scroll Ofs: %d' % vso, 'VSO: %d' % vso, 'VSO']])
            #block output
            self.blockstring += ' (vertical offset= %d rows)' % vso
            self.substate = 'COMMAND'

    def handle_par_0x2a(self, param): # set vert and left hor scrolling
        self.handle_par_0x29(param)

    def handle_par_0x40(self, param): # display start line
        #bit output:
        self.put0(7)
        self.put1(6)
        self.putd(5, 0, [anb_SL, ['Start line= %d' % (param & 0x3f),
             'st l %d' % (param & 0x3f), 'StL']])
        #cmd output: none
        #block output
        self.blockstring += '= %d' % (param & 0x3f)

    def handle_par_0x81(self, param): # set contrast
        #bit output: none
        #cmd output:
        self.putd(7, 0, [cmds2[self.prevreg][1], ['Contrast= %d' % param,
            'Ctr: %d' % param, 'Ctr']])
        #block output
        res = ' (reset)' if param == 0x7f else ''
        self.blockstring += ' to %d%s ' % (param, res)
        self.substate = 'COMMAND'

    def handle_par_0x8d(self, param): # set charge pump
        if param & 4:
            cp = 'on'
            res = ''
        else:
            cp = 'off'
            res = ' (reset)'
        #bit output:
        for i in (6,7): self.putr(i)
        for i in (0,1,3,5): self.put0(i)
        self.put1(4)
        self.putd(2, 2, [anb_par, ['cp: %s' % cp, 'cp', 'c']])
        #cmd output:
        self.putd(7, 0, [cmds2[self.prevreg][1], ['Charge pump= %s' % cp,
        'Ch p: %s' % cp, 'CP']])
        #block output:
        self.blockstring += ' to %s%s' % (cp, res)
        self.substate = 'COMMAND'

    def handle_par_0xa3(self, param): # A B - set vert scroll area
        if self.substate == 'PARAMETER':
            tfr = param & 0x3f
            res = ' (reset)' if tfr == 0 else ''
            #bit output:
            self.putr(7)
            self.putr(6)
            self.putd(5, 0, [anb_par, ['tfr: %d' % tfr, 'tfr']])
            #cmd output:
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Top fixed rows: %d%s' % (tfr,res), 
                 'Tp fx rws: %d' % tfr, 'TFR']])
            #block output:
            self.blockstring += ', top fixed rows: %d%s, ' % (tfr, res)
            self.substate = 'PARAMETER2'
        elif self.substate == 'PARAMETER2':
            sr = param & 0x7f
            res = ' (reset)' if sr == 0x40 else ''
            #bit output:
            self.putr(7)
            self.putd(6, 0, [anb_par, ['sr: %d' % sr, 'sr']])
            #cmd output: 
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['Scroll rows: %d%s' % (sr,res), 'Scr rws: %d' % sr, 'SR']])
            #block output
            self.blockstring += 'scroll rows: %d%s' % (sr,res)
            self.substate = 'COMMAND'

    def handle_par_0xa8(self, param): # set multiplex ratio
        mux = (param & 0x3f) + 1
        #bit output:
        for i in range(6,8):
            self.putr(i)
        self.putd(5, 0, [anb_mux, ['mux: %d' % mux, 'mux']])
        #cmd output:
        if mux < 16:
            self.put(self.sscmd, self.es, self.out_ann,
                 [anw_warn, ['invalid multiplex ratio < 16 (%d)' % mux]])
            self.blockstring=''     
        else:     
            res = ' (reset)' if mux == 64 else ''
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['%d %s' % (mux, res), '%d' % mux]])
            #block output
            self.blockstring += ' to %d%s' % (mux, res)
        self.substate = 'COMMAND'

    def handle_par_0xb0(self, param): # page start address
        #bit output:
        for i in (4, 5, 7): self.put1(i)
        for i in (3, 6): self.put0(i)
        self.putd(2, 0, [cmds2[self.prevreg][1], 
            ['Page start addr= %d' % param & 0x7,
             'pg st %d' % param & 0x7, 'PSA']])
        #cmd output: none
        #block output
        self.blockstring += '= %d' % param & 0x7

    def handle_par_0xd3(self, param): # set vertical offset
        vo = param & 0x3f
        #bit output:
        for i in (6,7): self.putr(i)
        self.putd(5, 0, [anb_par, ['vo: %d' % vo, 'vo']])
        #cmd output
        self.putd(7, 0, [cmds2[self.prevreg][1], ['Vertical offset = %d' % vo,
            'V ofs: %d' % vo, 'VO']])
        #block output
        self.blockstring += ' = %d' % vo
        self.substate = 'COMMAND'

    def handle_par_0xd5(self, param): # display clock ratio 
        of = (param >> 4) & 0xf
        dr = (param & 0xf) + 1
        #bit output
        self.putd(7, 4, [anb_par, ['of: %d' % of, 'of']])            
        self.putd(3, 0, [anb_par, ['dr: %d' % dr, 'dr']])
        #cmd output
        res = '(reset)' if of == 8 else ''
        self.putd(7, 0, [cmds2[self.prevreg][1], 
            ['Freq=%d, div ratio=%d %s' % (of,dr,res),
             'f, r: %d, %d' % (of,dr), 'FR']])
        #block output
        self.blockstring += ': fOSC=%d, divide ratio=%d %s' % (of,dr,res)
        self.substate = 'COMMAND'

    def handle_par_0xd6(self, param): # zoom-in
        if param & 1 == 0:
            zo = 'disable (reset)'
            z = 0
        else:
            zo = 'enable'    
            z = 1
        #bit output
        for i in range(1,8): self.put0(i)
        self.putd(0, 0, [anb_par, ['zo: %s' % z, 'zo']])            
        #cmd output
        self.putd(7, 0, [cmds2[self.prevreg][1], 
            ['Zoom-in: %s' % zo, 'Zoom: %d' % z, 'ZI']])
        #block output
        self.blockstring += ': %s' % zo
        self.substate = 'COMMAND'

    def handle_par_0xd9(self, param): # set pre-charge period
        p1 = param & 0xf
        p2 = (param >> 4) & 0xf
        #bit output:
        self.putd(3, 0, [anb_par, ['phase1', 'p1']])
        self.putd(7, 4, [anb_par, ['phase2', 'p2']])
        #cmd output:
        if p1 == 0 or p2 == 0: 
            self.put(self.ss_block, self.es, self.out_ann,
                 [anw_warn, 
                 ['invalid precharge period = 0 (p1: %d, p2: %d)' % (p1, p2)]])
            self.blockstring = ''     
        else:
            res1 = ' (reset)' if p1 == 2 else ''
            res2 = ' (reset)' if p2 == 2 else ''
            self.putd(7, 0, [cmds2[self.prevreg][1], 
                ['P1=%d%s, P2=%d%s' % (p1, res1, p2, res2),
                 'p1, p2: %d, %d' % (p1,p2), 'PC']])
            #block output
            self.blockstring += ': P1=%d%s, P2=%d%s' % (p1, res1, p2, res2)
        self.substate = 'COMMAND'

    def handle_par_0xda(self, param): # set COM pins
        (seq, s) = ('sequential', 's') if param & 0x20f else ('alternative', 'a')
        (lrm, l) = ('no ', 'N')  if param & 0x10f else ('', 'R')
        #bit output
        for i in (7,6,3,2,0): self.put0(i)
        self.put1(1)
        self.putd(5, 5, [anb_par, ['LRM', 'L']])
        self.putd(4, 4, [anb_par, ['SEQ', 'S']])
        #cmd output
        self.putd(7, 0, [cmds2[self.prevreg][1], 
            ['COM pins: %s, %s L/R remap' % (seq, lrm),
             '%s %s' % (s, l), 'CP']])
        #block output
        self.blockstring += ': %s, %s L/R remap' % (seq, lrm)
        self.substate = 'COMMAND'

    def handle_par_0xdb(self, param): # set Vcomh deselect
        vc = (param >> 4) & 7
        #bit output
        for i in (7,3,2,1,0): self.put0(i)
        self.putd(6, 4, [anb_par, ['Vch', 'V']])
        if vc not in (0, 2, 3): 
            self.put(self.ss_block, self.es, self.out_ann,
                 [anw_warn, ['invalid Vcomh deselect = 0x%02x' % vc]])
            self.blockstring = ''
        else:
            vcomh=('0.65 Vcc', '', '0.77 Vcc (reset)', '0.83 Vcc')
            self.putd(7, 0, [cmds2[self.prevreg][1], ['Vcomh = %s' % vcomh[vc],
                '%s' % vcomh[vc], 'VD']])
            self.blockstring += ': Vcomh = %s' % vcomh[vc]
        self.substate = 'COMMAND'

    def handle_command(self, b, param=-1):
        #normalize "range commands" ID to range start
        if b in range(0x0, 0x10):
            b1 = b
            b = 0x0
        if b in range(0x10, 0x20):
            b1 = b
            b = 0x10
        if b in range(0x40, 0x80):
            b1 = b
            b = 0x40
        if b in range(0xB0, 0xB8):
            b1 = b
            b = 0xB0            
        if b in cmds2:
            if self.substate == 'COMMAND':
                #bit output: none
                #cmd output:
                self.putd(7, 0, [cmds2[b][1], cmds2[b][2]])
                #block output:
                self.blockstring = cmds2[b][2][0]
                self.sscmd = self.ss_block
                self.prevreg = b
                #cmds w/ parameters
                if cmds2[b][3]:
                    self.substate='PARAMETER'
                if b in (0x0, 0x10, 0x40, 0xB0):
                    #handlers for 0x0 - 0xF, 0x10-0x1F, 0x40-0x7F, 0xB0-0xB7
                    fn = getattr(self, 'handle_par_0x%02x' % b)
                    fn(b1)
            else:
                fn = getattr(self, 'handle_par_0x%02x' % b)
                fn(param)
            if self.substate == 'COMMAND':
                #block output
                self.put(self.sscmd, self.es, self.out_ann,
                    [anbl_block, [self.blockstring]])

    def handle_data(self, b):
        self.putd(7, 0, [ann_GR, ['GDDRAM data: 0x%02X' % b, 
                                  'RAM: %02X' % b, 'RAM']])

    def is_correct_chip(self, addr):
        if addr in (SSD1306_I2C_ADDRESS, SSD1306_I2C_ADDRESS_2) :
            self.putd(7, 0, [ann_DA, ['Device address: 0x%02X' % addr,
              'Dev Addr: 0x%02X' % addr, 'Dev Addr', 'DA']])
            return True
        self.put(self.ss_block, self.es, self.out_ann,
                 [anw_warn, 
                     ['Ignoring non-SSD1306 data (slave 0x%02X)' % addr]])
        return False

    def handle_controlbyte(self, b):
        self.putd(7, 0, [ann_CB, ['Control byte = 0x%02X' % b, 
            'Ctrl bt = 0x%02X' % b, 'CB']])
        for i in range(0,6):
            self.putr(i)
        cb=b >> 7
        dc=(b >> 6) & 1    
        self.putd(7, 7, [anb_CB, ['Continuation bit: %d' % cb,
                  'cb: %d' % cb, 'CB']])
        self.putd(6, 6, [anb_DC, ['Data / command bit: %d' % dc,
                  'dc: %d' % dc, 'DC']])

    def decode(self, ss, es, data):
        cmd, databyte = data

        # Collect the 'BITS' packet, then return. The next packet is
        # guaranteed to belong to these bits we just stored.
        if cmd == 'BITS':
            self.bits = databyte
            return

        # Store the start/end samples of this I²C packet.
        self.ss, self.es = ss, es

        # State machine.
        if self.state == 'IDLE':
            # Wait for an I²C START condition.
            if cmd != 'START':
                return
            self.state = 'GET SLAVE ADDR'
            self.ss_block = ss
        elif self.state == 'GET SLAVE ADDR':
            # Wait for an address write operation.
            if cmd != 'ADDRESS WRITE':
                return
            if not self.is_correct_chip(databyte):
                self.state = 'IDLE'
                return
            self.state = 'WRITE CONTROL BYTE'
        elif self.state == 'WRITE CONTROL BYTE':
            # Get control byte.
            if cmd == 'DATA WRITE':
                self.handle_controlbyte(databyte)
                if databyte == 0x80:
                    self.state = 'SSD COMMAND'
                elif databyte == 0x40:    
                    self.state = 'SSD DATA'
                else:
                    self.state = 'IDLE'
            elif cmd == 'STOP':
                self.state = 'IDLE'        
        elif self.state == 'SSD COMMAND':        
            # Get command byte.
            if cmd == 'DATA WRITE':
                if self.substate == 'COMMAND':
                    self.handle_command(databyte)
                else: #substate 'PARAMETER' (2, ...)
                    self.handle_command(self.prevreg, databyte)    
                self.state='WRITE CONTROL BYTE'    
        elif self.state == 'SSD DATA':
            # Get data bytes until a STOP condition occurs.
            if cmd == 'DATA WRITE':
                self.handle_data(databyte)
            elif cmd == 'STOP':
                self.state = 'IDLE'
