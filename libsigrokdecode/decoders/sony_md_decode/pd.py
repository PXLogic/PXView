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

# Sony MD LCD Remote decoder

import sigrokdecode as srd

class SamplerateError(Exception):
    pass

class Decoder(srd.Decoder):
	api_version = 3
	id = 'sony_md_decode'
	name = 'Sony MD Remote Decode'
	longname = 'Sony MD LCD Remote Decoder'
	desc = ''
	license = 'unknown'
	inputs = ['sony_md']
	outputs = ['sony_md_decode']
	tags = ['']
	annotations = (
		('info', 'Info'),
		('transfer-block', 'Transfer block'),
		('raw-value', 'Raw Value'),
		('data-field-value-positive', 'Data Field Value (Positive)'),
		('debug', 'Debug'),
		('debug-two', 'Debug2'),
		('data-field-value-negative', 'Data Field Value (Negative)'),
		('sender-player', 'Transfer Block From Player'),
		('sender-remote', 'Transfer Block From Remote'),
		('data-field-name', 'Data Field Name'),
		('error', 'Error'),
		('warning', 'Warning'),
		('data-field-unused', 'Data Field (Unused)'),
		('data-field-unknown', 'Data Field (Unknown)'),
		('data-field-static', 'Data Field (Static)'),
		('command', 'Command'),
	)
	annotation_rows = (
		('informational', 'Informational', (0,)),
		('transfer-blocks', 'Data Transfer Blocks', (1,)),
		('senders', 'Block Sender', (7,8,)),
		('commands', 'Commands', (15,)),
		('raw-values', 'Raw Values', (2,)),
		('data-field-names', 'Data Field Names', (9,)),
		('data-field-values', 'Data Field Values', (3,6,12,13,14,)),
		('debugs', 'Debugs', (4,)),
		('debugs-two', 'Debugs 2', (5,)),
		('errors', 'Errors', (10,)),
		('warnings', 'Warnings', (11,)),
	)

	characters = {
		0x00: "<Unusued position>, 0x00",
		0x04: "<minidisc icon>, 0x04",
		0x06: "<group icon>, 0x06",
		0x0B: "<begin half-width katakana>, 0x0B",
		0x0C: "<end half-width katakana>, 0x0C",
		0x14: "<music note icon>, 0x14",

		0x20: "' ', space, 0x20",

		0xFF: "<End of string>, 0xFF",

	}
	
	def putMessageStart(self, messageStartSample):
		self.put(messageStartSample, messageStartSample, self.out_ann,
			[0, ['Message Start', 'S']])

	def putBinaryMSBFirst(self, bitData, startBit, numBits):
		currentBit = startBit
		bitsLeft = numBits
		valueStart = bitData[3][startBit][0]
		valueEnd = bitData[3][(startBit+numBits-1)][2]
		value = "0b"

		while bitsLeft > 0:
			value += str(bitData[3][currentBit][3])
			currentBit += 1
			bitsLeft -= 1
		
		self.put(valueStart, valueEnd, self.out_ann,
			[5, [value]])

	def putValueMSBFirst(self, bitData, startBit, numBits):
		currentBit = startBit
		bitsLeft = numBits
		valueStart = bitData[3][startBit][0]
		valueEnd = bitData[3][(startBit+numBits-1)][2]
		value = 0

		while bitsLeft > 0:
			value <<= 1
			value += bitData[3][currentBit][3]
			currentBit += 1
			bitsLeft -= 1

		self.checksum ^= value
		
		if numBits % 8 == 0:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value: 0x%02X' % value]])
			self.debugOutHex += ('0x%02X ' % value)
		elif numBits % 9 == 0:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value: 0o%03o' % value]])
			self.debugOutHex += ('0o%03o ' % value)
		else:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value (Low %d bits): 0x%X' % (numBits, value)]])
			self.debugOutHex += ('0x%X ' % value)
	
	def putValueLSBFirst(self, bitData, startBit, numBits):
		currentBit = startBit
		shiftBy = 0
		bitsLeft = numBits
		valueStart = bitData[3][startBit][0]
		valueEnd = bitData[3][(startBit+numBits-1)][2]
		value = 0

		while bitsLeft > 0:
			value += (bitData[3][currentBit][3] << shiftBy)
			shiftBy += 1
			currentBit += 1
			bitsLeft -= 1

		self.checksum ^= value
		self.values.append(value)
		
		if numBits % 8 == 0:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value: 0x%02X' % value]])
			self.debugOutHex += ('0x%02X ' % value)
		elif numBits % 9 == 0:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value: 0o%03o' % value]])
			self.debugOutHex += ('0o%03o ' % value)
		else:
			self.put(valueStart, valueEnd, self.out_ann,
				[2, ['Value (Low %d bits): 0x%X' % (numBits, value)]])
			self.debugOutHex += ('0x%X ' % value)
		
		return value

	def putStaticByte(self, bitData, currentBit, value, expectedValue):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[9, ['Static?']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[13, ['Static?']])
		if value != expectedValue:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[10, ['Previously static, expected 0x%02X!' % expectedValue]])
	
	def putUnusedByte(self, bitData, currentBit, value, expectedValue):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[9, ['Unused?']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[12, ['Unused?']])
		if value != expectedValue:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[10, ['Previously unused byte is not expected value!']])
		if value != 0x00:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[11, ['Unused byte has non-zero value!']])
	
	def putUnusedBits(self, bitData, currentBit, numBits, value, expectedValue):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
			[9, ['Unused?']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
			[12, ['Unused?']])
		if value != expectedValue:
			if numBits == 1:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
					[10, ['Previously unused bit is not expected value!']])
			else:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
					[10, ['Previously unused bits are not expected value!']])
		if value != 0x00:
			if numBits == 1:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
					[11, ['Unused bit has non-zero value!']])
			else:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+numBits-1][2], self.out_ann,
					[11, ['Unused bits have non-zero value!']])
	
	def putUnknownByte(self, bitData, currentBit, value):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[9, ['Unknown?']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[13, ['Unknown: 0x%02X' % value]])
		if value != 0x00:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[10, ['Unknown byte has non-zero value!']])

	def putRemoteHeader(self, bitData, currentBit):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[1, ['Header from remote']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[8, ['Remote', 'R']])
		self.putValueLSBFirst(bitData, currentBit, 8)

		self.putUnusedBits(bitData, currentBit, 1, (self.values[0] & 0x01), 0)

		if bitData[3][currentBit+1][3] == 1:
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+1][2], self.out_ann,
				[3, ['Remote is ready for text']])
		else:
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+1][2], self.out_ann,
				[6, ['Remote is NOT ready for text']])
		
		if bitData[3][currentBit+2][3] == 1:
			self.put(bitData[3][currentBit+2][0], bitData[3][currentBit+2][2], self.out_ann,
				[3, ['Remote is done scrolling text?']])
			self.put(bitData[3][currentBit+2][0], bitData[3][currentBit+2][2], self.out_ann,
				[9, ['Weird header, look here']])
		else:
			self.put(bitData[3][currentBit+2][0], bitData[3][currentBit+2][2], self.out_ann,
				[6, ['Remote is NOT done scrolling text?']])

		self.putUnusedBits(bitData, currentBit+3, 1, ((self.values[0] & 0x8) >> 3), 0)

		if bitData[3][currentBit+4][3] == 1:
			self.put(bitData[3][currentBit+4][0], bitData[3][currentBit+4][2], self.out_ann,
				[3, ['Remote HAS data to send', 'RY']])
		else:
			self.put(bitData[3][currentBit+4][0], bitData[3][currentBit+4][2], self.out_ann,
				[6, ['Remote has NO data to send', 'RN']])
		
		self.putUnusedBits(bitData, currentBit+5, 1, ((self.values[0] & 0x20) >> 5), 0)

		if bitData[3][currentBit+6][3] == 1:
			self.put(bitData[3][currentBit+6][0], bitData[3][currentBit+6][2], self.out_ann,
				[3, ['Remote IS Kanji-capable?']])
		else:
			self.put(bitData[3][currentBit+6][0], bitData[3][currentBit+6][2], self.out_ann,
				[6, ['Remote is NOT Kanji-capable?']])

		if bitData[3][currentBit+7][3] == 1:
			self.put(bitData[3][currentBit+7][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, ['Remote Present', 'RP']])
		else:
			self.put(bitData[3][currentBit+7][0], bitData[3][currentBit+7][2], self.out_ann,
				[6, ['Remote NOT Present', 'RNP']])
		
		#if (bitData[3][currentBit+7][3] == 1) and (bitData[3][currentBit+1][3] == 0):
			#self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				#[11, ['Remote present but not active!']])
	
	def putPlayerHeader(self, bitData, currentBit):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[1, ['Header from player']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
			[7, ['Player', 'P']])
		self.putValueLSBFirst(bitData, currentBit, 8)

		if bitData[3][currentBit][3] == 0:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit][2], self.out_ann,
				[3, ['Player HAS data to send', 'PY']])
		else:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit][2], self.out_ann,
				[6, ['Player has NO data to send', 'PN']])
		
		self.putUnusedBits(bitData, currentBit+1, 3, ((self.values[1] & 0xE) >> 1), 0)

		if bitData[3][currentBit+4][3] == 1:
			self.put(bitData[3][currentBit+4][0], bitData[3][currentBit+4][2], self.out_ann,
				[3, ['Player cedes the bus to remote after header', 'RDB']])
		else:
			self.put(bitData[3][currentBit+4][0], bitData[3][currentBit+4][2], self.out_ann,
				[6, ['Player does NOT cede the bus to remote after header', 'PDB']])
		
		self.putUnusedBits(bitData, currentBit+5, 2, ((self.values[1] & 0x60) >> 5), 0)

		self.put(bitData[3][currentBit+7][0], bitData[3][currentBit+7][2], self.out_ann,
			[3, ['Player Present']])
	
	def putLCDCharacter(self, bitData, currentBit, values, index):
		isFirstOfDouble = lambda x: x in range(0x81, 0x9f) or x in range(0xe0, 0xef)
		isSJISHalfKata = lambda x: x in range(0xa1, 0xdf)
		isPrintable = lambda x: x in range(0x20, 0x7e)

		twoByteStartIndices = []
		charIndex = 0
		while charIndex < len(values):
			if isFirstOfDouble(values[charIndex]):
				twoByteStartIndices.append(charIndex)
				charIndex += 1
			charIndex += 1

		value = values[index]
		nextValue = values[index + 1] if index < len(values) - 1 else None
		if value in self.characters: # self.characters takes priority
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, [self.characters[value]]])
			return
		if index - 1 in twoByteStartIndices: return # The correct character has already been displayed.
		if index in twoByteStartIndices:
			if nextValue is None:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
					[3, ['First byte of 2-byte SJIS sequence, see next message for remainder and decode.']])
				self.tempCarryoverShiftJISByte = value
			else:
				self.tempCarryoverShiftJISByte = 0
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
					[3, [bytes([value, nextValue]).decode('sjis')]])
		elif self.tempCarryoverShiftJISByte != 0:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, [bytes([self.tempCarryoverShiftJISByte, value]).decode('sjis')]])
			self.tempCarryoverShiftJISByte = 0
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[11, ['This is the second-half of a full-width SJIS, taking the first half from the previous message.']])
		elif isPrintable(value):
			self.tempCarryoverShiftJISByte = 0
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, [bytes([value]).decode('sjis')]])
		elif isSJISHalfKata(value):
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, ['SJIS half-width katakana - shouldn\'t be possible']])
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[11, ['Probably the second-half of a full-width SJIS, missed the previous message with the first half?']])
		else:
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[3, ['Unknown character']])
			self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
				[10, ['Unknown character']])

	def expandPlayerDataBlock(self, bitData, currentBit, packetType):
		currentByte = 2
		notDone = True

		while notDone:
			if currentByte >= 12:
				notDone = False
			elif self.values[currentByte] == 0x00:
				notDone = False
			else:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
					[9, ['Packet type']])
				if self.values[currentByte] == 0x01:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Request Remote Capabilities']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Request Remote capabilities']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Which block?']])
					if self.values[currentByte+1] == 0x01:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['First block']])
					elif self.values[currentByte+1] == 0x02:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Second block, LCD capabilities?']])
					elif self.values[currentByte+1] == 0x05:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Fifth block']])
					elif self.values[currentByte+1] == 0x06:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Sixth block?']])
					elif self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Unknown, seen from D-EJ955']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x02:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Unknown, seems to be two bytes sent soon after initialization?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, seems to be two bytes sent soon after initialization?']])

					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x80)

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x03:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+31][2], self.out_ann,
						[15, ['Scroll Control?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Scroll control?']])
					
					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x80)

					self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+31][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+31][2], self.out_ann,
						[9, ['Enable scrolling?']])
					if (self.values[currentByte+2] == 0x02) and (self.values[currentByte+3] == 0x80):
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+31][2], self.out_ann,
							[3, ['Scrolling: Enabled']])
					elif (self.values[currentByte+2] == 0x00) and (self.values[currentByte+3] == 0x00):
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+31][2], self.out_ann,
							[3, ['Scrolling: Disabled']])
					else:
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+31][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					currentBit += (4*8)
					currentByte += 4
				elif self.values[currentByte] == 0x05:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['LCD Backlight Control']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['LCD Backlight Control']])
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['LCD Backlight State']])

					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['LCD Backlight: Off']])
					elif self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['LCD Backlight: On']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x06:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+47][2], self.out_ann,
						[15, ['LCD Remote Service Mode?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['LCD Remote Service Mode Control?']])
					
					if self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['LCD Remote Service Mode End']])
					elif (self.values[currentByte+1] == 0x00) and (self.values[currentByte+2] == 0x06) and (self.values[currentByte+3] == 0x01) and (self.values[currentByte+4] == 0x03) and (self.values[currentByte+5] == 0x80):
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+47][2], self.out_ann,
							[11, ['Unsure']])
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+47][2], self.out_ann,
							[3, ['LCD Remote Service Mode All Segments On?']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					currentBit += (6*8)
					currentByte += 6
				elif self.values[currentByte] == 0x08:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+31][2], self.out_ann,
						[15, ['Unknown, seems to be sent before 0xC8 text updates?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, seems to be sent before 0xC8 text updates']])
					
					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x80)
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x07)
					self.putStaticByte(bitData, currentBit+24, self.values[currentByte+3], 0x80)
					
					currentBit += (4*8)
					currentByte += 4
				elif self.values[currentByte] == 0x09:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[15, ['Unsure, seems to be sent before 0xC8 text updates, but not always?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, seems to be sent before 0xC8 text updates, but not always sent']])
					
					currentBit += 8
					currentByte += 1
				elif self.values[currentByte] == 0x18:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[15, ['Unsure, seems to get a response from remote? Seen from D-EJ955']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unsure, seems to get a response from remote? Seen from D-EJ955']])
					
					currentBit += 8
					currentByte += 1
				elif self.values[currentByte] == 0x40:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Volume Level']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Volume Level']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Current Volume Level']])
					if self.values[currentByte+1] == 0xFF:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Volume Level: 32/32']])
					elif self.values[currentByte+1] < 32:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Volume Level: %d/32' % self.values[3]]])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x41:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Playback Mode']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Playback Mode']])
					
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Current Playback Mode']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: Normal']])
					elif self.values[currentByte+1] == 0x01:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: Repeat All Tracks']])
					elif self.values[currentByte+1] == 0x02:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: One Track, Stop Afterwards']])
					elif self.values[currentByte+1] == 0x03:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: Repeat One Track']])
					elif self.values[currentByte+1] == 0x04:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: Shuffle No Repeats']])
					elif self.values[currentByte+1] == 0x05:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: Shuffle With Repeats']])
					elif self.values[currentByte+1] == 0x06:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: PGM, No Repeats']])
					elif self.values[currentByte+1] == 0x07:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Current Playback Mode: PGM, Repeat']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x42:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Recording Indicator']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Recording Indicator']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Recording Indicator State']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Recording Indicator: Off']])
					elif self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Recording Indicator: On']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x43:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Battery Level Indicator']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Battery Level Indicator']])
						
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Battery Level Indicator State']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: Off']])
					elif self.values[currentByte+1] == 0x01:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: 1/4 bars, blinking']])
					elif self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: Charging']])
					elif self.values[currentByte+1] == 0x80:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: Empty, blinking']])
					elif self.values[currentByte+1] == 0x9F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: 1/4 bars']])
					elif self.values[currentByte+1] == 0xBF:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: 2/4 bars']])
					elif self.values[currentByte+1] == 0xDF:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: 3/4 bars']])
					elif self.values[currentByte+1] == 0xFF:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Battery Level Indicator: 4/4 bars']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x44:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Unknown, presumably an indicator control. Seen from D-EJ955.']])
					
					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x00)

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x46:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['EQ/Sound Indicator']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['EQ/Sound Indicator']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['EQ/Sound Indicator State']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['EQ/Sound Indicator: Normal']])
					elif self.values[currentByte+1] == 0x01:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[11, ['Unsure']])
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['EQ/Sound Indicator: Bass 1?']])
					elif self.values[currentByte+1] == 0x02:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[11, ['Unsure']])
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['EQ/Sound Indicator: Bass 2?']])
					elif self.values[currentByte+1] == 0x03:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['EQ/Sound Indicator: Sound 1']])
					elif self.values[currentByte+1] == 0x04:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['EQ/Sound Indicator: Sound 2']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x47:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+15][2], self.out_ann,
						[15, ['Alarm Indicator']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Alarm Indicator']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Alarm Indicator State']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Alarm Indicator: Off']])
					elif self.values[currentByte+1] == 0x7F:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Alarm Indicator: On']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (2*8)
					currentByte += 2
				elif self.values[currentByte] == 0x48:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[15, ['Unknown, happens near track changes?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, happens near track changes?']])

					currentBit += 8
					currentByte += 1
				elif self.values[currentByte] == 0x49:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[15, ['Unknown, happens 12 packets after a 0x46?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, happens 12 packets after a 0x46?']])

					currentBit += 8
					currentByte += 1
				elif self.values[currentByte] == 0x4A:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[15, ['Unknown, happens before 0xC8 text updates?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, happens before 0xC8 text updates?']])

					currentBit += 8
					currentByte += 1
				elif self.values[currentByte] == 0xA0:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+39][2], self.out_ann,
						[15, ['Track number']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Track number']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Track Number Indicator Enable']])
					if self.values[currentByte+1] == 0x00:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Track Number Indicator: On']])
					elif self.values[currentByte+1] == 0x80:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Track Number Indicator: Off']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x00)
					self.putStaticByte(bitData, currentBit+24, self.values[currentByte+3], 0x00)
					
					self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
						[9, ['Current Track Number']])
					self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
						[3, ['Current Track Number: %d' % self.values[currentByte+4]]])
					
					currentBit += (5*8)
					currentByte += 5
				elif self.values[currentByte] == 0xA1:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+39][2], self.out_ann,
						[15, ['LCD Disc Icon Control']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['LCD Disc Icon Control']])

					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x00)

					self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+23][2], self.out_ann,
						[9, ['LCD Disc Icon Outline']])
					if self.values[currentByte+2] == 0x00:
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+23][2], self.out_ann,
							[3, ['LCD Disc Icon Outline: Off']])
					elif self.values[currentByte+2] == 0x7F:
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+23][2], self.out_ann,
							[3, ['LCD Disc Icon Outline: On']])
					else:
						self.put(bitData[3][currentBit+16][0], bitData[3][currentBit+23][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					self.put(bitData[3][currentBit+24][0], bitData[3][currentBit+31][2], self.out_ann,
						[9, ['LCD Disc Icon Fill Segments Enable']])
					if self.values[currentByte+3] == 0x00:
						self.put(bitData[3][currentBit+24][0], bitData[3][currentBit+31][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segments: All disabled']])
					elif self.values[currentByte+3] == 0x7F:
						self.put(bitData[3][currentBit+24][0], bitData[3][currentBit+31][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segments: All enabled']])
					else:
						self.put(bitData[3][currentBit+24][0], bitData[3][currentBit+31][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
						[9, ['LCD Disc Icon Fill Segment Animation']])
					if self.values[currentByte+4] == 0x00:
						self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segment Animation: No animation, no segments displayed']])
					elif self.values[currentByte+4] == 0x01:
						self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segment Animation: "Fast Spinning" animation']])
					elif self.values[currentByte+4] == 0x03:
						self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segment Animation: "Spinning" animation']])
					elif self.values[currentByte+4] == 0x7F:
						self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
							[3, ['LCD Disc Icon Fill Segment Animation: No animation, all segments displayed']])
					else:
						self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])

					currentBit += (5*8)
					currentByte += 5
				elif self.values[currentByte] == 0xA2:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+39][2], self.out_ann,
						[15, ['Unknown, happens near track changes?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, happens near track changes?']])

					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x01)
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x01)
					self.putStaticByte(bitData, currentBit+24, self.values[currentByte+3], 0x7F)
					self.putStaticByte(bitData, currentBit+32, self.values[currentByte+4], 0x00)

					currentBit += (5*8)
					currentByte += 5
				elif self.values[currentByte] == 0xA3:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+39][2], self.out_ann,
						[15, ['Unknown, seen from D-EJ955']])
					
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, seen from D-EJ955']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])

					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x00)
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x00)
					self.putStaticByte(bitData, currentBit+24, self.values[currentByte+3], 0xFF)
					self.putStaticByte(bitData, currentBit+32, self.values[currentByte+4], 0xFF)

					currentBit += (5*8)
					currentByte += 5
				elif self.values[currentByte] == 0xA5:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+31][2], self.out_ann,
						[15, ['Unknown, happens after initialization?']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Unknown, happens after initialization?']])

					self.putStaticByte(bitData, currentBit+8, self.values[currentByte+1], 0x01)
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x76)
					self.putStaticByte(bitData, currentBit+24, self.values[currentByte+3], 0x81)

					currentBit += (4*8)
					currentByte += 4
				elif self.values[currentByte] == 0xC0:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+79][2], self.out_ann,
						[15, ['Player capabilities?']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['Player capabilities?']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Which block?']])
					if self.values[currentByte+1] == 0x05:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Fifth block?']])
						
						self.putUnknownByte(bitData, currentBit+16, self.values[currentByte+2])
						self.putUnknownByte(bitData, currentBit+24, self.values[currentByte+3])
						self.putUnknownByte(bitData, currentBit+32, self.values[currentByte+4])
						self.putUnknownByte(bitData, currentBit+40, self.values[currentByte+5])
						self.putUnknownByte(bitData, currentBit+48, self.values[currentByte+6])
						self.putUnknownByte(bitData, currentBit+56, self.values[currentByte+7])
						self.putUnknownByte(bitData, currentBit+64, self.values[currentByte+8])
						self.putUnknownByte(bitData, currentBit+72, self.values[currentByte+9])
					else:
						self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
					
					currentBit += (10*8)
					currentByte += 10
				elif self.values[currentByte] == 0xC8:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+79][2], self.out_ann,
						[15, ['LCD Text']])

					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[3, ['LCD Text']])

					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[11, ['Unsure']])
					self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
						[9, ['Which segment?']])
					if self.values[currentByte+1] == 0x02:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Non-final segment?']])
					elif self.values[currentByte+1] == 0x01:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[3, ['Final segment?']])
					else:
						self.put(bitData[3][currentBit+8][0], bitData[3][currentBit+15][2], self.out_ann,
							[10, ['UNRECOGNIZED VALUE']])
						
					self.putStaticByte(bitData, currentBit+16, self.values[currentByte+2], 0x00)

					splicedValues = self.values[(currentByte+3):(currentByte+10)]
					self.put(bitData[3][currentBit+24][0], bitData[3][currentBit+31][2], self.out_ann,
						[9, ['String position 1']])
					self.putLCDCharacter(bitData, currentBit+24, splicedValues, 0)
					self.put(bitData[3][currentBit+32][0], bitData[3][currentBit+39][2], self.out_ann,
						[9, ['String position 2']])
					self.putLCDCharacter(bitData, currentBit+32, splicedValues, 1)
					self.put(bitData[3][currentBit+40][0], bitData[3][currentBit+47][2], self.out_ann,
						[9, ['String position 3']])
					self.putLCDCharacter(bitData, currentBit+40, splicedValues, 2)
					self.put(bitData[3][currentBit+48][0], bitData[3][currentBit+55][2], self.out_ann,
						[9, ['String position 4']])
					self.putLCDCharacter(bitData, currentBit+48, splicedValues, 3)
					self.put(bitData[3][currentBit+56][0], bitData[3][currentBit+63][2], self.out_ann,
						[9, ['String position 5']])
					self.putLCDCharacter(bitData, currentBit+56, splicedValues, 4)
					self.put(bitData[3][currentBit+64][0], bitData[3][currentBit+71][2], self.out_ann,
						[9, ['String position 6']])
					self.putLCDCharacter(bitData, currentBit+64, splicedValues, 5)
					self.put(bitData[3][currentBit+72][0], bitData[3][currentBit+79][2], self.out_ann,
						[9, ['String position 7']])
					self.putLCDCharacter(bitData, currentBit+72, splicedValues, 6)

					currentBit += (11*8)
					currentByte += 11
				else:
					self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
						[10, ['UNRECOGNIZED VALUE']])
					currentBit += 8
					currentByte += 1
					notDone = False

		if currentBit < 96:
			self.put(bitData[3][currentBit][0], bitData[3][95][2], self.out_ann,
				[9, ['Segment not used by recognized message types']])
			self.put(bitData[3][currentBit][0], bitData[3][95][2], self.out_ann,
				[12, ['Segment not used by recognized message types']])
		
		while currentBit < 96:
			if self.values[currentByte] == 0x00:
				currentBit += 8
				currentByte += 1
			else:
				self.put(bitData[3][currentBit][0], bitData[3][currentBit+7][2], self.out_ann,
					[10, ['Unclaimed byte is nonzero!']])
				currentBit += 8
				currentByte += 1
	
	def putPlayerDataBlock(self, bitData, currentBit):
		#put up basic data about the message segment
		self.put(bitData[3][currentBit][0], bitData[3][(currentBit+87)][2], self.out_ann,
			[1, ['Player data block?']])
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+87][2], self.out_ann,
			[7, ['Player', 'P']])

		self.putValueLSBFirst(bitData, currentBit, 8)
		self.putValueLSBFirst(bitData, currentBit+8, 8)
		self.putValueLSBFirst(bitData, currentBit+16, 8)
		self.putValueLSBFirst(bitData, currentBit+24, 8)
		self.putValueLSBFirst(bitData, currentBit+32, 8)
		self.putValueLSBFirst(bitData, currentBit+40, 8)
		self.putValueLSBFirst(bitData, currentBit+48, 8)
		self.putValueLSBFirst(bitData, currentBit+56, 8)
		self.putValueLSBFirst(bitData, currentBit+64, 8)
		self.putValueLSBFirst(bitData, currentBit+72, 8)
		self.put(bitData[3][currentBit+80][0], bitData[3][currentBit+87][2], self.out_ann,
			[9, ['Checksum']])
		tempCalcedChecksum = self.checksum
		tempReceivedChecksum = self.putValueLSBFirst(bitData, currentBit+80, 8)
		if tempCalcedChecksum == tempReceivedChecksum:
			self.put(bitData[3][currentBit+80][0], bitData[3][currentBit+87][2], self.out_ann,
				[3, ['Checksum, calculated value 0x%02X, valid!' % tempCalcedChecksum]])
		else:
			self.put(bitData[3][currentBit+80][0], bitData[3][currentBit+87][2], self.out_ann,
				[6, ['Checksum, calculated value 0x%02X, invalid!' % tempCalcedChecksum]])

		self.expandPlayerDataBlock(bitData, currentBit, self.values[2])
	
	def putRemoteDataBlockTransfer(self, bitData, currentBit):
		self.put(bitData[3][currentBit][0], bitData[3][currentBit][2], self.out_ann,
			[7, ['Player', 'P']])
		self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
			[8, ['Remote', 'R']])
		return self.putValueLSBFirst(bitData, currentBit+1, 8)

	def expandRemoteDataBlock(self, bitData, currentBit, packetType):
		self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
			[9, ['Packet type?']])
		
		if packetType == 0x83:
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
				[11, ['Unsure']])
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
				[3, ['Serial number?']])

			self.putUnknownByte(bitData, currentBit+10, self.values[3])
			self.putUnknownByte(bitData, currentBit+19, self.values[4])
			self.putUnknownByte(bitData, currentBit+28, self.values[5])
			self.putUnknownByte(bitData, currentBit+37, self.values[6])

			currentBit += 45
		elif packetType == 0xC0:
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
				[3, ['Remote capabilities']])

			self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
				[11, ['Unsure']])
			self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
				[9, ['Which block?']])
			if self.values[3] == 0x01:
				self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
					[3, ['First block, LCD capabilities?']])
				
				self.putUnknownByte(bitData, currentBit+19, self.values[4])
				self.putUnknownByte(bitData, currentBit+28, self.values[5])
				self.putUnknownByte(bitData, currentBit+37, self.values[6])
				self.putUnknownByte(bitData, currentBit+46, self.values[7])
				self.putUnknownByte(bitData, currentBit+55, self.values[8])

				self.put(bitData[3][currentBit+64][0], bitData[3][currentBit+71][2], self.out_ann,
					[11, ['Unsure']])
				self.put(bitData[3][currentBit+64][0], bitData[3][currentBit+71][2], self.out_ann,
					[9, ['Pixels tall?']])
				self.put(bitData[3][currentBit+64][0], bitData[3][currentBit+71][2], self.out_ann,
					[3, ['Pixels tall: %d' % self.values[9]]])
				self.put(bitData[3][currentBit+73][0], bitData[3][currentBit+80][2], self.out_ann,
					[11, ['Unsure']])
				self.put(bitData[3][currentBit+73][0], bitData[3][currentBit+80][2], self.out_ann,
					[9, ['Pixels wide?']])
				self.put(bitData[3][currentBit+73][0], bitData[3][currentBit+80][2], self.out_ann,
					[3, ['Pixels wide: %d' % self.values[10]]])
				self.put(bitData[3][currentBit+82][0], bitData[3][currentBit+89][2], self.out_ann,
					[11, ['Unsure']])
				self.put(bitData[3][currentBit+82][0], bitData[3][currentBit+89][2], self.out_ann,
					[9, ['Character sets supported?']])
				currentBit += 90
			elif self.values[3] == 0x02:
				self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
					[3, ['Second block?']])

				self.put(bitData[3][currentBit+19][0], bitData[3][currentBit+26][2], self.out_ann,
					[11, ['Unsure']])
				self.put(bitData[3][currentBit+19][0], bitData[3][currentBit+26][2], self.out_ann,
					[9, ['Characters displayed?']])
				self.put(bitData[3][currentBit+19][0], bitData[3][currentBit+26][2], self.out_ann,
					[3, ['Characters displayed: %d' % self.values[4]]])

				self.putUnknownByte(bitData, currentBit+28, self.values[5])
				self.putUnknownByte(bitData, currentBit+37, self.values[6])
				self.putUnknownByte(bitData, currentBit+46, self.values[7])
				self.putUnknownByte(bitData, currentBit+55, self.values[8])
				self.putUnknownByte(bitData, currentBit+64, self.values[9])
				self.putUnknownByte(bitData, currentBit+73, self.values[10])
				self.putUnknownByte(bitData, currentBit+82, self.values[11])

				currentBit += 90
			elif self.values[3] == 0x05:
				self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
					[3, ['Fifth block?']])
				
				self.putUnknownByte(bitData, currentBit+19, self.values[4])
				self.putUnknownByte(bitData, currentBit+28, self.values[5])
				self.putUnknownByte(bitData, currentBit+37, self.values[6])
				self.putUnknownByte(bitData, currentBit+46, self.values[7])
				self.putUnknownByte(bitData, currentBit+55, self.values[8])
				self.putUnknownByte(bitData, currentBit+64, self.values[9])
				self.putUnknownByte(bitData, currentBit+73, self.values[10])
				self.putUnknownByte(bitData, currentBit+82, self.values[11])

				currentBit += 90
			else:
				self.put(bitData[3][currentBit+10][0], bitData[3][currentBit+17][2], self.out_ann,
					[10, ['UNRECOGNIZED VALUE']])
		else:
			self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
					[10, ['UNRECOGNIZED VALUE']])
			currentBit += 9
		
		if currentBit < 106:
			self.put(bitData[3][currentBit][0], bitData[3][105][2], self.out_ann,
				[9, ['Segment not used by recognized message types']])
			self.put(bitData[3][currentBit][0], bitData[3][105][2], self.out_ann,
				[12, ['Segment not used by recognized message types']])
		
		while currentBit < 106:
			if self.values[int(2+((currentBit-16)/9))] == 0x00:
				currentBit += 9
			else:
				self.put(bitData[3][currentBit+1][0], bitData[3][currentBit+8][2], self.out_ann,
					[10, ['Unclaimed byte is nonzero!']])
				currentBit += 9
	
	def putRemoteDataBlock(self, bitData, currentBit):
		#put up basic data about the transfer
		self.put(bitData[3][currentBit][0], bitData[3][currentBit+98][2], self.out_ann,
			[1, ['Remote Data Block (With timing bits from Player)']])
		self.putRemoteDataBlockTransfer(bitData, currentBit)
		self.putRemoteDataBlockTransfer(bitData, currentBit+9)
		self.putRemoteDataBlockTransfer(bitData, currentBit+18)
		self.putRemoteDataBlockTransfer(bitData, currentBit+27)
		self.putRemoteDataBlockTransfer(bitData, currentBit+36)
		self.putRemoteDataBlockTransfer(bitData, currentBit+45)
		self.putRemoteDataBlockTransfer(bitData, currentBit+54)
		self.putRemoteDataBlockTransfer(bitData, currentBit+63)
		self.putRemoteDataBlockTransfer(bitData, currentBit+72)
		self.putRemoteDataBlockTransfer(bitData, currentBit+81)
		self.put(bitData[3][currentBit+91][0], bitData[3][currentBit+98][2], self.out_ann,
			[9, ['Checksum']])
		tempCalcedChecksum = self.checksum
		tempReceivedChecksum = self.putRemoteDataBlockTransfer(bitData, currentBit+90)
		if tempCalcedChecksum == tempReceivedChecksum:
			self.put(bitData[3][currentBit+91][0], bitData[3][currentBit+98][2], self.out_ann,
				[3, ['Checksum, calculated value 0x%02X, valid!' % tempCalcedChecksum]])
		else:
			self.put(bitData[3][currentBit+91][0], bitData[3][currentBit+98][2], self.out_ann,
				[6, ['Checksum, calculated value 0x%02X, invalid!' % tempCalcedChecksum]])

		self.expandRemoteDataBlock(bitData, currentBit, self.values[2])


	def expandMessage(self, bitData):
		currentBit = 0

		self.putBinaryMSBFirst(bitData, 0, bitData[2])

		self.debugOutHex += str(bitData[2])
		self.debugOutHex += "   "

		self.putRemoteHeader(bitData, currentBit)
		currentBit += 8

		self.debugOutHex += "   "

		self.putPlayerHeader(bitData, currentBit)
		currentBit += 8

		self.debugOutHex += "   "
		self.checksum = 0

		if (bitData[3][8][3] == 0) and (bitData[3][12][3] == 0):
			self.putPlayerDataBlock(bitData, currentBit)
			currentBit += 88
		elif (bitData[3][12][3] == 1):
			if (bitData[3][4][3] == 0):
				self.put(bitData[3][12][0], bitData[3][12][2], self.out_ann,
					[10, ['Player ceded bus to Remote without Remote asking!']])
			self.putRemoteDataBlock(bitData, currentBit)
			currentBit += 99

		self.put(bitData[0], bitData[1], self.out_ann,
				[4, [self.debugOutHex]])
		self.debugOutHex = ""
		self.values = []

	def putMessageEnd(self, messageEndSample):
		self.put(messageEndSample, messageEndSample, self.out_ann,
			[0, ['Message End', 'E']])
	
	def reset(self):
		self.state = 'IDLE'

		self.values = []

		self.checksum = 0

		self.tempCarryoverShiftJISByte = 0

		self.debugOutHex = ""
		self.debugOutBinary = ""

	def __init__(self):
		self.reset()
	
	def start(self):
		#self.out_python = self.register(srd.OUTPUT_PYTHON)
		self.out_ann = self.register(srd.OUTPUT_ANN)
	
	def decode(self, startsample, endsample, data):
		syncData, bitData, cleanEnd = data
		
		startOfBits = bitData[0]
		endOfBits = bitData[1]
		numberOfBits = bitData[2]
		
		self.putMessageStart(startOfBits)
		#for index, dataBit in enumerate(byteData):
			#self.putDataByte(dataByte)
		self.expandMessage(bitData)
		self.putMessageEnd(endOfBits)
				
