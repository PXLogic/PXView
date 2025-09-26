## SENT SAE J2716 decoder
##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2014/2024 Volker Oth [0xdeadbeef]
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 3 of the License, or
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

'''
SENT (Single Edge Nibble Transmission) is a unidirectional single line protocol defined in the SAE J2716.
It's used in automotive applications, e.g. to deliver pressure or position values from a sensor to the engine control unit.

The nibble data is encoded in pulse lengths (falling edge to falling edge) with a calibration pulse starting/ending each message.
As the current clock period is derived from the calibration pulse, no precise clock is needed and clock drift is tolerated to some degree.
The typical clock period is 3us and since a calibration pulse has 56 clock ticks by definition, it's typically 168us long.
The pulses encoding nibbles vary from 12 clock ticks (0) to 27 clock ticks (15), i.e between 36us and 81us with 3us clock period.

Each message is protected by a four bit CRC (no normal CRC though due to different handling of start value, data and lack of padding zeroes).
The CRC algorithms for "recommended" mode is the same as for "legacy" mode apart from one additional access to the CRC table at the end.

A SENT message typically consists of 6 data nibbles and 8 nibbles in sum between two calibration pulses.
The data nibbles are always preceeded by a "status and communication nibble" and followed by the CRC nibble.
Optionally, a pause pulse can be used between the CRC nibble and calibration pulse, e.g. to create a constant message length.

The "status and communication nibble" can be used to embed a serial ("slow") message into a group of normal ("fast") SENT messages.
In the "short" message format, 16 consecutive messages are used to transmit three serial data nibbles and a 4bit CRC.
In the "enhanced" format, 18 consecutive messages are used to transmit five serial data nibbles and a 6bit CRC.
In the enhanced format, there is also a "configuration" bit transmitted that defines how the 5 data nibbles are distributed between data and ID.

Details
https://en.wikipedia.org/wiki/SENT_(protocol)
https://www.sae.org/standards/content/j2716_201604
'''

from .pd import Decoder