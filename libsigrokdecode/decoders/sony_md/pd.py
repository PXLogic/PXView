##
# This file is part of the libsigrokdecode project.
##
## Copyright (C) 2021 Ryan "Izzy" Bales <izzy84075@gmail.com>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
##
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
##
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
##


# Sony Minidisc LCD Remote protocol decoder

import sigrokdecode as srd

'''

OUTPUT_PYTHON format:

Packet:
	[<syncData>, <bitData>, <cleanEnd>]

	<syncData> is a tuple structure that looks like the following:
		[
			3x [
				startOfPulse as samplenum,
				endOfPulse as samplenum,
			],
		]

		The first of these is the Presync Pulse,
		the second is the Presync Delay,
		and the third is the actual Sync Pulse
	
	<bitData> is another tuple structure that looks like the following:
		[
			startOfBits as samplenum,
			endOfBits as samplenum,

			numberOfBits as int,

			?x [
				startOfBit as samplenum,
				middleOfBit as samplenum,
				endOfBit as samplenum,
				bitValue as int,
			],
		]

		There will generally be two bytes(16 bits), thirteen bytes(104 bits), or fourteen bytes and 3 bits(115 bits) worth of data.
	
	<cleanEnd> is a boolean that says whether this message ended "normally" or with an error
		True means it ended normally
		False means it ended with an error

	---------

	Cheatsheet:
		Sync data:
			Presync start: syncData[0][0]
			Presync end: syncData[0][1]
			Presync Delay start: syncData[1][0]
			Presync Delay end: syncData[1][1]
			Sync start: syncData[2][0]
			Sync end: syncData[2][1]

		Misc:
			Start of data bits: bitData[0]
			End of data bits: bitData[1]
			Number of data bits: bitData[2]

		Bit-level data:
			Start of bit: bitData[3][whichBit][0]
			Middle of bit: bitData[3][whichBit][1]
			End of bit: bitData[3][whichBit][2]
			Bit value: bitData[3][whichBit][3]
		
		See how many full bytes there are in the message:
			int(biteData[2] / 8)
		
		See if there are leftover bits after the full bytes:
			(bitData[2] - (int(bitData[2] / 8)*8))

'''

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
	api_version = 3
	id = 'sony_md'
	name = 'Sony MD Remote'
	longname = 'Sony MD LCD Remote'
	desc = ''
	license = 'unknown'
	inputs = ['logic']
	outputs = ['sony_md']
	tags = ['']
	channels = (
		{'id': 'data', 'name': 'data', 'desc': 'Data stream'},
	)
	options = (
		{'id': 'marginpct', 'desc': 'Error margin %', 'default': 20},
	)
	annotations = (
		('signals', 'Signals'),
		('bit-zero', '0'),
		('bit-one', '1'),
		('bit-error', 'Unknown half-cycle'),
		('state-error', 'State error'),
		('byte', 'Byte value'),
		('bit-count', 'Message bit count'),
		('bit-count-error', 'Expected multiple of 8 bits'),
	)
	annotation_rows = (
		('signalling', 'Signalling', (0,)),
		('raw-bits', 'Raw Bits', (1,2,)),
		('byte-values', 'Byte Values', (5,)),
		('Messages', 'Messages', (6,)),
		('errors', 'Errors', (3, 4, 7,)),
	)

	def putError(self):
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[3, ['Error']])
	
	def putErrorUnexpectedDataBit(self):
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[3, ['Unexpected data bit']])

	def putStateError(self):
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[4, ['State error: %s' % (self.state)]])

	def putResetPulse(self):
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[0, ['Reset+Presync pulse', 'Reset', 'R']])

	def putPresyncPulse(self):
		self.messageSyncData.append([self.lastedgesample, self.newedgesample])
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[0, ['Presync pulse', 'Presync', 'PS']])
	
	def putPresyncDelayPulse(self):
		self.messageSyncData.append([self.lastedgesample, self.newedgesample])
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[0, ['Presync delay', 'PSD']])
	
	def putSyncPulse(self):
		self.messageSyncData.append([self.lastedgesample, self.newedgesample])
		self.put(self.lastedgesample, self.newedgesample, self.out_ann,
				[0, ['Sync pulse', 'S']])
	
	def putRemoteHasData(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Remote HAS data to send', 'RY']])
	
	def putRemoteHasNoData(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Remote has NO data to send', 'RN']])
	
	def putPlayerHasData(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Player HAS data to send', 'PY']])
	
	def putPlayerHasNoData(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Player has NO data to send', 'PN']])
	
	def putPlayerCedesBusToRemote(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Player CEDES bus to Remote', 'RDB']])
	
	def putPlayerDoesNotCedeBusToRemote(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[0, ['Player does NOT cede bus to Remote', 'PDB']])
	
	def putPlayerCededBusWithoutRemoteAsking(self):
		self.put(self.databitstart, self.databitend, self.out_ann,
				[7, ['Player ceded bus to Remote without Remote asking!']])
	
	def putZeroBit(self):
		self.messageBitData.append([self.databitstart, self.lastedgesample, self.databitend, 0])
		self.dataBitCount = self.dataBitCount + 1
		self.put(self.databitstart, self.databitend, self.out_ann,
				[1, ['0']])
	
	def putOneBit(self):
		self.messageBitData.append([self.databitstart, self.lastedgesample, self.databitend, 1])
		self.dataBitCount = self.dataBitCount + 1
		self.put(self.databitstart, self.databitend, self.out_ann,
				[2, ['1']])
	
	def putEndOfPacket(self):
		self.put(self.newedgesample, self.newedgesample, self.out_ann,
				[0, ['Message End', 'St']])

	def putPacketBitCount(self):
		self.pythonOutputBitData.append([self.packetstartsample, self.packetendsample, self.dataBitCount, self.messageBitData])
		self.put(self.packetstartsample, self.packetendsample, self.out_python,
				[self.messageSyncData, [self.packetstartsample, self.packetendsample, self.dataBitCount, self.messageBitData], True])
		self.messageSyncData = []
		self.messageBitData = []
		self.pythonOutputBitData = []
		self.put(self.packetstartsample, self.packetendsample, self.out_ann,
				[6, ['Message, %d bits' % self.dataBitCount ]])
	
	def putExpectedBitError(self):
		self.put(self.newedgesample-1, self.newedgesample, self.out_ann,
				[7, ['Unexpected end of message']])

	def returnToIdle(self):
		self.state = 'IDLE'
		self.playerHasData = False
		self.remoteHasData = False
		self.playerCedesBus = False
		self.dataBitCount = 0
		self.expectedBitCount = 16
		self.messageSyncData = []
		self.messageBitData = []
		self.pythonOutputBitData = []

	def reset(self):
		self.state = 'IDLE'
		self.lastedgesample = 0
		self.lastedgestate = False
		self.newedgesample = 0
		self.newedgestate = False

		self.playerHasData = False
		self.remoteHasData = False
		self.playerCedesBus = False

		self.pulselength = 0

		self.dataBitCount = 0
		self.expectedBitCount = 16

		self.bytestartsample = 0
		self.byteendsample = 0
		self.bytevalue = 0

		self.packetstartsample = 0
		self.packetendsample = 0

		self.messageSyncData = []
		self.messageBitData = []
		self.pythonOutputBitData = []
	
	def __init__(self):
		self.reset()
	
	def start(self):
		self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
		
		self.marginpct = self.options['marginpct']
		
		self.resetCycles = int(self.samplerate * (40/1000))
		self.resetMinimumCycles = int(self.resetCycles * (1-(self.marginpct*0.01)))
		self.resetMaximumCycles = int(self.resetCycles * (1+(self.marginpct*0.01)))

		self.presyncCycles = int(self.samplerate * (1100/1000000))
		self.presyncMinimumCycles = int(self.presyncCycles * (1-(self.marginpct*0.01)))
		self.presyncMaximumCycles = int(self.presyncCycles * (1+(self.marginpct*0.01)))

		self.presyncDelayCycles = int(self.samplerate * (950/1000000))
		self.presyncDelayMinimumCycles = int(self.samplerate * (800/1000000))
		self.presyncDelayMaximumCycles = int(self.samplerate * (1500/1000000))

		self.syncCycles = int(self.samplerate * (220/1000000))
		self.syncMinimumCycles = int(self.samplerate * (20/1000000))
		self.syncMaximumCycles = int(self.syncCycles * (1+(self.marginpct*0.01)))

		self.bitDelayHighIdealCycles = int(self.samplerate * (32.5/1000000))
		self.bitDelayHighCyclesMinimum = int(self.bitDelayHighIdealCycles * (1-(self.marginpct*0.01)))

		self.shortMessageDataLongCycles = int(self.samplerate * (220/1000000))
		self.shortMessageDataLongCyclesMinimum = int(self.samplerate * (101/1000000))
		self.shortMessageDataLongCyclesMaximum = int(self.samplerate * (280/1000000))

		self.shortMessageDataShortCycles = int(self.samplerate * (17/1000000))
		self.shortMessageDataShortCyclesMinimum = int(self.samplerate * (10/1000000))
		self.shortMessageDataShortCyclesMaximum = int(self.samplerate * (100/1000000))

		self.extendedMessageTimeoutCycles = int(self.samplerate *(5/1000))
		#self.extendedMessageTimeoutCyclesSkip = self.extendedMessageTimeoutCycles + 50

	
	def metadata(self, key, value):
		if key == srd.SRD_CONF_SAMPLERATE:
			self.samplerate = value
	
	def decode(self):
		if not self.samplerate:
			raise SamplerateError('Cannot decode without samplerate.')

		while True:
			self.lastedgesample = self.newedgesample
			self.lastedgestate = self.newedgestate
			
			#if self.state == 'IDLE':
			(self.newedgestate,) = self.wait([{0: 'e'}])
			#else:
			#(self.newedgestate,) = self.wait([{0: 'e'}, {'skip': self.extendedMessageTimeoutCyclesSkip}])

			self.newedgesample = self.samplenum

			self.pulselength = self.newedgesample - self.lastedgesample
			
			if self.state == 'IDLE':
				#low or high
				if self.lastedgestate == False and self.newedgestate == True:
					#now high, was low
					if self.pulselength in range(self.resetMinimumCycles, self.resetMaximumCycles):
						self.packetstartsample = self.lastedgesample
						self.putResetPulse()
						self.state = 'PRESYNC'
					elif self.pulselength in range(self.presyncMinimumCycles, self.presyncMaximumCycles):
						self.packetstartsample = self.lastedgesample
						self.putPresyncPulse()
						self.state = 'PRESYNC'
					elif self.pulselength in range(self.shortMessageDataShortCyclesMinimum, self.shortMessageDataShortCyclesMaximum):
						self.putErrorUnexpectedDataBit()
					elif self.pulselength in range(self.shortMessageDataLongCyclesMinimum, self.shortMessageDataLongCyclesMaximum):
						self.putErrorUnexpectedDataBit()
			elif self.state == 'PRESYNC':
				#now low, was high
				if self.pulselength in range(self.presyncDelayMinimumCycles, self.presyncDelayMaximumCycles):
					self.putPresyncDelayPulse()
					self.state = 'SYNC'
				else:
					self.putError()
					self.returnToIdle()
			elif self.state == 'SYNC':
				#now high, was low
				if self.pulselength in range(self.syncMinimumCycles, self.syncMaximumCycles):
					self.putSyncPulse()
					self.bytevalue = 0
					self.bytestartsample = self.newedgesample
					self.state = 'DATA-BIT-HIGH'
				else:
					self.putError()
					self.returnToIdle()
			elif self.state == 'DATA-BIT-HIGH':
				#now low, was high
				self.databitstart = self.lastedgesample

				self.state = 'DATA-BIT-LOW'
			elif self.state == 'DATA-BIT-LOW':
				#now high, was low
				if self.pulselength in range(self.shortMessageDataShortCyclesMinimum, self.shortMessageDataShortCyclesMaximum):
					#1
					self.databitend = self.newedgesample
					self.putOneBit()

					if self.dataBitCount == 5:
						self.putRemoteHasData()
						self.remoteHasData = True

					if self.dataBitCount == 9:
						self.putPlayerHasNoData()
					
					if self.dataBitCount == 13:
						self.putPlayerCedesBusToRemote()
						self.playerCedesBus = True
						if self.playerCedesBus:
							if not self.remoteHasData:
								self.putPlayerCededBusWithoutRemoteAsking()
							self.expectedBitCount = 115
						elif self.playerHasData and not self.playerCedesBus:
							self.expectedBitCount = 104

					
					if self.dataBitCount == self.expectedBitCount:
						self.packetendsample = self.newedgesample
						self.putPacketBitCount()
						self.putEndOfPacket()
						self.returnToIdle()
					else:
						self.state = 'DATA-BIT-HIGH'
				elif self.pulselength in range(self.shortMessageDataLongCyclesMinimum, self.shortMessageDataLongCyclesMaximum):
					#0
					self.databitend = self.newedgesample
					self.putZeroBit()

					if self.dataBitCount == 5:
						self.putRemoteHasNoData()

					if self.dataBitCount == 9:
						self.putPlayerHasData()
						self.playerHasData = True

					if self.dataBitCount == 13:
						self.putPlayerDoesNotCedeBusToRemote()
						if self.playerCedesBus:
							if not self.remoteHasData:
								self.putPlayerCededBusWithoutRemoteAsking()
							self.expectedBitCount = 115
						elif self.playerHasData and not self.playerCedesBus:
							self.expectedBitCount = 104
					
					if self.dataBitCount == self.expectedBitCount:
						self.packetendsample = self.newedgesample
						self.putPacketBitCount()
						self.putEndOfPacket()
						self.returnToIdle()
					else:
						self.state = 'DATA-BIT-HIGH'
				else:
					self.putError()
					self.returnToIdle()
			else:
				self.putStateError()
				self.returnToIdle()

