##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2021 Guy Colin <guy.colin@gmail.com>
## This DS18B20 decoder is made from DS28EA00 decoder
## Original DS28EA00 made by Iztok Jeras
## Copyright (C) 2012 Iztok Jeras <iztok.jeras@gmail.com>
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
This decoder stacks on top of the 'onewire_network' PD and decodes the
Maxim DS18B20 1-Wire digital thermometer protocol.
'''

from .pd import Decoder
