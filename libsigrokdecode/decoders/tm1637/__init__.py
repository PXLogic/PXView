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
the display driver TM1637 controlling up to 6 pcs of 7-segment digital tubes
with decimal points or one or two colons.
The decode is compatible with TM1636 driver. However, it supports just 4 tubes
and does not have the keyboard scanning interface implemented.

NOTE:
The decoder has been tested on display modules with following configurations:
- 4 digital tubes with active decimal points for each tube without active colon
- 4 digital tubes with active colon with inactive decimal points

Althoug the driver TM1637 has keyboard scanning capability, it is not utilized
on display modules, which have no switches mounted.

"""

from .pd import Decoder
