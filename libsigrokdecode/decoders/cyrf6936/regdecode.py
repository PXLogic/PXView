##
## Copyright (C) 2016 Soenke J. Peters
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program. If not, see <http://www.gnu.org/licenses/>.

class RegDecode():
    regs = {}
    regnames = {}
    decoderfuncs = {}

    def __init__(self, regs = {}):
        RegDecode.defs(regs)

    def defs(regs):
        RegDecode.regs = regs
        regnames = {}
        for reg, name in RegDecode.regs.items():
            regnames[name] = reg
        RegDecode.regnames = regnames

    def name(r):
        if r in RegDecode.regs:
            return RegDecode.regs[r][0]
        elif r in RegDecode.regnames:
            return r
        else:
            return None # raise AttributeError('No such register "{}"'.format(r))

    def addr(r):
        try:
            r = int(r, 0)
        except TypeError:
            pass
        if r in RegDecode.regs:
            return r
        elif r in RegDecode.regnames:
            return RegDecode.regnames[r]
        else:
            return None # raise AttributeError('No such register "{}"'.format(r))

    def valid(r):
        try:
            RegDecode.addr(r)
        except AttributeError:
            return False
        return True

    def width(r):
        try:
            return RegDecode.regs[RegDecode.addr(r)][1]
        except KeyError:
            raise AttributeError('No such register "{}"'.format(r))

    def to_str(data, always_hex = True):
        '''Converts the data bytes 'data' of a multibyte command to text.
        If 'always_hex' is True, all bytes are decoded as hex codes, otherwise only non
        printable characters are escaped.'''

        prefix = ''
        if type(data) == int:
            data = [data]
        if always_hex:
            prefix = '0x'
            def escape(b):
                return '{:02X}'.format(b)
        else:
            def escape(b):
                c = chr(b)
                if not str.isprintable(c):
                    return '\\x{:02X}'.format(b)
                return c
        try:
            return '%s%s' % (prefix, ''.join([escape(b) for b in data]))
        except (TypeError, ValueError):
            return str(data)

    def add_decoderfunc(r, f):
        RegDecode.decoderfuncs[RegDecode.addr(r)] = f

    def decode(r, val):
        if (type(val) == list) and (len(val) == 1):
            val = val[0]
        try:
            val = int(val, 0)
        except TypeError:
            pass
        else:
            if val.bit_length() > (RegDecode.width(r) * 8):
                raise TypeError('Value {} exceeds register width {} of register {}'.format(val, RegDecode.width(r), r))
        if RegDecode.valid(r) and (r in RegDecode.decoderfuncs):
            ret = RegDecode.decoderfuncs[r](val)
            if type(ret) == int:
                return RegDecode.to_str(ret)
            else:
                return ret
        else:
            return RegDecode.to_str(val)

class RDecode(RegDecode):
    def __init__(self, f):
        regname = f.__name__.replace('reg_', '')
        self.f = f
        RegDecode.add_decoderfunc(regname, f)

    def __call__(self, *args, **kwargs):
        return self.f(*args, **kwargs)

