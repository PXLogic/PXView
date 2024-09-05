##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2018 Juan Carlos Galvez Villegas.
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
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
X10 Radio Frequency signal decoder.

In X10 RF protocol there is a preamble comprising a ~9 ms UP time and a ~4.5 ms
DOWN time. A Zero is a total time between rising edges of ~1.2 ms and a One is a
total time between rising edges of ~2.4 ms.

X10 RF bits for each byte are transmitted low significant bit first (from bit0 to
bit7. Bit reversing is necessary.

Once bits are reversed, from left to right, byte 4 is house code, byte 3 is house
code inverted, byte 2 is unit code and byte 1 is unit code reversed.

To evaluate unit and command, byte 1 becomes byte1 and byte 3 becomes byte2.

Only ON, OFF, DIM and BRIGHT actions are recognized. No extended codes are recognized.
'''

from .pd import Decoder
