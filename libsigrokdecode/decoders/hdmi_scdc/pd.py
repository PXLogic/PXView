##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 mesterhazi
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

class States:
    IDLE, GET_SLAVE_ADDR, READ_REGISTER, WRITE_REGISTER, GET_OFFSET, OFFSET_RECEIVED, READ_REGISTER_WOFFSET, WRITE_REGISTER_WOFFSET = range(8)

class Annotations:
    address, register, fields, debug = range(4)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'hdmi_scdc'
    name = 'HMDI_SCDC'
    longname = 'Status and Control Data Channel'
    desc = """Status and Control Data Channel:
    SCDC: Status and Control Data Channel for HDMI2.0 transmitted over Display Data Channel"""
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = ['scdc']
    tags = ['Embedded/industrial']
    options = (
        {'id': 'verbosity', 'desc': 'Verbosity',
        'default': 'short', 'values': ('short', 'long', 'debug')},
    )
    annotations = ( ('Address', 'I²C address'),
                    ('Register', 'Register name and offset'),
                    ('Fields', 'Readable register interpretation'),
                    ('Debug', 'Debug messages'))
    annotation_rows = ( ('scdc', 'SCDC', (0,1,2)),
                        ('debug', 'Debug', (3,)))

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = States.IDLE # I2C channel state
        self.reg = None     # actual register address
        self.offset = None  # offset is used in SCDC and HDCP register reads 
        self.protocol = None # 'scdc'
        self.databytes = [] # databytes
        self.err_det_lower = None # for Character_Error_Detection registers this is the lower 7bits of the error value
        self.block_s = None  # start and end sample of a block
        self.block_e = None

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def handle_SCDC(self):
        messages = []
        reg_val = self.databytes[-1] # one byte registers expected
        try:
            for i in range(len(SCDC_REG_LOOKUP[self.offset]['fields'])):
                mask = SCDC_REG_LOOKUP[self.offset]['fields'][i]['mask']
                try:
                    field_interpretation = SCDC_REG_LOOKUP[self.offset]['fields'][i]['interpretation'][(reg_val & mask)]
                    if self.options['verbosity'] in ['long', 'debug']:
                        messages.append(''.join(field_interpretation))
                    elif self.options['verbosity'] == 'short':
                        messages.append(field_interpretation[0])
                except TypeError:
                    messages.append(SCDC_REG_LOOKUP[self.offset]['fields'][i]['interpretation'] + str(reg_val))
                except KeyError:
                    messages.append('Unexpected value in register with mask:{}'.format(mask))
                
            self.put(self.ss, self.es, self.out_ann, [Annotations.fields, [' | '.join(messages)]])
        except:
            pass
        if self.offset in [0x50, 0x51, 0x52, 0x53, 0x54, 0x55]:  # Err det registers shall be read without setting new offset explicitly
            if self.offset in [0x50, 0x52, 0x54]:  # First byte of the 2 byte registers
                self.err_det_lower = reg_val
                self.block_s = self.ss

            elif self.offset in [0x51, 0x53, 0x55]:  # Second byte of the 2 byte registers
                error_counter = self.err_det_lower + ((0x7F & reg_val) << 8)
                channel = int ((self.offset - 0x51) / 2) # 0x51 is CH0, 0x53 is CH1, 0x55 is CH2
                messages.append('Channel {} Error Counter = {}'.format(channel, error_counter))
                messages.append('Ch{}_Valid = {}'.format(channel, reg_val >> 7))
                self.put(self.block_s, self.es, self.out_ann, [Annotations.fields, [' | '.join(messages)]])

            elif self.offset == 0x56:
                self.put(self.ss, self.es, self.out_ann, [Annotations.fields, ['Checksum of Character Error Detection registers']])
            
            self.offset += 1 # jump to the next Err det register
            
    def handle_EDID(self, data):
        pass

    def handle_HDCP(self, addr, read_notwrite, data):
        pass

    def handle_message(self):
        if self.protocol == 'scdc':
            self.handle_SCDC()
        else:
            pass
    

    def decode(self, ss, es, data):
        cmd, databyte = data
        # store start and end samples
        self.ss = ss
        self.es = es
        if self.options['verbosity'] == 'debug':
            self.put(self.ss, self.es, self.out_ann, [Annotations.debug, [str(self.state) + ' ' + cmd]])
            
        # State machine.
        if cmd in ['STOP']:
            self.reset()
            self.state = States.IDLE

        if self.state == States.IDLE:
            # Wait for an I²C START condition.
            if cmd != 'START':
                return
            
            self.state = States.GET_SLAVE_ADDR 
        elif cmd in ('ACK', 'NACK'):
            return           
        elif self.state == States.GET_SLAVE_ADDR:
            # Wait for an address read/write operation.
            if cmd in ('ADDRESS READ', 'ADDRESS WRITE'):
                # If address write is 0xA8 then SCDC and next byte is the offset
                if cmd == 'ADDRESS WRITE' and databyte == 0xA8:
                    self.put(self.ss, self.es, self.out_ann, [Annotations.address, ['SCDC write - Address : 0xA8']])
                    self.protocol = 'scdc'
                    self.state = States.GET_OFFSET
                # if address read is 0xA9
                elif cmd == 'ADDRESS READ' and databyte == 0xA9:                    
                    self.put(self.ss, self.es, self.out_ann, [Annotations.address, ['SCDC read - Address : 0xA9']])
                    self.protocol = 'scdc'
                    self.state = States.READ_REGISTER
                
        elif self.state == States.GET_OFFSET:
            if cmd == 'DATA WRITE':
                if self.protocol == 'scdc':
                    # get offset after this either:
                    self.offset = databyte
                    try:
                        self.put(self.ss, self.es, self.out_ann, [Annotations.register, ['Register: {} (0x{:2x})'.format(SCDC_REG_LOOKUP[self.offset]['name'], databyte)]])
                    except KeyError:
                        self.put(self.ss, self.es, self.out_ann, [Annotations.register, ['Unknown Register (0x{:2x})'.format(databyte)]])
                    self.state = States.OFFSET_RECEIVED

        elif self.state == States.OFFSET_RECEIVED:
            # - START REPEAT comes - register read
            if cmd == 'START REPEAT':
                self.state = States.GET_SLAVE_ADDR
            # - another data byte - register write
            elif cmd == 'DATA WRITE': 
                self.databytes.append(databyte)
                self.state = States.WRITE_REGISTER
                
                self.handle_message()                

        elif self.state in (States.READ_REGISTER, States.WRITE_REGISTER):
            if cmd in ('DATA READ', 'DATA WRITE'):
                self.read_or_write = cmd[5:]
                self.databytes.append(databyte)
                self.handle_message()
                

            elif cmd in ['STOP', 'START REPEAT']:
                # TODO: Any output?
                self.reset()
                self.state = States.IDLE
        




# SCDC register lookup
SCDC_REG_LOOKUP = {
    # Use the offset as key
    0x01 : {
        'name' : 'Sink version - R',  # Use 'name' as register name + the register type (R-ReadOnly, RW-Writable)
        'fields' : [  # Use 'fields' as bitfields in the register
            {'mask' : 0xFF, # Use 'mask' as the to select the bitfield
             'interpretation' :     # Use 'interpretation' to interpret the different bitfield values to human readable format
                                    # the interpretation for every value consists of a short and a long explanation as the 1st and 2nd element of a list
                                    {0x01 : ['Always 0x01 for HDMI2.0 compliant sinks', '']} 
                                    # If the bitfield is not discrete value instead of the dict a Prefix string can be added
            }   ]
    },
    0x02 : {
        'name' : 'Source version - RW',
        'fields' : [
            { 'mask' : 0xFF, 'interpretation' : {   0x01 : ['Source version = 1', ' - The Source is supporting HDMI2.0 SCDC registers'], 
                                                    0x00 : ['Source version = 0', ' - The Source is supporting HDMI2.0 SCDC registers']}}]
    },
    0x10 : {
        'name' : 'Update_0 - R',
        'fields' : [
            {'mask' : 0x01, 'interpretation' : {0x01 : ['Status_Update=1',  ' - Indicating a change in the Status Registers'],
                                                0x00 : ['Status_Update=0',  ' - No change in the Staus Register']}},
            {'mask' : 0x02, 'interpretation' : {0x02 : ['CED_Update=1', ' - Indicating a change in the Character Error Detection Registers'],
                                                0x00 : ['CED_Update=0',  ' - No change in the Character Error Detection Register']}},
            {'mask' : 0x04, 'interpretation' : {0x04 : ['RR_Test=1', ' - Generate test Read Request'],
                                                0x00 : ['RR_Test=0', ' - No no test Read Request']}},            
        ]
    },
    0x11 : {
        'name' : 'Update_1 - RW',
        'fields' : [
            {'mask' : 0xFF, 'interpretation' : ['Reserved to 0x00 in HDMI2.0b: ', '']},
        ]
    },
    0x20 : {
        'name' : 'TMDS_Config - RW',
        'fields' : [
            { 'mask' : 0x01, 'interpretation' : {0x01 : ['Scrambling Enable = ENABLED', ''], 0x00 : ['Scrambling Enable = DISABLED', '']}},
            { 'mask' : 0x02, 'interpretation' : {0x02 : ['TMDS_Bit_Clock_Ratio = 1/40', ''], 0x00 : ['TMDS_Bit_Clock_Ratio = 1/10', '']}}
        ]
    },
    0x21 : {
        'name' : 'Scrambler status - R',
        'fields' : [
            {'mask' : 0x01, 'interpretation' : {0x01 : ['Scrambling_Status = 1', ' - Scrambled control code detected by the Sink'],
                                                0x00 : ['Scrambling_Status = 0', ' - No scrambled control code detected by the Sink']}},
        ]
    },
    0x30 : {
        'name' : 'Config_0 - RW',
        'fields' : [
            {'mask' : 0x01, 'interpretation' : {0x01 : ['RR_Enable = 1', ' - Source is supporting Read Request'],
                                                0x00 : ['RR_Enable = 0', ' - Source only supports polling the Update Flags ']}
            }
        ]
    },
    0x40 : {
        'name' : 'Status_Flags_0 - R',
        'fields' : [
            {'mask' : 0x01, 'interpretation' : {0x01 : ['Clock_Detected = 1', ' - Sink detected a valid clock signal.'],
                                                0x00 : ['Clock_Detected = 0', ' - No valid clock signal detected by the Sink.']}},
            {'mask' : 0x02, 'interpretation' : {0x02 : ['CH0_Locked = 1', ' - Sink is successfully decoding data on HDMI Channel 0.'],
                                                0x00 : ['CH0_Locked = 0', ' - Sink is not able to decode data on HDMI Channel 0.']}},
            {'mask' : 0x04, 'interpretation' : {0x04 : ['CH1_Locked = 1', ' - Sink is successfully decoding data on HDMI Channel 1.'],
                                                0x00 : ['CH1_Locked = 0', ' - Sink is not able to decode data on HDMI Channel 1.']}},
            {'mask' : 0x08, 'interpretation' : {0x08 : ['CH2_Locked = 1', ' - Sink is successfully decoding data on HDMI Channel 2.'],
                                                0x00 : ['CH2_Locked = 0', ' - Sink is not able to decode data on HDMI Channel 2.']}},
        ]
    },
    0x41 : {
        'name' : 'Status_Flags_1 - R',
        'fields' : [
            {'mask' : 0xFF, 'interpretation' : ['Reserved to 0x00 in HDMI2.0b: ', '']},
        ]
    },
    0x50 : {
        'name' : 'Err_Det_0_L - R',
    },
    0x51 : {
        'name' : 'Err_Det_0_H - R',
    },
    0x52 : {
        'name' : 'Err_Det_1_L - R',
    },
    0x53 : {
        'name' : 'Err_Det_1_H - R',
    },
    0x54 : {
        'name' : 'Err_Det_2_L - R'
    },
    0x55 : {
        'name' : 'Err_Det_2_H - R',
    },
    0x56 : {
        'name' : 'Err_Det_Checksum - R',
    },
    # 0xC0 : {
    #     'name' : 'Test_Config',
    #     'fields' : [
    #         {'mask' : , 'interpretation' : {}},
    #     ]
    # },
}
