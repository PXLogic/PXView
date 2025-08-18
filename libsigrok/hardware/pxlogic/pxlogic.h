/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBDSL_HARDWARE_PX_H
#define LIBDSL_HARDWARE_PX_H

#include <glib.h>
#include "../../libsigrok-internal.h"
//#include <libsigrok/libsigrok.h>
 #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>
#include <inttypes.h>
#include "usb_ctrl.h"
#define PXVIEW_BL_EN 0
#define NUM_TRIGGER_STAGES	16
#define FIRMWARE_VERSION 0x56900027
#define FIRMWARE_BL_VERSION 0x56900000
#define PWM_CLK 125000000
#define PWM_MAX 1000000

#define TRIG_CHECKID 0x55555555

#define PXLOGIC_ATOMIC_BITS 6
#define PXLOGIC_ATOMIC_SAMPLES (1 << PXLOGIC_ATOMIC_BITS)
#define PXLOGIC_ATOMIC_SIZE (1 << (PXLOGIC_ATOMIC_BITS - 3))
#define PXLOGIC_ATOMIC_MASK (0xFFFFFFFF << PXLOGIC_ATOMIC_BITS)




struct PX_caps {
    uint64_t mode_caps;
    uint64_t feature_caps;
    uint64_t channels;
    uint64_t hw_depth;
    uint8_t  intest_channel;
    uint16_t default_channelmode;
    uint64_t default_timebase;
};

struct PX_profile {
    uint16_t vid;
    uint16_t pid;
    enum libusb_speed usb_speed;
    uint32_t   logic_mode;

    const char *vendor;
    const char *model;
    const char *model_version;

    const char *firmware;
    uint32_t   firmware_version;
    const char *firmware_bl;
    uint32_t   firmware_bl_version;
    const char *fpga_bit;
    const char *fpga_rst_bit;

    struct PX_caps dev_caps;
};



enum PX_CHANNEL_ID {
    BUFFER_LOGIC250x32 = 0,
    BUFFER_LOGIC250x16  ,
    BUFFER_LOGIC500x16  ,
    BUFFER_LOGIC1000x8  ,
    
//usb3.0 stream
    STREAM_LOGIC50x32   ,
    STREAM_LOGIC125x16  ,
    STREAM_LOGIC250x8   ,
    STREAM_LOGIC500x4   ,
    STREAM_LOGIC1000x2  ,

// usb 2.0 stream
    STREAM_LOGIC200x1   ,
    STREAM_LOGIC100x2   ,
    STREAM_LOGIC50x4    ,
    STREAM_LOGIC25x8    ,
    STREAM_LOGIC10x16   ,
    STREAM_LOGIC5x32    
};


struct PX_channels {
    enum PX_CHANNEL_ID id;
    enum OPERATION_MODE mode;
    enum CHANNEL_TYPE type;
    gboolean stream;
    uint16_t num;
    uint8_t unit_bits;
    uint64_t default_samplerate;
    uint64_t default_samplelimit;
    uint64_t min_samplerate;
    uint64_t max_samplerate;

    const char *descr;
};

struct PX_context {
    const struct PX_profile *profile;

    int pipe_fds[2];
    GIOChannel *channel;
    uint64_t cur_samplerate;
    uint64_t limit_samples;
    uint64_t limit_samples2Byte;
    uint64_t limit_samples_show;
    uint64_t limit_msec;
    uint8_t sample_generator;
    uint64_t samples_counter;
    uint64_t samples_counter_div2;
    volatile  int ch_num;
    void *cb_data;
    int64_t starttime;
    int stop;
    uint64_t timebase;
    enum PX_CHANNEL_ID ch_mode;
    uint16_t samplerates_min_index;
    uint16_t samplerates_max_index;
    gboolean instant;
    uint8_t max_height;
    uint64_t samples_not_sent;

    uint8_t *buf;
    uint64_t pre_index;
    struct sr_status mstatus;

	unsigned int num_transfers;
    unsigned int submitted_transfers;
    unsigned int rece_transfers;
	struct libusb_transfer **transfers;
	int *usbfd;
    enum libusb_speed usb_speed;
    int  send_total;

    int trigger_stage;
    uint16_t trigger_mask;
    uint16_t trigger_value;
    uint16_t trigger_edge;
    uint8_t trigger_slope;
    uint8_t trigger_source;
    uint16_t op_mode;
    gboolean stream;
    gboolean rle_mode;
    gboolean rle_support;
    uint8_t  test_mode;
    uint32_t block_size;
    gboolean acq_aborted;
    double vth;
    gboolean clock_edge;
    uint16_t ext_trig_mode;
    gboolean trig_out_en;
    uint16_t filter;
    uint32_t ch_en;
    uint32_t trig_zero;
    uint32_t trig_one;
    uint32_t trig_rise;
    uint32_t trig_fall;
    uint16_t trig_mask0[NUM_TRIGGER_STAGES];
    uint16_t trig_mask1[NUM_TRIGGER_STAGES];
    uint16_t trig_value0[NUM_TRIGGER_STAGES];
    uint16_t trig_value1[NUM_TRIGGER_STAGES];
    uint16_t trig_edge0[NUM_TRIGGER_STAGES];
    uint16_t trig_edge1[NUM_TRIGGER_STAGES];
    uint16_t trig_logic0[NUM_TRIGGER_STAGES];
    uint16_t trig_logic1[NUM_TRIGGER_STAGES];
    uint32_t trig_count[NUM_TRIGGER_STAGES];
    double   stream_buff_size;

    gboolean    pwm0_en;
    double      pwm0_freq;
    double      pwm0_duty;

    uint32_t    pwm0_freq_set;
    uint32_t    pwm0_duty_set;

    gboolean    pwm1_en;
    double      pwm1_freq;
    double      pwm1_duty;

    uint32_t    pwm1_freq_set;
    uint32_t    pwm1_duty_set;

    int is_loop;
    uint8_t  usb_data_align_en;
    struct ds_trigger_pos *trigger_pos;
    uint32_t trigger_pos_set;
    struct ctl_data cmd_data;

};

static const uint64_t samplerates[] = {
    SR_HZ(10),
    SR_HZ(20),
    SR_HZ(50),
    SR_HZ(100),
    SR_HZ(200),
    SR_HZ(500),
    SR_KHZ(1),
    SR_KHZ(2),
    SR_KHZ(5),
    SR_KHZ(10),
    SR_KHZ(20),
    SR_KHZ(40),
    SR_KHZ(50),
    SR_KHZ(100),
    SR_KHZ(200),
    SR_KHZ(400),
    SR_KHZ(500),
    SR_MHZ(1),
    SR_MHZ(2),
    SR_MHZ(4),
    SR_MHZ(5),
    SR_MHZ(10),
    SR_MHZ(20),
    //SR_MHZ(12.5),
    SR_MHZ(25),
    // SR_MHZ(40),
    SR_MHZ(50),
    SR_MHZ(100),
    SR_MHZ(125),
     SR_MHZ(200),
    SR_MHZ(250),
     SR_MHZ(400),
    SR_MHZ(500),
    SR_MHZ(800),
    SR_GHZ(1),
    // SR_GHZ(2),
    // SR_GHZ(5),
    // SR_GHZ(10),
};

/* hardware Capabilities */
#define CAPS_MODE_LOGIC (1 << 0)
#define CAPS_MODE_ANALOG (1 << 1)
#define CAPS_MODE_DSO (1 << 2)

#define CAPS_FEATURE_NONE 0
// voltage threshold
#define CAPS_FEATURE_VTH (1 << 0)
// with external buffer
#define CAPS_FEATURE_BUF (1 << 1)
// pre offset control
#define CAPS_FEATURE_PREOFF (1 << 2)
// small startup eemprom
#define CAPS_FEATURE_SEEP (1 << 3)
// zero calibration ability
#define CAPS_FEATURE_ZERO (1 << 4)
// use HMCAD1511 adc chip
#define CAPS_FEATURE_HMCAD1511 (1 << 5)
// usb 3.0
#define CAPS_FEATURE_USB30 (1 << 6)
// pogopin panel
#define CAPS_FEATURE_POGOPIN (1 << 7)
// use ADF4360-7 vco chip
#define CAPS_FEATURE_ADF4360 (1 << 8)
// 20M bandwidth limitation
#define CAPS_FEATURE_20M (1 << 9)
// use startup flash (fx3)
#define CAPS_FEATURE_FLASH (1 << 10)
// 32 channels
#define CAPS_FEATURE_LA_CH32 (1 << 11)
// auto tunning vgain
#define CAPS_FEATURE_AUTO_VGAIN (1 << 12)
/* end */

// zero calibration ability
#define CAPS_FEATURE_ZERO (1 << 4)
/* end */

#define USB_INTERFACE_C		0
#define USB_INTERFACE_D		1

static const char *maxHeights[] = {
    "1X",
    "2X",
    "3X",
    "4X",
    "5X",
};

/* We name the probes 0-7 on our demo driver. */
static const char *probe_names[] = {
    "0", "1", "2", "3",
    "4", "5", "6", "7",
    "8", "9", "10", "11",
    "12", "13", "14", "15",
    "16", "17", "18", "19",
    "20", "21", "22", "23",
    "24", "25", "26", "27",
    "28", "29", "30", "31",
    NULL,
};

static const char *probeMapUnits[] = {
    "V",
    "A",
    "°C",
    "°F",
    "g",
    "m",
    "m/s",
};

static const int hwoptions[] = {
    SR_CONF_OPERATION_MODE,
    SR_CONF_MAX_HEIGHT,
    SR_CONF_VTH,
    SR_CONF_EX_TRIGGER_MATCH,
    //SR_CONF_RLE_SUPPORT,
    SR_CONF_FILTER,
    SR_CONF_CLOCK_EDGE,
    SR_CONF_TRIGGER_OUT,
    
	SR_CONF_PWM0_FREQ ,
	SR_CONF_PWM0_DUTY ,
    SR_CONF_PWM0_EN   ,
	//SR_CONF_PWM1_FREQ ,
	//SR_CONF_PWM1_DUTY ,
    //SR_CONF_PWM1_EN   ,
    SR_CONF_STREAM_BUFF,
    
    
};

    // devc->pwm0_en   = 0;
    // devc->pwm0_freq = 1000;
    // devc->pwm0_duty = 50;
    // devc->pwm1_en   = 0;
    // devc->pwm1_freq = 1000;
    // devc->pwm1_duty = 50;
static const int32_t sessions[] = {
    SR_CONF_SAMPLERATE,
    SR_CONF_LIMIT_SAMPLES,
    SR_CONF_OPERATION_MODE,
    SR_CONF_CHANNEL_MODE,
    SR_CONF_MAX_HEIGHT,
    //SR_CONF_RLE_SUPPORT,
    SR_CONF_VTH,
    SR_CONF_EX_TRIGGER_MATCH,
    SR_CONF_FILTER,
    SR_CONF_CLOCK_EDGE,
    SR_CONF_TRIGGER_OUT,
    
	SR_CONF_PWM0_FREQ ,
	SR_CONF_PWM0_DUTY ,
    SR_CONF_PWM0_EN   ,
    SR_CONF_STREAM_BUFF,
	
	//SR_CONF_PWM1_FREQ ,
	//SR_CONF_PWM1_DUTY ,
    //SR_CONF_PWM1_EN   ,
    
    
};






static const struct PX_profile supported_PX[] = {
    /*
     * 32 ch old pid vid
     */
    {0x1A86, 
      0x5237, 
      LIBUSB_SPEED_SUPER,
      0,
      "PX_Tool",
      "PX-Logic U3 channel 32",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x32) | (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC50x32) | (1 << STREAM_LOGIC125x16) | (1 << STREAM_LOGIC250x8) | (1 << STREAM_LOGIC500x4) | (1 << STREAM_LOGIC1000x2) ,
      SR_Gn(4),
      0,
      BUFFER_LOGIC250x32,
      SR_NS(500)
      }
    },
// usb 2.0 stream
 
     {0x1A86, 
      0x5237, 
      LIBUSB_SPEED_HIGH,
      0,
      "PX_Tool",
      "PX-Logic U2 channel 32",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x32) | (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC200x1) | (1 << STREAM_LOGIC100x2) | (1 << STREAM_LOGIC50x4) | (1 << STREAM_LOGIC25x8) | (1 << STREAM_LOGIC10x16) | (1 << STREAM_LOGIC5x32),
      SR_Gn(4),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
    },   

    //32 ch new pid vid
     {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_SUPER,
      0,
      "PX_Tool",
      "PX-Logic U3 channel 32",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x32) | (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC50x32) | (1 << STREAM_LOGIC125x16) | (1 << STREAM_LOGIC250x8) | (1 << STREAM_LOGIC500x4) | (1 << STREAM_LOGIC1000x2) ,
      SR_Gn(4),
      0,
      BUFFER_LOGIC250x32,
      SR_NS(500)
      }
    },
// usb 2.0 stream
 
     {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_HIGH,
      0,
      "PX_Tool",
      "PX-Logic U2 channel 32",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x32) | (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC200x1) | (1 << STREAM_LOGIC100x2) | (1 << STREAM_LOGIC50x4) | (1 << STREAM_LOGIC25x8) | (1 << STREAM_LOGIC10x16) | (1 << STREAM_LOGIC5x32),
      SR_Gn(4),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
    }, 



//16 ch 1G new pid vid
    {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_SUPER,
      1,
      "PX_Tool",
      "PX-Logic U3 channel 16 Pro",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC125x16) | (1 << STREAM_LOGIC250x8) | (1 << STREAM_LOGIC500x4) | (1 << STREAM_LOGIC1000x2) ,
      SR_Gn(4),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
    },
// usb 2.0 stream
 
     {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_HIGH,
      1,
      "PX_Tool",
      "PX-Logic U2 channel 16 Pro",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC500x16)| (1 << BUFFER_LOGIC1000x8) |
      (1 << STREAM_LOGIC200x1) | (1 << STREAM_LOGIC100x2) | (1 << STREAM_LOGIC50x4) | (1 << STREAM_LOGIC25x8) | (1 << STREAM_LOGIC10x16),
      SR_Gn(4),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
     },


//16 ch 500M new pid vid
    {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_SUPER,
      2,
      "PX_Tool",
      "PX-Logic U3 channel 16 Plus",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC500x16)|
      (1 << STREAM_LOGIC125x16) | (1 << STREAM_LOGIC250x8) | (1 << STREAM_LOGIC500x4) ,
      SR_Gn(2),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
    },
// usb 2.0 stream
 
     {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_HIGH,
      2,
      "PX_Tool",
      "PX-Logic U2 channel 16 Plus",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC500x16)|
      (1 << STREAM_LOGIC200x1) | (1 << STREAM_LOGIC100x2) | (1 << STREAM_LOGIC50x4) | (1 << STREAM_LOGIC25x8) | (1 << STREAM_LOGIC10x16),
      SR_Gn(2),
      0,
      BUFFER_LOGIC500x16,
      SR_NS(500)
      }
    }, 

//16 ch 250M new pid vid
    {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_SUPER,
      3,
      "PX_Tool",
      "PX-Logic U3 channel 16 Base",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x16)|
      (1 << STREAM_LOGIC125x16) | (1 << STREAM_LOGIC250x8) ,
      SR_Gn(1),
      0,
      BUFFER_LOGIC250x16,
      SR_NS(500)
      }
    },
// usb 2.0 stream
 
     {0x16C0, 
      0x05DC, 
      LIBUSB_SPEED_HIGH,
      3,
      "PX_Tool",
      "PX-Logic U2 channel 16 Base",
      NULL,
      "SCI_LOGIC.bin",
      FIRMWARE_VERSION,
      "SCI_LOGIC_BL.bin",
      FIRMWARE_BL_VERSION,
      "hspi_ddr.bin",
      "hspi_ddr_RST.bin",
//PX_caps;
     {
      CAPS_MODE_LOGIC,
     // CAPS_FEATURE_NONE|CAPS_FEATURE_USB30|CAPS_FEATURE_LA_CH32|CAPS_FEATURE_BUF,
      CAPS_FEATURE_USB30|CAPS_FEATURE_BUF,
      (1 << BUFFER_LOGIC250x16)|
      (1 << STREAM_LOGIC200x1) | (1 << STREAM_LOGIC100x2) | (1 << STREAM_LOGIC50x4) | (1 << STREAM_LOGIC25x8) | (1 << STREAM_LOGIC10x16),
      SR_Gn(1),
      0,
      BUFFER_LOGIC250x16,
      SR_NS(500)
      }
    }, 

    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,{0, 0, 0, 0, 0, 0, 0}}
};



#endif
