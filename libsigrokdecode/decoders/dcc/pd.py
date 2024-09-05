##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2013-2020 Sven Bursch-Osewold
##               2020      Roland Noell  
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

'''
Used norms:
RCN-210 (01.12.2019)
RCN-211 (02.12.2018) 
RCN-212 (01.12.2019)
RCN-213 (27.07.2015)
RCN-214 (02.12.2018)
RCN-216 (17.12.2017)
RCN-217 (01.12.2019)
'''

import sigrokdecode as srd

class SamplerateError(Exception):
    pass

class Ann:
    BITS, BITS_OTHER, FRAME, FRAME_OTHER, DATA, DATA_ACC, DATA_DEC, DATA_CV, COMMAND, ERROR, SEARCH_ACC, SEARCH_DEC, SEARCH_CV, SEARCH_BYTE = range(14)

class Decoder(srd.Decoder):
    maxInterferingPulseWidth = 4 #µs (ignoreInterferingPulse)

    api_version = 3
    id          = 'dcc'
    name        = 'DCC'
    longname    = 'Digital Command Control'
    desc        = 'DCC protocol (operate model railways digitally)'
    license     = 'gplv2+'
    inputs      = ['logic']
    outputs     = []
    tags        = ['Encoding']
    channels    = (
        {'id': 'data', 'name': 'D0', 'desc': 'Data line'},
    )
    annotations = (
        ('bits1',   'Bits'),
        ('bits2',   'Other'),
        ('frame1',  'Frame'),
        ('frame2',  'Other'),
        ('data1',   'Data'),
        ('data2',   'Accessory address'),
        ('data3',   'Decoder address'),
        ('data4',   'CV'),
        ('command', 'Command'),
        ('error',   'Error'),
        ('search1', 'Accessory address'),
        ('search2', 'Decoder address'),
        ('search3', 'CV'),
        ('search4', 'Byte'),
    )
    annotation_rows = (
        ('bits_',    'Bits',    (Ann.BITS, Ann.BITS_OTHER,)),
        ('frame_',   'Frame',   (Ann.FRAME, Ann.FRAME_OTHER,)),
        ('data_',    'Data',    (Ann.DATA_ACC, Ann.DATA_DEC, Ann.DATA_CV, Ann.DATA,)),
        ('command_', 'Command', (Ann.COMMAND,)),
        ('error_',   'Error',   (Ann.ERROR,)),
        ('search_',  'Search',  (Ann.SEARCH_ACC, Ann.SEARCH_DEC, Ann.SEARCH_CV, Ann.SEARCH_BYTE,)),
    )
    options = (
        {'id': 'CV_29_1',            'desc': 'CV29 Bit 1',              'default': '1: 28/128 speed mode', 'values': ('1: 28/128 speed mode', '0: 14 speed mode') },
        {'id': 'Mode_112_127',       'desc': 'addr. 112-127',           'default': 'operation mode', 'values': ('operation mode', 'service mode') },
        {'id': 'Addr_offset',        'desc': 'accessory addr. offset',  'default': 0 },
        {'id': 'Search_acc_addr',    'desc': 'search acc. addr. [dec]', 'default': '' },
        {'id': 'Search_dec_addr',    'desc': 'search dec. addr. [dec]', 'default': '' },
        {'id': 'Search_cv',          'desc': 'search CV [dec]',         'default': '' },
        {'id': 'Search_byte',        'desc': 'search byte [dec/0b/0x]', 'default': '' },
        {'id': 'Ignore_short_pulse', 'desc': 'ignore pulse <= '+str(maxInterferingPulseWidth)+' µs', 'default': 'no', 'values': ('no', 'yes') },
    )

    weekday = ['Monday',    #0
               'Tuesday',   #1
               'Wednesday', #2
               'Thursday',  #3
               'Friday',    #4
               'Saturday',  #5
               'Sunday'     #6
              ]
    weekday_short = ['Mo', #0
                     'Tu', #1
                     'We', #2
                     'Th', #3
                     'Fr', #4
                     'Sa', #5
                     'Su'  #6
                    ]
    month = ['?',     #0
             'Jan. ', #1
             'Feb. ', #2
             'Mar. ', #3
             'Apr. ', #4
             'Mai ',  #5
             'Jun. ', #6
             'Jul. ', #7
             'Aug. ', #8
             'Sep. ', #9
             'Oct. ', #10
             'Nov. ', #11
             'Dec. '  #12
            ]
    
    def putx(self, start, end, data):
        self.put(start, end, self.out_ann, data)
        
    def put_signal(self, data):
        self.put(self.edge_1, self.edge_3, self.out_ann, data)
        
    def put_packetbyte(self, packetByte, pos, data):
        self.put(packetByte[pos][1][0], packetByte[pos][1][8], self.out_ann, data)
        
    def put_packetbytes(self, packetByte, start, end, data):
        self.put(packetByte[start][1][0], packetByte[end][1][8], self.out_ann, data)
    
    def __init__(self):
        self.reset()

    def reset(self):
        #This function is called before the beginning of the decoding. This is the place to reset variables internal to your protocol decoder to their initial state, such as state machines and counters.
        self.dccStart               = 0
        self.dccLast                = 0
        self.dccBitCounter          = 0
        self.dccBitPos              = []
        self.dccValue               = 0
        self.decodedBytes           = []
        self.dccStatus              = 'WAITINGFORPREAMBLE'
        self.syncSignal             = True
        self.cond1                  = 'r'  #raising-edge
        self.cond2                  = 'f'  #falling-edge
        self.dec_addr_search        = -2
        self.acc_addr_search        = -2
        self.cv_addr_search         = -2
        self.byte_search            = -2
        self.speed14                = False
        self.serviceMode            = False
        self.addrOffset             = 0
        self.ignoreInterferingPulse = 'no'

    def start(self):
        #This function is called before the beginning of the decoding. This is the place to register() the output types, check the user-supplied PD options for validity, and so on.
        self.out_ann = self.register(srd.OUTPUT_ANN)

        ##############
        #read and verify options
        self.AddrOffset             = self.options['Addr_offset']
        self.ignoreInterferingPulse = self.options['Ignore_short_pulse']

        if self.options['CV_29_1']      == '0: 14 speed mode':
            self.speed14     = True;

        if self.options['Mode_112_127'] == 'service mode':
            self.serviceMode = True;
        
        try:
            self.acc_addr_search = int(self.options['Search_acc_addr'])
        except:
            self.acc_addr_search = -2
        if self.acc_addr_search < 1 or self.acc_addr_search > 2047:
            self.acc_addr_search = -2
        
        try:
            self.dec_addr_search = int(self.options['Search_dec_addr'])
        except:
            self.dec_addr_search = -2
        if self.dec_addr_search < 0 or self.dec_addr_search > 10239:
            self.dec_addr_search = -2
        
        try:
            self.cv_addr_search  = int(self.options['Search_cv'])
        except:
            self.cv_addr_search  = -2
        if self.cv_addr_search < 1 or self.cv_addr_search > 16777216:
            self.cv_addr_search = -2

        try:
            self.byte_search = int(self.options['Search_byte'], base=10)
        except:
            try:
                self.byte_search = int(self.options['Search_byte'], base=2)
            except:
                try:
                    self.byte_search = int(self.options['Search_byte'], base=16)
                except:
                    self.byte_search = -2
        if self.byte_search < 0 or self.byte_search > 255:
            self.byte_search = -2
        
    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value;

    def incPos(self, pos, packetByte):
        #Support function: Returns next position of packet if position exists
        if pos+1 < len(packetByte):
            return pos+1, False
        else:
            self.put_packetbyte(packetByte, pos, [Ann.ERROR, ['Byte missing at next position: ' + str(pos+2)]])
            return pos, True  #avoid access violation
            
    def handleDecodedBytes(self, packetByte):
        validPacketFound = False
        acc_addr         = -1  #found accessory address
        dec_addr         = -1  #found decoder address
        cv_addr          = -1  #found CV

        if len(packetByte) < 3:
            self.put_packetbytes(packetByte, 0, len(packetByte)-1, [Ann.ERROR, ['Paket too short: ' + str(len(packetByte)) + ' Byte only']])
            return

        pos      = 0  #position within packet
        idPacket = packetByte[pos][0] 

        ##############
        ## Servicemode
        if self.serviceMode == True:
            if 112 <= idPacket <= 127:
                if packetByte[pos][0] >> 4 == 0b0111 and len(packetByte) == 3:
                    ##[RCN-214 5] Register/Page Mode packet
                    if (packetByte[pos][0] >> 3) & 1 == 0:
                        output_long  = 'Verify, Register:'
                        output_short = 'v, R:'
                    else:
                        output_long  = 'Write, Register:'
                        output_short = 'w, R:'
                    output_long  += str((packetByte[pos][0] & 0b111) + 1)
                    output_short += str((packetByte[pos][0] & 0b111) + 1)
                    self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                    pos, error = self.incPos(pos, packetByte)
                    if error == True: return
                    if packetByte[pos-1][0] == 0b01111101 and packetByte[pos][0] == 1:
                        ##[RCN-216 4.2]
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, ['Register/Page Mode (outdated): Page Preset']])
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [str(packetByte[pos][0])]])
                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.COMMAND, ['Register/Page Mode (outdated)']])
                    
                    validPacketFound = True
                
                elif packetByte[pos][0] >> 4 == 0b0111 and len(packetByte) == 4:
                    ##[RCN-214 2]
                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Service Mode', 'Service']])
                    if (packetByte[pos][0] >> 2) & 0b11 == 0b01:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, ['Verify byte', 'v']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        cv_addr = (packetByte[pos-1][0] & 0b00000011)*256 + packetByte[pos][0] + 1
                        self.put_packetbyte(packetByte, pos, [Ann.DATA_CV, [str(cv_addr)]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Value']])
                    
                    elif (packetByte[pos][0] >> 2) & 0b11 == 0b11:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Write byte', 'w']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        cv_addr = (packetByte[pos-1][0] & 0b00000011)*256 + packetByte[pos][0] + 1
                        self.put_packetbyte(packetByte, pos, [Ann.DATA_CV, [str(cv_addr)]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Value']])
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                    
                    elif (packetByte[pos][0] >> 2) & 0b11 == 0b10:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Bit manipulation', 'bit']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        cv_addr = (packetByte[pos-1][0] & 0b00000011)*256 + packetByte[pos][0] + 1
                        self.put_packetbyte(packetByte, pos, [Ann.DATA_CV, [str(cv_addr)]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if ((packetByte[pos][0] & 0b00010000) == 0b00010000):
                            output_long = 'Write, '
                            output_short = 'w,'
                        else:
                            output_long = 'Verify, '
                            output_short = 'v,'
                        output_long  += str(packetByte[pos][0] & 0b00000111)
                        output_short += str(packetByte[pos][0] & 0b00000111)
                        if ((packetByte[pos][0] & 0b00001000) == 0b00001000):
                            output_long  += ', 1'
                            output_short += ',1'
                        else:
                            output_long  += ', 0'
                            output_short += ',0'
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [output_long, output_short]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Operation, Position, Value', 'Op.,Pos,Value', 'O,P,V']])
                    
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, ['Reserved for future use', 'Res.']])
                    
                    validPacketFound = True

        #############################
        ## Normal = (Not Servicemode)
        if     (self.serviceMode == False)\
            or (self.serviceMode == True and not (112 <= idPacket <= 127)):
            pos = 0  #position within packet
            if     (0   <= idPacket <= 127)\
                or (192 <= idPacket <= 231):
                ##[RCN-211 3] Multi-Function Decoder
            
                if idPacket == 0:
                    dec_addr = 0
                    self.put_packetbyte(packetByte, pos, [Ann.DATA_DEC, ['Broadcast']])
                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND,  ['Broadcast']])
                
                elif 1 <= idPacket <= 127:
                    dec_addr = packetByte[pos][0] & 0b01111111
                    self.put_packetbyte(packetByte, pos, [Ann.DATA_DEC, [str(dec_addr)]])
                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND,  ['Multi Function Decoder with 7 bit address', 'Decoder with 7 bit address', '7 bit addr.']])
                
                elif 192 <= idPacket <= 231:
                    pos, error = self.incPos(pos, packetByte)
                    if error == True: return
                    dec_addr = ((packetByte[pos-1][0] & 0b00111111)*256) + packetByte[pos][0]
                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.DATA_DEC, [str(dec_addr)]])
                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.COMMAND,  ['Multi Function Decoder with 14 bit address', 'Decoder with 14 bit address', '14 bit addr.']])
            
                pos, error = self.incPos(pos, packetByte)
                if error == True: return
                cmd    = (packetByte[pos][0] & 0b11100000) >> 5
                subcmd = (packetByte[pos][0] & 0b00011111)
                if cmd == 0b000:  
                    ##[RCN-212 2.1] Decoder Control
                    if   subcmd == 0b00000:
                        if dec_addr == 0:
                            ##[RCN-211 4.1]
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Decoder Reset packet', 'Dec. Reset', 'Reset']])
                        else:
                            ##[RCN-212 2.5.1]
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Decoder Reset', 'Dec. Reset', 'Reset']])
                    
                    elif subcmd == 0b00001:
                        ##[RCN-212 2.5.2]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Decoder Hard Reset', 'Hard Reset', 'Reset']])
                    
                    elif subcmd & 0b11110 == 0b00010:
                        ##[RCN-212 2.5.3]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Factory Test Instruction', 'Fac. Test', 'Test']])
                        validPacketFound = True
                    
                    elif subcmd & 0b11110 == 0b01010:
                        ##[RCN-212 2.5.4]
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0] & 0b00000001)]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Set Advanced Addressing (CV #29 Bit 5)', 'Set advanced addressing', 'Set adv. addr.']])
                    
                    elif subcmd == 0b01111:
                        ##[RCN-212 2.5.5]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Decoder Acknowledgment Request', 'Dec. Ack Req.', 'Ack Req.']])
                    
                    elif subcmd & 0b10000 == 0b10000:
                        ##[RCN-212 2.4.1]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Consist Control']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if subcmd & 0b11110 == 0b10010:
                            if packetByte[pos-1][0] & 1 == 0:
                                value = 'normal'
                            else:
                                value = 'reverse'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0] & 0b01111111) + ', dir:' + str(value)]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Set consist address', 'Set']])
                        else:
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
                    
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
                
                elif cmd == 0b001:  
                    ##[RCN-212 2.1] Advanced Operations Instruction
                    if subcmd == 0b11111:
                        ##[RCN-212 2.2.2]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['128 Speed Step Control - Instruction']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if dec_addr == 0:
                            output_long  = 'Broadcast'
                            output_short = 'B'
                        else:
                            if packetByte[pos][0] >> 7 == 1:
                                output_long  = 'Forward'
                                output_short = 'F'
                            else:
                                output_long  = 'Reverse'
                                output_short = 'R'
                        if packetByte[pos][0] & 0b01111111 == 0b00000000:
                            output_long  = 'STOP (' + output_long  + ')'
                            output_short = 'STOP (' + output_short + ')'
                        elif packetByte[pos][0] & 0b01111111 == 0b00000001:
                            output_long  = 'EMERGENCY STOP (HALT) (' + output_long  + ')'
                            output_short = 'ESTOP ('                 + output_short + ')'
                        else:
                            speed = str(((packetByte[pos][0]) & 0b01111111)-1)
                            output_long  += ' Speed: ' + speed + ' / 126'
                            output_short += ':'        + speed
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                    
                    elif subcmd == 0b11110:
                        ##[RCN-212 2.2.3]
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbytes(packetByte, pos-1, pos, [Ann.COMMAND, ['Special operation mode (unless received via consist address in CV#19)', 'Special operation mode']])
                        output_1 = ''
                        if (packetByte[pos][0] >> 2) & 0b11 == 0b00:
                            output_1 += 'Not part of a multiple traction'
                        elif (packetByte[pos][0] >> 2) & 0b11 == 0b10:
                            output_1 += 'Leading loco of multiple traction'
                        elif (packetByte[pos][0] >> 2) & 0b11 == 0b01:
                            output_1 += 'Middle loco in a multiple traction'
                        elif (packetByte[pos][0] >> 2) & 0b11 == 0b11:
                            output_1 += 'Final loco of a multiple traction'
                        output_1 += ', shunting key:' + str((packetByte[pos][0] >> 4) & 1)
                        output_1 += ', west-bit:'     + str((packetByte[pos][0] >> 5) & 1)
                        output_1 += ', east-bit:'     + str((packetByte[pos][0] >> 6) & 1)
                        output_1 += ', MAN-bit:'      + str((packetByte[pos][0] >> 7) & 1)
                        self.put_packetbytes(packetByte, pos-1, pos, [Ann.DATA,    [output_1]])
                            
                    elif subcmd == 0b11101:
                        ##[RCN-212 2.3.8]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Analog Function Group']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if packetByte[pos][0] == 0b00000001:
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Volume control']])
                        elif 0b00010000 <= packetByte[pos][0] <= 0b00011111:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0] & 0b00001111)]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Position control']])
                        elif 0b10000000 <= packetByte[pos][0] <= 0b11111111:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0] & 0b01111111)]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Any control']])
                        else:
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data']])
                    
                    elif subcmd == 0b11100:
                        ##[RCN-212 2.3.7]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Speed, Direction, Function']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if dec_addr == 0:
                            output_long  = 'Broadcast'
                            output_short = 'B'
                        else:
                            if packetByte[pos][0] >> 7 == 1:
                                output_long  = 'Forward'
                                output_short = 'F'
                            else:
                                output_long  = 'Reverse'
                                output_short = 'R'
                        if packetByte[pos][0] & 0b01111111 == 0b00000000:
                            output_long  = 'STOP (' + output_long  + ')'
                            output_short = 'STOP (' + output_short + ')'
                        elif packetByte[pos][0] & 0b01111111 == 0b00000001:
                            output_long  = 'EMERGENCY STOP (HALT) (' + output_long  + ')'
                            output_short = 'ESTOP ('                 + output_short + ')'
                        else:
                            speed = str(((packetByte[pos][0]) & 0b01111111)-1)    
                            output_long  += ' Speed: ' + speed + ' / 126'
                            output_short += ':'        + speed
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                        numbers = [0, 8, 16, 24]
                        for f in numbers:
                            if len(packetByte) > pos+2:  #more data + checksum
                                pos, error = self.incPos(pos, packetByte)
                                if error == True: return
                                value = packetByte[pos][0]
                                output_long  = ''
                                output_short = 'F' + str(f) + ':'
                                for i in range(0, 8):
                                    output_long  += 'F' + str(f + i) + ':' + str(value & 1)
                                    output_short += str(value & 1)
                                    if (i<7):
                                        output_long  += ', '
                                        output_short += ','
                                    value = value >> 1
                                self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                            else:
                                break
                                                    
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
                
                elif cmd in [0b010, 0b011]:  
                    ##[RCN-212 2.2.1]
                    if self.speed14 == True:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Basis Speed and Direction Instruction 14 speed step mode (CV#29=0)', 'Speed + Dir. 14 step', 'Speed 14']])
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Basis Speed and Direction Instruction 28 speed step mode (CV#29=1)', 'Speed + Dir. 28 step', 'Speed 28']])
                    output_long14  = ''
                    output_short14 = ''
                    output_long28  = ''
                    output_short28 = ''
                    bit5           = (subcmd & 0b10000) >> 4
                    if dec_addr == 0:
                        output_long14  = 'Broadcast'
                        output_short14 = 'B'
                    else:
                        if cmd & 0b001 == 0b001:
                            output_long14  = 'Forward'
                            output_short14 = 'F'
                        else:
                            output_long14  = 'Reverse'
                            output_short14 = 'R'
                    output_long28  = output_long14
                    output_short28 = output_short14
                    if subcmd & 0b01111 == 0b00000:
                        output_long14  = 'STOP (' + output_long14  + ')'
                        output_short14 = 'STOP (' + output_short14 + ')'
                        output_long28  = 'STOP (' + output_long28  + ')'
                        output_short28 = 'STOP (' + output_short28 + ')'
                    elif subcmd & 0b01111 == 0b00001:
                        output_long14  = 'EMERGENCY STOP (HALT) (' + output_long14  + ')'
                        output_short14 = 'ESTOP ('                 + output_short14 + ')'
                        output_long28  = 'EMERGENCY STOP (HALT) (' + output_long28  + ')'
                        output_short28 = 'ESTOP ('                 + output_short28 + ')'
                    else:
                        output_long14  += ' Speed: ' + str((subcmd & 0b1111)-1) + ' / 14'
                        output_short14 += ':'       + str((subcmd & 0b1111)-1)
                        output_long28  += ' Speed: ' + str((((((subcmd & 0b01111)-1)*2)-1) + bit5)) + ' / 28'
                        output_short28 += ':'       + str((((((subcmd & 0b01111)-1)*2)-1) + bit5))
                    if dec_addr > 0:
                        output_long14  += ', F0=' + str(bit5)
                        output_short14 += ', F0=' + str(bit5)
                    if self.speed14 == True:
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long14, output_short14]])
                    else:    
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long28, output_short28]])
                
                elif cmd == 0b100:
                    ##[RCN-212 2.3.1]
                    if self.speed14 == True:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Function Group One Instruction 14 speed step mode (CV#29=0)',     'FG1 14 step',     'FG1']])
                    else:    
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Function Group One Instruction 28/128 speed step mode (CV#29=1)', 'FG1 28/128 step', 'FG1']])

                    f = 1
                    output_long  = ''
                    output_short = ''
                    value = subcmd
                    for i in range(0, 4):
                        output_long  = output_long  + 'F' + str(f) + ':' + str(value & 1)
                        output_short = output_short + str(value & 1)
                        if (i<3):
                            output_long  = output_long  + ', '
                            output_short = output_short + ','
                        value = value >> 1
                        f += 1
                        
                    if self.speed14 == True:
                        output_short = 'F1:' + output_short
                    else:
                        output_long  = 'F0:' + str(subcmd >> 4) + ', ' + output_long
                        output_short = 'F0:' + str(subcmd >> 4) + ','  + output_short
                    self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                
                elif cmd == 0b101:
                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Function Group Two Instruction', 'FG2']])
                    if subcmd & 0b10000 == 0b10000:
                        ##[RCN-212 2.3.2]
                        f = 5
                    else:
                        ##[RCN-212 2.3.3]
                        f = 9
                    output_long  = ''
                    output_short = 'F' + str(f) + ':'
                    value = subcmd
                    for i in range(0, 4):
                        output_long  = output_long  + 'F' + str(f) + ':' + str(value & 1)
                        output_short = output_short + str(value & 1)
                        if (i<3):
                            output_long  = output_long  + ', '
                            output_short = output_short + ','
                        value = value >> 1
                        f += 1
                    self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                
                elif cmd == 0b110:
                    ##[RCN-212 2.3.4]
                    pos, error = self.incPos(pos, packetByte)
                    if error == True: return
                    self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND, ['Future Expansion Instruction']])
                    if subcmd in [0b11111, 0b11110, 0b11100, 0b11011, 0b11010, 0b11001, 0b11000]: #F13 - F68
                        value = packetByte[pos][0]
                        f = 0
                        if subcmd == 0b11110:
                            f = 13
                        if subcmd == 0b11111:
                            f = 21
                        if subcmd == 0b11000:
                            f = 29
                        if subcmd == 0b11001:
                            f = 37
                        if subcmd == 0b11010:
                            f = 45
                        if subcmd == 0b11011:
                            f = 53
                        if subcmd == 0b11100:
                            f = 61
                        output_long  = ''
                        output_short = 'F' + str(f) + ':'
                        for i in range(0, 8):
                            output_long  = output_long  + 'F' + str(f + i) + ':' + str(value & 1)
                            output_short = output_short + str(value & 1)
                            if (i<7):
                                output_long  = output_long  + ', '
                                output_short = output_short + ','
                            value = value >> 1
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                        
                    elif subcmd == 0b11101:
                        ##[RCN-212 2.3.5]
                        ##[RCN-217 4.3.1]
                        address = packetByte[pos][0] & 0b01111111
                        self.put_packetbyte(packetByte, pos-1, [Ann.DATA, ['Binary State Control Instruction short form', 'Binarystate short']])
                        if address == 0:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0] >> 7)]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Broadcast F29-F127']])
                        elif 1 <= address <= 15:
                            ##[RCN-217 4.3.1]
                            if address == 1:
                                ##[RCN-217 5.3.1]
                                if packetByte[pos][0] >> 7 == 0:
                                    output_long  = 'XF=1 (Requesting the location information)'
                                else:
                                    output_long  = 'XF=1'
                                output_short = 'XF=1'
                            elif address == 2:
                                ##[RCN-217 5.2.2]
                                if packetByte[pos][0] >> 7 == 0:
                                    output_long  = 'XF=2 (Rerail search)'
                                else:
                                    output_long  = 'XF=2'
                                output_short = 'XF=2'
                            else:
                                output_long  = 'XF=' + str(address) + ' (Reserved)'
                                output_short = 'XF=' + str(address) + ' (Res.)'
                            if packetByte[pos][0] >> 7 == 0:
                                output_long  += ':off'
                                output_short += ':off'
                            else:
                                output_long  += ':on'
                                output_short += ':on'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [output_long, output_short]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['RailCom']])
                        elif 16 <= address <= 28:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [hex(packetByte[pos][0]) + '/' + str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Special uses']])
                        else:
                            if packetByte[pos-1][0] >> 7 == 0:
                                output_1 = 'off'
                            else:
                                output_1 = 'on'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['F' + str(address) + ':' + output_1]])
                            
                    elif subcmd == 0b00000:
                        ##[RCN-212 2.3.6]
                        self.put_packetbyte(packetByte, pos-1, [Ann.DATA, ['Binary State Control Instruction long form', 'Binarystate long']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        address = (packetByte[pos][0]*128) + (packetByte[pos-1][0] & 0b01111111)
                        if packetByte[pos-1][0] >> 7 == 0:
                            output_1 = 'off'
                        else:
                            output_1 = 'on'
                        if address == 0:
                            self.put_packetbytes(packetByte, pos-1, pos, [Ann.DATA,    [output_1]])
                            self.put_packetbytes(packetByte, pos-1, pos, [Ann.COMMAND, ['Broadcast F29-F32767']])
                        elif packetByte[pos-1][0] & 0b01111111 == 0:
                            self.put_packetbytes(packetByte, pos-1, pos, [Ann.ERROR,   ['Use binarystate short']])
                        else:
                            self.put_packetbytes(packetByte, pos-1, pos, [Ann.DATA,    ['F' + str(address) + ':' + output_1]])
                            
                    elif subcmd == 0b00001:
                        ##[RCN-212 2.3.9]
                        if dec_addr != 0:
                            self.put_packetbytes(packetByte, 0, len(packetByte)-2, [Ann.ERROR, ['Only Broadcast allowed']])
                        value = packetByte[pos][0]
                        if (value >> 6) & 0b11 == 0b00:
                            self.put_packetbyte(packetByte, pos-1, [Ann.DATA,  ['Model-Time']])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['00MMMMMM']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['WWWHHHHH']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['U0BBBBBB']])
                            output_long  = self.weekday[packetByte[pos-1][0] >> 5] + ' ' + '{:02.0f}'.format(packetByte[pos-1][0] & 0b00011111) + ':'\
                                           + '{:02.0f}'.format(packetByte[pos-2][0] & 0b00111111) + ' hrs, Update:' + str(packetByte[pos][0] >> 7) + ', Acceleration:' + str(packetByte[pos][0] & 0b00111111)
                            output_short = self.weekday_short[packetByte[pos-1][0] >> 5] + ' ' + '{:02.0f}'.format(packetByte[pos-1][0] & 0b00011111) + ':'\
                                           + '{:02.0f}'.format(packetByte[pos-2][0] & 0b00111111) + ', U:' + str(packetByte[pos][0] >> 7) + ', Acc:' + str(packetByte[pos][0] & 0b00111111)
                        elif (value >> 6) & 0b11 == 0b01:
                            self.put_packetbyte(packetByte, pos-1, [Ann.DATA,  ['Model-Date']])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['010TTTTT']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['MMMMYYYY']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['YYYYYYYY']])
                            output_long  = str(packetByte[pos-2][0] & 0b00011111) + '. ' + self.month[(packetByte[pos-1][0] >> 4)] + str(((packetByte[pos-1][0] & 0b00001111) << 8) + packetByte[pos][0])
                            output_short = str(packetByte[pos-2][0] & 0b00011111) + '.'  + str(packetByte[pos-1][0] >> 4) + '.'    + str(((packetByte[pos-1][0] & 0b00001111) << 8) + packetByte[pos][0])
                        else:
                            output_long  = 'Reserved'
                            output_short = 'Res.'
                            self.put_packetbyte(packetByte, pos-1, [Ann.DATA,   ['Reserved']])
                        self.put_packetbytes(packetByte, pos-2, pos, [Ann.DATA, [output_long, output_short]])
                            
                    elif subcmd == 0b00010:
                        ##[RCN-212 2.3.10]
                        if dec_addr != 0:
                            self.put_packetbytes(packetByte, 0, len(packetByte)-2, [Ann.ERROR, ['Only Broadcast allowed']])
                        self.put_packetbyte(packetByte, pos-1,       [Ann.DATA,    ['Systemtime']])
                        self.put_packetbyte(packetByte, pos,         [Ann.COMMAND, ['MMMMMMMM']])
                        value = packetByte[pos][0]
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos,         [Ann.COMMAND, ['MMMMMMMM']])
                        value = value * 256 + packetByte[pos][0]
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos,         [Ann.COMMAND, ['MMMMMMMM']])
                        value = value * 256 + packetByte[pos][0]
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        self.put_packetbyte(packetByte, pos,         [Ann.COMMAND, ['MMMMMMMM']])
                        value = value * 256 + packetByte[pos][0]
                        self.put_packetbytes(packetByte, pos-3, pos, [Ann.DATA, [str(value) + ' ms since systemstart (' + '{:.0f}'.format(value/60000) + ' minutes = ' + '{:.1f}'.format(value/3600000) + ' hours)',\
                                                                                 str(value) + ' ms since systemstart', str(value)]])
                    else:
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
                
                elif cmd == 0b111:  
                    if subcmd & 0b10000 == 0b10000:  #Short Form
                        ##[RCN-214 3]
                        ##[RCN-217 4.3.2]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND,     ['Configuration Variable Access Instruction - Short Form', 'CV Access Instruction short', 'CV short']])
                        if subcmd & 0b1111 == 0b0000:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Not available for use', 'Not av.']])
                        elif subcmd & 0b1111 == 0b0010:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Acceleration Value (CV#23)', 'CV#23']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data']])
                        elif subcmd & 0b1111 == 0b0011:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Deceleration Value (CV#24)', 'CV#24']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data']])
                        elif subcmd & 0b1111 == 0b0100:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Write CV#17 + CV#18', 'w CV#17+18']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV17']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV18']])
                        elif subcmd & 0b1111 == 0b0101:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Write CV#31 + CV#32', 'w CV#31+32']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV31']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV32']])
                        elif subcmd & 0b1111 == 0b1001:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Reserved (outdated: Service Mode Decoder Lock Instruction)', 'Res. (old: Dec. Lock)', 'Res.']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str((packetByte[pos][0] & 0b01111111))]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Short address', 'Addr.']])
                        else:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    ['Reserved (maybe service mode packet)', 'Reserved', 'Res.']])
                            
                    elif    (pos == 1 and len(packetByte) == 5)\
                         or (pos == 2 and len(packetByte) == 6):
                        ##[RCN-214 2]
                        ##[RCN-217 5.1]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Configuration Variable Access Instruction - Long Form (POM)', 'CV Access Instruction long (POM)', 'CV long (POM)']])
                        if (subcmd >> 2) & 0b11 in [0b01, 0b11, 0b10]:
                            if (subcmd >> 2) & 0b11 == 0b01:
                                output_long  = 'Read/Verify byte'
                                output_short = 'r/v'
                            elif (subcmd >> 2) & 0b11 == 0b11:
                                output_long  = 'Write byte'
                                output_short = 'w'
                            else:    
                                output_long  = 'Bit manipulation'
                                output_short = 'Bit'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,       [output_long, output_short]])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            cv_addr = (packetByte[pos-1][0] & 0b00000011)*256 + packetByte[pos][0] + 1
                            self.put_packetbyte(packetByte, pos, [Ann.DATA_CV,    [str(cv_addr)]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND,    ['CV']])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            if (subcmd >> 2) & 0b11 != 0b10:
                                self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                                self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Value']])
                            else:    
                                if packetByte[pos][0] & 0b10000 == 0b10000:
                                    output_long  = 'Write, '
                                    output_short = 'w,'
                                else:
                                    output_long  = 'Verify, '
                                    output_short = 'v,'
                                output_long  += str(packetByte[pos][0] & 0b00000111)
                                output_short += str(packetByte[pos][0] & 0b00000111)
                                if packetByte[pos][0] & 0b1000 == 0b1000:
                                    output_long  = output_long  + ', 1'
                                    output_short = output_short + ',1'
                                else:
                                    output_long  = output_long  + ', 0'
                                    output_short = output_short + ',0'
                                self.put_packetbyte(packetByte, pos, [Ann.DATA,    [output_long, output_short]])
                                self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Operation, Position, Value', 'Op.,Pos,Value', 'O,P,V']])
                        else:
                            output_long  = 'Reserved for future use'
                            output_short = 'Res.'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                            
                    elif    (pos == 1 and len(packetByte) >= 6)\
                         or (pos == 2 and len(packetByte) >= 7):
                        ##[RCN-214 4]
                        ##[RCN-217 5.5]
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['XPOM']])
                        if (subcmd >> 2) & 0b11 in [0b01, 0b11, 0b10]:
                            if (subcmd >> 2) & 0b11 == 0b01:
                                output_long  = 'Read bytes'
                                output_short = 'r'
                            elif (subcmd >> 2) & 0b11 == 0b11:
                                output_long  = 'Write byte(s)'
                                output_short = 'w'
                            elif (subcmd >> 2) & 0b11 == 0b10:
                                output_long  = 'Bit write'
                                output_short = 'bit'
                            output_long  += ', SS:' + str(packetByte[pos][0] & 0b11)
                            output_short += ',SS:'  + str(packetByte[pos][0] & 0b11)
                            self.put_packetbyte(packetByte, pos,         [Ann.DATA,    [output_long, output_short]])
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            pos, error = self.incPos(pos, packetByte)
                            if error == True: return
                            cv_addr = (packetByte[pos-2][0]*256 + packetByte[pos-1][0])*256 + packetByte[pos][0] + 1
                            self.put_packetbytes(packetByte, pos-2, pos, [Ann.DATA_CV, [str(cv_addr)]])
                            self.put_packetbytes(packetByte, pos-2, pos, [Ann.COMMAND, ['CV']])
                            if (subcmd >> 2) & 0b11 == 0b01:  ##read command end
                                pass
                            else:
                                ##[RCN-217 6.7]
                                pos, error = self.incPos(pos, packetByte)
                                if error == True: return
                                if      (subcmd >> 2) & 0b11    == 0b10\
                                    and packetByte[pos][0] >> 4 == 0b1111:  ##Bit write
                                    output_long  = str(packetByte[pos][0] & 0b00000111)
                                    output_short = str(packetByte[pos][0] & 0b00000111)
                                    if packetByte[pos][0] & 0b1000 == 0b1000:
                                        output_long  += ', 1'
                                        output_short += ',1'
                                    else:
                                        output_long  += ', 0'
                                        output_short += ',0'
                                    self.put_packetbyte(packetByte, pos, [Ann.DATA,        [output_long, output_short]])
                                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND,     ['Position, Value', 'Pos, Value', 'P,V']])
                                elif (subcmd >> 2) & 0b11 == 0b11:
                                    self.put_packetbyte(packetByte, pos, [Ann.COMMAND,     ['Data-1']])
                                    self.put_packetbyte(packetByte, pos, [Ann.DATA,        [str(packetByte[pos][0])]])
                                    if len(packetByte) > pos+2: #more data + checksum
                                        pos, error = self.incPos(pos, packetByte)
                                        if error == True: return
                                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data-2']])
                                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                                    if len(packetByte) > pos+2: #more data + checksum
                                        pos, error = self.incPos(pos, packetByte)
                                        if error == True: return
                                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data-3']])
                                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                                    if len(packetByte) > pos+2: #more data + checksum
                                        pos, error = self.incPos(pos, packetByte)
                                        if error == True: return
                                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Data-4']])
                                        self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                        else:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA, ['Reserved for future use', 'Res.']])
                                    
            elif 128 <= idPacket <= 191:
                ##[RCN-211 3] Accessory Decoder
                pos, error = self.incPos(pos, packetByte)
                if error == True: return
                
                #10AAAAAA 1AAADAAR                             #Basic Accessory Decoder Packet Format
                #10111111 1000DAAR                             #Broadcast Command for Basic Accessory Decoders (only NMRA, not RCN)
                #                                              #D:activate/deactivate addressed device AA:Pair of 4 R:Pair of output
                #10111111 10000110                             #ESTOP
                #10AAAAAA 1AAA1AA0 1110CCVV VVVVVVVV DDDDDDDD  #Basic Accessory Decoder Packet address for operations mode programming (POM)
                #10AAAAAA 0AAA0AA1 DDDDDDDD                    #Extended Accessory Decoder Control Packet Format
                #10111111 00000111 DDDDDDDD                    #Broadcast Command for Extended Accessory Decoders 
                #10111111 00000111 00000000                    #ESTOP
                #10AAAAAA 0AAA0AA1 1110CCVV VVVVVVVV DDDDDDDD  #Extended Decoder Control Packet address for operations mode programming (POM)
                #10AAAAAA 0AAA1AAT                             #NOP
                #  ^^^^^^  ^^^ ^^
                #  A1      A2  A3

                A1       = packetByte[pos-1][0]        & 0b00111111        #6 bits addr. high
                A2       = ~((packetByte[pos][0] >> 4) & 0b0111) & 0b0111  #3 bits addr. low (inverted)
                A3       = (packetByte[pos][0]         & 0b00000110) >> 1  #2 bits bits 1-2 of bit two (port address)        
                decoder  = (A2 << 6) + A1        
                port     =  A3        
                decaddr  = (A2 << 8) + (A1 << 2) + A3 - 3 
                acc_addr = decaddr + self.AddrOffset
                
                if decaddr < 1:
                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.ERROR, ['Address < 1 not allowed']])
                
                pom = False
                if packetByte[pos][0] & 0b10001000 == 0b00001000:
                    ##[RCN-213 2.5]
                    ##[RCN-217 4.3.3]
                    self.put_packetbyte(packetByte, pos,   [Ann.DATA, ['Railcom NOP (AccQuery)', 'RC NOP']])
                    self.put_packetbyte(packetByte, pos-1, [Ann.DATA_ACC, [str(acc_addr)]])
                    if packetByte[pos][0] & 1 == 0:
                        self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND, ['Basic Accessory Decoder', 'Basic Accessory', 'Basic Acc.']])
                    else:
                        self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND, ['Extended Accessory Decoder', 'Extended Accessory', 'Ext. Acc.']])
                
                elif packetByte[pos][0] & 0b10000000 == 0b10000000:
                    if     len(packetByte) == 3\
                        or len(packetByte) == 4:
                        ##[RCN-213 2.1]
                        self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND, ['Basic Accessory Decoder', 'Basic Accessory', 'Basic Acc.']])
                        if acc_addr+3 == 2047:
                            ##[RCN-213 2.2]
                            if (packetByte[pos][0] >> 3) & 1 == 0 and packetByte[pos][0] & 1 == 0:
                                self.put_packetbyte(packetByte, pos-1, [Ann.DATA_ACC, ['Broadcast']])
                                self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND,  ['Broadcast']])
                                self.put_packetbyte(packetByte, pos,   [Ann.DATA,     ['ESTOP']])
                            else:
                                self.put_packetbyte(packetByte, pos,   [Ann.ERROR,    ['Unknown (maybe NMRA-Broadcast)', 'Unknown']])
                        else:
                            if len(packetByte) == 3:
                                output_1 = str(packetByte[pos][0] & 1)
                                if (packetByte[pos][0] >> 3) & 1 == 0:
                                    output_2 = 'off'
                                else:
                                    output_2 = 'on'
                                self.put_packetbyte(packetByte, pos-1,       [Ann.DATA_ACC, [str(acc_addr) + ' (decoder:' + str(decoder) + ', port:' + str(port) + ')',\
                                                                                             str(acc_addr) + ' (' + str(decoder) + ',' + str(port) + ')', str(acc_addr)]])
                                self.put_packetbyte(packetByte, pos,         [Ann.DATA,     [str(output_1) + ':' + str(output_2)]])
                            elif    len(packetByte) == 4\
                                and packetByte[pos][0] & 0b1001 == 0b0000:
                                pos, error = self.incPos(pos, packetByte)
                                if error == True: return
                                if packetByte[pos][0] == 0: 
                                    self.put_packetbyte(packetByte, pos-1,       [Ann.DATA_ACC, [str(acc_addr) + ' (decoder:' + str(decoder) + ', port:' + str(port) + ')',\
                                                                                                 str(acc_addr) + ' (' + str(decoder) + ',' + str(port) + ')', str(acc_addr)]])
                                    self.put_packetbyte(packetByte, pos,         [Ann.COMMAND,  ['Decoder reset', 'Reset']])
                                else:
                                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.ERROR, ['Unknown']])
                            else:        
                                self.put_packetbyte(packetByte, pos, [Ann.ERROR, ['Unknown']])
                    
                    elif len(packetByte) == 6:
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if packetByte[pos][0] >> 4 == 0b1110:
                            ##[RCN-217 6.2]
                            pom = True
                            self.put_packetbyte(packetByte, pos-2,           [Ann.COMMAND,  ['POM for Basic Accessory Decoder', 'POM Basic Accessory', 'POM Basic Acc.']])
                            self.put_packetbyte(packetByte, pos-1,           [Ann.DATA_ACC, [str(acc_addr) + ' (decoder:' + str(decoder) + ', port:' + str(port) + ')',\
                                                                                             str(acc_addr) + ' (' + str(decoder) + ',' + str(port) + ')', str(acc_addr)]])
                            self.put_packetbyte(packetByte, pos-1,           [Ann.COMMAND,  ['Address', 'Addr.']])
                        else:
                            self.put_packetbytes(packetByte, pos-2, pos,     [Ann.ERROR, ['Unknown']])
                
                else:
                    ##[RCN-213 2.3]
                    if len(packetByte) == 4:
                        self.put_packetbyte(packetByte, pos-1, [Ann.COMMAND, ['Extended Accessory Decoder Control Packet', 'Extended Accessory', 'Ext. Acc.']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if acc_addr+3 == 2047:
                            ##[RCN-213 2.4]
                            if packetByte[pos][0] == 0:
                                self.put_packetbyte(packetByte, pos-1,       [Ann.DATA_ACC, ['Broadcast']])
                                self.put_packetbyte(packetByte, pos-1,       [Ann.COMMAND,  ['Broadcast']])
                                self.put_packetbyte(packetByte, pos,         [Ann.DATA,     ['ESTOP']])
                            else:                                            
                                self.put_packetbyte(packetByte, pos-1,       [Ann.DATA,  [hex(packetByte[pos-1][0]) + '/' + str(packetByte[pos-1][0])]])
                                self.put_packetbyte(packetByte, pos,         [Ann.DATA,  [hex(packetByte[pos][0]) + '/' + str(packetByte[pos][0])]])
                                self.put_packetbytes(packetByte, pos-1, pos, [Ann.ERROR, ['Unknown']])
                        else:                                                
                            self.put_packetbytes(packetByte, pos-2, pos-1,   [Ann.DATA_ACC, [str(acc_addr) + ' (decoder:' + str(decoder) + ', port:' + str(port) + ')',\
                                                                                             str(acc_addr) + ' (' + str(decoder) + ',' + str(port) + ')', str(acc_addr)]])
                            self.put_packetbyte(packetByte, pos,             [Ann.DATA, ['Aspect:' + hex(packetByte[pos][0]) + '/' + str(packetByte[pos][0])]])
                            if packetByte[pos][0] & 0b01111111 == 0b01111111:
                                output_1 = 'on'
                            elif packetByte[pos][0] & 0b01111111 == 0b00000000:
                                output_1 = 'off'
                            else:
                                output_1 = str(packetByte[pos][0] & 0b01111111)
                            self.put_packetbyte(packetByte, pos,             [Ann.COMMAND, ['Switching time:' + output_1 + ', output:' + str((packetByte[pos][0] >> 7))]])
                    
                    elif len(packetByte) == 6:
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if packetByte[pos][0] >> 4 == 0b1110:
                            ##[RCN-217 6.2]
                            pom = True
                            self.put_packetbyte(packetByte, pos-2,           [Ann.COMMAND,  ['POM for Extended Accessory Decoder', 'POM Extended Accessory', 'POM Extended Acc.']])
                            self.put_packetbyte(packetByte, pos-1,           [Ann.DATA_ACC, [str(acc_addr) + ' (decoder:' + str(decoder) + ', port:' + str(port) + ')',\
                                                                                             str(acc_addr) + ' (' + str(decoder) + ',' + str(port) + ')', str(acc_addr)]])
                            self.put_packetbyte(packetByte, pos-1,           [Ann.COMMAND,  ['Address', 'Addr.']])
                        else:
                            self.put_packetbytes(packetByte, pos-2, pos,     [Ann.ERROR, ['Unknown']])
                
                if pom == True:
                    subcmd = (packetByte[pos][0] & 0b00011111)
                    if (subcmd >> 2) & 0b11 in [0b01, 0b11, 0b10]:
                        if (subcmd >> 2) & 0b11 == 0b01:
                            output_long  = 'Read/Verify byte'
                            output_short = 'r/v'
                        elif (subcmd >> 2) & 0b11 == 0b11:
                            output_long  = 'Write byte'
                            output_short = 'w'
                        else:    
                            output_long  = 'Bit manipulation'
                            output_short = 'Bit'
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        cv_addr = (packetByte[pos-1][0] & 0b00000011)*256 + packetByte[pos][0] + 1
                        self.put_packetbyte(packetByte, pos, [Ann.DATA_CV, [str(cv_addr)]])
                        self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['CV']])
                        pos, error = self.incPos(pos, packetByte)
                        if error == True: return
                        if (subcmd >> 2) & 0b11 != 0b10:
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [str(packetByte[pos][0])]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Value']])
                        else:    
                            if packetByte[pos][0] & 0b10000 == 0b10000:
                                output_long  = 'Write, '
                                output_short = 'w,'
                            else:
                                output_long  = 'Verify, '
                                output_short = 'v,'
                            output_long  += str(packetByte[pos][0] & 0b00000111)
                            output_short += str(packetByte[pos][0] & 0b00000111)
                            if packetByte[pos][0] & 0b1000 == 0b1000:
                                output_long  = output_long  + ', 1'
                                output_short = output_short + ',1'
                            else:
                                output_long  = output_long  + ', 0'
                                output_short = output_short + ',0'
                            self.put_packetbyte(packetByte, pos, [Ann.DATA,    [output_long, output_short]])
                            self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Operation, Position, Value', 'Op.,Pos,Value', 'O,P,V']])
                    else:
                        output_long  = 'Reserved for future use'
                        output_short = 'Res.'
                        self.put_packetbyte(packetByte, pos, [Ann.DATA, [output_long, output_short]])
                
                
            elif 232 <= idPacket <= 254:
                ##[RCN-211 3] Reserved
                self.put_packetbyte(packetByte, pos, [Ann.COMMAND, ['Reserved']])
            
            elif idPacket == 255:
                ##[RCN-211 3] Idle
                pos, error = self.incPos(pos, packetByte)
                if error == True: return
                if packetByte[pos][0] == 0:
                      ##[RCN-211 4.2] Idle
                    self.put_packetbytes(packetByte, pos-1, pos, [Ann.COMMAND, ['Idle']])
                else: ##[RCN-211 4.3] System command
                    validPacketFound = True
                    self.put_packetbytes(packetByte, pos-1, pos-1, [Ann.COMMAND, ['RailComPlus®']])
                    if len(packetByte) >= 5 and packetByte[pos+1][0] == 62 and packetByte[pos+2][0] == 7 and packetByte[pos+3][0] == 64:
                        self.put_packetbytes(packetByte, pos, len(packetByte)-2, [Ann.COMMAND, ['System command (not documented) (IDNotify?)', 'System command']])
                    else:
                        self.put_packetbytes(packetByte, pos, len(packetByte)-2, [Ann.COMMAND, ['System command (not documented)', 'System command']])
                    pos = -1

        ## remaining bytes in packet
        if pos == -1:  #Railcomplus
            pos = 0
        elif pos == 0: #nothing valid found
            pos -= 1
            
        for x in range(pos+1, len(packetByte)-1):
            output_1  = '?:' + hex(packetByte[x][0]) + '/' + str(packetByte[x][0])
            self.put_packetbyte(packetByte, x,         [Ann.DATA, [output_1]])
            if validPacketFound == False:
                self.put_packetbyte(packetByte, x,     [Ann.COMMAND, [output_1]])
                if self.serviceMode == False and 112 <= idPacket <= 127:
                    self.put_packetbyte(packetByte, x, [Ann.ERROR, ['Unknown (maybe service mode packet)', 'Unknown']])
                elif self.serviceMode == True:
                    self.put_packetbyte(packetByte, x, [Ann.ERROR, ['Unknown (maybe operation mode packet)', 'Unknown']])
                else:
                    self.put_packetbyte(packetByte, x, [Ann.ERROR, ['Unknown']])


        ##################
        ##[RCN-211 2] Checksum
        if pos+1 < len(packetByte):
            output_1 = ''
            checksum = packetByte[0][0]
            for x in range(1, len(packetByte)-1):
                checksum = checksum ^ packetByte[x][0]
            if checksum == packetByte[len(packetByte)-1][0]:
                output_1 = 'OK'
                self.put_packetbyte(packetByte, len(packetByte)-1,     [Ann.FRAME, ['Checksum: ' + output_1, output_1]])
            else:
                output_1 = str(checksum) + '<>' + str(packetByte[len(packetByte)-1][0])
                self.put_packetbytes(packetByte, 0, len(packetByte)-1, [Ann.ERROR, ['Checksum']])
                self.put_packetbyte(packetByte, len(packetByte)-1,     [Ann.FRAME_OTHER, ['Checksum: ' + output_1, output_1]])
        else:
            self.put_packetbytes(packetByte, 0, len(packetByte)-1,     [Ann.ERROR, ['Checksum missing']])

        
        ##################
        ## Search function
        ## byte
        byte_found = False
        for x in range(0, len(packetByte)):
            if self.byte_search == packetByte[x][0]:
                byte_found = True
                if (  (self.dec_addr_search < 0 and self.acc_addr_search < 0 and self.cv_addr_search < 0)
                    or dec_addr == self.dec_addr_search
                    or acc_addr == self.acc_addr_search
                    or cv_addr  == self.cv_addr_search
                    ): 
                    self.put_packetbyte(packetByte, x, [Ann.SEARCH_BYTE, ['BYTE:' + hex(self.byte_search) + '/' + str(self.byte_search)]])
        ## dec_addr
        if  (   self.dec_addr_search == dec_addr
            and (   self.byte_search < 0
                 or byte_found       == True)
            ):
            self.put_packetbyte(packetByte, 0, [Ann.SEARCH_DEC, ['DECODER:' + str(self.dec_addr_search)]])
        ## acc_addr
        if  (   self.acc_addr_search == acc_addr
            and (   self.byte_search < 0
                 or byte_found       == True)
            ):
            self.put_packetbytes(packetByte, 0, len(packetByte)-2, [Ann.SEARCH_ACC, ['ACCESSORY:' + str(self.acc_addr_search)]])
        ## cv_addr
        if  (    self.cv_addr_search == cv_addr
            and (   self.byte_search < 0
                 or byte_found       == True)
            ):
            self.put_packetbyte(packetByte, 1, [Ann.SEARCH_CV, ['CV:' + str(self.cv_addr_search)]])

        
    def setNextStatus(self, newstatus):
        self.dccStatus     = newstatus
        self.dccBitCounter = 0
        self.decodedBytes  = []

    def collectDataBytes(self, start, stop, data):
        ##[RCN-211 2]

        #Test for invalid bits
        if data not in ['0', '1']:               #invalid timing
            self.setNextStatus('WAITINGFORPREAMBLE')

        #Wait for the first 1
        elif self.dccStatus == 'WAITINGFORPREAMBLE':
            if data == '1':                      #preamble start
                self.dccStart      = start
                self.setNextStatus('PREAMBLE')

        #Collect the preamble bits
        elif self.dccStatus == 'PREAMBLE':
            if data == '1':                      #preamble bit
                self.dccBitCounter += 1
                self.dccLast       = stop
            else:                                #preamble end
                if self.dccBitCounter+1+1 >= 10: #valid preamble (minimum 10 bit wherby last stop bit can usually be counted among them)
                    output_long  = 'Preamble: ' + str(self.dccBitCounter+1) + ' bits'
                    output_short = 'Preamble'
                    output_3     = 'P'
                    self.putx(start, stop,                 [Ann.FRAME, ['Start Packet', 'Start', 'S']]) #Packet Start Bit
                    if self.syncSignal == True:
                        self.syncSignal = False
                        output_long  += ' (sync in progress)'
                        output_short += ' (sync)'
                        output_3     += ' (s)'
                    self.putx(self.dccStart, self.dccLast, [Ann.FRAME, [output_long, output_short, output_3]])
                    self.setNextStatus('ADDRESSDATABYTE')
                else:                            #invalid preamble
                    self.setNextStatus('WAITINGFORPREAMBLE')
                    if self.syncSignal == False:
                        self.putx(self.dccStart, self.dccLast, [Ann.ERROR, ['Invalid preamble']])
                    self.syncSignal = True       #resynchronize
                    self.put_signal(                       [Ann.FRAME_OTHER, ['Resynchronize (Wait for preamble)', 'Resynchronize','Resync.','R']])

        #Collection 8 databits and one bit indicating the end of data
        elif self.dccStatus == 'ADDRESSDATABYTE':
            if self.dccBitCounter == 0:          #first bit of new byte
                self.dccValue  = 0
                self.dccStart  = start
                self.dccBitPos = []
            if self.dccBitCounter < 8:           #build byte 
                self.dccBitPos.append(start)
                self.dccBitCounter += 1
                self.dccValue      = ((self.dccValue) << 1) + int(data);
                if self.dccBitCounter == 8:      #byte complete
                    self.dccBitPos.append(stop)
                    self.decodedBytes.append([self.dccValue, self.dccBitPos])
            else:
                if data == '0':                  #separator to next byte
                    self.dccBitCounter = 0
                    self.dccValue      = 0
                    self.putx(start, stop,                 [Ann.FRAME, ['Start Databyte', 'Start', 'S']])
                else:                            #end identifier
                    self.putx(start, stop,                 [Ann.FRAME, ['Stop Packet', 'Stop', 'S']])
                    self.handleDecodedBytes(self.decodedBytes)
                    self.setNextStatus('WAITINGFORPREAMBLE')

    def decode(self):
        if self.samplerate is None:
            raise SamplerateError('Cannot decode without samplerate.')
        elif (self.samplerate < 25000):
            raise SamplerateError('Minimum samplerate >= 25kHz.')
        accuracy = 1/self.samplerate*1000000  #µs (accuracy is depending on sample rate, it is about recognizing a packet, not checking the correct timing)

        self.wait({0: self.cond1})
        self.edge_1 = self.samplenum
        self.wait({0: self.cond2})
        self.edge_2 = self.samplenum

        #Info at the start
        output_1      = 'Samplerate: '
        if self.samplerate/1000 < 1000:
            output_1 += '{:.0f}'.format(self.samplerate/1000) + ' kHz'
        else:
            output_1 += '{:.0f}'.format(self.samplerate/1000000) + ' MHz'
        output_1     += ', Accuracy: '    
        if accuracy >= 1:
            output_1 += '{:.0f}'.format(accuracy) + ' µs'
        else:
            output_1 += '{:.0f}'.format(accuracy*1000) + ' ns'
        self.putx(self.edge_1, self.edge_2, [Ann.FRAME_OTHER, [output_1]])
        
        firstChangeCond = True
        while True:
            output_1       = ''
            unknownTiming  = False
            railcomCutout  = False
            strechedZero   = False
            
            self.wait({0: self.cond1})
            self.edge_3 = self.samplenum
            self.wait({0: self.cond2})
            self.edge_4 = self.samplenum  #Look into the future to filter out short pulses (see below)
            
            '''
                             ______        ____________              ______
            signal        __|      |______|            |____________|      |__
                            ^      ^      ^            ^            ^      ^
            edge            1      2      3            4
            edge next run                 1            2            3      4
                            |part 1|part 2|   part 1   |   part 2   |part 1|
                            |    total    |          total          |
            '''
            total = (self.edge_3-self.edge_1)/self.samplerate*1000000 #µs
            part1 = (self.edge_2-self.edge_1)/self.samplerate*1000000 #µs
            part2 = (self.edge_3-self.edge_2)/self.samplerate*1000000 #µs
            
            ##[RCN-210 5]
            if (     52-accuracy <= part1 <= 64+accuracy              #'1' part1 = 52us - 64us
                 and 52-accuracy <= part2 <= 64+accuracy              #'1' part2 = 52us - 64us
                 and abs(part1-part2) <= max(6, 2*accuracy)           #difference part1/part2 = +/- 6us or 2*accuracy
                ): 
                value = '1'
            
            elif (   (    90-accuracy <= part1 <= 10000+accuracy      #'0' part1 = 90us - 10000us
                      and 90-accuracy <= part2 <= 119  +accuracy)     #'0' part2 = 90us - 116us
                  or (    90-accuracy <= part2 <= 10000+accuracy      #'0' part2 = 90us - 10000us
                      and 90-accuracy <= part1 <= 119  +accuracy)     #'0' part1 = 90us - 116us
                 ):
                value = '0'
                if (2*119)+accuracy <= total <= 12000+accuracy:       #min. 2*half'0'
                    output_1 = 'stretched zero?'
                    strechedZero = True
            
            elif 90+52-accuracy <= total <= 64+119+accuracy:          #half '0' + half '1' -> adjust edge detection
                if self.cond1 == 'r':
                    self.cond1 = 'f'  #falling-edge
                    self.cond2 = 'r'  #raising-edge
                else:
                    self.cond1 = 'r'  #falling-edge
                    self.cond2 = 'f'  #raising-edge
                if firstChangeCond == True:                           #first sync is no error
                    firstChangeCond = False
                else:    
                    self.put_signal([Ann.ERROR,       ['Edge-Detection changed to falling edge - should not occur - dirty signal?']])
                    self.put_signal([Ann.FRAME_OTHER, ['Resynchronize (Wait for preamble)', 'Resynchronize','Resync.','R']])
                self.syncSignal   = True                              #resynchronize
                self.decodedBytes = []
                self.setNextStatus('WAITINGFORPREAMBLE')              #wait for new preamble
                self.wait({0: 'e'})                                   #skip one edge
                self.edge_1 = self.edge_4
                self.edge_2 = self.samplenum
                continue
            
            else:
                output_1      = 'unknown timing'
                unknownTiming = True

            #filter out short pulses
            if self.ignoreInterferingPulse == 'yes':
                output_2 = 'Short pulse ignored'
                if      (self.edge_4 - self.edge_3)/self.samplerate*1000000 <= self.maxInterferingPulseWidth\
                    and (self.edge_3 - self.edge_2)/self.samplerate*1000000 <= self.maxInterferingPulseWidth:
                    self.edge_2 = int((self.edge_2 + self.edge_4) / 2) #not quite accurate but sufficient enough
                    self.putx(self.edge_2, self.edge_4, [Ann.ERROR, [output_2]])
                    continue
                elif (self.edge_4 - self.edge_3)/self.samplerate*1000000 <= self.maxInterferingPulseWidth\
                    and value not in ['0', '1']:
                    self.putx(self.edge_3, self.edge_4, [Ann.ERROR, [output_2]])
                    continue
                elif (self.edge_3 - self.edge_2)/self.samplerate*1000000 <= self.maxInterferingPulseWidth: 
                    self.putx(self.edge_2, self.edge_3, [Ann.ERROR, [output_2]])
                    self.edge_2 = self.edge_4
                    continue

            if unknownTiming == True or strechedZero == True:
                if strechedZero == True:
                    value_2   = '0 - ({:.0f}'.format(total) + 'µs=' + '{:.0f}'.format(part1) + 'µs+' + '{:.0f}'.format(part2) + 'µs)'
                else:
                    value     = '{:.0f}'.format(total) + 'µs=' + '{:.0f}'.format(part1) + 'µs+' + '{:.0f}'.format(part2) + 'µs'
                value_long    = '{:.0f}'.format(total) + 'µs=' + '{:.0f}'.format(part1) + 'µs+' + '{:.0f}'.format(part2) + 'µs'
                value_short   = '{:.0f}'.format(total) + 'µs'

            ##[RCN-217 2.4]
            if 454-accuracy <= total <= 488+119+6+accuracy:           #454us - 488us (+119+6=next 1-bit)
                if output_1 == '':
                    output_1 = 'Railcom cutout?'
                else:
                    output_1 = 'Railcom cutout or ' + output_1
                railcomCutout = True
            
            if unknownTiming == True and railcomCutout == False:      #resynchronize
                self.syncSignal   = True
                self.decodedBytes = []
                self.setNextStatus('WAITINGFORPREAMBLE')              #wait for new preamble
                self.put_signal([Ann.FRAME_OTHER, ['Resynchronize (Wait for preamble)', 'Resynchronize','Resync.','R']])
                self.put_signal([Ann.ERROR,       [output_1 + ' - should not occur - dirty signal?']])
            elif output_1 != '':
                self.put_signal([Ann.FRAME_OTHER, [output_1]])
                    
            if self.syncSignal == True:
                if value in ['0', '1']:
                    if strechedZero == True:
                        self.put_signal([Ann.BITS_OTHER, [value_2 + ' (sync in progress)', value_2 + ' (sync)', value_2]])
                    else:
                        self.put_signal([Ann.BITS,       [value + ' (sync in progress)', value + ' (sync)', value]])
                else:
                    self.put_signal(    [Ann.BITS_OTHER, [value + ' (sync in progress)', value_long + ' (sync)', value_short]])
            else:
                if value in ['0', '1']:
                    if strechedZero == True:
                        self.put_signal([Ann.BITS_OTHER, [value_2, '0 - (' + value_long + ')', '0']])
                    else:
                        self.put_signal([Ann.BITS,       [value]])
                else:
                    self.put_signal(    [Ann.BITS_OTHER, [value, value_long, value_short]])
            
            self.collectDataBytes(self.edge_1, self.edge_3, value)
            self.edge_1 = self.edge_3
            self.edge_2 = self.edge_4
