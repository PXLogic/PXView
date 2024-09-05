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

class Decoder(srd.Decoder):
	api_version = 3
	id = 'ir_recoil'
	name = 'IR Recoil'
	longname = 'Recoil laser tag IR'
	desc = 'A decoder for the Recoil laser tag IR protocol'
	license = 'unknown'
	inputs = ['logic']
	outputs = ['ir_recoil']
	tags = ['Embedded/industrial']
	channels = (
		{'id': 'ir', 'name': 'IR', 'desc': 'Demodulated IR'},
	)
	options = (
		{'id': 'polarity', 'desc': 'Polarity', 'default': 'active-low',
			'values': ('active-low', 'active-high')},
	)
	annotations = (
		('sync', 'SYNC'),
		('sync-pause', 'SYNC PAUSE'),
		('bit', 'Bit'),
		('packet', 'Packet'),
	)
	annotation_rows = (
		('bits', 'Bits', (0, 1, 2)),
		('packets', 'Packet', (3,)),
	)
	
	def putsync(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[0, ['SYNC Pulse', 'SYNC', 'S']])
	
	def putsyncpause(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[1, ['SYNC Pause', 'SYNC P', 'SP']])
	
	def putbit(self):
		self.put(self.oldedgesample, self.newedgesample, self.out_ann,
				[2, ['%d' % self.lastbit]])
	
	def putpacket(self):
		self.put(self.packetstartsample, self.oldedgesample + 1, self.out_ann,
				[3, ['Packet, %d bits: 0b%s' % (self.count, self.data), 'Pack, %d: 0b%s' % (self.count, self.data), 'P %d: 0b%s' % (self.count, self.data)]])
	
	def __init__(self):
		self.state = 'IDLE'
		self.lastbit = None
		self.newedgesample = self.oldedgesample = self.data = self.count = length = 0
	
	def reset(self):
		self.__init__(self)
		
	def start(self):
		self.out_ann = self.register(srd.OUTPUT_ANN)
		self.activeState = 0 if self.options['polarity'] == 'active-low' else 1
		self.ir = 1
	
	def metadata(self, key, value):
		if key == srd.SRD_CONF_SAMPLERATE:
			self.samplerate = value
			self.margin = int(self.samplerate * 0.0002) - 1 # 0.2ms
			self.sync = int(self.samplerate * 0.0033) - 1 # 3.3ms
			self.syncpause = int(self.samplerate * 0.0015) - 1 #1.5ms
			self.dazero = int(self.samplerate * 0.0004) - 1 # 0.4ms
			self.daone = int(self.samplerate * 0.0008) - 1 # 0.8ms
			self.dathreshold = int(self.samplerate * 0.00059) - 1 #0.6ms
			self.daminimum = int(self.samplerate * 0.0002) - 1 #0.2ms
			self.damaximum = int(self.samplerate * 0.0012) - 1 #1.2ms
	
	def handle_bit(self, tick):
		self.lastbit = None
		#if tick in range(self.dazero - self.margin, self.dazero + self.margin):
		#	self.lastbit = 0
		#elif tick in range(self.daone - self.margin, self.daone + self.margin):
		#	self.lastbit = 1
		
		#print('Processing bit of', tick, 'length.')
		
		if tick in range(self.daminimum, self.dathreshold):
			self.lastbit = 0
		elif tick in range(self.dathreshold, self.damaximum):
			self.lastbit = 1
		
		if self.lastbit in (0, 1):
			self.putbit()
			#self.data = (self.data << 1)
			#self.data |= self.lastbit
			self.data = self.data + str(self.lastbit)
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
			if self.state == 'DATA':
				(self.ir,) = self.wait([{0: 'e'}, {'skip': self.damaximum + self.margin}])
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
				#Looking for a SYNC pulse
				if length in range(self.sync - self.margin, self.sync + self.margin) and self.oldpinstate == self.activeState:
					self.putsync()
					self.data = ''
					self.count = 0
					self.packetstartsample = self.oldedgesample
					self.state = 'SYNCING'
					continue
			elif self.state == 'SYNCING':
				#Looking for a SYNC Pause
				if length in range(self.syncpause - self.margin, self.syncpause + self.margin):
					self.putsyncpause()
					self.state = 'DATA'
					continue
				else:
					#self.putstop()
					self.putpacket()
					self.state = 'IDLE'
					continue
			elif self.state == 'DATA':
				#looking for data bits or a long break
				self.handle_bit(length)
				if self.lastbit == None:
					#Data bit error, call this packet done, since I don't yet know how else to tell...
					#self.putstop()
					self.putpacket()
					self.state = 'IDLE'
					continue
				continue
			