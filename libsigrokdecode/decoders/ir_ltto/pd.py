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
	[<synclength>, <bitcount>, <data>]
	
	<synclength> is either 'SHORT' or 'LONG'.
	<bitcount> is an int that counts the bits of the signature(You should only ever see 5, 7, 8, or 9 here).
	<data> is the data from the signature, as an int.
	
'''

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
	api_version = 3
	id = 'ir_ltto'
	name = 'IR LTTO'
	longname = 'LTTO laser tag IR'
	desc = 'A decoder for the LTTO laser tag IR protocol'
	license = 'unknown'
	inputs = ['logic']
	outputs = ['ir_ltto']
	tags = ['Embedded/industrial']
	channels = (
		{'id': 'ir', 'name': 'IR', 'desc': 'Demodulated IR'},
	)
	options = (
		{'id': 'polarity', 'desc': 'Polarity', 'default': 'active-low',
			'values': ('active-low', 'active-high')},
	)
	annotations = (
		('pre-sync', 'PRE-SYNC'),
		('pre-sync-pause', 'PRE-SYNC PAUSE'),
		('sync', 'SYNC'),
		('long-sync', 'LONG-SYNC'),
		('bit-pause', 'Bit Pause'),
		('bit', 'Bit'),
		('signature', 'Signature'),
		('long-sync-signature', 'Long SYNC Signature'),
		('signature-error', 'Error'),
	)
	annotation_rows = (
		('bits', 'Bits', (0, 1, 2, 3, 4, 5,)),
		('signatures', 'Signatures', (6, 7, 8,)),
	)
	
	def putpresync(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[0, ['PRE-SYNC Pulse', 'PRE-SYNC', 'PS']])
	
	def putpresyncpause(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[1, ['PRE-SYNC Pause', 'PRE-SYNC P', 'PSP']])
	
	def putsync(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[2, ['SYNC Pulse', 'SYNC P', 'SP']])
				
	def putlongsync(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[3, ['Long SYNC Pulse', 'Long SYNC P', 'LSP']])
	
	def putbitpause(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[4, ['Bit Pause', 'Bit P', 'BP']])
	
	def putbit(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[5, ['%d' % self.lastbit]])
	
	def putsignature(self):
		self.put(self.packetstartsample, self.oldedgesample, self.out_python,
				['SHORT', self.count, self.data])
		self.put(self.packetstartsample, self.oldedgesample, self.out_ann,
				[6, ['Signature, %d bits: 0x%03X' % (self.count, self.data), 'Sig, %d: 0x%03X' % (self.count, self.data), 'S %d: 0x%03X' % (self.count, self.data)]])
	
	def putlongsyncsignature(self):
		self.put(self.packetstartsample, self.oldedgesample, self.out_python,
				['LONG', self.count, self.data])
		self.put(self.packetstartsample, self.oldedgesample, self.out_ann,
				[7, ['Signature, long SYNC, %d bits: 0x%03X' % (self.count, self.data), 'Sig, LS, %d: 0x%03X' % (self.count, self.data), 'S LS %d: 0x%03X' % (self.count, self.data)]])
	
	def putsignatureerror(self):
		self.put(self.packetstartsample, self.oldedgesample, self.out_ann,
				[8, ['Error', 'Err', 'E']])
	
	def __init__(self):
		self.state = 'IDLE'
		self.lastbit = None
		self.newedgesample = self.oldedgesample = self.data = self.count = self.waslongsync = 0
		
	def reset(self):
		self.__init__(self)
	
	def start(self):
		self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
		self.activeState = 0 if self.options['polarity'] == 'active-low' else 1
		self.ir = 1
	
	def metadata(self, key, value):
		if key == srd.SRD_CONF_SAMPLERATE:
			self.samplerate = value
			self.margin = int(self.samplerate * 0.0005) - 1 # 0.5ms
			
			self.presync = int(self.samplerate * 0.003) - 1 # 3ms
			self.presyncpause = int(self.samplerate * 0.006) - 1 #6ms
			self.sync = int(self.samplerate * 0.003) - 1 #3ms
			self.longsync = int(self.samplerate * 0.006) - 1 #6ms
			
			self.bitpause = int(self.samplerate * 0.002) - 1 #2ms
			self.dazero = int(self.samplerate * 0.001) - 1 # 1ms
			self.daone = int(self.samplerate * 0.002) - 1 # 2ms
	
	def handle_bit(self, tick):
		self.lastbit = None
		#if tick in range(self.dazero - self.margin, self.dazero + self.margin):
		#	self.lastbit = 0
		#elif tick in range(self.daone - self.margin, self.daone + self.margin):
		#	self.lastbit = 1
		
		#print('Processing bit of', tick, 'length.')
		
		if tick in range(self.dazero - self.margin, self.dazero + self.margin):
			self.lastbit = 0
		elif tick in range(self.daone - self.margin, self.daone + self.margin):
			self.lastbit = 1
		
		if self.lastbit in (0, 1):
			self.putbit()
			self.data = (self.data << 1)
			self.data |= self.lastbit
			#self.data = self.data + str(self.lastbit)
			self.count = self.count + 1
		
	
	def decode(self):
		if not self.samplerate:
			raise SamplerateError('Cannot decode without samplerate.')
		
		while True:
			#Save the most recent edge sample for the next length
			self.oldedgesample = self.newedgesample
			#Save the old pin state, too
			self.oldpinstate = self.ir
			
			#Wait for any edge
			if self.state == 'BIT' or self.state == 'BITPAUSE':
				(self.ir,) = self.wait([{0: 'e'}, {'skip': self.bitpause + self.margin + self.margin}])
			else:
				(self.ir,) = self.wait({0: 'e'})
			#self.log(5, 'Found edge at sample ' + self.samplenum)
			#print('Found edge at', self.samplenum, '!')
			
			#Save the new edge
			self.newedgesample = self.samplenum
			
			length = self.newedgesample - self.oldedgesample
			#print('Length since last edge', length, '.')
			
			# Decoder state machine
			if self.state == 'IDLE':
				#Looking for a PRE-SYNC Pulse
				if length in range(self.presync - self.margin, self.presync + self.margin) and self.oldpinstate == self.activeState:
					self.putpresync()
					self.data = 0
					self.count = 0
					self.waslongsync = 0
					self.packetstartsample = self.oldedgesample
					self.state = 'PSP'
					continue
			elif self.state == 'PSP':
				#Looking for a PRE-SYNC Pause
				if length in range(self.presyncpause - self.margin, self.presyncpause + self.margin):
					self.putpresyncpause()
					self.state = 'SYNC'
					continue
				else:
					#self.putstop()
					self.putsignatureerror()
					self.state = 'IDLE'
					continue
			elif self.state == 'SYNC':
				#Looking for a SYNC pulse
				if length in range(self.sync - self.margin, self.sync + self.margin):
					self.putsync()
					self.state = 'BITPAUSE'
					continue
				elif length in range(self.longsync - self.margin, self.longsync + self.margin):
					self.putlongsync()
					self.waslongsync = 1
					self.state = 'BITPAUSE'
					continue
				else:
					self.putsignatureerror()
					self.state = 'IDLE'
					continue
			elif self.state == 'BITPAUSE':
				if length in range(self.bitpause - self.margin, self.bitpause + self.margin):
					self.putbitpause()
					self.state = 'BIT'
					continue
				else:
					if self.count == 0:
						self.putsignatureerror()
					else:
						if self.waslongsync == 0:
							self.putsignature()
						else:
							self.putlongsyncsignature()
					self.state = 'IDLE'
					continue
			elif self.state == 'BIT':
				self.handle_bit(length)
				self.state = 'BITPAUSE'
				if self.lastbit == None:
					self.putsignatureerror()
					self.state = 'IDLE'
					continue