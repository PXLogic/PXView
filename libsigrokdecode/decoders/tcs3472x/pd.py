##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2010-2016 Uwe Hermann <uwe@hermann-uwe.de>
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

import sigrokdecode as srd

masks = {
    "PON": 0x01,
    "AEN": 0x02,
    "WEN": 0x08,
    "AIEN": 0x10,
    "APERS": 0x0F,
    "AGAIN": 0x03,
    "AINT": 0x10,
    "AVALID": 0x01,
    "WLONG": 0x02,
}

#   Current tcs3472x register, used for next read/writes
register_id = 0
#   Where the command start in decoder
sequence_start = 0
#   Where the command end in decoder
sequence_end = 0
#   Values read/written under current command
byte_values = []
#
protocol_analyzer = None


'''
    A collection of functions for putting data in the UI.
    In general, one function for each TCS3472x command.
'''
def put_command_0x00():
    options = "("
    for value in byte_values:
        options = "("
        for mask in ["AIEN", "WEN", "AEN", "PON"]:
            if not value & masks[mask]:
                options += "~"
            options += mask + ","
    options += ")"

    long_desc = "Register {0:#04x} {1:s}={2:s}".format(register_id, register_set[register_id][0], options)
    medium_desc = "Register {1:s}={2:s}".format(register_id, register_set[register_id][0], options)
    short_desc = "{0:s}={1:s}".format(register_set[register_id][0], options)
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_register(reg_id, value):
    long_desc = "Register {0:#04x} {1:s}={2:d}".format(reg_id, register_set[reg_id][0], value)
    medium_desc = "Register {1:s}={2:d}".format(reg_id, register_set[reg_id][0], value)
    short_desc = "{0:s}={1:d}".format(register_set[reg_id][0], value)
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_color(color, value):

    long_desc = "{0:s} Channel={1:d}".format(color, value)
    medium_desc = "{0:s}={1:d}".format(color, value)
    short_desc = "{0:s}={1:d}".format(color[0], value)

    return [0, [long_desc, medium_desc, short_desc]]


def evaluate_bytes_values():
    '''
    Evaluate a 8/16 bit values read from the chip.
    '''
    if len(byte_values) == 1:
        value = byte_values[0]
    else:
        value = (byte_values[1] << 8) + byte_values[0]
    return value


def put_command_0x01():
    return put_command_register(0x01, byte_values[0])


def put_command_0x03():
    return put_command_register(0x03, byte_values[0])


def put_command_0x04():
    return put_command_register(0x04, evaluate_bytes_values())


def put_command_0x05():
    return put_command_register(0x05, evaluate_bytes_values())


def put_command_0x06():
    return put_command_register(0x06, evaluate_bytes_values())


def put_command_0x07():
    return put_command_register(0x06, evaluate_bytes_values())


def put_command_0x0C():
    persistence = byte_values[0] & masks["APERS"]
    if persistence > 3:
        persistence = (persistence - 3) * 5

    if persistence == 0:
        option = "Int after each RGBC cycle"
    else:
        option = "Int after {0} out of range".format(persistence)

    long_desc = "Register {0:#04x} {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    medium_desc = "Register {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    short_desc = "{0:s}={1:s}".format(register_set[register_id][0], option)
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_0x0D():
    wlong = byte_values[0] & masks["WLONG"]
    if wlong == 1:
        option = "Wait 12x"
    else:
        option = "Wait 1x"

    long_desc = "Register {0:#04x} {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    medium_desc = "Register {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    short_desc = "{0:s}={1:s}".format(register_set[register_id][0], option)
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_0x0F():
    val = byte_values[0] & masks["AGAIN"]
    again = ["1x", "4x", "16x", "60x"][val]

    long_desc = "Register {0:#04x} {1:s} AGAIN={2:s}".format(register_id, register_set[register_id][0], again)
    medium_desc = "{1:s} AGAIN ={2:s}".format(register_id, register_set[register_id][0], again)
    short_desc = "AGAIN={1:s}".format(register_set[register_id][0], again)
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_0x12():
    if byte_values[0] == 0x44:
        option = "(TCS34721/TCS34725)"
    else:
        if byte_values[0] == 0x4D:
            option = "(TCS34723/TCS34727)"
        else:
            option = "unknown"

    long_desc = "Chip id={0:#04x} {1:s}".format(byte_values[0], option)
    medium_desc = "Chip id {0:#04x}".format(byte_values[0])
    short_desc = "{0:#04x}".format(byte_values[0])
    return [0, [long_desc, medium_desc, short_desc]]


def put_command_0x13():
    option = ""

    if byte_values[0] & masks["AINT"]:
        option = "CLEAR valid, "

    if byte_values[0] & masks["AVALID"]:
        option += "Integration complete"

    long_desc = "Register {0:#04x} {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    medium_desc = "Register {1:s}={2:s}".format(register_id, register_set[register_id][0], option)
    short_desc = "{0:s}={1:s}".format(register_set[register_id][0], option)

    return [0, [long_desc, medium_desc, short_desc]]


def put_command_0x14():
    return put_command_color("Clear", evaluate_bytes_values())


def put_command_0x15():
    return put_command_color("Clear", evaluate_bytes_values())


def put_command_0x16():
    return put_command_color("Red", evaluate_bytes_values())


def put_command_0x17():
    return put_command_color("Red", evaluate_bytes_values())


def put_command_0x18():
    return put_command_color("Green", evaluate_bytes_values())


def put_command_0x19():
    return put_command_color("Green", evaluate_bytes_values())


def put_command_0x1A():
    return put_command_color("Blue", evaluate_bytes_values())


def put_command_0x1B():
    return put_command_color("Blue", evaluate_bytes_values())


def put_command():
    if register_id == 0x66:
        long_desc = "CLEAR channel interrupt clear"
        medium_desc = "CLEAR interrupt clear"
        short_desc = "C interrupt clear"
    else:
        long_desc = "Select register {0:#04x} {1:s}".format(register_id, register_set[register_id][0])
        medium_desc = "Register {0:#04x} {1:s}".format(register_id, register_set[register_id][0])
        short_desc = "{0:#04x}".format(register_id)

    return [0, [long_desc, medium_desc, short_desc]]


def command(ss, es, value):
    '''
    Start collecting data for a new command
    '''
    global sequence_end
    global register_id
    global byte_values
    byte_values = []
    sequence_end = es
    register_id = value & 0x7F

    # print("Register {0:#04x} {1:s}".format(register_id, register_set[register_id][0]))


def set_sequence_start(ss, es, value):
    '''
    Remember where the command start in the sequence
    '''
    global sequence_start
    sequence_start = ss


def set_sequence_end(ss, es, value):
    '''
    Remember where the command end in the sequence
    '''
    global sequence_end
    sequence_end = es


def append_byte_value(ss, es, value):
    '''
    Append a data value in the command
    '''
    global byte_values
    byte_values.append(value)


def interpret_byte_values(ss, es, value):
    global register_id
    global byte_values
    global protocol_analyzer

    if byte_values:
        fcn = register_set[register_id][1]
        protocol_analyzer.put(sequence_start, sequence_end, protocol_analyzer.out_ann, fcn())
    else:
        protocol_analyzer.put(sequence_start, sequence_end, protocol_analyzer.out_ann, put_command())


register_set = {
    0x00: ["ENABLE", put_command_0x00],
    0x01: ["ATIME", put_command_0x01],
    0x03: ["WTIME", put_command_0x03],
    0x04: ["AILTL", put_command_0x04],
    0x05: ["AILTH", put_command_0x05],
    0x06: ["AIHTL", put_command_0x06],
    0x07: ["AIHTH", put_command_0x07],
    0x0C: ["PERS", put_command_0x0C],
    0x0D: ["CONFIG", put_command_0x0D],
    0x0F: ["CONTROL", put_command_0x0F],
    0x12: ["ID", put_command_0x12],
    0x13: ["STATUS", put_command_0x13],
    0x14: ["CDATAL", put_command_0x14],
    0x15: ["CDATAH", put_command_0x15],
    0x16: ["RDATAL", put_command_0x16],
    0x17: ["RDATAH", put_command_0x17],
    0x18: ["GDATAL", put_command_0x18],
    0x19: ["GDATAH", put_command_0x19],
    0x1A: ["BDATAL", put_command_0x1A],
    0x1B: ["BDATAH", put_command_0x1B],
}


class State:
    '''
    A state for the state machine
    '''

    def __init__(self, name):
        self.name = name
        self.inputs = []
        self.action = []

    def set_action(self, action):
        '''
        Receive a function that will called when reaching this state.
        '''
        self.action.append(action)

    def do_action(self, ss, es, value):
        '''
        Execute the registered action for this state.
        '''
        for action in self.action:
            action(ss, es, value)

    def add_input(self, ptype, pnext):
        '''
        Define next state upon receiving giving input (ptype)
        '''
        self.inputs.append([ptype, pnext])

    def next(self, ptype):
        '''
        Return the next state giving the input
        '''
        for inp in self.inputs:
            if inp[0] == ptype:
                return inp[1]

        return None

    def __str__(self):
        return self.name

    def __format__(self, format_spec):
        return self.name

'''
 State Machine definition.
'''
state_initial = State("Initial")
start = State("Start")

address_write = State("Address Write")
address_write.set_action(set_sequence_start)
ack_address_write = State("Ack Address Write")
ack_data_write = State("Ack Data Write")

address_read = State("Address Read")
address_read.set_action(set_sequence_start)
ack_address_read = State("Ack Address Read")
ack_data_read = State("Ack Data Read")
nack_data_read = State("Nack Data Read")

wait_ack = State("Wait_Ack")
data_write_command = State("Command")
data_write_command.set_action(command)
data_write = State("Data Write")
data_write.set_action(set_sequence_end)
data_write.set_action(append_byte_value)

data_read = State("Data Read")
data_read.set_action(set_sequence_end)
data_read.set_action(append_byte_value)
state_initial.add_input("START", start)
state_initial.set_action(interpret_byte_values)

start.add_input("ADDRESS WRITE", address_write)

address_write.add_input("ACK", ack_address_write)
ack_address_write.add_input("DATA WRITE", data_write_command)
data_write_command.add_input("ACK", ack_data_write)
data_write.add_input("ACK", ack_data_write)
ack_data_write.add_input("STOP", state_initial)
ack_data_write.add_input("DATA WRITE", data_write)

start.add_input("ADDRESS READ",  address_read)
address_read.add_input("ACK", ack_address_read)
ack_address_read.add_input("DATA READ", data_read)
data_read.add_input("ACK", ack_data_read)
data_read.add_input("NACK", nack_data_read)
ack_data_read.add_input("STOP", state_initial)
ack_data_read.add_input("DATA READ", data_read)
nack_data_read.add_input("STOP", state_initial)


class Decoder(srd.Decoder):
    api_version = 3
    id = 'tcs3472x'
    name = 'TCS3472X'
    longname = 'TCS3472X'
    desc = 'Color light-to-digital converter with IR filter'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['Embedded/industrial']
    options = (
        {'id': 'device_address',
         'desc': 'I2C device address',
          'default': '0x29', 'values': ('0x29', '0x39')},
    )

    annotations = (
        ('register', 'Register'),
    )
    annotation_rows = (
        ('registers', 'Data', (0,)),
    )

    def __init__(self):
        global protocol_analyzer
        protocol_analyzer = self
        self.commands = list()
        self.state = state_initial
        self.out_ann = None
        self.out_python = None
        self.reset()

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

        self.reset()

    def reset(self):
        self.state = state_initial

    def decode(self, ss, es, data):

        global sequence_start
        global sequence_end
        global register_id
        global register_set

        ptype = data[0]
        value = data[1]

        if ptype == "BITS":
            return

#        print(ss, es, ptype, value)

        next_state = self.state.next(ptype)

        # Filter out things not addressed to our chip
        if next_state == address_write or next_state == address_read:
            if value != int(self.options["device_address"], 0):
                next_state = None

        if next_state is None:
            self.reset()
        else:
            old_state = self.state
            self.state = next_state
            self.state.do_action(ss, es, value)
         #   print("State {0:s} -> {1:s} {2:d}, {3:d}".format(old_state, self.state, sequence_start, sequence_end))


