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
TMC is a Titan Micro Electronics communication protocol for driving chips based
on bidirectional two or three wire serial bus using 2 or 3 signals
(CLK = serial clock, DIO = data input/output, STB = strobe). Those chips drive
7-segment digital tubes with simple keyboards.
This protocole decoder is suitable for the chips TM1636, TMP1637, TMP1638, etc.
- TM1636 is compatible with TM1637 as for driving 7-segment digital tu. It
  drives up to 4 digital tubes and has no support for keyboard scanning.
- TM1637 drives up to 6 digital tubes and has support for keyboards, but they
  are practically not used on breaboards.
- TMP1638 drives up to 8 LED digital tubes, upt to 8 two-color LEDs (logically
  10 segment tubes), and has support for keyboard scanning. Those chips are
  usually used on breakout boards with full set of digital tubes, full set of
  LED at least unicolor, and with 8 key or 24 keys keyboards.

NOTE:
The TMC two-wire bus is not I²C (Inter-Integrated Circuit) bus because there
is no slave address and in contrast data bytes are transmitted with least
significant bit first.

"""

from .pd import Decoder
