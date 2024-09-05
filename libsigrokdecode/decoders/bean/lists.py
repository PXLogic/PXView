##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2014 Uwe Hermann <uwe@hermann-uwe.de>
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

# Systems/addresses (0..31). Items that are not listed are reserved/unknown.
command = {
    'ABA180'      :	'OK - Power Window Master SW',
    'AB4080'      :	'OK - ??? 1 ??? ',
    'ABAE0'      :	'OK - ??? 2 ??? ',
    'ABE00'      :	'OK - ??? 3 ??? ',
    'ABA80'      :	'OK - ??? 4 ??? ',
    'ABAB0'      :	'OK - ??? 5 ??? ',
    'DB01'        :	'UnBlock Control',
    'DB021'       :	'Block Control',
    'DB4C81'      :	'Door Lock',
    'DB4C41'      :	'Door UnLock',
    'E000400'     : 'Window Rear Left - Down',
    'E000600'     : 'Window Rear Left - Full Down',
    'E000800'     : 'Window Rear Left - Up',
    'E000A00'     : 'Window Rear Left - Full Up',
    'E004000'     : 'Window Rear Right - Down',
    'E006000'     : 'Window Rear Right - Full Down',
    'E008000'     : 'Window Rear Right - Up',
    'E00A000'     : 'Window Rear Right - Full Up',
    'E040000'     : 'Window Front Right - Down',
    'E060000'     : 'Window Front Right - Full Down',
    'E080000'     : 'Window Front Right - Up',
    'E0A0000'     : 'Window Front Right - Full Up',
}

digits = {
    0: ['0', '0'],
    1: ['1', '1'],
    2: ['2', '2'],
    3: ['3', '3'],
    4: ['4', '4'],
    5: ['5', '5'],
    6: ['6', '6'],
    7: ['7', '7'],
    8: ['8', '8'],
    9: ['9', '9'],
}
