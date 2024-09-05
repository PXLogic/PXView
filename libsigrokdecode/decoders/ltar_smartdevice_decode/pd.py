##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2017 Ryan "Izzy" Bales <izzy84075@gmail.com>
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


# LTAR SmartDevice decoder

import sigrokdecode as srd

class SamplerateError(Exception):
    pass

btype = {
	0x01: 'PRIORITY-UPDATE',
	0x02: 'TAGGER-STATUS',
	
	0x18: 'COUNT-DOWN',
	
	0x20: 'VARIABLE-CONTENTS',
	
	0x22: 'GAME-CONTENTS',
	
	0xA0: 'READ-VARIABLE',
	
	0xC0: 'WRITE-VARIABLE',
	
	0xC2: 'WRITE-GAME',
}

weapmode = {
	0x01: 'Semi-Automatic',
	0x02: 'Full-Automatic',
}

shieldstatus = {
	0x00: 'Ready',
	0x01: 'Active',
	0x02: 'Cooldown',
}

huntingdirection = {
	0x00: 'Normal',
	0x01: 'Reversed',
}

class Decoder(srd.Decoder):
	api_version = 3
	id = 'ltar_smartdevice_decode'
	name = 'LTAR SmartDevice Decode'
	longname = 'LTAR SmartDevice Decode'
	desc = 'A decoder for the LTAR SmartDevice protocol'
	license = 'unknown'
	inputs = ['ltar_smartdevice']
	outputs = ['ltar_smartdevice_decode']
	tags = ['Embedded/industrial']
	annotations = (
		('frame-name', 'Frame Name'),
		('frame-error', 'Frame Error'),
		('frame-bit-name', 'Frame Bit Name'),
		('frame-bits-data', 'Frame Bits Data'),
		('block-error', 'Block Errors'),
		('block-data', 'Block Data'),
	)
	annotation_rows = (
		('frame-names', 'Frame name', (0,)),
		('frame-errors', 'Frame errors', (1,)),
		('frame-bit-names', 'Frame bit names', (2,)),
		('frame-bits-datas', 'Frame bits data', (3,)),
		('block-errors', 'Block errors', (4,)),
	)
	
	def putBlockLengthError(self, blockstartsample, blockendsample):
		self.put(blockstartsample, blockendsample, self.out_ann,
				[4, ['Invalid block length', 'Invalid B length', 'E: B length', 'E: BL']])
	
	def checkBlockLength(self, btype, length, blockstartsample, blockendsample):
		if btype == 0x02:
			if length != 11:
				self.putBlockLengthError(blockstartsample, blockendsample)
	
	def putBlockCSumError(self, blockstartsample, blockendsample):
		self.put(blockstartsample, blockendsample, self.out_ann,
				[4, ['Invalid block checksum', 'Invalid B CSum', 'E: B CSum', 'E: B CS']])
	
	def checkBlockCSum(self, blockdata, blockstartsample, blockendsample):
		temp = 0xFF
		for item in blockdata:
			#print(item)
			temp = temp - item[1]
		if temp < 0:
			temp = temp * -1
		temp = temp & 0xFF
		
		if temp != 0:
			self.putBlockCSumError(blockstartsample, blockendsample)
		else:
			self.put(blockdata[len(blockdata)-1][0][1][0], blockdata[len(blockdata)-1][0][8][1], self.out_ann,
				[3, ['Valid Checksum', 'Valid CSum']])
	
	def putBlockType(self, frame):
		self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
				[0, ['Block Type', 'BType', 'BT']])
		self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
				[2, ['Block Type', 'BType', 'BT']])
		if btype.get(frame[1]) != None:
			self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%s (0x%02X)' % (btype[frame[1]], frame[1])]])
		else:
			self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['Unknown Block Type (0x%02X)' % frame[1], 'Unknown BType (0x%02X)' % frame[1], 'Unk BType (0x%02X)' % frame[1], 'E: BT 0x%02X' % frame[1]]])
	
	def putCSum(self, frame):
		self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
				[0, ['Block Checksum', 'B Checksum', 'B CSum', 'B CS']])
		self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
				[2, ['Block Checksum', 'B Checksum', 'B CSum', 'B CS']])
	
	def putData(self, btype, index, frame):
		self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
				[0, ['Block Data %d' % index, 'BData%d' % index]])
		if btype == 0x02:
			#TAGGER-STATUS
			if index == 0:
				#BData0
				#Game Settings
				self.put(frame[0][1][0], frame[0][3][1], self.out_ann,
					[2, ['Player Number', 'Player Num', 'Player #', 'Play #', 'P']])
				self.put(frame[0][1][0], frame[0][3][1], self.out_ann,
					[3, ['%d' % (frame[1] & 0x07),]])
				self.put(frame[0][4][0], frame[0][5][1], self.out_ann,
					[2, ['Team Number', 'Team Num', 'Team #', 'T']])
				self.put(frame[0][4][0], frame[0][5][1], self.out_ann,
					[3, ['%d' % ((frame[1] & 0x18) >> 3),]])
			elif index == 1:
				#BData1
				#Game Status
				
				#Weapon mode
				selectedweapmode = frame[1] & 0x03;
				self.put(frame[0][1][0], frame[0][2][1], self.out_ann,
					[2, ['Weapon Mode', 'Weap Mode', 'WM']])
				if weapmode.get(selectedweapmode) != None:
					self.put(frame[0][1][0], frame[0][2][1], self.out_ann,
						[3, ['%s' % weapmode[selectedweapmode],]])
				else:
					self.put(frame[0][1][0], frame[0][2][1], self.out_ann,
						[3, ['Unknown', 'Unk']])
				
				#Shield status
				currentshieldstatus = (frame[1] & 0x0C) >> 2;
				self.put(frame[0][3][0], frame[0][4][1], self.out_ann,
					[2, ['Shield State', 'Shield St', 'Shld']])
				if shieldstatus.get(currentshieldstatus) != None:
					self.put(frame[0][3][0], frame[0][4][1], self.out_ann,
						[3, ['%s' % shieldstatus[currentshieldstatus],]])
				else:
					self.put(frame[0][3][0], frame[0][4][1], self.out_ann,
						[3, ['Unknown', 'Unk']])
				
				#Hunting direction
				currenthuntingdirection = (frame[1] & 0x20) >> 5;
				self.put(frame[0][6][0], frame[0][6][1], self.out_ann,
					[2, ['Hunting Direction', 'Hunting Dir', 'Hnt Dir']])
				self.put(frame[0][6][0], frame[0][6][1], self.out_ann,
					[3, ['%s' % huntingdirection[currenthuntingdirection],]])
			elif index == 2:
				#BData2
				#Health remaining
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Health Remaining', 'Health Remain', 'Health Rem', 'Health', 'H']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
			elif index == 3:
				#BData3
				#Loaded Ammo
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Loaded Ammo', 'Ammo']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
			elif index == 4:
				#BData4
				#Remaining Ammo Low Byte
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Remaining Ammo, Low', 'Remain Ammo, Low', 'Rem Ammo, L']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
			elif index == 5:
				#BData5
				#Remaining Ammo High Byte
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Remaining Ammo, High', 'Remain Ammo, High', 'Rem Ammo, H']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % (frame[1] << 8),]])
			elif index == 6:
				#BData6
				#Shield Time
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Shield Time', 'Shld Tim']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
			elif index == 7:
				#BData7
				#Game Time Minutes
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Game Time, Minutes', 'Game Time, Min', 'Game Tim, Min', 'Game Min']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
			elif index == 8:
				#BData8
				#Game Time Seconds
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[2, ['Game Time, Seconds', 'Game Time, Sec', 'Game Tim, Sec', 'Game Sec']])
				self.put(frame[0][1][0], frame[0][8][1], self.out_ann,
					[3, ['%d' % frame[1],]])
	
	def __init__(self):
		self.state = 'IDLE'
	
	def reset(self):
		self.__init__(self)
		
	def start(self):
		#self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
	
	def decode(self, startsample, endsample, data):
		
		garbage, data = data
		
		length = len(data)
		btype = data[0][1]
		blockstartsample = startsample
		blockendsample = endsample
		
		self.checkBlockLength(btype, length, blockstartsample, blockendsample)
		
		self.checkBlockCSum(data, blockstartsample, blockendsample)
		
		for index, frame in enumerate(data):
			if index == 0:
				self.putBlockType(frame)
			elif index == (length-1):
				self.putCSum(frame)
			else:
				dataCount = index - 1
				self.putData(btype, dataCount, frame)
				