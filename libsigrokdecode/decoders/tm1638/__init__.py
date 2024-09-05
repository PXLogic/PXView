##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2019 Libor Gabaj <libor.gabaj@gmail.com>
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## Permission is hereby granted, free of charge, to any person obtaining a copy
## of this software and associated documentation files (the "Software"), to deal
## in the Software without restriction, including without limitation the rights
## to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
## copies of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:
##
## The above copyright notice and this permission notice shall be included in all
## copies or substantial portions of the Software.
##
## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
## IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
## FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
## AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
## LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
## OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.


"""
DECODER:
This decoder stacks on top of the ``tmc`` protocol decoder and decodes
the display driver TM1638 controlling up to 8 pcs of 7-segment digits with
decimal points, up to 8 two-color LEDs, and up to 24 keys scanning.

NOTE:
The decoder has been tested on display modules with following configurations:
- 8 digital tubes
- 8 digital tubes, 8 red LEDs, 8 keys
- 8 digital tubes, 8 red&green LEDs, 8 keys

All those modules have digital tubes connected to even display addresses
(counting from 0), i.e., 0x00, 0x02, ..., 0x0E, and LED to odd addresses, i.e.,
0x01, 0x03, ..., 0x0F.

Bit 0 (register value 0x01) of an LED address turns on a red LED.
It is equivalent to segment "a" of a digital tube.
Bit 1 (register value 0x02) of an LED address turns on a green LED.
It is equivalent to segment "b" of a digital tube.
Both colors of an LED cannot be turned on at once just the last of them.

Key scan bits K1 ~ K3 and nibbles KS1 ~ KS8 are mapped to physical switches
S1 - S24. Only a modul with 8 keys has been tested.

"""

from .pd import Decoder
