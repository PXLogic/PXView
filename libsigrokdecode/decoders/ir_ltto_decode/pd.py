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

# Recoil laser tag IR protocol decoder

import sigrokdecode as srd

class SamplerateError(Exception):
    pass

healthtext = [
	'0%',
	'<25%',
	'<50%',
	'>50%',
]

ptype = {
	0x00: 'GAME START',
	0x01: 'JOIN CONFIRMED',
	0x02: 'HOSTING CUSTOM',
	0x03: 'HOSTING 2-TEAMS',
	0x04: 'HOSTING 3-TEAMS',
	0x05: 'HOSTING HIDE-AND-SEEK',
	0x06: 'HOSTING HUNTER-HUNTED',
	0x07: 'HOSTING 2-KINGS',
	0x08: 'HOSTING 3-KINGS',
	0x09: 'HOSTING OWN-THE-ZONE',
	0x0A: 'HOSTING 2-TEAM OWN-THE-ZONE',
	0x0B: 'HOSTING 3-TEAM OWN-THE-ZONE',
	0x0C: 'HOSTING HOOK GAME',
	0x0D: 'RESERVED(0x0D)',
	0x0E: 'RESERVED(0x0E)',
	0x0F: 'CHANNEL FAILURE',
	0x10: 'REQUEST TO JOIN',
	0x11: 'CHANNEL RELEASE',
	0x12: 'RESERVED(0x12)',
	
	0x20: 'MEDIC REQUEST',
	0x21: 'MEDIC ASSIST',
	0x22: 'MEDIC RELEASE',
	
	0x30: 'RESERVED(0x30)',
	0x31: 'DEBRIEF DATA NEEDED',
	0x32: 'RANKINGS',
	0x33: 'NAME-DATA',
	
	0x40: 'BASIC DEBRIEF DATA',
	0x41: 'GROUP 1 DEBRIEF DATA',
	0x42: 'GROUP 2 DEBRIEF DATA',
	0x43: 'GROUP 3 DEBRIEF DATA',
	0x44: 'RESERVED(0x44)',
	
	0x48: 'HEAD-TO-HEAD SCORE DATA',
	0x49: 'RESERVED(0x49)',
	0x4A: 'RESERVED(0x4A)',
	0x4B: 'RESERVED(0x4B)',
	
	0x50: 'BASIC DE-CLONING DATA',
	0x51: 'GROUP 1 DE-CLONING DATA',
	0x52: 'GROUP 2 DE-CLONING DATA',
	0x53: 'GROUP 3 DE-CLONING DATA',
	0x54: 'DE-CLONING REQUEST',
	
	0x80: 'TEXT MESSAGE',
	0x81: 'LTAR GAME',
	0x82: 'LTAR RTJ',
	0x83: 'LTAR PLAYER',
	0x84: 'LTAR ACCEPT',
	0x85: 'LTAR NAME',
	0x86: 'LTAR WHODAT',
	0x87: 'LTAR RELEASE',
	0x88: 'LTAR START COUNTDOWN',
	
	0x8F: 'LTAR ABORT',
	0x90: 'LTAR SPECIAL-ATTACK',
}

class Decoder(srd.Decoder):
	api_version = 3
	id = 'ir_ltto_decode'
	name = 'IR LTTO Decode'
	longname = 'LTTO laser tag IR Decode'
	desc = 'A decoder for the LTTO laser tag IR protocol'
	license = 'unknown'
	inputs = ['ir_ltto']
	outputs = ['ir_ltto_decode']
	tags = ['Embedded/industrial']
	annotations = (
		('signature-type', 'Signature Type'),
		('signature-error', 'Error'),
		('signature-data', 'Signature Data'),
		('packet-type', 'Packet Type'),
		('packet-error', 'Packet Error'),
		('packet-data', 'Packet Data'),
	)
	annotation_rows = (
		('signature-types', 'Signature type', (0, 1,)),
		('signature-datas', 'Signature data', (2,)),
		('packet-types', 'Packet type', (3, 4,)),
		('packet-datas', 'Packet data', (5,)),
	)
	
	def putTagSignature(self, startsample, endsample, bitdata):
		team = 0
		if bitdata & 0x60 == 0x00:
			#Team 0
			team = 0 
		elif bitdata & 0x60 == 0x20:
			#Team 1
			team = 1
		elif bitdata & 0x60 == 0x40:
			#Team 2
			team = 2
		elif bitdata & 0x60 == 0x60:
			#Team 3
			team = 3
		
		player = 0
		if bitdata & 0x1b == 0x00:
			#Player 0
			player = 0
		elif bitdata & 0x1b == 0x04:
			#Player 1
			player = 1
		elif bitdata & 0x1b == 0x08:
			#Player 2
			player = 2
		elif bitdata & 0x1b == 0x0b:
			#Player 3
			player = 3
		elif bitdata & 0x1b == 0x10:
			#Player 4
			player = 4
		elif bitdata & 0x1b == 0x14:
			#Player 5
			player = 5
		elif bitdata & 0x1b == 0x18:
			#Player 6
			player = 6
		elif bitdata & 0x1b == 0x1b:
			#Player 7
			player = 7
		
		megatag = 0
		if bitdata & 0x03 == 0x00:
			megatag = 0
		elif bitdata & 0x03 == 0x01:
			megatag = 1
		elif bitdata & 0x03 == 0x02:
			megatag = 2
		elif bitdata & 0x03 == 0x03:
			megatag = 3
		
		self.put(startsample, endsample, self.out_ann,
				[0, ['Tag', 'T']])
		if team == 0:
			if player == 0:
				self.put(startsample, endsample, self.out_ann,
						[2, ['LTAG(SOLO), Megatag: %d' % megatag, 'LTAG, Mega %d' % megatag, 'LTAG, M %d' % megatag]])
			elif player == 1:
				self.put(startsample, endsample, self.out_ann,
						[2, ['TTAG(Team 1), Megatag: %d' % megatag, 'TTAG1, Mega %d' % megatag, 'TTAG1, M %d' % megatag]])
			elif player == 2:
				self.put(startsample, endsample, self.out_ann,
						[2, ['TTAG(Team 2), Megatag: %d' % megatag, 'TTAG2, Mega %d' % megatag, 'TTAG2, M %d' % megatag]])
			elif player == 3:
				self.put(startsample, endsample, self.out_ann,
						[2, ['TTAG(Team 3), Megatag: %d' % megatag, 'TTAG3, Mega %d' % megatag, 'TTAG3, M %d' % megatag]])
			elif player == 4:
				self.put(startsample, endsample, self.out_ann,
						[2, ['Neutral Base, Megatag: %d' % megatag, 'Neut Base, Mega %d' % megatag, 'NB, M %d' % megatag]])
			elif player == 5:
				self.put(startsample, endsample, self.out_ann,
						[2, ['Team 1 Base, Megatag: %d' % megatag, 'T1 Base, Mega %d' % megatag, 'T1B, M %d' % megatag]])
			elif player == 6:
				self.put(startsample, endsample, self.out_ann,
						[2, ['Team 2 Base, Megatag: %d' % megatag, 'T2 Base, Mega %d' % megatag, 'T2B, M %d' % megatag]])
			elif player == 7:
				self.put(startsample, endsample, self.out_ann,
						[2, ['Team 3 Base, Megatag: %d' % megatag, 'T3 Base, Mega %d' % megatag, 'T3B, M %d' % megatag]])
		else:
			self.put(startsample, endsample, self.out_ann,
					[2, ['Team: %d, Player: %d, Megatag: %d' % (team, player, megatag), 'Team %d, Player %d, Mega %d' % (team, player, megatag), 'T %d, P %d, M %d' % (team, player, megatag)]])
	
	def putMultibyteStartSignature(self, startsample, endsample, bitdata):
		self.put(startsample, endsample, self.out_ann,
				[0, ['Multibyte Packet Type', 'Multibyte P Type', 'PType']])
		self.put(startsample, endsample, self.out_ann,
				[2, ['%s' % ptype[(bitdata & 0xFF)]]])
	
	def putMultibyteEndSignature(self, startsample, endsample, bitdata):
		self.put(startsample, endsample, self.out_ann,
				[0, ['Multibyte Packet Checksum', 'Multibyte P CSum', 'CSum']])
		self.put(startsample, endsample, self.out_ann,
				[2, ['0x%02X' % (bitdata & 0xFF)]])
	
	def putMultibyteDataSignature(self, startsample, endsample, bitdata):
		self.put(startsample, endsample, self.out_ann,
				[0, ['Multibyte Packet Data %d' % (len(self.multibytedata)-2), 'Multibyte P Data %d' % (len(self.multibytedata)-2), 'PData%d' % (len(self.multibytedata)-2)]])
		self.put(startsample, endsample, self.out_ann,
				[2, ['0x%02X' % (bitdata & 0xFF)]])
	
	def putMultibytePacket(self):
		self.put(self.multibytestartsample, self.multibyteendsample, self.out_ann,
				[3, ['Multibyte Packet', 'Multibyte P', 'MP']])
	
	def putLTTOBeaconSignature(self, startsample, endsample, bitdata):
		team = 0
		if bitdata & 0x18 == 0x00:
			#Team 0
			team = 0 
		elif bitdata & 0x18 == 0x08:
			#Team 1
			team = 1
		elif bitdata & 0x18 == 0x10:
			#Team 2
			team = 2
		elif bitdata & 0x18 == 0x18:
			#Team 3
			team = 3
		
		hitflag = 0
		if bitdata & 0x04 == 0x04:
			hitflag = 1
		
		extra = 0
		if bitdata & 0x03 == 0x00:
			extra = 0
		elif bitdata & 0x03 == 0x01:
			extra = 1
		elif bitdata & 0x03 == 0x02:
			extra = 2
		elif bitdata & 0x03 == 0x03:
			extra = 3
		
		if hitflag == 0 and extra != 0:
			#Special Team 0 beacon
			self.put(startsample, endsample, self.out_ann,
				[0, ['Area Beacon', 'Area B', 'AB']])
			if extra == 1:
				#Mine fire
				self.put(startsample, endsample, self.out_ann,
					[2, ['Mine Tag', 'Mine T', 'MT']])
			elif extra == 2:
				#Zone
				self.put(startsample, endsample, self.out_ann,
					[2, ['Zone', 'Z']])
			elif extra == 3:
				if team == 0:
					#Neutral base
					self.put(startsample, endsample, self.out_ann,
						[2, ['Neutral Base', 'Neut Base', 'NB']])
				elif team == 1:
					#T1 base
					self.put(startsample, endsample, self.out_ann,
						[2, ['Team 1 Base', 'T1 Base', 'T1B']])
				elif team == 2:
					#T1 base
					self.put(startsample, endsample, self.out_ann,
						[2, ['Team 2 Base', 'T2 Base', 'T2B']])
				elif team == 3:
					#T1 base
					self.put(startsample, endsample, self.out_ann,
						[2, ['Team 3 Base', 'T3 Base', 'T3B']])
		else:
			#Normal beacon
			self.put(startsample, endsample, self.out_ann,
				[0, ['LTTO Beacon', 'LTTO B', 'OB']])
			self.put(startsample, endsample, self.out_ann,
				[2, ['Team: %d, Just hit: %d, Extra damage: %d' % (team, hitflag, extra), 'Team %d, Hit %d, Extra %d' % (team, hitflag, extra), 'T %d, H %d, X %d' % (team, hitflag, extra)]])
	
	def putLTARBeaconSignature(self, startsample, endsample, bitdata):
		hitflag = 0
		if bitdata & 0x100 == 0x100:
			hitflag = 1
		
		shields = 0
		if bitdata & 0x80 == 0x80:
			shields = 1
		
		health = 0
		if bitdata & 0x60 == 0x00:
			health = 0
		elif bitdata & 0x60 == 0x20:
			health = 1
		elif bitdata & 0x60 == 0x40:
			health = 2
		elif bitdata & 0x60 == 0x60:
			health = 3
		
		team = 0
		if bitdata & 0x18 == 0x00:
			#Team 0
			team = 0 
		elif bitdata & 0x18 == 0x08:
			#Team 1
			team = 1
		elif bitdata & 0x18 == 0x10:
			#Team 2
			team = 2
		elif bitdata & 0x18 == 0x18:
			#Team 3
			team = 3
		
		player = 0
		if bitdata & 0x07 == 0x00:
			#Player 0
			player = 0
		elif bitdata & 0x07 == 0x01:
			#Player 1
			player = 1
		elif bitdata & 0x07 == 0x02:
			#Player 2
			player = 2
		elif bitdata & 0x07 == 0x03:
			#Player 3
			player = 3
		elif bitdata & 0x07 == 0x04:
			#Player 4
			player = 4
		elif bitdata & 0x07 == 0x05:
			#Player 5
			player = 5
		elif bitdata & 0x07 == 0x06:
			#Player 6
			player = 6
		elif bitdata & 0x07 == 0x07:
			#Player 7
			player = 7
		
		self.put(startsample, endsample, self.out_ann,
				[0, ['LTAR Beacon', 'LTAR B', 'NB']])
		self.put(startsample, endsample, self.out_ann,
			[2, ['Just hit: %d, Shields up: %d, Rough health: %s, Team: %d, Player: %d' % (hitflag, shields, healthtext[health], team, player), 'Hit %d, Shields %d, RH %s, Team %d, Player %d' % (hitflag, shields, healthtext[health], team, player), 'H %d, S %d, RH %s, T %d, P %d' % (hitflag, shields, healthtext[health], team, player)]])
	
	def __init__(self):
		self.state = 'SINGLE'
		self.multibytestartsample = 0
		self.multibyteendsample = 0
		self.multibytedata = []
	
	def reset(self):
		self.__init__(self)
		
	def start(self):
		self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
	
	def decode(self, startsample, endsample, data):
		synclength, bitcount, bitdata = data
		
		if synclength == 'SHORT':
			if bitcount == 7:
				#Tag
				self.putTagSignature(startsample, endsample, bitdata)
			elif bitcount == 9:
				#Multibyte start/end
				if bitdata & 0x100 == 0x000:
					#Multibyte start
					self.multibytestartsample = startsample
					self.multibytedata = []
					self.multibytedata.append([startsample, endsample, bitdata])
					self.putMultibyteStartSignature(startsample, endsample, bitdata)
				else:
					#Multibyte end
					self.multibyteendsample = endsample
					self.multibytedata.append([startsample, endsample, bitdata])
					self.putMultibyteEndSignature(startsample, endsample, bitdata)
					self.putMultibytePacket()
			elif bitcount == 8:
				#Multibyte data
				self.multibytedata.append([startsample, endsample, bitdata])
				self.putMultibyteDataSignature(startsample, endsample, bitdata)
		elif synclength == 'LONG':
			if bitcount == 5:
				#LTTO Beacon
				self.putLTTOBeaconSignature(startsample, endsample, bitdata)
			elif bitcount == 9:
				#LTAR beacon
				self.putLTARBeaconSignature(startsample, endsample, bitdata)