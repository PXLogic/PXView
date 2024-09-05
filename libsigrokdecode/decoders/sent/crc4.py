##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2023 enp6s0
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


#
# SAE J2716 (SENT) decoder
# CRC4 related functions
#
def crc4(data, legacy = False):
    '''
    CRC4 function with both new and legacy methods for calculation,
    as specified by the J2716 standard (see page 24 on the 2016 version for
    implementation info)
    '''

    assert type(data) == list, 'Data must be a list'
    assert len(data) >= 1, 'Data must not be empty!'

    # CRC lookup table
    crcTable = [
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        13,12,15,14,9,8,11,10,5,4,7,6,1,0,3,2,
        7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8,
        10,11,8,9,14,15,12,13,2,3,0,1,6,7,4,5,
        14,15,12,13,10,11,8,9,6,7,4,5,2,3,0,1,
        3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
        9,8,11,10,13,12,15,14,1,0,3,2,5,4,7,6,
        4,5,6,7,0,1,2,3,12,13,14,15,8,9,10,11,
        1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14,
        12,13,14,15,8,9,10,11,4,5,6,7,0,1,2,3,
        6,7,4,5,2,3,0,1,14,15,12,13,10,11,8,9,
        11,10,9,8,15,14,13,12,3,2,1,0,7,6,5,4,
        15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
        2,3,0,1,6,7,4,5,10,11,8,9,14,15,12,13,
        8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7,
        5,4,7,6,1,0,3,2,13,12,15,14,9,8,11,10
    ]

    # Start checksum with a seed value of 5
    checksum = 5

    # Calculate checksum
    for d in data:
        checksum = crcTable[d + checksum * 16]

    # If not legacy CRC (i.e., if the 'recommended' CRC method is to be used), add extra '0' value
    if(not legacy):
        checksum = crcTable[checksum * 16]

    return checksum

# Infineon method of CRC4
def crc4_infineon(data):
    '''
    CRC4 implementation, Infineon style (as specified in TLE4998 user manual)
    data should also include the status nibble
    '''
    assert type(data) == list, 'Data must be a list'
    assert len(data) >= 1, 'Data must not be empty!'

    crcTable = [0, 13, 7, 10, 14, 3, 9, 4, 1, 12, 6, 11, 15, 2, 8, 5]
    checksum = 5
    for d in data:
        checksum = crcTable[checksum ^ d]

    return checksum

# Test fixture
def crc4_test(input, expected, legacy = False):
    crc = crc4(input, legacy)
    testName = 'CRC4 test' if not legacy else 'CRC4 test (legacy)'

    if(crc == expected):
        print(testName + ' : passed')
        return True
    else:
        print(testName + ' : FAILED') # (input {input}, got {crc}, expecting {expected})
        return False

# If file is executed directly, run tests
if __name__ == '__main__':

    # New CRC
    crc4_test([5, 3, 14, 5, 3, 14], 15, legacy = False)
    crc4_test([7, 4, 8, 7, 4, 8], 3, legacy = False)
    crc4_test([4, 10, 12, 4, 10, 12], 10, legacy = False)
    crc4_test([7, 8, 15, 7, 8, 15], 5, legacy = False)
    crc4_test([9, 1, 13, 9, 1, 13], 6, legacy = False)
    crc4_test([0, 0, 0, 0, 0, 0], 5, legacy = False)

    # Legacy CRC
    crc4_test([5, 3, 14, 5, 3, 14], 12, legacy = True)
    crc4_test([7, 4, 8, 7, 4, 8], 5, legacy = True)
    crc4_test([4, 10, 12, 4, 10, 12], 3, legacy = True)
    crc4_test([7, 8, 15, 7, 8, 15], 15, legacy = True)
    crc4_test([9, 1, 13, 9, 1, 13], 10, legacy = True)
    crc4_test([0, 0, 0, 0, 0, 0], 15, legacy = True)
