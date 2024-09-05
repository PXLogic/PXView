##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2022 Sergey Spivak <sespivak@yandex.ru>
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

'''
Protocol decoder for Streletz Guard system on top of UART sigrok decoder.

UART communication typically runs at 57.6kbps with 8n1 frame format.

Streletz messages starts with header followed by datasize byte.
At the end of packet checksum byte transmitted.
Xor operation on whole packet must be equal to 0.
Maximum packet length is 64 bytes.
'''

from .pd import Decoder
