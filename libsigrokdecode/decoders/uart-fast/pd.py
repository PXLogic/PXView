##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2011-2014 Uwe Hermann <uwe@hermann-uwe.de>
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

import sigrokdecode as srd
from common.srdhelper import bitpack
from math import floor, ceil

'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <rxtx>, <pdata>]

This is the list of <ptype>s and their respective <pdata> values:
 - 'STARTBIT': The data is the (integer) value of the start bit (0/1).
 - 'DATA': This is always a tuple containing two items:
   - 1st item: the (integer) value of the UART data. Valid values
     range from 0 to 511 (as the data can be up to 9 bits in size).
   - 2nd item: the list of individual data bits and their ss/es numbers.
 - 'PARITYBIT': The data is the (integer) value of the parity bit (0/1).
 - 'STOPBIT': The data is the (integer) value of the stop bit (0 or 1).
 - 'INVALID STARTBIT': The data is the (integer) value of the start bit (0/1).
 - 'INVALID STOPBIT': The data is the (integer) value of the stop bit (0/1).
 - 'PARITY ERROR': The data is a tuple with two entries. The first one is
   the expected parity value, the second is the actual parity value.
 - 'BREAK': The data is always 0.
 - 'FRAME': The data is always a tuple containing two items: The (integer)
   value of the UART data, and a boolean which reflects the validity of the
   UART frame.
 - 'PACKET': The data is always a tuple containing two items: The list of
   (integer) values of the UART data packet, and a boolean which reflects
   the validity of the UART frames in the data packet.
 - 'IDLE': The data is always 0.

The <rxtx> field is 0 for RX packets, 1 for TX packets.
'''

# Used for differentiating between the two data directions.
RX = 0
TX = 1

# Used for protocols stackable with the uart and which require
# several uniform idle periods, such as lin PD
IDLE_NUM_WITHOUT_GROWTH = 2

# Given a parity type to check (odd, even, zero, one), the value of the
# parity bit, the value of the data, and the length of the data (5-9 bits,
# usually 8 bits) return True if the parity is correct, False otherwise.
# 'none' is _not_ allowed as value for 'parity_type'.
def parity_ok(parity_type, parity_bit, data, data_bits):

    if parity_type == 'ignore':
        return True

    # Handle easy cases first (parity bit is always 1 or 0).
    if parity_type == 'zero':
        return parity_bit == 0
    elif parity_type == 'one':
        return parity_bit == 1

    # Count number of 1 (high) bits in the data (and the parity bit itself!).
    ones = bin(data).count('1') + parity_bit

    # Check for odd/even parity.
    if parity_type == 'odd':
        return (ones % 2) == 1
    elif parity_type == 'even':
        return (ones % 2) == 0

class SamplerateError(Exception):
    pass

class BaudrateError(Exception):
    pass

class ChannelError(Exception):
    pass

# 给各个变量分配唯一ID（0 ~ 12）属于Ann类下
class Ann:
    RX_WARN, TX_WARN, RX_DATA, TX_DATA, RX_START, TX_START, RX_PARITY_OK, \
    TX_PARITY_OK, RX_PARITY_ERR, TX_PARITY_ERR, RX_STOP, TX_STOP, \
    ATK_POINT, \
        = range(13)

# 给各个变量分配唯一ID（0 ~ 2）属于Bin类下
class Bin:
    RX, TX, RXTX = range(3)
    RX_OK, TX_OK, RXTX_OK = range(3,6)

# 给各个变量分配唯一ID（0 ~ 4）属于State类下
class State:
    WAIT_FOR_START_BIT, \
    GET_START_BIT, \
    GET_DATA_BITS, \
    GET_PARITY_BIT, \
    GET_STOP_BITS, \
        = range(5)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'uart-fast'
    name = 'UART-fast'
    longname = 'Universal Asynchronous Receiver/Transmitter'
    desc = 'Asynchronous, serial bus.(Ultra-fast version)'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['uart']
    tags = ['Embedded/industrial']

    # 上位机协议设置--可选通道
    optional_channels = (
        {'id': 'rx', 'name': 'RX', 'desc': 'UART receive line'},
        {'id': 'tx', 'name': 'TX', 'desc': 'UART transmit line'},
    )
    # 上位机协议设置
    options = (
        {'id': 'baudrate', 'desc': 'Baud rate(波特率)', 'default': 115200},
        {'id': 'data_bits', 'desc': 'Data bits(数据位数)', 'default': 8,
            'values': (5, 6, 7, 8, 9)},
        {'id': 'parity', 'desc': 'Parity(校验位)', 'default': 'none',
            'values': ('none', 'odd', 'even', 'zero', 'one', 'ignore')},
        {'id': 'stop_bits', 'desc': 'Stop bits(停止位)', 'default': 1.0,
            'values': (0.0, 0.5, 1.0, 1.5, 2.0)},
        {'id': 'bit_order', 'desc': 'Bit order(位序)', 'default': 'lsb-first',
            'values': ('lsb-first', 'msb-first')},
        {'id': 'format', 'desc': 'Data format(数据格式)', 'default': 'hex',
            'values': ('ascii', 'dec', 'hex', 'oct', 'bin')},
        {'id': 'invert', 'desc': 'Invert RX/TX(反转信号)', 'default': 'no',
            'values': ('yes', 'no')},
        {'id': 'packet_idle_us', 'desc': 'Packet delimit by idle time, us(按空闲时间划分包)', 'default': -1},
        {'id': 'show_data_point', 'desc': 'Show data point(数据点显示)', 'default': 'no',
            'values': ('yes', 'no')},
    )
    # 注释
    annotations = (
        ('rx-data', 'RX data'),#0
        ('tx-data', 'TX data'),
        ('rx-start', 'RX start bit'),#2
        ('tx-start', 'TX start bit'),
        ('rx-parity-ok', 'RX parity OK bit'),#4
        ('tx-parity-ok', 'TX parity OK bit'),
        ('rx-parity-err', 'RX parity error bit'),#6
        ('tx-parity-err', 'TX parity error bit'),
        ('rx-stop', 'RX stop bit'),#8
        ('tx-stop', 'TX stop bit'),
        ('rx-warning', 'RX warning'),#10
        ('tx-warning', 'TX warning'),
        ('atk-data-point', 'ATK Data point'),
    )
    # 注释行
    annotation_rows = (
        ('rx-data-vals', 'RX data', (Ann.RX_DATA, Ann.RX_START, Ann.RX_PARITY_OK, Ann.RX_PARITY_ERR, Ann.RX_STOP)),
        ('rx-warnings', 'RX warnings', (Ann.RX_WARN,)),
        ('tx-data-vals', 'TX data', (Ann.TX_DATA, Ann.TX_START, Ann.TX_PARITY_OK, Ann.TX_PARITY_ERR, Ann.TX_STOP)),
        ('tx-warnings', 'TX warnings', (Ann.TX_WARN,)),
        ('atk-signs', 'ATK signs', (Ann.ATK_POINT,)),
    )
    binary = (
        ('rx', 'RX dump'),
        ('tx', 'TX dump'),
        ('rxtx', 'RX/TX dump'),
        ('rx-ok', 'RX dump (no error)'),  
        ('tx-ok', 'TX dump (no error)'),  
        ('rxtx-ok', 'RX/TX dump (no error)'),
    )
    idle_state = [State.WAIT_FOR_START_BIT, State.WAIT_FOR_START_BIT]

    def putx(self, ss, es, data, out_type):
        if out_type == 'ann':
            self.put(ss, es, self.out_ann, data)
        elif out_type == 'python':
            self.put(ss, es, self.out_python, data)
        elif out_type == 'binary':
            self.put(ss, es, self.out_binary, data)

    def __init__(self):
        self.reset()

    def reset(self):
        self.state_num = [0, 0]
        self.state = [State.WAIT_FOR_START_BIT, State.WAIT_FOR_START_BIT]
        self.data_bounds = [0, 0]
        self.samplerate = None
        self.frame_start = [-1, -1]
        self.frame_valid = [True, True]
        self.packet_valid = [True, True]
        self.datavalue = [0, 0]
        self.paritybit = [-1, -1]
        self.stopbits = [[], []]
        self.databits = [[], []]
        self.break_start = [None, None]
        self.packet_data = [[], []]
        self.ss_packet, self.es_packet = [None, None], [None, None]

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.stop_bits = float(self.options['stop_bits'])
        self.msb_first = self.options['bit_order'] == 'msb-first'
        self.data_bits = self.options['data_bits']
        self.parity_type = self.options['parity']
        self.bw = (self.data_bits + 7) // 8
        self.show_data_point = self.options['show_data_point'] == 'yes'
        self.check_settings_required()
        self.init_packet_idle()
        self.init_state_machine()

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value
            self.baudrate = float(self.options['baudrate'])
            self.bit_width = float(self.samplerate) / self.baudrate
            self.half_bit_width = self.bit_width * 0.5
            self.bit_samplenum = self.bit_width * 0.5

    def get_sample_point(self, rxtx):
        state_num = self.state_num[rxtx]
        _, samplenum, _ = self.state_machine[state_num][1]
        return self.frame_start[rxtx] + samplenum

    def wait_for_start_bit(self, rxtx, signal):
        # Save the sample number where the start bit begins.
        self.frame_start[rxtx] = self.samplenum
        self.frame_valid[rxtx] = True

        self.advance_state_machine(rxtx, signal)

    def frame_bit_bounds(self, rxtx):
        start = self.frame_start[rxtx]
        state_num = self.state_num[rxtx]
        # Relative start and end samples of the current bit
        rel_ss, _, rel_es = self.state_machine[state_num][1]
        return start + rel_ss, start + rel_es

    def reset_data_receive(self, rxtx):
        # Reset internal state for the pending UART frame.
        self.databits[rxtx].clear()
        self.datavalue[rxtx] = 0
        self.paritybit[rxtx] = -1
        self.stopbits[rxtx].clear()

    def get_start_bit(self, rxtx, signal):
        startbit = signal
        frame_ss, frame_es = self.frame_bit_bounds(rxtx)

        if startbit != 0:
            frame_es = self.samplenum
            self.putx(frame_ss, frame_es, ['INVALID STARTBIT', rxtx, startbit], 'python')
            self.putx(frame_ss, frame_es, [Ann.RX_WARN + rxtx, ['Start bit error', 'Start err', 'SE']], 'ann')
            self.frame_valid[rxtx] = False
            self.handle_frame(rxtx, frame_ss, frame_es)
            self.advance_state_machine(rxtx, signal, startbit_error=True)
            return

        self.putx(frame_ss, frame_es, ['STARTBIT', rxtx, startbit], 'python')
        self.putx(frame_ss, frame_es, [Ann.RX_START + rxtx, ['Start bit', 'Start', 'S']], 'ann')

        self.advance_state_machine(rxtx, signal)
        self.reset_data_receive(rxtx)

    def handle_packet(self, rxtx):
        ss, es = self.ss_packet[rxtx], self.es_packet[rxtx]
        self.putx(ss, es, ['PACKET', rxtx, (self.packet_data[rxtx], self.packet_valid[rxtx])], 'python')
        self.packet_data[rxtx].clear()

    def get_packet_data(self, rxtx, frame_end_sample):
        if len(self.packet_data[rxtx]) == 0:
            self.ss_packet[rxtx] = self.frame_start[rxtx]
            self.packet_valid[rxtx] = self.frame_valid[rxtx]
        else:
            if not self.frame_valid[rxtx]:
                self.packet_valid[rxtx] = False
        self.packet_data[rxtx].append(self.datavalue[rxtx])
        self.es_packet[rxtx] = frame_end_sample

    def frame_data_bounds(self, rxtx):
        start = self.frame_start[rxtx]
        # Relative start and end samples of the data bits
        rel_ss, rel_es = self.data_bounds
        return start + rel_ss, start + rel_es

    def get_data_bits(self, rxtx, signal):
        ss, es = self.frame_bit_bounds(rxtx)

        if self.show_data_point:
            #计算中心点
            center = int(self.bit_width / 2) + ss
            self.putx(center,center,[Ann.ATK_POINT, ['%d' % rxtx]], 'ann')

        self.databits[rxtx].append([signal, ss, es])
        if len(self.databits[rxtx]) == self.data_bits:
            self.handle_data(rxtx)

        self.advance_state_machine(rxtx, signal)

    def handle_data(self, rxtx):
        bits = [b[0] for b in self.databits[rxtx]]
        if self.msb_first:
            bits.reverse()
        b = bitpack(bits)
        # 存储所有数据位
        self.datavalue[rxtx] = b

        ss_data, es_data = self.frame_data_bounds(rxtx)
        self.putx(ss_data, es_data, ['DATA', rxtx, (self.datavalue[rxtx], self.databits[rxtx])], 'python')
        self.putx(ss_data, es_data, [Ann.RX_DATA + rxtx, [self.format_value(b)]], 'ann')
        
        self.databits[rxtx].clear()

    def get_bit_bounds(self, bit_num, half_bit=False):
        ss = bit_num * self.bit_width
        if not half_bit:
            return (
                round(ss),
                round(ss + self.bit_samplenum),
                round(ss + self.bit_width),
            )
        else:
            return (
                round(ss),
                round(ss + self.bit_samplenum * 0.5),
                round(ss + self.bit_width * 0.5),
            )

    def init_state_machine(self):
        sm = list()

        # Get START bit
        sm.append((State.WAIT_FOR_START_BIT, (0, 0, 0)))
        sm.append((State.GET_START_BIT, self.get_bit_bounds(0)))

        # Get DATA bits
        self.data_bounds[0] = sm[-1][1][2]  # end of start bit and start of first data bit
        for data_bit_num in range(self.data_bits):
            sm.append((State.GET_DATA_BITS, self.get_bit_bounds(data_bit_num+1)))
        self.data_bounds[1] = sm[-1][1][2]  # end of last data bit

        # Get PARITY bit
        frame_bit_num = 1 + self.data_bits
        if self.parity_type != 'none':
            sm.append((State.GET_PARITY_BIT, self.get_bit_bounds(frame_bit_num)))
            frame_bit_num += 1

        # Get STOP bit(s)
        stop_bits = self.stop_bits
        while stop_bits > 0.4:  # we can't check float equality with exact values due to rounding
            if stop_bits > 0.9:
                sm.append((State.GET_STOP_BITS, self.get_bit_bounds(frame_bit_num)))
                stop_bits -= 1
                frame_bit_num += 1
            elif stop_bits > 0.4:
                sm.append((State.GET_STOP_BITS, self.get_bit_bounds(frame_bit_num, half_bit=True)))
                stop_bits = 0

        # Looping state machine to simplify advance_state_machine function
        sm.append(sm[0])
        self.state_machine = sm

        # Init state machine
        for rxtx in (RX, TX):
            self.state[rxtx] = State.WAIT_FOR_START_BIT
            self.state_num[rxtx] = 0

    def init_packet_idle(self):
        packet_idle_us = self.options['packet_idle_us']
        if packet_idle_us > 0:
            self.packet_idle_samples = int(round(packet_idle_us * 1e-6 * self.samplerate))
            self.packet_idle_samples = max(1, self.packet_idle_samples)
        else:
            self.packet_idle_samples = None

    def format_value(self, v):
        self.fmt = self.options['format']
        if self.fmt == 'ascii':
            if 32 <= v <= 126:
                return chr(v)
            else:
                return '0x{:02X}'.format(v) if self.data_bits <= 8 else '0x{:03X}'.format(v)
        elif self.fmt == 'dec':
            return str(v)
        elif self.fmt == 'hex':
            return "{:02X}".format(v)
        elif self.fmt == 'oct':
            return "{:03o}".format(v)
        elif self.fmt == 'bin':
            return "{:0{width}b}".format(v, width=self.data_bits)        

    def get_parity_bit(self, rxtx, signal):
        self.paritybit[rxtx] = signal
        ss, es = self.frame_bit_bounds(rxtx)
        if parity_ok(self.parity_type, self.paritybit[rxtx],
                     self.datavalue[rxtx], self.data_bits):
            self.putx(ss, es, ['PARITYBIT', rxtx, self.paritybit[rxtx]], 'python')
            self.putx(ss, es, [Ann.RX_PARITY_OK + rxtx, ['Parity bit', 'Parity', 'P']], 'ann')
        else:
            # Return expected/actual parity values.
            self.putx(ss, es, ['PARITY ERROR', rxtx, ((not signal)*1, signal*1)], 'python')
            self.putx(ss, es, [Ann.RX_PARITY_ERR + rxtx, ['Parity error', 'Parity err', 'PE']], 'ann')
            self.frame_valid[rxtx] = False

        self.advance_state_machine(rxtx, signal)

    def get_stop_bits(self, rxtx, signal):
        # 将信号存入停止位列表
        self.stopbits[rxtx].append(signal)
        ss, es = self.frame_bit_bounds(rxtx)

        # Stop bits must be 1. If not, we report an error.
        stopbit_error = signal != 1
        if stopbit_error:
            es = self.samplenum
            self.putx(ss, es, ['INVALID STOPBIT', rxtx, signal], 'python')  
            self.putx(ss, es, [Ann.RX_WARN + rxtx, ['Stop bit error', 'Stop err', 'TE']], 'ann') 
            self.frame_valid[rxtx] = False  
        # elif self.show_start_stop:
        self.putx(ss, es, [Ann.RX_STOP + rxtx, ['Stop bit', 'Stop', 'T']], 'ann')
        self.putx(ss, es, ['STOPBIT', rxtx, signal], 'python')

        self.advance_state_machine(rxtx, signal, stopbit_error=stopbit_error)

    def advance_state_machine(self, rxtx, signal, startbit_error=False, stopbit_error=False):
        if startbit_error or stopbit_error:
            self.state_num[rxtx] = 0
            self.state[rxtx] = State.WAIT_FOR_START_BIT
            if startbit_error:
                return
        else:
            self.state_num[rxtx] += 1
            self.state[rxtx] = self.state_machine[self.state_num[rxtx]][0]

        if self.state[rxtx] == State.WAIT_FOR_START_BIT:
            self.state_num[rxtx] = 0
            frame_ss = self.frame_start[rxtx]
            if not stopbit_error:
                frame_es = frame_ss + self.frame_len_sample_count
            else:
                frame_es = self.samplenum
            self.handle_frame(rxtx, frame_ss, frame_es)
            self.get_packet_data(rxtx, frame_es)

    def handle_frame(self, rxtx, ss, es):
        # Pass the complete UART frame to upper layers.
        self.putx(ss, es, ['FRAME', rxtx,
            (self.datavalue[rxtx], self.frame_valid[rxtx])], 'python')
        # 在帧结束时统一输出二进制数据
        bdata = self.datavalue[rxtx].to_bytes(self.bw, byteorder='big')

        # 所有数据（无论是否有错）输出到 rx/tx/rxtx
        self.putx(ss, es, [Bin.RX + rxtx, bdata], 'binary')  # rx/tx
        self.putx(ss, es, [Bin.RXTX, bdata], 'binary')       # rxtx

        # 仅无错误数据输出到 rx-ok/tx-ok/rxtx-ok
        if self.frame_valid[rxtx]:
            self.putx(ss, es, [Bin.RX_OK + rxtx, bdata], 'binary')  # rx-ok/tx-ok
            self.putx(ss, es, [Bin.RXTX_OK, bdata], 'binary')        # rxtx-ok

    # 合并后的事件处理
    def handle_event(self, event, rxtx, ss=None, es=None, extra=None):
        if event == 'break':
            self.putx(ss, es, ['BREAK', rxtx, 0], 'python')
        elif event == 'packet_idle':
            if not self.packet_data[rxtx] or self.packet_idle_samples is None:
                return
            if es >= self.es_packet[rxtx] + self.packet_idle_samples:
                self.handle_packet(rxtx)

    # 合并后的条件生成器
    def get_cond(self, rxtx, inv, cond_type):
        if cond_type == 'wait':
            if self.state[rxtx] == State.WAIT_FOR_START_BIT:
                return {rxtx: 'r' if inv else 'f'}
            else:
                want_samplenum = self.get_sample_point(rxtx)
                return {'skip': want_samplenum - self.samplenum}
        
    # 合并后的采样/事件分发
    def inspect(self, rxtx, signal, inv, mode):
        if inv:
            signal = not signal
        if mode == 'sample':
            state = self.state[rxtx]
            if state == State.WAIT_FOR_START_BIT:
                self.handle_event('packet_idle', rxtx, None, self.samplenum)
                self.wait_for_start_bit(rxtx, signal)
            elif state == State.GET_START_BIT:
                self.get_start_bit(rxtx, signal)
            elif state == State.GET_DATA_BITS:
                self.get_data_bits(rxtx, signal)
            elif state == State.GET_PARITY_BIT:
                self.get_parity_bit(rxtx, signal)
            elif state == State.GET_STOP_BITS:
                self.get_stop_bits(rxtx, signal)
        elif mode == 'edge':
            if not signal:
                self.break_start[rxtx] = self.samplenum
                return
            if self.break_start[rxtx] is None:
                return
            diff = self.samplenum - self.break_start[rxtx]
            if diff >= self.break_min_sample_count:
                ss, es = self.frame_start[rxtx], self.samplenum
                self.handle_event('break', rxtx, ss, es)
            self.break_start[rxtx] = None

    def check_settings_required(self):
        if not self.baudrate or self.baudrate < 0:
            raise BaudrateError('Cannot decode without baudrate > 0.')

    def decode(self):
        self.putx(self.samplenum, self.samplenum, [Ann.ATK_POINT,["color:#fbca47"]], 'ann')
        self.check_settings_required()
        if not self.samplerate:
            return

        enabled_rxtx = [rxtx for rxtx in (RX, TX) if self.has_channel(rxtx)]
        if not enabled_rxtx:
            raise ChannelError('Need at least one of TX or RX pins.')

        inv = self.options['invert'] == 'yes'
        frame_samples = 1 + self.data_bits + (0 if self.parity_type == 'none' else 1) + self.stop_bits
        frame_samples *= self.bit_width
        self.frame_len_sample_count = round(frame_samples)
        self.break_min_sample_count = self.frame_len_sample_count + 1

        cond_data_idx = [None] * 2
        cond_edge_idx = [None] * 2
        conds = []

        while True:
            conds.clear()
            for rxtx in enabled_rxtx:
                cond_data_idx[rxtx] = len(conds)
                conds.append(self.get_cond(rxtx, inv, 'wait'))
                cond_edge_idx[rxtx] = len(conds)
                conds.append({rxtx: 'e'})

            (signal1,signal0) = self.wait(conds)
            signal = [signal1,signal0]

            for rxtx in enabled_rxtx:
                sig = signal[rxtx]
                if inv:
                    sig = not sig
                if cond_data_idx[rxtx] is not None and (self.matched & (0b1 << cond_data_idx[rxtx])):
                    self.inspect(rxtx, sig, False, 'sample')
                if cond_edge_idx[rxtx] is not None and (self.matched & (0b1 << cond_edge_idx[rxtx])):
                    self.inspect(rxtx, sig, False, 'edge')
                    self.inspect(rxtx, sig, False, 'idle')
                