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

'''
OUTPUT_PYTHON format:

Packet:
	[<ptype>, <pdata>]
	
	<pytpe> currently only has one value, 'BLOCK'.
	
	<pdata> is an annoyingly complicated tuple structure, that looks like the following:
		[
			[
				10x [
					startofbit as samplenum,
					endofbit as samplenum,
					bitdata as int
				],
				
				framedata as int
			],
		]
	This does allow for an arbitrary number of frames in a block, but makes accessing the individual pieces annoying.
	
	Note also that there are 10 bits in the frame, this includes the "start"(bit 0) and "stop"(bit 9) bits!
	
	framedata is the middle 8 bits, bit-swapped to account for the protocol being LSB-first and most people wanting MSB-first.
	
	Cheatsheet:
		bit-level data:
			startofbit is pdata[whichframe][0][whichbit][0]
			endofbit is pdata[whichframe][0][whichbit][1]
			bitdata is pdata[whichframe][0][whichbit][2]
		
		frame-level data:
			framedata is pdata[whichframe][1]
		
		See how many frames are in the block:
			len(pdata)
			
'''

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
	api_version = 3
	id = 'ltar_smartdevice'
	name = 'LTAR SmartDevice'
	longname = 'LTAR SmartDevice'
	desc = 'A decoder for the LTAR laser tag blaster\'s Smart Device protocol'
	license = 'unknown'
	inputs = ['afsk_bits']
	outputs = ['ltar_smartdevice']
	tags = ['Embedded/industrial']
	annotations = (
		('bit-start', 'Start Bit'),
		('bit-data', 'Data Bit'),
		('bit-stop', 'Stop Bit'),
		('bit-spacer', 'Spacer Bit'),
		('bit-blockend', 'Block Stop Bit'),
		('frame', 'Data frame'),
		('frame-error', 'Framing error'),
		('block', 'Data block'),
		('block-error', 'Block error'),
	)
	annotation_rows = (
		('bits', 'Bits', (0, 1, 2, 3, 4)),
		('frames', 'Frames', (5, 6,)),
		('blocks', 'Blocks', (7, 8,)),
	)
	
	def putbitstart(self, startsample, endsample):
		self.put(startsample, endsample, self.out_ann,
				[0, ['Start Bit', 'Start B', 'Start']])
	
	def putbitdata(self, startsample, endsample, bitdata):
		self.put(startsample, endsample, self.out_ann,
				[1, ['%d' % bitdata]])
	
	def putbitstop(self, startsample, endsample):
		self.put(startsample, endsample, self.out_ann,
				[2, ['Stop Bit', 'Stop B', 'Stop']])
	
	def putbitspacer(self, startsample, endsample):
		self.put(startsample, endsample, self.out_ann,
				[3, ['Spacer Bit', 'Spacer']])
	
	def putbitblockend(self, startsample, endsample):
		self.put(startsample, endsample, self.out_ann,
				[4, ['Block Stop', 'Block']])
	
	def putframe(self, currentframedata, data):
		self.put(currentframedata[0][0], currentframedata[len(currentframedata)-1][1], self.out_ann,
				[5, ['Data frame: 0x%02X' % data, 'Data: 0x%02X' % data, 'D 0x%02X' % data]])
	
	def putframingerror(self, currentframedata):
		self.put(currentframedata[0][0], currentframedata[len(currentframedata)-1][1], self.out_ann,
				[6, ['Data framing error', 'Framing error', 'Frame Error', 'FE']])
	
	def putblock(self, currentblockdata, endsample):
		self.put(currentblockdata[0][0][0][0], endsample, self.out_python,
				['BLOCK', currentblockdata])
		self.put(currentblockdata[0][0][0][0], endsample, self.out_ann,
				[7, ['Block, %d frames' % len(currentblockdata), 'B %d' % len(currentblockdata)]])
	
	def putblockerror(self, currentblockdata, endsample):
		self.put(currentblockdata[0][0][0][0], endsample, self.out_ann,
				[8, ['Block Error', 'Block E', 'BE']])
	
	def __init__(self):
		self.state = 'IDLE'
		self.framestartsample = self.frameendsample = 0
		self.currentframedata = []
		self.blockstartsample = self.blockendsample = 0
		self.currentblockdata = []
		self.spacercount = 0
	
	def reset(self):
		self.__init__(self)
	
	def start(self):
		self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
	
	def decode(self, startsample, endsample, afskdata):
		datatype, argument = afskdata
		
		if datatype == 'BIT':
			#A probably-valid data bit!
			if self.state == 'IDLE':
				if argument == 0:
					#Start bit!
					self.putbitstart(startsample, endsample)
					self.currentframedata.append([startsample, endsample, argument])
					self.state = 'DATA'
			elif self.state == 'DATA':
				#Capture 8 data bits
				self.putbitdata(startsample, endsample, argument)
				self.currentframedata.append([startsample, endsample, argument])
				if len(self.currentframedata) == 9:
					self.state = 'FRAMESTOP'
			elif self.state == 'FRAMESTOP':
				if argument == 1:
					#End of a data frame
					self.putbitstop(startsample, endsample)
					self.currentframedata.append([startsample, endsample, argument])
					
					data = (self.currentframedata[8][2] << 7) | (self.currentframedata[7][2] << 6) | (self.currentframedata[6][2] << 5) | (self.currentframedata[5][2] << 4) | (self.currentframedata[4][2] << 3) | (self.currentframedata[3][2] << 2) | (self.currentframedata[2][2] << 1) | self.currentframedata[1][2]
					
					self.putframe(self.currentframedata, data)
					self.currentblockdata.append([self.currentframedata, data])
					self.currentframedata = []
					self.state = 'WAITINGFORBLOCKEND'
				else:
					#Framing error!
					self.putframingerror(self.currentframedata)
					self.currentframedata = []
					if len(self.currentblockdata) != 0:
						self.putblockerror(self.currentblockdata, endsample)
						self.currentblockdata = []
					self.spacercount = 0
					self.state = 'IDLE'
			elif self.state == 'WAITINGFORBLOCKEND':
				if argument == 1:
					#Spacer bits between frames/blocks
					#15 spacer bits marks the end of a block.
					#Frames within a block should have no more than 10 spacers between them.
					#A device should generate 20 spacers at the end of a block, to guarantee proper synchronization between devices
					if self.spacercount < 14:
						self.putbitspacer(startsample, endsample)
						self.spacercount = self.spacercount + 1
					else:
						#End of a block!
						self.putbitblockend(startsample, endsample)
						self.putblock(self.currentblockdata, endsample)
						self.currentblockdata = []
						self.spacercount = 0
						self.state = 'IDLE'
				else:
					#Start bit of another frame
					if self.spacercount < 10:
						self.putbitstart(startsample, endsample)
						self.currentframedata.append([startsample, endsample, argument])
						self.spacercount = 0
						self.state = 'DATA'
					else:
						if len(self.currentframedata) == 0:
							self.currentframedata.append([startsample, endsample, argument])
						self.putframingerror(self.currentframedata)
						self.currentframedata = []
						if len(self.currentblockdata) != 0:
							self.putblockerror(self.currentblockdata, endsample)
							self.currentblockdata = []
						self.spacercount = 0
						self.state = 'IDLE'
		elif datatype == 'ERROR':
			if argument == 'PHASE':
				#Resynced to the proper phase of the signal. Abort all current decodes.
				if len(self.currentframedata) != 0:
					self.putframingerror(self.currentframedata)
					self.currentframedata = []
				if len(self.currentblockdata) != 0:
					self.putblockerror(self.currentblockdata, endsample)
					self.currentblockdata = []
				self.spacercount = 0
				self.state = 'IDLE'
			elif argument == 'INVALID':
				#A cycle that doesn't match our AFSK settings. Abort all current decodes.
				if len(self.currentframedata) != 0:
					self.putframingerror(self.currentframedata)
					self.currentframedata = []
				if len(self.currentblockdata) != 0:
					self.putblockerror(self.currentblockdata, endsample)
					self.currentblockdata = []
				self.spacercount = 0
				self.state = 'IDLE'