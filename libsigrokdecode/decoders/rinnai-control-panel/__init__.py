##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
## Copyright (C) 2017 Kevin Redon <kingkevin@cuvoodoo.info>
## Copyright (C) 2020 Arik Yavilevich <arik_sigrok@yavilevich.com>
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
This Sigrok Protocol Decoder (PD) handles the internal protocol of a Rinnai gas heater control panel.

This PD is based on onewire_link PD.

The protocol is digital and has 0 and 1 encoded with variable pulse length encoding.

Each packet has a sync followed by 48 bits. This forms 6 bytes lsb first.
The first 5 bytes are data and the last is a xor of the first 5.
'''

from .pd import Decoder
