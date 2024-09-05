##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017 Hattori, Hiroki <seagull.kamome@gmail.com>
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

RX = 0
TX = 1


hcicmds = {
        0x0000:'NOP',
        0x0401:'Inquiry',
        0x0402:'Inquiry_Cancel',
        0x0403:'Periodic_Inquiry_Mode',
        0x0404:'Exit_Periodic_Inquiry_Mode',
        0x0405:'Create_Connection',
        0x0406:'Disconnect',
        0x0407:'Add_SCO_Connection',
        0x0408:'Create_Connection_Cancel',
        0x0409:'Accept_Connection_Request',
        0x040A:'Reject_Connection_Request',
        0x040B:'Link_Key_Request_Reply',
        0x040C:'Link_Key_Request_Negative_Reply',
        0x040D:'PIN_Code_Request_Reply',
        0x040E:'PIN_Code_Request_Negative_Reply',
        0x040F:'Change_Connection_Packet_Type',
        0x0411:'Authentication_Request',
        0x0413:'Set_Connection_Encrypt',
        0x0415:'Change_Connection_Link_Key',
        0x0417:'Master_Link_Key',
        0x0419:'Remote_Name_Request',
        0x041A:'Remote_Name_Request_Cancel',
        0x041B:'Read_Remote_Supported_Features',
        0x041C:'Read_Remote_Extended_Features',
        0x041D:'Read_Remote_Version_Information',
        0x041F:'Read_Clock_Offset',
        0x0420:'Read_LMP_Handle',
        0x0428:'Setup_Synchronous_Connection',
        0x0429:'Accept_Synchronous_Connection_Request',
        0x042A:'Reject_Synchronous_Connection_Request',

        0x0C01:'Set_Event_Mask',
        0x0C03:'Reset',
        0x0C05:'Set_Event_Filter',
        0x0C08:'Flush',
        0x0C09:'Read_PIN_Type',
        0x0C0A:'Write_PIN_Type',
        0x0C0B:'Create_New_Unit_Key',
        0x0C0D:'Read_Stored_Link_Key',
        0x0C11:'Write_Stored_Link_Key',
        0x0C12:'Delete_Stored_Link_Key',
        0x0C13:'Write_Local_Name',
        0x0C14:'Read_Local_Name',
        0x0C15:'Read_Connection_Accept_Timeout',
        0x0C16:'Write_Connection_Accept_Timeout',
        0x0C17:'Read_Page_Timeout',
        0x0C18:'Write_Page_Timeout',
        0x0C19:'Read_Scan_Enable',
        0x0C1A:'Write_Scan_Enable',
        0x0C1B:'Read_Page_Scan_Activity',
        0x0C1C:'Write_Page_Scan_Activity',
        0x0C1D:'Read_Inquiry_Scan_Activity',
        0x0C1E:'Write_Inquiry_Scan_Activity',
        0x0C1F:'Read_Authentication_Enable',
        0x0C20:'Write_Authentication_Enable',
        0x0C21:'Read_Encryption_Mode',
        0x0C22:'Write_Encryption_Mode',
        0x0C23:'Read_Class_of_Device',
        0x0C24:'Read_Class_of_Device',
        0x0C25:'Read_Voice_Setting',
        0x0C26:'Write_Voice_Setting',
        0x0C27:'Read_Automatic_Flush_Timeout',
        0x0C28:'Write_Automatic_Flush_Timeout',
        0x0C29:'Read_Num_Broadcast_Retransmissions',
        0x0C2A:'Write_Num_Broadcast_Retransmissions',
        0x0C2B:'Read_Hold_Mode_Activity',
        0x0C2C:'Write_Hold_Mode_Activity',
        0x0C2D:'Read_Transmit_Power_Level',
        0x0C2E:'Read_SCO_Flow_Control_Enable',
        0x0C2F:'Write_SCO_Flow_Control_Enable',
        0x0C31:'Set_Host_Controller_To_Host_Flow_Control',
        0x0C33:'Host_Buffer_Size',
        0x0C35:'Host_Number_Of_Completed_Packets',
        0x0C36:'Read_Link_Supervision_Timeout',
        0x0C37:'Write_Link_Supervision_Timeout',
        0x0C38:'Read_Number_Of_Supported_IAC',
        0x0C39:'Read_current_IAC_LAP',
        0x0C3A:'Write_Current_IAC_LAP',
        0x0C3B:'Read_Page_Scan_Period_Mode',
        0x0C3C:'Write_Page_Scan_Period_Mode',
        0x0C3D:'Read_Page_Scan_Mode',
        0x0C3E:'Write_Page_Scan_Mode',
        0x0C3F:'Set_AFH_Host_Channel_Classification',
        0x0C42:'Read_Inqury_Scan_Type',
        0x0C43:'Write_Inqury_Scan_Type',
        0x0C44:'Read_Inqury_Mode',
        0x0C45:'Write_Inqury_Mode',
        0x0C46:'Read_Page_Scan_Type',
        0x0C47:'Write_Page_Scan_Type',
        0x0C48:'Read_AFH_Channel_Assessment_Mode',
        0x0C49:'Write_AFH_Channel_Assessment_Mode',

        0x1001:'Read_Local_Version_Information',
        0x1002:'Read_Local_Supported_Commands',
        0x1003:'Read_Supported_Features',
        0x1004:'Read_Local_Extended_Features',
        0x1005:'Read_Buffer_Size',
        0x1007:'Read_Country_Code',
        0x1009:'Read_BD_ADDR'

        }

class Decoder(srd.Decoder):
    api_version = 3
    id = 'bluetooth_h4'
    name = 'bluetooth_h4'
    longname = 'Blueetooth H4 packet decoder'
    desc = 'Bluetooth H4 packet decoder.'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = ['bluetooth_h4']
    tags = ['Embedded/bluetooth']
    annotations = (
        ('rx-cmd', 'RX Command packet.'),
        ('rx-acl', 'RX ACL data packet.'),
        ('rx-sco', 'RX SCO data packet.'),
        ('rx-event', 'RX Event data packet.'),
        ('rx-error', 'RX Error message packet.'),
        ('rx-nego', 'RX Negotiation packet.'),
        ('rx-junk', 'RX Garbages.'),
        ('rx-desc', 'RX packet description.'),
        ('rx-bin', 'RX packet binary.'),

        ('tx-cmd', 'TX Command packet.'),
        ('tx-acl', 'TX ACL data packet.'),
        ('tx-sco', 'TX SCO data packet.'),
        ('tx-event', 'TX Event data packet.'),
        ('tx-error', 'TX Error message packet.'),
        ('tx-nego', 'TX Negotiation packet.'),
        ('tx-junk', 'TX Garbages.'),
        ('tx-desc', 'TX packet description.'),
        ('tx-bin', 'TX packet binary.'),
    )
    annotation_rows = (
        ('rx', 'RX', (0, 1, 2, 3, 4, 5, 6)),
        #('rx-desc', 'RX description', (7,)),
        ('rx-bins', 'RX binary', (8,)),

        ('tx', 'TX', (9, 10, 11, 12, 13, 14, 15)),
        #('tx-desc', 'TX description', (16,)),
        ('tx-bins', 'TX binary', (17,))
    )


    def __init__(self):
        self.cmd = ['', '']
        self.bintext = ['', '']
        self.datavalues = [[], []]
        self.ss_block = [None, None]
        self.packet_length = [None, None]
        self.dir = ['RX', 'TX']
    
    def reset(self):
        self.__init__(self)

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)

    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data

        if ptype != 'DATA':
            return

        pdata = pdata[0]
        self.cmd[rxtx] += "{:02X} ".format(pdata)
        self.bintext[rxtx] += "{:02X} ".format(pdata)
        self.datavalues[rxtx].append(pdata);

        if self.ss_block[rxtx] is None:
            if pdata < 0x01 or pdata > 0x04:
                self.put(ss, es, self.out_ann, [6 + rxtx * 9, [self.cmd[rxtx]]])
                self.put(ss, es, self.out_python, [6 + rxtx * 9, self.datavalues[rxtx]])
                self.cmd[rxtx] = ''
                self.bintext[rxtx] = ''
                self.datavalues[rxtx] = []
            else:
                self.ss_block[rxtx] = ss
        elif self.packet_length[rxtx] is None:
            if self.datavalues[rxtx][0] == 0x01: # CMD
                if len(self.datavalues[rxtx]) >= 4:
                    self.packet_length[rxtx] = self.datavalues[rxtx][3]

                    opcd = self.datavalues[rxtx][2] * 256 + self.datavalues[rxtx][1]

                    self.cmd[rxtx] = "{:}: CMD={:02X}{:02X} [{:}] LEN={:d}({:02X}h) D=".format(
                            self.dir[rxtx],
                            self.datavalues[rxtx][2],
                            self.datavalues[rxtx][1],
                            (hcicmds[opcd] if opcd in hcicmds.keys() else '*UNKNOWN*'),
                            self.packet_length[rxtx],
                            self.datavalues[rxtx][3])
            elif self.datavalues[rxtx][0] == 0x02: # ACL
                if len(self.datavalues[rxtx]) >= 5:
                    self.packet_length[rxtx] = self.datavalues[rxtx][3] + self.datavalues[rxtx][4] * 256
                    self.cmd[rxtx] = "{:}: ACL H={:02X}{:02X} LEN={:d}({:02X}{:02X}h) D=".format(
                            self.dir[rxtx],
                            self.datavalues[rxtx][2],
                            self.datavalues[rxtx][1],
                            self.packet_length[rxtx],
                            self.datavalues[rxtx][4],
                            self.datavalues[rxtx][3])
            elif self.datavalues[rxtx][0] == 0x03: # SCO
                if len(self.datavalues[rxtx]) >= 4:
                    self.packet_length[rxtx] = self.datavalues[rxtx][3]
                    self.cmd[rxtx] = "{:}: SCO H={:02X}{:02X} LEN={:d}({:02X}h) D=".format(
                            self.dir[rxtx],
                            self.datavalues[rxtx][2],
                            self.datavalues[rxtx][1],
                            self.packet_length[rxtx],
                            self.datavalues[rxtx][3])
            elif self.datavalues[rxtx][0] == 0x04: # EVENT
                if len(self.datavalues[rxtx]) >= 3:
                    self.packet_length[rxtx] = self.datavalues[rxtx][2]
                    self.cmd[rxtx] = "{:}: EVENT={:02X} LEN={:d}({:02X}h) D=".format(
                            self.dir[rxtx],
                            self.datavalues[rxtx][1],
                            self.packet_length[rxtx],
                            self.datavalues[rxtx][2])
        else:
            self.packet_length[rxtx] -= 1

        if self.packet_length[rxtx] == 0:
            #self.put(self.ss_block[rxtx], es, self.out_ann, [8 + rxtx * 9, [self.dir[rxtx] + '-bin: ' + self.bintext[rxtx]]])
            self.put(self.ss_block[rxtx], es, self.out_ann, [self.datavalues[rxtx][0] - 1 + rxtx * 9, [self.cmd[rxtx]]])
            self.put(self.ss_block[rxtx], es, self.out_python, [self.datavalues[rxtx][0] - 1 + rxtx * 9, self.datavalues[rxtx]])
            self.cmd[rxtx] = ''
            self.bintext[rxtx] = ''
            self.datavalues[rxtx] = []
            self.ss_block[rxtx] = None
            self.packet_length[rxtx] = None

