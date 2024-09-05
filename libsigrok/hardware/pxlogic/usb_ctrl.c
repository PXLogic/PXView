#include "usb_ctrl.h"
//libusb_device_handle *usbdevh = NULL;
//volatile bool usb_busy = false;

unsigned int usb_wr_reg(libusb_device_handle *usbdevh,unsigned int reg_addr,unsigned int reg_data){
    int rc = 0;
    unsigned  int buf[4]={};
    buf[0]=0xfefe0000;
    buf[1]=0x08;
    buf[2] = reg_addr;
    buf[3] = reg_data;
     if(usbdevh ){
        rc=libusb_bulk_transfer(usbdevh, 0x01, (uint8_t*)buf, 16,NULL, 1000);
        if(rc!=0){
            return 1;
        }
        rc=libusb_bulk_transfer(usbdevh, 0x81, (uint8_t*)buf, 16,NULL, 1000);
        if(rc!=0){
            return 2;
        }
        if(buf[3]!=0xfefefefe) return 3;
        return 0;
     }
    return 1;
}




unsigned int usb_rd_reg(libusb_device_handle *usbdevh,unsigned int reg_addr,unsigned int *reg_data){
    int rc = 0;
    unsigned  int buf[4]={};
    buf[0]=0xfefe0001;
    buf[1]=0x08;
    buf[2] = reg_addr;
    buf[3] = 0;
     if(usbdevh){
         //发送寄存器读请求
         rc=libusb_bulk_transfer(usbdevh, 0x01, (uint8_t*)buf, 16,NULL, 1000);
         if(rc!=0){
             return 1;
         }
         //读取寄存器值
         rc=libusb_bulk_transfer(usbdevh, 0x81, (uint8_t*)buf, 16,NULL, 1000);
         if(rc!=0){
             return 2;
         }
         *reg_data = buf[3];
         return 0;
      }
    return 1;
}



unsigned int usb_wr_data(libusb_device_handle *usbdevh,unsigned char *buff,int length,unsigned int timeout){
    int rc = 0;
     if(usbdevh){
        rc=libusb_bulk_transfer(usbdevh, 0x01, (uint8_t*)buff, length,NULL, timeout);
        if(rc!=0){
            return 1;
        }
        return 0;
     }
    return 1;
}

unsigned int usb_rd_data(libusb_device_handle *usbdevh,unsigned char *buff,int length,unsigned int timeout){
    int rc = 0;
     if(usbdevh){
        rc=libusb_bulk_transfer(usbdevh, 0x81, (uint8_t*)buff, length,NULL, timeout);
        if(rc!=0){
            return 1;
        }
        return 0;
     }
    return 1;
}
//base_addr     写入基地址
//length        数据长度
//mode          模式 0 ：ch569w flash
//                  1 fpga flash
//                  2 fpga ddr

unsigned int usb_wr_data_update(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout){
    unsigned  int addr;
    int rc = 0;
    int align_length;
    if(length%4096){
        align_length = (length/4096 +1)*4096;
    }
    else{
        align_length = length;
    }

    addr = 8192+6*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr);
    if(rc!=0){
        return 1;
    }

    addr = 8192+7*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr+align_length);
    if(rc!=0){
        return 1;
    }

    addr = 8192+8*4;
    rc=usb_wr_reg(usbdevh,addr,mode);
    if(rc!=0){
        return 1;
    }

     if(usbdevh){
        //usb_busy = true;//加锁，禁止寄存器读写
        //libusb_clear_halt(usbdevh,0x03);
        rc=libusb_bulk_transfer(usbdevh, 0x03, (uint8_t*)buff, align_length,NULL, timeout);
        //usb_busy = false;
        if(rc!=0){
            return 1;
        }
        return 0;
     }
    return 1;
}

//base_addr     写入基地址
//length        数据长度
//mode          模式 0 ：ch569w flash
//                  1 fpga flash
//                  2 fpga ddr

unsigned int usb_wr_data_req(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout){
    unsigned  int addr;
    int rc = 0;
    int align_length;
    if(length%4096){
        align_length = (length/4096 +1)*4096;
    }
    else{
        align_length = length;
    }

    addr = 8192+6*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr);
    if(rc!=0){
        return 1;
    }

    addr = 8192+7*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr+align_length);
    if(rc!=0){
        return 1;
    }

    addr = 8192+8*4;
    rc=usb_wr_reg(usbdevh,addr,mode);
    if(rc!=0){
        return 1;
    }

    return 0;

     if(usbdevh){
        //usb_busy = true;//加锁，禁止寄存器读写
         //libusb_clear_halt(usbdevh,0x03);
        rc=libusb_bulk_transfer(usbdevh, 0x03, (uint8_t*)buff, align_length,NULL, timeout);
        //usb_busy = false;
        if(rc!=0){
            return 1;
        }
        return 0;
     }
    return 1;
}

//base_addr     读取基地址
//length        数据长度
//mode          模式 0 ：ch569w flash
//                  1 fpga flash
//                  2 fpga ddr
unsigned int usb_rd_data_update(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout){
    unsigned  int addr;
    int rc = 0;

    int align_length;
    if(length%4096){
        align_length = (length/4096 +1)*4096;
    }
    else{
        align_length = length;
    }
    addr = 8192+3*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr);
    if(rc!=0){
        return 1;
    }

    addr = 8192+4*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr+align_length);
    if(rc!=0){
        return 2;
    }

    addr = 8192+5*4;
    rc=usb_wr_reg(usbdevh,addr,mode);
    if(rc!=0){
        return 3;
    }

     if(usbdevh){
        //usb_busy = true;//加锁，禁止寄存器读写
         //libusb_clear_halt(usbdevh,0x83);
        rc=libusb_bulk_transfer(usbdevh, 0x83, (uint8_t*)buff, align_length,NULL, timeout);
        //usb_busy = false;
        if(rc!=0){
            return rc;
        }
        return 0;
     }
    return 1;
}


//base_addr     读取基地址
//length        数据长度
//mode          模式 0 ：ch569w flash
//                  1 fpga flash
//                  2 fpga ddr
unsigned int usb_rd_data_req(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout){
    unsigned  int addr;
    int rc = 0;

    int align_length;
    if(length%4096){
        align_length = (length/4096 +1)*4096;
    }
    else{
        align_length = length;
    }
    addr = 8192+3*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr);
    if(rc!=0){
        return 1;
    }

    addr = 8192+4*4;
    rc=usb_wr_reg(usbdevh,addr,base_addr+align_length);
    if(rc!=0){
        return 2;
    }

    addr = 8192+5*4;
    rc=usb_wr_reg(usbdevh,addr,mode);
    if(rc!=0){
        return 3;
    }
    
libusb_clear_halt(usbdevh,0x83);
    return 0;

     if(usbdevh){
        //usb_busy = true;//加锁，禁止寄存器读写
         libusb_clear_halt(usbdevh,0x83);
        //rc=libusb_bulk_transfer(usbdevh, 0x83, (uint8_t*)buff, align_length,NULL, timeout);
        //usb_busy = false;
        if(rc!=0){
            return rc;
        }
        return 0;
     }
    return 1;
}




