miscellaneous = {
    0x00:['Diagnose'],
    0x02:['GetFirmwareVersion'],
    0x04:['GetGeneralStatus'],
    0x06:['ReadRegister'],
    0x08:['WriteRegister'],
    0x0C:['ReadGPIO'],
    0x0E:['WriteGPIO'],
    0x10:['SetSerialBaudRate'],
    0x12:['SetParameters'],
    0x14:['SAMConfiguration'],
    0x16:['PowerDown'],
}

rf_communication = {
    0x32:['RFConfiguration'],
    0x58:['RFRegulationTest'],
}

initiator = {
    0x56:['InJumpForDEP'],
    0x46:['InJumpForPSL'],
    0x4A:['InListPassiveTarget'],
    0x50:['InATR'],
    0x4E:['InPSL'],
    0x40:['InDataExchange'],
    0x42:['InCommunicateThru'],
    0x44:['InDeselect'],
    0x52:['InRelease'],
    0x54:['InSelect'],
    0x60:['InAutoPoll'],
}

target = {
    0x8C:['TgInitAsTarget'],
    0x92:['TgSetGeneralBytes'],
    0x86:['TgGetData'],
    0x8E:['TgSetData'],
    0x94:['TgSetMetaData'],
    0x88:['TgGetInitiatorCommand'],
    0x90:['TgResponseToInitiator'],
    0x8A:['TgGetTargetStatus'],
}

errors = {
    0x01:['Time Out, the target has not answered'],
    0x02:['A CRC error has been detected by the CIU'],
    0x03:['A Parity error has been detected by the CIU'],
    0x04:['During an anti-collision/select operation an erroneous Bit Count has been detected'],
    0x05:['Framing error during Mifare operation'],
    0x06:['An abnormal bit-collision has been detected during bit wise anti-collision at 106 kbps'],
    0x07:['Communication buffer size insufficien'],
    0x09:['RF Buffer overflow has been detected by the CIU'],
    0x0A:['The RF field has not been switched on in time by the counterpart'],
    0x0B:['RF Protocol error'],
    0x0D:['Temperature error'],
    0x0E:['Internal buffer overflow'],
    0x10:['Invalid parameter'],
    0x12:['The PN532 configured in target mode does not support the command received from the initiator'],
    0x13:['he data format does not match to the specification'],
    0x14:['Mifare: Authentication error '],
    0x23:['UID Check byte is wrong '],
    0x25:['Invalid device state, the system is in a state which does not allow the operation'],
    0x26:['Operation not allowed in this configuration'],
    0x27:['This command is not acceptable due to the current context of the PN532'],
    0x29:['The PN532 configured as target has been released by its initiator'],
    0x2A:['The ID of the card does not match'],
    0x2B:['The card previously activated has disappeared.'],
    0x2C:['Mismatch between the NFCID3 initiator and the NFCID3 target in DEP 212/424 kbps passive.'],
    0x2D:['An over-current event has been detected '],
    0x2E:['NAD missing in DEP frame'],
}