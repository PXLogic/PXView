#ifndef USB_CTRL_H
#define USB_CTRL_H
#include <stdio.h>
#include <string.h>
#include "libusb.h"

//extern bool usb_busy ;
unsigned int usb_wr_reg(libusb_device_handle *usbdevh,unsigned int reg_addr,unsigned int reg_data);
unsigned int usb_rd_reg(libusb_device_handle *usbdevh,unsigned int reg_addr,unsigned int *reg_data);
unsigned int usb_wr_data(libusb_device_handle *usbdevh,unsigned char *buff,int length,unsigned int timeout);
unsigned int usb_rd_data(libusb_device_handle *usbdevh,unsigned char *buff,int length,unsigned int timeout);
unsigned int usb_wr_data_update(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout);
unsigned int usb_rd_data_update(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout);

unsigned int usb_wr_data_req(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout);
unsigned int usb_rd_data_req(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout);


#endif // USB_CTRL_H
