##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
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

'''
High-Level Data Link Control (HDLC) is a bit-oriented code-transparent 
synchronous data link layer protocol developed by the 
International Organization for Standardization (ISO). 
The standard for HDLC is ISO/IEC 13239:2002.
If EN is not supplied, data is decoded on every clock transition (depending on clock mode).
'''

from .pd import Decoder
