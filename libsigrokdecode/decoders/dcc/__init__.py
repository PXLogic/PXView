##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2013-2020 Sven Bursch-Osewold
##               2020      Roland Noell  
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
## along with this program; if not, write to the Free Software
## Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
##

'''
A decoder for DCC-Signals (operate model railways digitally):
- Decoding of individual packets
- Search functions for
-- accessory address
-- decoder address
-- CV
-- single byte ('and' linked if address or CV filled)
- 'ignore pulse <= 4 µs':
   Short pulses are ignored
   (what would the signal look like without the short pulse?)
- No decoding of packet sequences (e.g. programming mode)
- No evaluation of the preamble length for packet detection
- Rudimentary decoding of register and page mode packets
- RailComPlus® system commands not decoded (as not documented)

Used norms:
RCN 210, 211, 212, 213, 214, 216, 217
(NMRA S-9.2, S-9.2.1, S-9.2.3, S-9.3.2)
http://www.vhdm.de
https://www.nmra.org

RailCom®(Lenz Elektronik GmbH,Gießen)
RailComPlus®(Lenz Elektronik GmbH,Gießen, ESU electronic solutions,Ulm)
'''

from .pd import Decoder

