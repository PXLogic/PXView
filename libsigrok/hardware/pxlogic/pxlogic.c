/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2011 Olivier Fauchon <olivier@aixmarseille.com>
 * Copyright (C) 2012 Alexandru Gagniuc <mr.nuke.me@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "pxlogic.h"
#include <math.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "usb_ctrl.h"

#include <inttypes.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define pipe(fds) _pipe(fds, 4096, _O_BINARY)
#endif

#include "../../log.h"

/* Message logging helpers with subsystem-specific prefix string. */

#undef LOG_PREFIX 
#define LOG_PREFIX "px logic: "

/* The size of chunks to send through the session bus. */
/* TODO: Should be configurable. */
//#define BUFSIZE                1024*1024*2

uint32_t                 BUFSIZE = 1024*1024*1;
#define DSO_BUFSIZE            10*1024

// static const struct PX_channels channel_modes[] = {
//     // LA Stream
//     {PX_LOGIC100x16,  LOGIC,  SR_CHANNEL_LOGIC,  32, 1, SR_MHZ(1), SR_Mn(1),
//      SR_KHZ(10), SR_GHZ(1), "Use 32 Channels (Max 1000MHz)"}
// };

#define CHANNEL_MODE_LIST_LEN 10
static struct sr_list_item channel_mode_list[CHANNEL_MODE_LIST_LEN];
enum DSLOGIC_OPERATION_MODE2
{
    /** Buffer mode */
    OP_BUFFER = 0,
    /** Stream mode */
    OP_STREAM = 1,
    /** Internal pattern test mode */
    OP_INTEST = 2,
    /** External pattern test mode */
    OP_EXTEST = 3,
    /** SDRAM loopback test mode */
    OP_LPTEST = 4,
};

static const struct sr_list_item opmode_list[] = {
    {OP_BUFFER,"Buffer Mode"},
    {OP_STREAM,"Stream Mode"},
    //{OP_INTEST,"Internal Test"},
    // {OP_EXTEST,"External Test"},
    // {OP_LPTEST,"DRAM Loopback Test"},
    {-1, NULL},
};
static const struct sr_list_item filter_list[] = {
    {SR_FILTER_NONE, "None"},
    {SR_FILTER_1T,"1 Sample Clock"},
    {-1, NULL},
};

enum pxlogic_extern_edge_modes {
	PX_TRIGGER_CLOSE   ,
	PX_TRIGGER_RISING  ,
	PX_TRIGGER_ONE     ,
	PX_TRIGGER_FALLING ,
	PX_TRIGGER_ZERO    ,
	PX_TRIGGER_EDGE    ,
};

static const struct sr_list_item extern_trigger_matches[] = {
	{PX_TRIGGER_CLOSE   , "close"   },
    {PX_TRIGGER_RISING  , "Rising"  },
    {PX_TRIGGER_ONE     , "One"     },
    {PX_TRIGGER_FALLING , "Falling" },
	{PX_TRIGGER_ZERO    , "Zero"    },
	{PX_TRIGGER_EDGE    , "Edge"    },
    {-1, NULL},
};



static const struct PX_channels channel_modes[] = {
    // LA Stream
    //buff  mode
    {BUFFER_LOGIC250x32,  LOGIC,  SR_CHANNEL_LOGIC,  0,32, 1, SR_MHZ(250), SR_MHZ(250),
     SR_KHZ(2), SR_MHZ(250), "Use 32 Channels (Max 250MHz)"},

    {BUFFER_LOGIC250x16,  LOGIC,  SR_CHANNEL_LOGIC,  0,16, 1, SR_MHZ(250), SR_MHZ(250),
     SR_KHZ(2), SR_MHZ(250), "Use 16 Channels (Max 250MHz)"},

    {BUFFER_LOGIC500x16,  LOGIC,  SR_CHANNEL_LOGIC,  0,16, 1, SR_MHZ(500), SR_MHZ(500),
     SR_KHZ(2), SR_MHZ(500), "Use 16 Channels (Max 500MHz)"},

    {BUFFER_LOGIC1000x8,  LOGIC,  SR_CHANNEL_LOGIC, 0,8, 1, SR_GHZ(1), SR_GHZ(1),
     SR_KHZ(2), SR_GHZ(1), "Use 8 Channels (Max 1000MHz)"},

// usb 3.0 stream mode
    {STREAM_LOGIC50x32,  LOGIC,  SR_CHANNEL_LOGIC,  1,32, 1, SR_MHZ(50), SR_MHZ(50),
     SR_KHZ(2), SR_MHZ(50), "Use 32 Channels (Max50MHz)"},

    {STREAM_LOGIC125x16,  LOGIC,  SR_CHANNEL_LOGIC,  1,16, 1, SR_MHZ(125), SR_MHZ(125),
     SR_KHZ(2), SR_MHZ(125), "Use 16 Channels (Max 125MHz)"},

    {STREAM_LOGIC250x8,  LOGIC,  SR_CHANNEL_LOGIC, 1,8, 1, SR_MHZ(250), SR_MHZ(250),
     SR_KHZ(2), SR_MHZ(250), "Use 8 Channels (Max 250MHz)"},

    {STREAM_LOGIC500x4,  LOGIC,  SR_CHANNEL_LOGIC, 1,4, 1, SR_MHZ(500), SR_MHZ(500),
     SR_KHZ(2), SR_MHZ(500), "Use 4 Channels (Max 500MHz)"},

    {STREAM_LOGIC1000x2,  LOGIC,  SR_CHANNEL_LOGIC, 1,2, 1, SR_MHZ(1000), SR_MHZ(1000), //带宽不够
     SR_KHZ(2), SR_MHZ(1000), "Use 2 Channels (Max 1000MHz)"},


// usb 2.0 stream mode
    {STREAM_LOGIC200x1,  LOGIC,  SR_CHANNEL_LOGIC,  1,1, 1, SR_MHZ(200), SR_MHZ(200),
     SR_KHZ(2), SR_MHZ(200), "Use 1 Channels (Max200MHz)"},

    {STREAM_LOGIC100x2,  LOGIC,  SR_CHANNEL_LOGIC,  1,2, 1, SR_MHZ(100), SR_MHZ(100),
     SR_KHZ(2), SR_MHZ(100), "Use 2 Channels (Max100MHz)"},

    {STREAM_LOGIC50x4,  LOGIC,  SR_CHANNEL_LOGIC,  1,4, 1, SR_MHZ(50), SR_MHZ(50),
     SR_KHZ(2), SR_MHZ(50), "Use 4 Channels (Max50MHz)"},

    {STREAM_LOGIC25x8,  LOGIC,  SR_CHANNEL_LOGIC,  1,8, 1, SR_MHZ(25), SR_MHZ(25),
     SR_KHZ(2), SR_MHZ(25), "Use 8 Channels (Max25MHz)"},

    {STREAM_LOGIC10x16,  LOGIC,  SR_CHANNEL_LOGIC,  1,16, 1, SR_MHZ(10), SR_MHZ(10),
     SR_KHZ(2), SR_MHZ(10), "Use 16 Channels (Max10MHz)"},
     
    {STREAM_LOGIC5x32,  LOGIC,  SR_CHANNEL_LOGIC,  1,32, 1, SR_MHZ(5), SR_MHZ(5),
     SR_KHZ(2), SR_MHZ(5), "Use 32 Channels (Max5MHz)"}

};



static struct sr_list_item channel_mode_cn_map[] = {
    {BUFFER_LOGIC250x32, "使用32个通道(最大采样率 250MHz)"},
    {BUFFER_LOGIC500x16, "使用16个通道(最大采样率 500MHz)"},
    {BUFFER_LOGIC1000x8, "使用8个通道(最大采样率 1000MHz)"},

    {STREAM_LOGIC50x32, "使用32个通道(最大采样率 50MHz)"},
    {STREAM_LOGIC125x16, "使用16个通道(最大采样率 125MHz)"},
    {STREAM_LOGIC250x8, "使用8个通道(最大采样率 250MHz)"},
    {STREAM_LOGIC500x4, "使用4个通道(最大采样率500MHz)"},
    {STREAM_LOGIC1000x2, "使用2个通道(最大采样率 1000MHz)"},


};


static struct lang_text_map_item lang_text_map[] = 
{
	{SR_CONF_OPERATION_MODE, OP_BUFFER, "Buffer Mode", "Buffer模式"},
	{SR_CONF_OPERATION_MODE, OP_STREAM, "Stream Mode", "Stream模式"},
	// {SR_CONF_OPERATION_MODE, OP_INTEST, "Internal Test", "内部测试"},
	// {SR_CONF_OPERATION_MODE, OP_EXTEST, "External Test", "外部测试"},
	// {SR_CONF_OPERATION_MODE, OP_LPTEST, "DRAM Loopback Test", "内存回环测试"},

    // {SR_CONF_BUFFER_OPTIONS, SR_BUF_STOP, "Stop immediately", "立即停止"},
    // {SR_CONF_BUFFER_OPTIONS, SR_BUF_UPLOAD, "Upload captured data", "上传已采集的数据"},

    {SR_CONF_THRESHOLD, SR_TH_3V3, "1.8/2.5/3.3V Level", NULL},
    {SR_CONF_THRESHOLD, SR_TH_5V0, "5.0V Level", NULL},

    {SR_CONF_FILTER, SR_FILTER_NONE, "None", "无"},
    {SR_CONF_FILTER, SR_FILTER_1T, "1 Sample Clock", "1个采样周期"},

    // {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_CLOSE   , "close"   ,"关闭"},
    // {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_RISING  , "Rising"  ,"上升沿"},
    // {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_ONE     , "One"     ,"高电平"},
    // {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_FALLING , "Falling" ,"下降沿"},
	// {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_ZERO    , "Zero"    ,"低电平"},
	// {SR_CONF_EX_TRIGGER_MATCH,PX_TRIGGER_EDGE    , "Edge"    ,"双沿"},
};




/* Private, per-device-instance driver context. */
/* TODO: struct context as with the other drivers. */

/* List of struct sr_dev_inst, maintained by dev_open()/dev_close(). */
SR_PRIV struct sr_dev_driver px_driver_test_info;
static struct sr_dev_driver *di = &px_driver_test_info;

extern struct ds_trigger *trigger;

static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data);
static void finish_acquisition(struct sr_dev_inst *sdi);


static int hw_init(struct sr_context *sr_ctx)
{
	return std_hw_init(sr_ctx, di, LOG_PREFIX);
}

static void adjust_samplerate(struct PX_context *devc)
{
    devc->samplerates_max_index = ARRAY_SIZE(samplerates) - 1;
    while (samplerates[devc->samplerates_max_index] >
           channel_modes[devc->ch_mode].max_samplerate)
        devc->samplerates_max_index--;

    devc->samplerates_min_index = 0;
    while (samplerates[devc->samplerates_min_index] <
           channel_modes[devc->ch_mode].min_samplerate)
        devc->samplerates_min_index++;

    assert(devc->samplerates_max_index >= devc->samplerates_min_index);

    if (devc->cur_samplerate > samplerates[devc->samplerates_max_index])
        devc->cur_samplerate = samplerates[devc->samplerates_max_index];

    if (devc->cur_samplerate < samplerates[devc->samplerates_min_index])
        devc->cur_samplerate = samplerates[devc->samplerates_min_index];
}

static void probe_init(struct sr_dev_inst *sdi)
{
    GSList *l;
    struct PX_context *devc = sdi->priv;

    for (l = sdi->channels; l; l = l->next) {
        struct sr_channel *probe = (struct sr_channel *)l->data;
        probe->bits = channel_modes[devc->ch_mode].unit_bits;
        probe->vdiv = 1000;
        probe->vfactor = 1;
        probe->coupling = SR_AC_COUPLING;
        probe->trig_value = (1 << (probe->bits - 1));
        probe->hw_offset = (1 << (probe->bits - 1));
        probe->offset = probe->hw_offset +
                        (probe->index - (channel_modes[devc->ch_mode].num - 1) /2.0) * (1 << (probe->bits - 2));

        probe->map_default = TRUE;
        probe->map_unit = probeMapUnits[0];
        probe->map_min = -(probe->vdiv * probe->vfactor * DS_CONF_DSO_VDIVS / 2000.0);
        probe->map_max = probe->vdiv * probe->vfactor * DS_CONF_DSO_VDIVS / 2000.0;
    }
}

static int setup_probes(struct sr_dev_inst *sdi, int num_probes)
{
    uint16_t j;
    struct sr_channel *probe;
    struct PX_context *devc = sdi->priv;

    for (j = 0; j < num_probes; j++) {
        if (!(probe = sr_channel_new(j, channel_modes[devc->ch_mode].type,
                                   TRUE, probe_names[j])))
            return SR_ERR;
        sdi->channels = g_slist_append(sdi->channels, probe);
    }
    probe_init(sdi);
    return SR_OK;
}

static struct PX_context *DSLogic_dev_new(const struct PX_profile *prof)
{
    struct PX_context *devc;
    unsigned int i;

    if (!(devc = g_try_malloc(sizeof(struct PX_context)))) {
        sr_err("Device context malloc failed.");
		return NULL;
	}

    for (i = 0; i < ARRAY_SIZE(channel_modes); i++){
       if(channel_modes[i].id != i)
           assert(0);
    }

 sr_info("devc->profile = prof");
    devc->channel = NULL;
    devc->profile = prof;
    devc->ch_mode = devc->profile->dev_caps.default_channelmode;
    devc->cur_samplerate = channel_modes[devc->ch_mode].default_samplerate;
    devc->limit_samples = channel_modes[devc->ch_mode].default_samplelimit;
    devc->limit_samples_show = devc->limit_samples;
    devc->limit_msec = 0;
    devc->timebase = devc->profile->dev_caps.default_timebase;
    devc->max_height = 0;
    devc->op_mode = OP_BUFFER;
    devc->stream = (devc->op_mode != OP_BUFFER);
    devc->test_mode = SR_TEST_NONE;
    devc->rle_mode = FALSE;
    devc->vth = 2.0;
    devc-> ch_num = 16;
    devc->instant = FALSE;
    devc->clock_edge = 0;
    devc->ext_trig_mode = 0;
    devc->trig_out_en = 0;
    devc->filter = 0;

    devc->pwm0_en   = 0;
    devc->pwm0_freq = 1000;
    devc->pwm0_duty = 50;
    devc->pwm1_en   = 0;
    devc->pwm1_freq = 1000;
    devc->pwm1_duty = 50;
    devc->is_loop = 0;

    devc->stream_buff_size = 16;




    adjust_samplerate(devc);
    sr_info("adjust_samplerate");

	return devc;
}

SR_PRIV gboolean logic_check_conf_profile(libusb_device *dev,uint32_t *logic_mode)
{
    struct libusb_device_descriptor des;
    struct libusb_device_handle *hdl;
    int ret;
    gboolean bSucess;
    unsigned char strdesc[64];

    hdl = NULL;
    bSucess = FALSE;
    ret = 0;

    while (!bSucess) {
        /* Assume the FW has not been loaded, unless proven wrong. */
        if ((ret = libusb_get_device_descriptor(dev, &des)) < 0){
            sr_err("%s:%d, Failed to get device descriptor: %s", 
			    __func__, __LINE__, libusb_error_name(ret));
            break;
        }

        if ((ret = libusb_open(dev, &hdl)) < 0){
            sr_err("%s:%d, Failed to open device: %s", 
			    __func__, __LINE__, libusb_error_name(ret));
            // Mybe the device is busy, add it to list.
            return FALSE;
        }

        ret = libusb_claim_interface(hdl, USB_INTERFACE_C);
        ret = libusb_claim_interface(hdl, USB_INTERFACE_D);

        if ((ret = libusb_get_string_descriptor_ascii(hdl,
                des.iManufacturer, strdesc, sizeof(strdesc))) < 0){
            sr_err("%s:%d, Failed to get device descriptor ascii: %s", 
			    __func__, __LINE__, libusb_error_name(ret));
            break;
        }

        if (strncmp((const char *)strdesc, "PX", 2))
            break;

        uint32_t reg_addr;
        uint32_t reg_data;
        int ret = 0;
         //char *res_path = DS_RES_PATH;
        reg_addr = 8192+22*4;
        //ret =  usb_wr_reg(usb->devhdl,reg_addr,0x0);
        ret =  usb_rd_reg(hdl,reg_addr,&reg_data);

        if(ret == 0){
            bSucess = TRUE;
            *logic_mode = reg_data;

        }
        else {
            bSucess = FALSE;
            break;
        }
        // if ((ret = libusb_get_string_descriptor_ascii(hdl,
        //         des.iProduct, strdesc, sizeof(strdesc))) < 0){
        //     sr_err("%s:%d, Failed to get device descriptor ascii: %s", 
		// 	    __func__, __LINE__, libusb_error_name(ret));
        //     break;
        // }

        // if (strncmp((const char *)strdesc, "USB-based DSL Instrument v2", 27))
        //     break;

        /* If we made it here, it must be an dsl device. */
        //bSucess = TRUE;
    }

    if (hdl){
        ret = libusb_release_interface(hdl, USB_INTERFACE_C);
        ret = libusb_release_interface(hdl, USB_INTERFACE_D);
        libusb_close(hdl);
    }

    return bSucess;
}

int dev_destroy(struct sr_dev_inst *sdi);
static GSList *hw_scan(GSList *options)
{
	struct sr_dev_inst *sdi;
	struct drv_context *drvc;
    struct PX_context *devc;
    struct sr_usb_dev_inst *usb;
    struct sr_config *src;
    const struct PX_profile *prof;
	GSList *l, *devices, *conn_devices;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
    libusb_device *device_handle = NULL;
    int devcnt, ret, i, j;
	const char *conn;
    enum libusb_speed usb_speed;
    struct sr_usb_dev_inst *usb_dev_info;
    uint8_t bus;
    uint8_t address;
    int num = 0;
	(void)options;
	drvc = di->priv;
	devices = NULL;

    if (options != NULL)
        sr_info("%s", "Scan ZZY device with options.");
    else 
        sr_info("%s", "Scan ZZY device.");

	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
        case SR_CONF_CONN:
            conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (conn){
        sr_info("%s", "Find usb device with connect config.");
        conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
    }
	else
		conn_devices = NULL;

    /* Find all DSLogic compatible devices and upload firmware to them. */
	devices = NULL;
    devlist = NULL;

    libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
    if (devlist == NULL){
        sr_info("%s: Failed to call libusb_get_device_list(), it returns a null list.", __func__);
        return NULL;
    }
    for (i = 0; devlist[i]; i++) 
    {
        device_handle = devlist[i];

		if (conn) {
			usb = NULL;
			for (l = conn_devices; l; l = l->next) {
				usb = l->data;
				if (usb->bus == libusb_get_bus_number(device_handle)
					&& usb->address == libusb_get_device_address(device_handle))
					break;
			}
			if (!l)
				/* This device matched none of the ones that
				 * matched the conn specification. */
				continue;
		}

		if ((ret = libusb_get_device_descriptor(device_handle, &des)) != 0) {
            sr_warn("Failed to get device descriptor: %s.",
				libusb_error_name(ret));
			continue;
		}

        if (des.idVendor != supported_PX[0].vid && des.idVendor != supported_PX[2].vid)
            continue;


        sr_info("enter libusb_get_device_speed");
        usb_speed = libusb_get_device_speed(device_handle);
        if ((usb_speed != LIBUSB_SPEED_HIGH) && (usb_speed != LIBUSB_SPEED_SUPER)){
            sr_info("usb_speed errr");
            continue;
        }
        else{
            sr_info("enter libusb_get_device_speed = %d",usb_speed);
            sr_info("usb_speed ok");

        }

        /* Check manufactory id and product id, and speed type. */
		prof = NULL;
        for (j = 0; supported_PX[j].vid; j++) {
            if (des.idVendor == supported_PX[j].vid &&
                des.idProduct == supported_PX[j].pid) {
                if (usb_speed == supported_PX[j].usb_speed) {
                    prof = &supported_PX[j];
                    sr_info("Found a PX usb: vid:0x%4x,address:0x%4x", supported_PX[j].vid , supported_PX[j].pid);
                    break;
                }

			}
		}

		/* Skip if the device was not found. */
		if (prof == NULL){
            sr_info("Skip if the device was not found");
            continue;
        }
			
        
        if (sr_usb_device_is_exists(device_handle)){
            sr_detail("Device is exists, handle: %p", device_handle);
            continue;
        }
        
        bus = libusb_get_bus_number(device_handle);
        address = libusb_get_device_address(device_handle);
        sr_info("Found a new device,handle:%p,bus:%d,address:%d", device_handle, bus, address);
        uint32_t logic_mode = 0;
        if (logic_check_conf_profile(device_handle,&logic_mode)) {
            for (j = 0; supported_PX[j].vid; j++) {
                if (des.idVendor == supported_PX[j].vid &&
                    des.idProduct == supported_PX[j].pid) {
                    if (usb_speed == supported_PX[j].usb_speed &&
                        logic_mode == supported_PX[j].logic_mode 
                        ) {
                        prof = &supported_PX[j];
                        sr_info("Found a PX usb: vid:0x%4x,address:0x%4x", supported_PX[j].vid , supported_PX[j].pid);
                        break;
                    }

                }
            }

            devc = DSLogic_dev_new(prof);
            devc ->usb_speed = usb_speed;
            sr_info("DSLogic_dev_new");
            if (!devc)
                break;
                //return NULL;
            
            sdi = sr_dev_inst_new(channel_modes[devc->ch_mode].mode, SR_ST_INITIALIZING,
                                prof->vendor, prof->model, prof->model_version);
            if (sdi == NULL) {
                g_free(devc);
                sr_info("sr_dev_inst_new error");
                break;
                //return NULL;
            }
            sdi->priv = devc;
            sdi->driver = di;
            sdi->dev_type = DEV_TYPE_USB;
            sdi->handle = (ds_device_handle)device_handle;
    
            /* Fill in probelist according to this device's profile. */
            if (setup_probes(sdi, channel_modes[devc->ch_mode].num) != SR_OK){
                sr_err("%s", "eng_setup_probes() error");
                dev_destroy(sdi);
                break;
                //return NULL;
            }


            /* Already has the firmware, so fix the new address. */
            sr_info("Found a device,name:\"%s\",handle:%p", prof->model,device_handle);
            usb_dev_info = sr_usb_dev_inst_new(bus, address);
            usb_dev_info->usb_dev = device_handle;
            sdi->conn = usb_dev_info;
            sdi->status = SR_ST_INACTIVE;          

            devices = g_slist_append(devices, sdi);
            sr_info("enter eng_check_conf_profile");
        }

	}

	libusb_free_device_list(devlist, 0);

    if (conn_devices){
        g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);
    }


	return devices;
}

static const GSList *hw_dev_mode_list(const struct sr_dev_inst *sdi)
{
    struct PX_context *devc;
    GSList *l = NULL;
    unsigned int i;

    devc = sdi->priv;
    for (i = 0; i < ARRAY_SIZE(sr_mode_list); i++) {
        if (devc->profile->dev_caps.mode_caps & (1 << i))
            l = g_slist_append(l, &sr_mode_list[i]);
    }

    return l;
}

SR_PRIV int firmware_config(struct libusb_device_handle *usbdevh, const char *filename,unsigned int mode)
{
    FILE *fw;
    int chunksize, ret;
    unsigned char *buf;
    int transferred;
    uint64_t filesize;

	struct stat f_stat;
    unsigned int base_addr;
    int length;

    sr_info("Configure FPGA using \"%s\"", filename);
    if ((fw = fopen(filename, "rb")) == NULL) {
        sr_err("Unable to open FPGA bit file %s for reading: %s",
               filename, strerror(errno));
        ds_set_last_error(SR_ERR_FIRMWARE_NOT_EXIST);
        return SR_ERR;
    }
	
    if (stat(filename, &f_stat) == -1){
        fclose(fw);
        return SR_ERR;
    }

    filesize = (uint64_t)f_stat.st_size;

    if(mode == 0){
        if ((buf = x_malloc(48*4*1024)) == NULL) {
            sr_err("wch569 app configure buf malloc failed.");
            fclose(fw);
            return SR_ERR;
        }

    }
    else if(mode == 2){
        if ((buf = x_malloc(48*4*1024)) == NULL) {
            sr_err("wch569 bl configure buf malloc failed.");
            fclose(fw);
            return SR_ERR;
        }

    }
    else{
        if ((buf = x_malloc(filesize*2)) == NULL) {
            sr_err("FPGA configure buf malloc failed.");
            fclose(fw);
            return SR_ERR;
        }

    }

//ch569w
if(mode == 0){
    base_addr = 48*1024;
    length = 48*1024;
    fread(buf, 1, filesize, fw);
    memset(buf+filesize,0xff,length-filesize);
    memcpy(buf+length,buf,length);
    //memset(buf+filesize+length,0xff,length-filesize);
    memcpy(buf+length*2,buf,length);


    length =length*3;
    libusb_clear_halt(usbdevh,0x03);
    ret =  usb_wr_data_update(usbdevh,base_addr,length,0,buf,0);
    //g_usleep(10* 1000);
    // base_addr = 96*1024;
    // ret =  usb_wr_data_update(usbdevh,base_addr,length,0,buf,1000);

}
else if(mode == 2){
    base_addr = 0;
    length = 32*1024;
    fread(buf, 1, filesize, fw);
    memset(buf+filesize,0xff,length-filesize);
    memcpy(buf+length,buf,length);

    length =length;
    libusb_clear_halt(usbdevh,0x03);
    ret =  usb_wr_data_update(usbdevh,base_addr,length,0,buf,0);
    //g_usleep(10* 1000);
    // base_addr = 96*1024;
    // ret =  usb_wr_data_update(usbdevh,base_addr,length,0,buf,1000);

}
else if(mode == 1){ //fpga
    base_addr = 0;
    //length = filesize*2;
    length = filesize;
    fread(buf, 1, filesize, fw);
    libusb_clear_halt(usbdevh,0x03);
    ret =  usb_wr_data_update(usbdevh,base_addr,length,4,buf,0);
    if(ret!=0){
        sr_err("FPGA configure usb_wr_data_update error");
    }

}

//unsigned int usb_wr_data_update(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout)

    fclose(fw);
    x_free(buf);

    if (ret != SR_OK){

		return SR_ERR;
    }



    sr_info("FPGA configure done: %d bytes.", filesize);
    return SR_OK;
}

static int hw_usb_open(struct sr_dev_driver *di, struct sr_dev_inst *sdi, gboolean *fpga_done)
{
    libusb_device *dev_handel=NULL;
    struct sr_usb_dev_inst *usb;
    struct PX_context *devc;
    struct drv_context *drvc;
    int ret, skip, i, device_count;

    drvc = di->priv;
    devc = sdi->priv;
    usb = sdi->conn;
  
    if (usb->usb_dev == NULL){
        sr_err("%s", "hw_dev_open(), usb->usb_dev is null.");
        return SR_ERR;
    }

    if (sdi->status == SR_ST_ACTIVE) {
        /* Device is already in use. */
        sr_detail("The usb device is opened, handle:%p", usb->usb_dev);
        return SR_OK;
    }

    if (sdi->status == SR_ST_INITIALIZING) {
        sr_info("%s", "The device instance is still boosting.");        
    }
    dev_handel = usb->usb_dev;

    sr_info("Open usb device instance, handle: %p", dev_handel);

    if (libusb_open(dev_handel, &usb->devhdl) != 0){
        sr_err("Failed to open device: %s, handle:%p",
                libusb_error_name(ret), dev_handel);
        return SR_ERR;
    }

    ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE_C);
    ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE_D);
    // ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE_D+1);
    // ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE_D+2);

    if (usb->address == 0xff){
        /*
        * First time we touch this device after FW
        * upload, so we don't know the address yet.
        */
        usb->address = libusb_get_device_address(dev_handel);
    }
     //sdi->status = SR_ST_ACTIVE;
     
    //if (sdi->status == SR_ST_ACTIVE) {
        if (1) {
        uint32_t reg_addr;
        uint32_t reg_data;
         //char *res_path = DS_RES_PATH;
        reg_addr = 8192+13*4;
        //ret =  usb_wr_reg(usb->devhdl,reg_addr,0x0);
        ret =  usb_rd_reg(usb->devhdl,reg_addr,&reg_data);
       if(ret == 0){
            sr_info("current   firmware_version = %x   new firmware_version = %x",reg_data,devc->profile->firmware_version);
            if(reg_data == devc->profile->firmware_bl_version && PXVIEW_BL_EN == 1){
                char *firmware;
                char *res_path = DS_RES_PATH;
                sr_info(" open cpu firmware file %s ", res_path);
                if (!(firmware = x_malloc(strlen(res_path)+strlen(devc->profile->firmware_bl) + 5))) {
                    sr_err("firmware  path malloc error!");
                    return SR_ERR_MALLOC;
                }
                strcpy(firmware, res_path);
                strcat(firmware, "/");
                strcat(firmware, devc->profile->firmware_bl);
                sr_info(" open bl bin file %s ", firmware);
                ret = firmware_config(usb->devhdl, firmware,2);
                x_free(firmware);
                sr_info("firmware  end");
                //rst usb

            }

            if(reg_data != devc->profile->firmware_version){
                char *firmware;
                char *res_path = DS_RES_PATH;
                sr_info(" open cpu firmware file %s ", res_path);
                if (!(firmware = x_malloc(strlen(res_path)+strlen(devc->profile->firmware) + 5))) {
                    sr_err("firmware  path malloc error!");
                    return SR_ERR_MALLOC;
                }
                strcpy(firmware, res_path);
                strcat(firmware, "/");
                strcat(firmware, devc->profile->firmware);
                sr_info(" open app bin file %s ", firmware);
                ret = firmware_config(usb->devhdl, firmware,0);
                x_free(firmware);
                sr_info("firmware  end");
                //rst usb
                sr_info("rst usb ");
                reg_addr = 8192+12*4;
                reg_data = 0;
                ret =  usb_wr_reg(usb->devhdl,reg_addr,reg_data);
                sdi->status = SR_ST_INITIALIZING;

                return SR_ERR_DEVICE_CLOSED;

            }
            sdi->status = SR_ST_ACTIVE;

       }
    }

 if (sdi->status == SR_ST_ACTIVE) {
        if (!(*fpga_done)) {




            sr_info("fpag_bit start");
            char *fpga_bit;
            char *res_path = DS_RES_PATH;
            // char *res_path = "/usr/local/share/PXView";

            char *fpga_rst_bit;
            if (!(fpga_rst_bit = x_malloc(strlen(res_path)+strlen(devc->profile->fpga_rst_bit) + 5))) {
                sr_err("fpag_bit path malloc error!");
                return SR_ERR_MALLOC;
            }
            
            sr_info(" open FPGA bit file %s ", res_path);
            if (!(fpga_bit = x_malloc(strlen(res_path)+strlen(devc->profile->fpga_bit) + 5))) {
                sr_err("fpag_bit path malloc error!");
                return SR_ERR_MALLOC;
            }
            strcpy(fpga_rst_bit, res_path);
            strcat(fpga_rst_bit, "/");
            strcat(fpga_rst_bit, devc->profile->fpga_rst_bit);

            ret = firmware_config(usb->devhdl, fpga_rst_bit,1);
            //sr_info(" open FPGA bit file %s ", fpga_bit);

            strcpy(fpga_bit, res_path);
            strcat(fpga_bit, "/");
            strcat(fpga_bit, devc->profile->fpga_bit);

            sr_info(" open FPGA bit file %s ", fpga_bit);

            ret = firmware_config(usb->devhdl, fpga_bit,1);
            x_free(fpga_bit);
            *fpga_done = 1;
             sr_info("fpag_bit end");
            }
       

    
    }
    
    return SR_OK;
}





static int hw_dev_open(struct sr_dev_inst *sdi)
{
    //(void)sdi;
    struct PX_context *const devc = sdi->priv;
    gboolean fpga_done = 0;

    if(sdi->status != SR_ST_ACTIVE){
        fpga_done = 0;
    }

    hw_usb_open(di,sdi,&fpga_done);
    //sdi->status = SR_ST_ACTIVE;
    sr_info("hw_dev_open");
    //if (pipe(devc->pipe_fds)) {
    //    /* TODO: Better error message. */
    //    sr_err("%s: pipe() failed", __func__);
    //    return SR_ERR;
    //}
    //devc->channel = g_io_channel_unix_new(devc->pipe_fds[0]);
    //g_io_channel_set_flags(devc->channel, G_IO_FLAG_NONBLOCK, NULL);
    ///* Set channel encoding to binary (default is UTF-8). */
    //g_io_channel_set_encoding(devc->channel, NULL, NULL);
    ///* Make channels to unbuffered. */
    //g_io_channel_set_buffered(devc->channel, FALSE);

	return SR_OK;
}

SR_PRIV int hw_usb_close(struct sr_dev_inst *sdi)
{
    struct sr_usb_dev_inst *usb;

    usb = sdi->conn;
    if (usb->devhdl == NULL){
        sr_detail("%s", "eng_dev_close(),libusb_device_handle is null.");
        return SR_ERR;
    }

    sr_info("%s: Closing device %d on %d.%d interface %d.",
        sdi->driver->name, sdi->index, usb->bus, usb->address, USB_INTERFACE_C);
    
   libusb_release_interface(usb->devhdl, USB_INTERFACE_C);
   libusb_release_interface(usb->devhdl, USB_INTERFACE_D);
//    libusb_release_interface(usb->devhdl, USB_INTERFACE_D+1);
//    libusb_release_interface(usb->devhdl, USB_INTERFACE_D+2);
   libusb_close(usb->devhdl);
    usb->devhdl = NULL;

    return SR_OK;
}

static int hw_dev_close(struct sr_dev_inst *sdi)
{
    //(void)sdi;
    struct PX_context * devc = sdi->priv;
    //hw_dev_acquisition_stop(sdi,NULL);
    sr_info("hw_dev_close");
    hw_usb_close(sdi);
    // if (sdi->status == SR_ST_ACTIVE && devc->channel) {
    //     g_io_channel_shutdown(devc->channel, FALSE, NULL);
    //     g_io_channel_unref(devc->channel);
    //     devc->channel = NULL;
    // }
    sdi->status = SR_ST_INACTIVE;
    //g_free(devc);
    
    return SR_OK;
}

//static int dev_destroy(struct sr_dev_inst *sdi)
int dev_destroy(struct sr_dev_inst *sdi)
{
     assert(sdi);

    struct sr_dev_driver *driver;
    driver = sdi->driver;

    //hw_dev_close(sdi);
    if (driver->dev_close){
		driver->dev_close(sdi);
    }

    if (sdi->conn) {
        if (sdi->dev_type == DEV_TYPE_USB)
            sr_usb_dev_inst_free(sdi->conn);
        else if (sdi->dev_type == DEV_TYPE_SERIAL)
            sr_serial_dev_inst_free(sdi->conn);
    }
    //hw_dev_close(sdi);
    sr_dev_inst_free(sdi);
    return SR_OK;
}

static int hw_cleanup(void)
{ 
    safe_free(di->priv);
	return SR_OK;
}

static unsigned int en_ch_num_mask(const struct sr_dev_inst *sdi)
{
    GSList *l;
    unsigned int channel_en_mask = 0;
     unsigned int i = 0;

    for (l = sdi->channels; l; l = l->next) {
        struct sr_channel *probe = (struct sr_channel *)l->data;
        channel_en_mask =   channel_en_mask |  (probe->enabled << i);
        i++;
    }

    return channel_en_mask;
}

static unsigned int en_ch_num(const struct sr_dev_inst *sdi)
{
    GSList *l;
    unsigned int channel_en_cnt = 0;

    for (l = sdi->channels; l; l = l->next) {
        struct sr_channel *probe = (struct sr_channel *)l->data;
        channel_en_cnt += probe->enabled;
    }

    return channel_en_cnt;
}

static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch,
                      const struct sr_channel_group *cg)
{
    (void) cg;

    struct PX_context *devc;

    assert(sdi);
    assert(sdi->priv);

    devc = sdi->priv;
   devc-> ch_num = en_ch_num(sdi);
	switch (id) {
    case SR_CONF_OPERATION_MODE:
        *data = g_variant_new_int16(devc->op_mode);
        break;

    case SR_CONF_EX_TRIGGER_MATCH:
        *data = g_variant_new_int16(devc->ext_trig_mode);
        break;

    case SR_CONF_CHANNEL_MODE:
        *data = g_variant_new_int16(devc->ch_mode);
        break;
    // case SR_CONF_RLE:
    //     *data = g_variant_new_boolean(devc->rle_mode);
    //     sr_info("config_get   SR_CONF_RLE");
    //     break;
    // case SR_CONF_RLE_SUPPORT:
    //     *data = g_variant_new_boolean(devc->rle_support);
    //     sr_info("config_get   SR_CONF_RLE_SUPPORT");
    //     if(devc->rle_support == TRUE){
    //         sr_info("rle_support   TRUE");
    //     }
    //     else{
    //         sr_info("rle_support   FLASE");
    //     }
    //     break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
        *data = g_variant_new_uint64(devc->limit_samples_show);
		break;
	case SR_CONF_LIMIT_MSEC:
		*data = g_variant_new_uint64(devc->limit_msec);
		break;
    case SR_CONF_DEVICE_MODE:
        *data = g_variant_new_int16(sdi->mode);
        break;
    case SR_CONF_TEST:
    //sr_info("config_get   SR_CONF_TEST  all");
        *data = g_variant_new_boolean(FALSE);
        break;
    case SR_CONF_INSTANT:
        *data = g_variant_new_boolean(devc->instant);
        break;
    //case SR_CONF_PATTERN_MODE:
    //sr_info("config_get   SR_CONF_PATTERN_MODE====logic");
    //    *data = g_variant_new_string(pattern_strings[devc->sample_generator]);
	//	break;
    case SR_CONF_MAX_HEIGHT:
        *data = g_variant_new_string(maxHeights[devc->max_height]);
        break;
    case SR_CONF_MAX_HEIGHT_VALUE:
        *data = g_variant_new_byte(devc->max_height);
        break;
    case SR_CONF_HW_DEPTH:
        if(devc->op_mode == OP_BUFFER){
            *data = g_variant_new_uint64(devc->profile->dev_caps.hw_depth / channel_modes[devc->ch_mode].unit_bits/devc-> ch_num); 
        }
        else if(devc->op_mode == OP_STREAM){
            //*data = g_variant_new_uint64(devc->profile->dev_caps.hw_depth *4*8/ channel_modes[devc->ch_mode].unit_bits/devc-> ch_num); 
            *data = g_variant_new_uint64(devc->stream_buff_size*1024*1024*1024*8/ channel_modes[devc->ch_mode].unit_bits/devc-> ch_num); 
            
        }
        else{
            *data = g_variant_new_uint64(devc->profile->dev_caps.hw_depth / channel_modes[devc->ch_mode].unit_bits/devc-> ch_num); 
        }
        break;
    case SR_CONF_VLD_CH_NUM:
        *data = g_variant_new_int16(channel_modes[devc->ch_mode].num);
        break;
    case SR_CONF_USB_SPEED:
        if (!sdi)
            return SR_ERR;
        //*data = g_variant_new_int32(devc->profile->usb_speed);
        *data = g_variant_new_int32(devc->usb_speed);
        break;
    case SR_CONF_USB30_SUPPORT:
        if (!sdi)
            return SR_ERR;

        if(devc->usb_speed == LIBUSB_SPEED_SUPER){
            *data = g_variant_new_boolean((devc->profile->dev_caps.feature_caps & CAPS_FEATURE_USB30) != 0);
        }
        else{
                //*data = g_variant_new_boolean(0);
                *data = g_variant_new_boolean((devc->profile->dev_caps.feature_caps & 0) != 0);
        }
        break;
        // case SR_CONF_LA_CH32:
        // sr_info("config_get   SR_CONF_VLD_CH_NUM====logic");
        // if (!sdi)
        //     return SR_ERR;
        // *data = g_variant_new_boolean((devc->profile->dev_caps.feature_caps & CAPS_FEATURE_LA_CH32) != 0);
        // break;
        // case SR_CONF_TOTAL_CH_NUM:
        //     //*data = g_variant_new_int16(devc->profile->dev_caps.total_ch_num);
        //     *data = g_variant_new_int16(32);
        //     break;

        case SR_CONF_VTH:
            *data = g_variant_new_double(devc->vth);
        break;

        case SR_CONF_CLOCK_EDGE:
            *data = g_variant_new_boolean(devc->clock_edge);
        break;
        case SR_CONF_TRIGGER_OUT:
            *data = g_variant_new_boolean(devc->trig_out_en);
        break;
        case SR_CONF_FILTER:
            *data = g_variant_new_int16(devc->filter);
        break;

        case SR_CONF_PWM0_EN:
            *data = g_variant_new_boolean(devc->pwm0_en);
        break;
        case SR_CONF_PWM0_FREQ:
            *data = g_variant_new_double(devc->pwm0_freq);
        break;
        case SR_CONF_PWM0_DUTY:
            *data = g_variant_new_double(devc->pwm0_duty);
        break;

        case SR_CONF_PWM1_EN:
            *data = g_variant_new_boolean(devc->pwm1_en);
        break;
        case SR_CONF_PWM1_FREQ:
            *data = g_variant_new_double(devc->pwm1_freq);
        break;
        case SR_CONF_PWM1_DUTY:
            *data = g_variant_new_double(devc->pwm1_duty);
        break;
        case SR_CONF_STREAM_BUFF:
            *data = g_variant_new_double(devc->stream_buff_size);
        break;

        case SR_CONF_STREAM:
            *data = g_variant_new_boolean(devc->stream);
        break;
        

    default:
		return SR_ERR_NA;
	}

	return SR_OK;
}


SR_PRIV int sci_adjust_probes(struct sr_dev_inst *sdi, int num_probes)
{
    uint16_t j;
    struct sr_channel *probe;
    struct PX_context *devc = sdi->priv;
    GSList *l;

    assert(num_probes > 0);

    j = g_slist_length(sdi->channels);
    while(j < num_probes) {
        if (!(probe = sr_channel_new(j, channel_modes[devc->ch_mode].type,
                                   TRUE, probe_names[j])))
            return SR_ERR;
        sdi->channels = g_slist_append(sdi->channels, probe);
        j++;
    }

    while(j > num_probes) {
        sdi->channels = g_slist_delete_link(sdi->channels, g_slist_last(sdi->channels));
        j--;
    }

    for(l = sdi->channels; l; l = l->next) {
        probe = (struct sr_channel *)l->data;
        probe->enabled = TRUE;
        probe->type = channel_modes[devc->ch_mode].type;
    }
    return SR_OK;
}

static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch,
                      struct sr_channel_group *cg)
{
    (void) cg;

    uint16_t i,nv;
    int ret, num_probes;
	const char *stropt;
    uint64_t tmp_u64;
    struct PX_context *devc;
    struct sr_usb_dev_inst *usb;

    assert(sdi);
    assert(sdi->priv);
    
    devc = sdi->priv;
    usb = sdi->conn;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEVICE_CLOSED;
    ret = SR_OK;
	if (id == SR_CONF_SAMPLERATE) {
		devc->cur_samplerate = g_variant_get_uint64(data);
        devc->samples_counter = 0;
        devc->pre_index = 0;
		sr_dbg("%s: setting samplerate to %llu", __func__,
		       devc->cur_samplerate);
		ret = SR_OK;
	}
    else if (id == SR_CONF_LIMIT_SAMPLES) {
        devc->limit_msec = 0;
        devc->limit_samples = g_variant_get_uint64(data);
        devc->limit_samples = (devc->limit_samples + 63) & ~63;
        devc->limit_samples_show = devc->limit_samples;
        if (sdi->mode == DSO && en_ch_num(sdi) == 1) {
            devc->limit_samples /= 2;
        }
		sr_dbg("%s: setting limit_samples to %llu", __func__,
		       devc->limit_samples);
		ret = SR_OK;
	} 
    else if (id == SR_CONF_LIMIT_MSEC) {
		devc->limit_msec = g_variant_get_uint64(data);
		devc->limit_samples = 0;
        devc->limit_samples_show = devc->limit_samples;
		sr_dbg("%s: setting limit_msec to %llu", __func__,
		       devc->limit_msec);
        ret = SR_OK;
    } 
    else if (id == SR_CONF_DEVICE_MODE) {
        sdi->mode = g_variant_get_int16(data);
        
        if (sdi->mode == LOGIC) {
            for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                if ((int)channel_modes[i].mode == sdi->mode &&
                    devc->profile->dev_caps.channels & (1 << i)) {
                    devc->ch_mode = channel_modes[i].id;
                    break;
                }
            }
            num_probes = channel_modes[devc->ch_mode].num;
            devc->cur_samplerate = channel_modes[devc->ch_mode].default_samplerate;
            devc->limit_samples = channel_modes[devc->ch_mode].default_samplelimit;
            devc->limit_samples_show = devc->limit_samples;
            devc->timebase = devc->profile->dev_caps.default_timebase;
            sr_dev_probes_free(sdi);
            setup_probes(sdi, num_probes);
            adjust_samplerate(devc);
            sr_info("%s: setting mode to %d", __func__, sdi->mode);
            ret = SR_OK;
        }
        else {
            ret = SR_ERR;
        }
    }
   //else if (id == SR_CONF_PATTERN_MODE) {
   //    sr_info("config_set   SR_CONF_PATTERN_MODE====logic");
   //    stropt = g_variant_get_string(data, NULL);
   //    ret = SR_OK;
   //    if (!strcmp(stropt, pattern_strings[PATTERN_SINE])) {
   //        devc->sample_generator = PATTERN_SINE;
   //    } else if (!strcmp(stropt, pattern_strings[PATTERN_SQUARE])) {
   //        devc->sample_generator = PATTERN_SQUARE;
   //    } else if (!strcmp(stropt, pattern_strings[PATTERN_TRIANGLE])) {
   //        devc->sample_generator = PATTERN_TRIANGLE;
   //    } else if (!strcmp(stropt, pattern_strings[PATTERN_SAWTOOTH])) {
   //        devc->sample_generator = PATTERN_SAWTOOTH;
   //    } else if (!strcmp(stropt, pattern_strings[PATTERN_RANDOM])) {
   //        devc->sample_generator = PATTERN_RANDOM;
	//	} else {
   //        ret = SR_ERR;
	//	}
   //    sr_dbg("%s: setting pattern to %d",
	//		__func__, devc->sample_generator);
   //}
    else if (id == SR_CONF_MAX_HEIGHT) {
        stropt = g_variant_get_string(data, NULL);
        ret = SR_OK;
        for (i = 0; i < ARRAY_SIZE(maxHeights); i++) {
            if (!strcmp(stropt, maxHeights[i])) {
                devc->max_height = i;
                break;
            }
        }
        sr_dbg("%s: setting Signal Max Height to %d",
            __func__, devc->max_height);
    }
    else if (id == SR_CONF_INSTANT) {
        devc->instant = g_variant_get_boolean(data);
        sr_dbg("%s: setting INSTANT mode to %d", __func__,
               devc->instant);
        ret = SR_OK;
    }
    else if (id == SR_CONF_OPERATION_MODE) {
        ret = SR_OK;
        nv = g_variant_get_int16(data);

        if (sdi->mode == LOGIC && devc->op_mode != nv) 
        //if (sdi->mode == LOGIC ) 
        {
            if (nv == OP_BUFFER) {
                devc->op_mode = OP_BUFFER;
                devc->test_mode = SR_TEST_NONE;
                devc->stream = FALSE;
                
                for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                    if (channel_modes[i].mode == LOGIC &&
                        channel_modes[i].stream == devc->stream &&
                        devc->profile->dev_caps.channels & (1 << i)) {
                        devc->ch_mode = channel_modes[i].id;
                        break;
                    }
                }
            } 
            else if (nv == OP_STREAM) {
                devc->op_mode = OP_STREAM;
                devc->test_mode = SR_TEST_NONE;
                devc->stream = TRUE;
                
                for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                    if (channel_modes[i].mode == LOGIC &&
                        channel_modes[i].stream == devc->stream &&
                        devc->profile->dev_caps.channels & (1 << i)) {
                        devc->ch_mode = channel_modes[i].id;
                        break;
                    }
                }
            } 
            else if (nv == OP_INTEST) {
                devc->op_mode = OP_INTEST;
                devc->test_mode = SR_TEST_INTERNAL;
                devc->ch_mode = devc->profile->dev_caps.intest_channel;
                devc->stream = !(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_BUF);
            } 
            else {
                ret = SR_ERR;
            }

             
             
             sci_adjust_probes(sdi, channel_modes[devc->ch_mode].num);
             adjust_samplerate(devc);

            if (devc->op_mode == OP_INTEST) {
                // devc->cur_samplerate = devc->stream ? channel_modes[devc->ch_mode].max_samplerate / 10 :
                //                                       SR_MHZ(100);
                // devc->limit_samples = devc->stream ? devc->cur_samplerate * 3 :
                //                                      devc->profile->dev_caps.hw_depth / dsl_en_ch_num(sdi);
            }
        }
        sr_dbg("%s: setting pattern to %d",
            __func__, devc->op_mode);
    } 
    else if(id == SR_CONF_EX_TRIGGER_MATCH){
        ret = SR_OK;
        devc->ext_trig_mode = g_variant_get_int16(data);
    }
    else if (id == SR_CONF_CHANNEL_MODE) {
        ret = SR_OK;
        nv = g_variant_get_int16(data);
        if (sdi->mode == LOGIC) {
            for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                if (devc->profile->dev_caps.channels & (1 << i)) {
                    if (channel_modes[i].id == nv) {
                        devc->ch_mode =  nv;
                        break;
                    }
                }
            }
           
            
             sci_adjust_probes(sdi, channel_modes[devc->ch_mode].num);
             adjust_samplerate(devc);
        }
        sr_dbg("%s: setting channel mode to %d",
            __func__, devc->ch_mode);
    }
    // else if (id == SR_CONF_RLE) {
    //     ret = SR_OK;
    //     devc->rle_mode = g_variant_get_boolean(data);
    //      sr_info("config_set   SR_CONF_RLE");
    // } 
    // else if (id == SR_CONF_RLE_SUPPORT) {
    //     devc->rle_support = g_variant_get_boolean(data);
        
    //     sr_info("config_set   SR_CONF_RLE_SUPPORT");
    //     // if(devc->rle_support == TRUE){
    //     //         sr_info("rle_support   TRUE");
    //     // }
    //     // else{
    //     //         sr_info("rle_support   FLASE");
    //     // }
    // } 
    else if (id == SR_CONF_VTH) {
        ret = SR_OK;
        devc->vth = g_variant_get_double(data);
    }
    else if (id == SR_CONF_CLOCK_EDGE) {
        devc->clock_edge = g_variant_get_boolean(data);
    } 
    else if (id == SR_CONF_TRIGGER_OUT) {
        devc->trig_out_en = g_variant_get_boolean(data);
    }
    else if (id == SR_CONF_FILTER) {
        nv = g_variant_get_int16(data);
        if (nv == SR_FILTER_NONE || nv == SR_FILTER_1T) 
            devc->filter = nv;
        else 
            ret = SR_ERR;
        
        sr_dbg("%s: setting filter to %d",
            __func__, devc->filter);
    }
    else if (id == SR_CONF_PWM0_EN) {
        devc->pwm0_en = g_variant_get_boolean(data);
    } 
    else if (id == SR_CONF_PWM0_FREQ) {
        ret = SR_OK;
        devc->pwm0_freq = g_variant_get_double(data);
        devc->pwm0_freq_set = (uint32_t)((double)PWM_CLK/devc->pwm0_freq);
        sr_dbg("pwm0_freq_set =  %d", devc->pwm0_freq_set);
        devc->pwm0_freq = (double)PWM_CLK/(double)devc->pwm0_freq_set;

    }
    else if (id == SR_CONF_PWM0_DUTY) {
        ret = SR_OK;
        devc->pwm0_duty = g_variant_get_double(data);
        //devc->pwm0_duty_set = (uint32_t)(devc->pwm0_freq_set*(uint32_t)devc->pwm0_duty/100);
        devc->pwm0_duty_set = (uint32_t)((double)devc->pwm0_freq_set*devc->pwm0_duty/100);
        sr_dbg("pwm0_duty_set =  %d", devc->pwm0_duty_set);
        devc->pwm0_duty = (double)devc->pwm0_duty_set*100/(double)devc->pwm0_freq_set;

        usb_wr_reg(usb->devhdl,16<<2,0);
        usb_wr_reg(usb->devhdl,17<<2,devc->pwm0_freq_set-1);
        usb_wr_reg(usb->devhdl,18<<2,devc->pwm0_duty_set-1);
        usb_wr_reg(usb->devhdl,16<<2,(uint32_t)devc->pwm0_en);

    }
    else if (id == SR_CONF_PWM1_EN) {
        devc->pwm1_en = g_variant_get_boolean(data);
        usb_wr_reg(usb->devhdl,16<<2,(uint32_t)devc->pwm0_en);



    } 
    else if (id == SR_CONF_PWM1_FREQ) {
        ret = SR_OK;
        devc->pwm1_freq = g_variant_get_double(data);
        devc->pwm1_freq_set = (uint32_t)((double)PWM_CLK/devc->pwm1_freq);
        sr_dbg("pwm1_freq_set =  %d", devc->pwm1_freq_set);
        devc->pwm1_freq = (double)PWM_CLK/(double)devc->pwm1_freq_set;
    }
    else if (id == SR_CONF_PWM1_DUTY) {
        ret = SR_OK;
        devc->pwm1_duty = g_variant_get_double(data);
        devc->pwm1_duty_set = (uint32_t)(devc->pwm1_freq_set*(uint32_t)devc->pwm1_duty/100);
        sr_dbg("pwm1_duty_set =  %d", devc->pwm1_duty_set);
        devc->pwm1_duty = (double)devc->pwm1_duty_set*100/(double)devc->pwm1_freq_set;

        usb_wr_reg(usb->devhdl,19<<2,0);
        usb_wr_reg(usb->devhdl,20<<2,devc->pwm1_freq_set-1);
        usb_wr_reg(usb->devhdl,21<<2,devc->pwm1_duty_set-1);
        usb_wr_reg(usb->devhdl,19<<2,(uint32_t)devc->pwm1_en);
    }
    else if (id == SR_CONF_LOOP_MODE){
        devc->is_loop = g_variant_get_boolean(data);
        sr_dbg("Set device loop mode:%d", devc->is_loop);
    } 
    else if (id == SR_CONF_STREAM_BUFF) {
        ret = SR_OK;
        devc->stream_buff_size = g_variant_get_double(data);
    }
    else {
        ret = SR_ERR_NA;
	}

	return ret;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg)
{
    struct PX_context *devc;
	GVariant *gvar;
	GVariantBuilder gvb;
    int i;
    int num;

    (void)cg;
    devc = sdi->priv;

	switch (key) {
    case SR_CONF_DEVICE_OPTIONS:
//		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
//				hwcaps, ARRAY_SIZE(hwcaps), sizeof(int32_t));
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                hwoptions, ARRAY_SIZE(hwoptions)*sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_DEVICE_SESSIONS:
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                sessions, ARRAY_SIZE(sessions)*sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_SAMPLERATE:
		g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
        gvar = g_variant_new_from_data(G_VARIANT_TYPE("at"),
                                       samplerates + devc->samplerates_min_index,
                                       (devc->samplerates_max_index - devc->samplerates_min_index + 1) * sizeof(uint64_t), TRUE, NULL, NULL);
        g_variant_builder_add(&gvb, "{sv}", "samplerates", gvar);
		*data = g_variant_builder_end(&gvb);
		break;
    //case SR_CONF_PATTERN_MODE:
    //sr_info("config_list   SR_CONF_PATTERN_MODE====logic");
	//	*data = g_variant_new_strv(pattern_strings, ARRAY_SIZE(pattern_strings));
	//	break;
    case SR_CONF_MAX_HEIGHT:
        *data = g_variant_new_strv(maxHeights, ARRAY_SIZE(maxHeights));
        break;

    case SR_CONF_OPERATION_MODE:
        *data = g_variant_new_uint64((uint64_t)&opmode_list);
        break;
    case SR_CONF_EX_TRIGGER_MATCH:
        *data = g_variant_new_uint64((uint64_t)&extern_trigger_matches);
        break;
    case SR_CONF_CHANNEL_MODE: 
        num = 0;
        for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
            if (channel_modes[i].stream == devc->stream && devc->profile->dev_caps.channels & (1 << i))
           //if ( devc->profile->dev_caps.channels & (1 << i))
            {
                // if (devc->test_mode != SR_TEST_NONE && devc->profile->dev_caps.intest_channel != channel_modes[i].id)
                //     continue;
                
                if (num == CHANNEL_MODE_LIST_LEN - 1){
                    assert(0);
                }
                channel_mode_list[num].id = channel_modes[i].id;
                channel_mode_list[num].name = channel_modes[i].descr;
                num++;
            }
        }
        channel_mode_list[num].id = -1;
        channel_mode_list[num].name = NULL;
        *data = g_variant_new_uint64((uint64_t)&channel_mode_list);
        break;
    case SR_CONF_FILTER: 
        *data = g_variant_new_uint64((uint64_t)&filter_list);
        break;
	default:
        return SR_ERR_NA;
	}

    return SR_OK;
}


static void free_transfer(struct libusb_transfer *transfer)
{
    struct PX_context *devc = transfer->user_data;
    struct sr_dev_inst *sdi = devc->cb_data;
    unsigned int i;

    //devc = transfer->user_data;

    g_free(transfer->buffer);
    transfer->buffer = NULL;
    libusb_free_transfer(transfer);
    sr_info("free_transfer: devc->num_transfers = %d",devc->num_transfers);
    for (i = 0; i < devc->num_transfers; i++) {
        if (devc->transfers[i] == transfer) {
            devc->transfers[i] = NULL;
            devc->submitted_transfers--;
            break;
        }
    }
    // unsigned int  free_num = 0;
    // for (i = 0; i < devc->num_transfers; i++) {
    //     if (devc->transfers[i] == NULL) {
    //         free_num++;
    //     }
    // }
    // sr_info("free_num: =  %d",free_num);

    // if(free_num == devc->num_transfers-1){
    //     if (devc->num_transfers != 0) {
    //     devc->num_transfers = 0;
    //     g_free(devc->transfers);
    //     sr_info("g_free(devc->transfers);");
    // }
    // }

	//devc->submitted_transfers--;
	if (devc->submitted_transfers == 0){
        sr_info("submitted_transfers == 0");
		finish_acquisition(sdi);
    }


}


static void resubmit_transfer(struct libusb_transfer *transfer)
{
    int ret;
    int i = 10;

    if ((ret = libusb_submit_transfer(transfer)) == LIBUSB_SUCCESS){
        //sr_info("resubmit_transfer OK ");
         return;
    }
    else{
        free_transfer(transfer);
        sr_info("resubmit_transfer error ");
    }
    
    /* TODO: Stop session? */

    sr_err("%s: %s", __func__, libusb_error_name(ret));
}


/* Callback handling data */
SR_PRIV void abort_acquisition(struct PX_context *devc);
static void receive_transfer(struct libusb_transfer *transfer)
{
    struct PX_context *devc = transfer->user_data;
    struct sr_dev_inst *sdi = devc->cb_data;
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    double samples_elaspsed;
    uint64_t samples_to_send = 0, sending_now;
    uint64_t i;
    uint64_t samples_counter2;
    uint64_t offset = 0;
    // if(devc->buf != NULL){
    //    g_free(devc->buf);
    //}
    devc->buf = transfer->buffer;
    sr_info("%llu: receive_transfer(): status %d; timeout %d; received %d bytes.",
        g_get_monotonic_time(), transfer->status, transfer->timeout, transfer->actual_length);

    //  if (devc->status == DSL_START)
    //      devc->status = DSL_DATA;

    //  if (devc->abort)
    //      devc->status = DSL_STOP;

    //op_mode =  devc->op_mode;
    //devc->op_mode == OP_STREAM

    if (devc->acq_aborted) {
        free_transfer(transfer);
        return;
    }

     switch (transfer->status) {
    case LIBUSB_TRANSFER_STALL:
    case LIBUSB_TRANSFER_NO_DEVICE:
        //free_transfer(transfer);
    	abort_acquisition(devc);
        free_transfer(transfer);
		
        return;
    case LIBUSB_TRANSFER_CANCELLED:
            //free_transfer(transfer);break;
     case LIBUSB_TRANSFER_COMPLETED:

     case LIBUSB_TRANSFER_TIMED_OUT: /* We may have received some data though. */
         break;
     default:
         //devc->status = DSL_ERROR;
         break;
     }



    //sr_info("transfer->status = %d",transfer->status);
            //packet.status = SR_PKT_OK;
    if( transfer->actual_length != 0 &&  transfer->status == LIBUSB_TRANSFER_COMPLETED){
        packet.status = SR_PKT_OK;
        devc->rece_transfers ++;
        if (devc->limit_samples) {       
            //samples_to_send = MIN(devc->limit_samples-devc->samples_counter,  BUFSIZE);
            //samples_to_send = BUFSIZE;
            samples_to_send = transfer->actual_length;
        }

        if (samples_to_send > 0 && !devc->stop) {
            sending_now = samples_to_send;
            //sending_now = MIN(samples_to_send,  BUFSIZE);
            if (sdi->mode == LOGIC) {
                //devc->samples_counter += sending_now;
                // if(devc->samples_counter == 0){
                //     std_session_send_df_header(sdi, LOG_PREFIX);
                // }
                if(devc->op_mode == OP_BUFFER ||
                   (devc->op_mode == OP_STREAM && devc->is_loop==0)
                
                ){
                    if(devc->samples_counter+(sending_now* 8)/devc-> ch_num >= devc->limit_samples){
                        sending_now = (devc->limit_samples - devc->samples_counter)*devc-> ch_num/8;
                        devc->samples_counter = devc->limit_samples;
                        
                    }else{
                        devc->samples_counter = devc->samples_counter+(sending_now* 8)/devc-> ch_num;
                    }
                }

                //devc->samples_counter = devc->samples_counter+(sending_now* 8)/devc-> ch_num;

                // sr_info("(sending_now* 8)/devc-> ch_num  = %d",(sending_now* 8)/devc-> ch_num);
                // sr_info("samples_counter  = %d",devc->samples_counter);
                // sr_info("sending_now  = %d",sending_now);
                // sr_info("devc-> ch_num  = %d",devc-> ch_num);


            }
            if(devc->usb_data_align_en){
                offset = (devc-> ch_num - (64%devc-> ch_num))*8;
                sr_info("usb_data_align_en");
            }
            devc->usb_data_align_en = 0;
            offset = 0;
           //if (devc->trigger_stage == 0){
             if (1){
                //sr_info("devc->trigger_stage == 0   case 1");
                if (sdi->mode == LOGIC) {
                    //logic.index = 0;
                    packet.type = SR_DF_LOGIC;
                    packet.payload = &logic;
                    //logic.length = sending_now * (channel_modes[devc->ch_mode].num >> 3);
                    logic.length = sending_now-offset;
                    //sr_info(" logic.length = %d ",logic.length);
                    logic.format = LA_CROSS_DATA;
                    //logic.data = devc->buf;
                    logic.data = transfer->buffer+offset;
                    logic.data_error = 0;
                    //logic.unitsize = devc-> ch_num*8;
                } 


                ds_data_forward(sdi, &packet);
                devc->samples_counter_div2 = devc->samples_counter/2;
                //devc->mstatus.trig_hit = (devc->trigger_stage == 0);
                devc->mstatus.trig_hit = 1;
                devc->mstatus.vlen = devc->block_size;
                devc->mstatus.captured_cnt0 = devc->samples_counter;
                devc->mstatus.captured_cnt1 = devc->samples_counter >> 8;
                devc->mstatus.captured_cnt2 = devc->samples_counter >> 16;
                devc->mstatus.captured_cnt3 = devc->samples_counter >> 24;
            }
        }

    }


    if ((sdi->mode == LOGIC || devc->instant) && devc->limit_samples &&
        devc->samples_counter >= devc->limit_samples
        //devc->op_mode == OP_BUFFER
        ) {
        sr_dbg("last  transfer");
        //abort_acquisition(devc);
        //free_transfer(transfer);
        devc->stop = TRUE;
        
        //free_transfer(transfer);
        abort_acquisition(devc);
        free_transfer(transfer);
    }
    else if(devc->stop != TRUE){

            
            // if( transfer->status == LIBUSB_TRANSFER_COMPLETED){
            //     sr_dbg("resubmit_transfer  submitted_transfers = %d",devc->submitted_transfers);
            //      resubmit_transfer(transfer);
            // }
        //transfer->status = 
        //transfer->timeout = 0;
        resubmit_transfer(transfer);
                
    }

    if(transfer->status == LIBUSB_TRANSFER_COMPLETED){
        if(devc->block_size != transfer->actual_length && devc-> usb_speed != LIBUSB_SPEED_SUPER ){
            devc->usb_data_align_en = 1;
        }
        else{
            devc->usb_data_align_en = 0;
        }
            
    }

}
SR_PRIV void abort_acquisition(struct PX_context *devc)
{
	int i;
    

	devc->acq_aborted = TRUE;
    //devc->stop = TRUE;

	for (i = devc->num_transfers - 1; i >= 0; i--) {
		if (devc->transfers[i])
			libusb_cancel_transfer(devc->transfers[i]);
	}
}

uint64_t align_2m_64(uint64_t pix){
    uint64_t align_pix;
    if(pix%BUFSIZE){
        align_pix = (pix/BUFSIZE +1)*BUFSIZE;
    }
    else{
        align_pix = pix;
    }
    return align_pix;

}

uint64_t align_4k(uint64_t pix){
    uint64_t align_pix;
    uint64_t align = 4096;
    if(pix%align){
        align_pix = (pix/align +1)*align;
    }
    else{
        align_pix = pix;
    }
    return align_pix;

}

enum trigger_matches {
	SR_TRIGGER_ZERO = 1,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
	SR_TRIGGER_EDGE,
	SR_TRIGGER_OVER,
	SR_TRIGGER_UNDER,
};

// static bool set_trigger(const struct sr_dev_inst *sdi)
// {
// 	struct sr_trigger *trigger;
// 	struct sr_trigger_stage *stage;
// 	struct sr_trigger_match *match;
// 	struct PX_context *devc;
// 	const GSList *l, *m;
// 	const unsigned int num_enabled_channels = en_ch_num(sdi);
// 	int num_trigger_stages = 0;

// 	int channelbit, i = 0;
// 	uint32_t trigger_point;

// 	devc = sdi->priv;

// 	devc->ch_en = en_ch_num_mask(sdi);


//     devc-> trig_zero  = 0;
//     devc->trig_one    = 0;
//     devc->trig_rise    = 0;
//     devc->trig_fall     = 0;


// 	// trigger_point = (devc->capture_ratio * devc->limit_samples) / 100;
// 	// if (trigger_point < DSLOGIC_ATOMIC_SAMPLES)
// 	// 	trigger_point = DSLOGIC_ATOMIC_SAMPLES;
// 	// const uint32_t mem_depth = devc->profile->mem_depth;
// 	// const uint32_t max_trigger_point = devc->continuous_mode ? ((mem_depth * 10) / 100) :
// 	// 	((mem_depth * DS_MAX_TRIG_PERCENT) / 100);
// 	// if (trigger_point > max_trigger_point)
// 	// 	trigger_point = max_trigger_point;
// 	// cfg->trig_pos = trigger_point & ~(DSLOGIC_ATOMIC_SAMPLES - 1);

// 	if (!(trigger = sr_session_trigger_get(sdi->session))) {
// 		sr_dbg("No session trigger found");
// 		return false;
// 	}

// 	for (l = trigger->stages; l; l = l->next) {
// 		stage = l->data;
// 		num_trigger_stages++;
// 		for (m = stage->matches; m; m = m->next) {
// 			match = m->data;
// 			if (!match->channel->enabled)
// 				/* Ignore disabled channels with a trigger. */
// 				continue;
// 			channelbit = 1 << (match->channel->index);
// 			/* Simple trigger support (event). */
// 			if (match->match == SR_TRIGGER_ONE) {
// 				devc->trig_one |= channelbit;
// 			} else if (match->match == SR_TRIGGER_ZERO) {
// 				devc->trig_zero |= channelbit;
// 			} else if (match->match == SR_TRIGGER_FALLING) {
// 				devc->trig_fall |= channelbit;
// 			} else if (match->match == SR_TRIGGER_RISING) {
// 				devc->trig_rise |= channelbit;
// 			} else if (match->match == SR_TRIGGER_EDGE) {
// 				devc->trig_fall |= channelbit;
// 				devc->trig_rise |= channelbit;
// 			}
// 		}
// 	}

//     sr_info(" devc->trig_one =  %8x",devc->trig_one);
//     sr_info(" devc->trig_zero =  %8x",devc->trig_zero);
//     sr_info(" devc->trig_fall =  %8x",devc->trig_fall);
//     sr_info(" devc->trig_rise =  %8x",devc->trig_rise);
// 	return num_trigger_stages != 0;
// }


// static bool set_trigger1(const struct sr_dev_inst *sdi)
// {

// 	struct PX_context *devc;
// 	const GSList *l, *m;
// 	const unsigned int num_enabled_channels = en_ch_num(sdi);
// 	int num_trigger_stages = 0;
//     gboolean qutr_trig;
//     gboolean half_trig;
// 	int channelbit, i = 0;
// 	uint32_t trigger_point;

// 	devc = sdi->priv;

// 	devc->ch_en = en_ch_num_mask(sdi);


//     devc-> trig_zero  = 0;
//     devc->trig_one    = 0;
//     devc->trig_rise    = 0;
//     devc->trig_fall     = 0;



//     if (trigger->trigger_mode == SIMPLE_TRIGGER) {
//         // qutr_trig = !(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_USB30) && (setting.mode & (1 << QUAR_MODE_BIT));
//         // half_trig = (!(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_USB30) && setting.mode & (1 << HALF_MODE_BIT)) ||
//         //             ((devc->profile->dev_caps.feature_caps & CAPS_FEATURE_USB30) && setting.mode & (1 << QUAR_MODE_BIT));

//         qutr_trig = 0;
//         half_trig = 0;

//         devc.trig_mask0[0] = ds_trigger_get_mask0(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);
//         devc.trig_mask1[0] = ds_trigger_get_mask1(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);
//         devc.trig_value0[0] = ds_trigger_get_value0(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);
//         devc.trig_value1[0] = ds_trigger_get_value1(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);
//         devc.trig_edge0 [0] = ds_trigger_get_edge0(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);
//         devc.trig_edge1 [0] = ds_trigger_get_edge1(TriggerStages, TriggerProbes-1, 0, qutr_trig, half_trig);

//         devc.trig_mask0[1] = ds_trigger_get_mask0(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);
//         devc.trig_mask1[1] = ds_trigger_get_mask1(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);
//         devc.trig_value0[1] = ds_trigger_get_value0(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);
//         devc.trig_value1[1] = ds_trigger_get_value1(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);
 //        devc.trig_edge0 [1] = ds_trigger_get_edge0(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);
 //        devc.trig_edge1 [1] = ds_trigger_get_edge1(TriggerStages, 2*TriggerProbes-1, TriggerProbes, qutr_trig, half_trig);


//         setting.trig_logic0[0] = (trigger->trigger_logic[TriggerStages] << 1) + trigger->trigger0_inv[TriggerStages];
//         setting.trig_logic1[0] = (trigger->trigger_logic[TriggerStages] << 1) + trigger->trigger1_inv[TriggerStages];

//         setting.trig_count[0] = trigger->trigger0_count[TriggerStages];


//     } 


//     sr_info(" devc->trig_one =  %8x",devc->trig_one);
//     sr_info(" devc->trig_zero =  %8x",devc->trig_zero);
//     sr_info(" devc->trig_fall =  %8x",devc->trig_fall);
//     sr_info(" devc->trig_rise =  %8x",devc->trig_rise);
// 	return num_trigger_stages != 0;
// }


SR_PRIV uint64_t px_channel_depth(const struct sr_dev_inst *sdi)
{
    struct PX_context *devc = sdi->priv;
    int ch_num = en_ch_num(sdi);
    return (devc->profile->dev_caps.hw_depth / (ch_num ? ch_num : 1)) & ~SAMPLES_ALIGN;
}


static void set_trigger(const struct sr_dev_inst *sdi)
{
	// struct sr_trigger *trigger;
	// struct sr_trigger_stage *stage;
	// struct sr_trigger_match *match;
	struct PX_context *devc;
	uint32_t i, m;
	const unsigned int num_enabled_channels = en_ch_num(sdi);
	int num_trigger_stages = 0;

	int channelbit;
	uint32_t trigger_point;
    uint16_t stage = 16;
	devc = sdi->priv;

	devc->ch_en = en_ch_num_mask(sdi);


    devc-> trig_zero  = 0;
    devc->trig_one    = 0;
    devc->trig_rise    = 0;
    devc->trig_fall     = 0;


	// trigger_point = (devc->capture_ratio * devc->limit_samples) / 100;
	// if (trigger_point < DSLOGIC_ATOMIC_SAMPLES)
	// 	trigger_point = DSLOGIC_ATOMIC_SAMPLES;
	// const uint32_t mem_depth = devc->profile->mem_depth;
	// const uint32_t max_trigger_point = devc->continuous_mode ? ((mem_depth * 10) / 100) :
	// 	((mem_depth * DS_MAX_TRIG_PERCENT) / 100);
	// if (trigger_point > max_trigger_point)
	// 	trigger_point = max_trigger_point;
	// cfg->trig_pos = trigger_point & ~(DSLOGIC_ATOMIC_SAMPLES - 1);




    for (i = 0; i<32; i = i+1) {
        channelbit = 1 << (i);

        if (devc->ch_en & channelbit ){
            sr_info(" trigger->trigger0[stage][%d]  =  %c",i,trigger->trigger0[stage][i] );
				/* Ignore disabled channels with a trigger. */
			/* Simple trigger support (event). */
			if (trigger->trigger0[stage][i] == '1') {
				devc->trig_one |= channelbit;
			} else if (trigger->trigger0[stage][i] == '0') {
				devc->trig_zero |= channelbit;
			} else if (trigger->trigger0[stage][i] == 'F') {
				devc->trig_fall |= channelbit;
			} else if (trigger->trigger0[stage][i] == 'R') {
				devc->trig_rise |= channelbit;
			} else if (trigger->trigger0[stage][i] == 'C' ) {
				devc->trig_fall |= channelbit;
				devc->trig_rise |= channelbit;
			}
        }
    }


    sr_info(" devc->trig_one =  %8x",devc->trig_one);
    sr_info(" devc->trig_zero =  %8x",devc->trig_zero);
    sr_info(" devc->trig_fall =  %8x",devc->trig_fall);
    sr_info(" devc->trig_rise =  %8x",devc->trig_rise);


        uint32_t tmp_u32;
        tmp_u32 = max((uint32_t)(trigger->trigger_pos / 100.0 * devc->limit_samples), PXLOGIC_ATOMIC_SAMPLES);

        if (devc->stream)
            tmp_u32 = min(tmp_u32, px_channel_depth(sdi) * 10 / 100);
        else
            tmp_u32 = min(tmp_u32, px_channel_depth(sdi) * DS_MAX_TRIG_PERCENT / 100);

        devc->trigger_pos_set = tmp_u32;


    // if (!(devc->trigger_pos = (struct ds_trigger_pos *)g_try_malloc(1024))) {
    //     sr_err("%s: USB trigger_pos buffer malloc failed.", __func__);
    //     //return SR_ERR_MALLOC;
    // }
    // else{

    //     sr_info("trigger_pos req ok");

    //     struct sr_datafeed_packet packet;

    //     (devc->trigger_pos)->check_id = TRIG_CHECKID;
    //     sr_info("check_id = %x",(devc->trigger_pos)->check_id);
    //     //devc->trigger_pos->real_pos = trigger->trigger_pos;

    //     sr_info("trigger_pos = %d",trigger->trigger_pos);

    //     uint32_t tmp_u32;
    //     tmp_u32 = max((uint32_t)(trigger->trigger_pos / 100.0 * devc->limit_samples), PXLOGIC_ATOMIC_SAMPLES);

    //     if (devc->stream)
    //         tmp_u32 = min(tmp_u32, px_channel_depth(sdi) * 10 / 100);
    //     else
    //         tmp_u32 = min(tmp_u32, px_channel_depth(sdi) * DS_MAX_TRIG_PERCENT / 100);

    //     devc->trigger_pos_set = tmp_u32;
    //     devc->trigger_pos->real_pos = tmp_u32;
    //     sr_info("trigger_real_pos = %d",devc->trigger_pos->real_pos);

    //     devc->trigger_pos->ram_saddr = 0;
    //     devc->trigger_pos->remain_cnt_l = 0;
    //     devc->trigger_pos->remain_cnt_h = 0;
    //     devc->trigger_pos->status = 0x01;
    //     sr_info("status = %d",devc->trigger_pos->status);

    //     packet.status = SR_PKT_OK;
    //     packet.type = SR_DF_TRIGGER;
    //     packet.payload = devc->trigger_pos;
    //     //ds_data_forward(sdi, &packet);

    // }


}



static void set_trigger_pos(const struct sr_dev_inst *sdi)
{
	// struct sr_trigger *trigger;
	// struct sr_trigger_stage *stage;
	// struct sr_trigger_match *match;
	struct PX_context *devc;
    devc = sdi->priv;



    if (!(devc->trigger_pos = (struct ds_trigger_pos *)g_try_malloc(1024))) {
        sr_err("%s: USB trigger_pos buffer malloc failed.", __func__);
        //return SR_ERR_MALLOC;
    }
    else{
        // struct ds_trigger_pos {
        // uint32_t check_id;
        // uint32_t real_pos;
        // uint32_t ram_saddr;
        // uint32_t remain_cnt_l;
        // uint32_t remain_cnt_h;
        // uint32_t status;
        // };
        sr_info("trigger_pos req ok");

        struct sr_datafeed_packet packet;

        (devc->trigger_pos)->check_id = TRIG_CHECKID;
        sr_info("check_id = %x",(devc->trigger_pos)->check_id);
        //devc->trigger_pos->real_pos = trigger->trigger_pos;

        sr_info("trigger_pos = %d",trigger->trigger_pos);

        devc->trigger_pos->real_pos = devc->trigger_pos_set;
        sr_info("trigger_real_pos = %d",devc->trigger_pos->real_pos);

        devc->trigger_pos->ram_saddr = 0;
        devc->trigger_pos->remain_cnt_l = 0;
        devc->trigger_pos->remain_cnt_h = 0;
        devc->trigger_pos->status = 0x01;
        sr_info("status = %d",devc->trigger_pos->status);

        packet.status = SR_PKT_OK;
        packet.type = SR_DF_TRIGGER;
        packet.payload = devc->trigger_pos;
        ds_data_forward(sdi, &packet);

    }


}







SR_PRIV int start_transfers(const struct sr_dev_inst *sdi)
{
    struct PX_context *devc = sdi->priv;
    struct sr_usb_dev_inst *usb;
    struct libusb_transfer *transfer;
    unsigned int i, num_transfers = 0;
    int ret,rc;
    unsigned char *buf = NULL;
    size_t size;
    uint64_t samples_to_send = 0, sending_total = 0,sending_last = 0;
    usb = sdi->conn;
    unsigned int ch_num ;
    unsigned int ch_en = 0;
    unsigned int gpio_mode = 0;
    unsigned int gpio_div = 0;
    uint16_t op_mode;
    uint32_t stream_mask = 0;
    uint64_t dma_size = 4096;
    uint64_t dma_size_min = 4096;

    uint64_t samples_ch_1s = 0;
    uint64_t samples_ch_1s_align_4k = 0;

    uint64_t usb_samples_1s = 0;

    int time_out = 0;
    uint64_t  usb_buff_max = 8*1024*1024;

    devc->acq_aborted = FALSE;

    devc->usb_data_align_en = 0;


    devc->cmd_data.sync_cur_sample = 0;
    devc->cmd_data.trig_out_validset = 0;
    devc->cmd_data.real_pos = 0;

    usb_wr_reg(usb->devhdl,16<<2,0);
    usb_wr_reg(usb->devhdl,17<<2,devc->pwm0_freq_set-1);
    usb_wr_reg(usb->devhdl,18<<2,devc->pwm0_duty_set-1);
    // usb_wr_reg(usb->devhdl,17<<2,2);
    // usb_wr_reg(usb->devhdl,18<<2,1);
    usb_wr_reg(usb->devhdl,16<<2,(uint32_t)devc->pwm0_en);
    usb_wr_reg(usb->devhdl,19<<2,0);
    //usb_wr_reg(usb->devhdl,20<<2,devc->pwm1_freq_set-1);
    //usb_wr_reg(usb->devhdl,21<<2,devc->pwm1_duty_set-1);
    //usb_wr_reg(usb->devhdl,19<<2,(uint32_t)devc->pwm1_en);

    op_mode =  devc->op_mode;
    ch_num = en_ch_num(sdi);
    ch_en = en_ch_num_mask(sdi);
    set_trigger(sdi);
    if(op_mode == OP_STREAM){
        stream_mask = 1 <<1;
    }
    else {
        stream_mask = 0 <<1;
    }

    dma_size = 4096;

    if(devc->usb_speed == LIBUSB_SPEED_SUPER){
        usb_samples_1s = 5*1000*1000*1000; //5G USB3.0
    }
    else{
        usb_samples_1s = 480*1000*1000; //480M USB2.0
    }

    sr_info(" usb_samples_1s =  %d",usb_samples_1s);

   devc-> ch_num = ch_num;
    sr_info(" ch_num =  %d",ch_num);
    sr_info(" devc-> ch_num =  %d",devc-> ch_num);

    sr_info(" devc->limit_samples =  %d",devc->limit_samples);

    samples_ch_1s = devc->cur_samplerate/100/8;
    sr_info(" samples_ch_1s =  %d",samples_ch_1s);
    samples_ch_1s_align_4k = align_4k(samples_ch_1s);
    sr_info(" samples_ch_1s_align_4k =  %d",samples_ch_1s_align_4k);


    //usb_buff_max = usb_samples_1s/100/8; //10ms
    if(devc->usb_speed == LIBUSB_SPEED_SUPER){
        //usb_buff_max = usb_samples_1s/100/8; //10ms
        usb_buff_max = 4*1024*1024; //4M
    }
    else{
        usb_buff_max = usb_samples_1s/100/8; //10ms
    }

    //usb_buff_max = 8*1024*1024;
    usb_buff_max = align_4k(usb_buff_max);
    sr_info(" usb_buff_max =  %d",usb_buff_max);

    if(samples_ch_1s_align_4k *ch_num > usb_buff_max){


        //devc->block_size = 8*1024*1024;

        devc->block_size = (usb_buff_max/ch_num/4096)*4096*ch_num;
    }
    else {
        devc->block_size = samples_ch_1s_align_4k *ch_num;
    }

    //time_out = 500;

    sr_info(" devc->block_size =  %d",devc->block_size);
    if(devc->cur_samplerate >= 500000){
        time_out = 100;
    }else{
        time_out = 0;
    }
    time_out = 0;
    //devc->block_size = 64*4096*ch_num;
    //devc->block_size = 1*1024*1024;
    BUFSIZE = devc->block_size;



    devc->limit_samples2Byte = devc->limit_samples*ch_num/8;
    devc->limit_samples2Byte =  devc->limit_samples2Byte+BUFSIZE;
    sr_err("BUFSIZE = %d",BUFSIZE);
    
    //devc->limit_samples2Byte = align_2m_64 (devc->limit_samples2Byte)+BUFSIZE;

    while(sending_total < devc->limit_samples2Byte  &&  devc->limit_samples){
        samples_to_send = MIN(devc->limit_samples2Byte-sending_total,  BUFSIZE);
        sending_last = samples_to_send;
        sending_total  = sending_total + samples_to_send;
        num_transfers ++;
    }
    
    num_transfers = 4;
    sr_err("num_transfers = %d",num_transfers);

    devc->transfers = g_try_malloc0(sizeof(*devc->transfers) * (num_transfers ));
    if (!devc->transfers) {
        sr_err("%s: USB transfer malloc failed.", __func__);
        return SR_ERR_MALLOC;
    }

     rc =usb_wr_reg(usb->devhdl,8192+(11<<2), 0); //set_block_start
    libusb_clear_halt(usb->devhdl,0x82);
    libusb_clear_halt(usb->devhdl,0x04);
    libusb_clear_halt(usb->devhdl,0x84);
    //libusb_reset_device(usb->devhdl);
    uint32_t pwm_freq = 10000;
    uint32_t pwm_max = 120000000/pwm_freq;

     rc = usb_wr_reg(usb->devhdl, 2<<1, pwm_max);
     rc = usb_wr_reg(usb->devhdl, 2<<2, (uint32_t)(devc->vth*(100.0/200.0)/3.334*pwm_max));
    sr_info(" devc->vth =  %f",devc->vth);
    sr_info(" pwm_max =  %d",pwm_max);
     sr_info(" pwm =  %d",(uint32_t)(devc->vth*(100.0/200.0)/3.334*pwm_max));
     rc =usb_wr_reg(usb->devhdl,4<<2,0);
     rc =usb_wr_reg(usb->devhdl,0<<2,5|stream_mask);
     rc =usb_wr_reg(usb->devhdl,0<<2,5|stream_mask | (1<<4));
     rc =usb_wr_reg(usb->devhdl,0<<2,5|stream_mask);

    //rc =usb_wr_reg(usb->devhdl,0<<2,1|stream_mask);
    rc =usb_wr_reg(usb->devhdl,8<<2,0xffffffff);
    //rc =usb_wr_reg(usb->devhdl,0<<2,0|stream_mask);
    
     rc =usb_wr_reg(usb->devhdl,7<<2,BUFSIZE);
     rc =usb_wr_reg(usb->devhdl,8192+(2<<2),BUFSIZE);
    
    rc =usb_wr_reg(usb->devhdl,8192+(9<<2), devc->limit_samples2Byte);
    rc =usb_wr_reg(usb->devhdl,8192+(10<<2), devc->limit_samples2Byte>>32);
    sr_info(" devc->limit_samples2Byte =  %d",devc->limit_samples2Byte);

sr_info(" devc->cur_samplerate =  %d",devc->cur_samplerate);


      
    if(devc->cur_samplerate ==1000000000 ) gpio_mode = 0;
    else if(devc->cur_samplerate ==500000000 ) gpio_mode = 1;
    else if(devc->cur_samplerate ==250000000 ) gpio_mode = 2;
    else if(devc->cur_samplerate ==125000000 ) gpio_mode = 3;
    else if(devc->cur_samplerate ==800000000 ) gpio_mode = 0+4;
    else if(devc->cur_samplerate ==400000000 ) gpio_mode = 1+4;
    else if(devc->cur_samplerate ==200000000 ) gpio_mode = 2+4;
    else if(devc->cur_samplerate ==100000000 ) gpio_mode = 3+4;
    else {
        gpio_mode = 3+4;
    if(devc->cur_samplerate ==50000000 ) gpio_div = 1;
    else if(devc->cur_samplerate ==25000000 ) gpio_div = 3;
    else if(devc->cur_samplerate ==20000000 ) gpio_div = 4;
    else if(devc->cur_samplerate ==10000000 ) gpio_div = 9;
    else if(devc->cur_samplerate ==5000000 ) gpio_div = 19;
    else if(devc->cur_samplerate ==4000000 ) gpio_div = 24;
    else if(devc->cur_samplerate ==2000000 ) gpio_div = 49;
    else if(devc->cur_samplerate ==1000000 ) gpio_div = 99;   
    else if(devc->cur_samplerate ==500000 ) gpio_div = 199;   
    else if(devc->cur_samplerate ==400000 ) gpio_div = 249;   
    else if(devc->cur_samplerate ==200000 ) gpio_div = 499;   
    else if(devc->cur_samplerate ==100000 ) gpio_div = 999;   
    else if(devc->cur_samplerate ==50000 ) gpio_div = 1999;   
    else if(devc->cur_samplerate ==40000 ) gpio_div = 2499;   
    else if(devc->cur_samplerate ==20000 ) gpio_div = 4999;   
    else if(devc->cur_samplerate ==10000 ) gpio_div = 9999;   
    else if(devc->cur_samplerate ==5000 ) gpio_div = 19999;   
    else if(devc->cur_samplerate ==2000 ) gpio_div = 49999;   
    else  gpio_div = 0;  
        }

    //devc->ext_trig_mode;
    rc = usb_wr_reg(usb->devhdl,15<<2,devc->ext_trig_mode);
    rc = usb_wr_reg(usb->devhdl,22<<2,devc->trig_out_en);

  sr_info(" gpio_mode =  %x",gpio_mode);
    rc = usb_wr_reg(usb->devhdl,5<<2,gpio_mode|(devc->clock_edge <<3));
     if(rc != 0)sr_info("usb_wr_reg gpio_mode error : rc =  %d",rc);
      else{
       sr_info("usb_wr_reg gpio_mode success : rc =  %d",rc);
      }
  sr_info(" gpio_div =  %d",gpio_div);
    rc = usb_wr_reg(usb->devhdl,6<<2,gpio_div);
     if(rc != 0)sr_info("usb_wr_reg gpio_div error : rc =  %d",rc);
      else{
       sr_info("usb_wr_reg gpio_div success : rc =  %d",rc);
      }

    usb_wr_reg(usb->devhdl,8192+(19<<2), ch_num);
    usb_wr_reg(usb->devhdl,8192+(20<<2), devc->trigger_pos_set);
    //sr_info("devc->trigger_pos_set = %d",devc->trigger_pos_set);

    rc =usb_wr_reg(usb->devhdl,8192+(11<<2), 0); //set_block_start
    rc = usb_rd_reg(usb->devhdl,6<<2,&gpio_div);
     if(rc != 0){
        sr_info("usb_rd_reg gpio_div error : rc =  %d",rc);
     }
      else{
        sr_info("gpio_div  =  %d",gpio_div);
       sr_info("usb_rd_reg gpio_div success : rc =  %d",rc);
      }

     
    sr_info(" ch_en =  %x",ch_en);
    rc =usb_wr_reg(usb->devhdl,4<<2,ch_en);
     if(rc != 0)sr_info("usb_wr_reg ch_en error : rc =  %d",rc);
      else{
       sr_info("usb_wr_reg ch_en success : rc =  %d",rc);
      }
    


    // sr_info(" devc->trig_one =  %8x",devc->trig_one);
    // sr_info(" devc->trig_zero =  %8x",devc->trig_zero);
    // sr_info(" devc->trig_fall =  %8x",devc->trig_fall);
    // sr_info(" devc->trig_rise =  %8x",devc->trig_rise);

    rc =usb_wr_reg(usb->devhdl,0<<2,0|stream_mask | (devc->filter<<3));
    //rc =usb_wr_reg(usb->devhdl,0<<2,0|stream_mask);
    rc =usb_wr_reg(usb->devhdl,9<<2,devc->trig_zero);
    rc =usb_wr_reg(usb->devhdl,10<<2,devc->trig_one);
    rc =usb_wr_reg(usb->devhdl,11<<2,devc->trig_rise);
    rc =usb_wr_reg(usb->devhdl,12<<2,devc->trig_fall);

    // rc =usb_wr_reg(usb->devhdl,9<<2,devc->trig_zero);
    // rc =usb_wr_reg(usb->devhdl,10<<2,devc->trig_one);
    // rc =usb_wr_reg(usb->devhdl,11<<2,devc->trig_rise);
    // rc =usb_wr_reg(usb->devhdl,12<<2,devc->trig_fall);

    // rc =usb_wr_reg(usb->devhdl,9<<2,devc->trig_zero);
    // rc =usb_wr_reg(usb->devhdl,10<<2,devc->trig_one);
    // rc =usb_wr_reg(usb->devhdl,11<<2,devc->trig_rise);
    // rc =usb_wr_reg(usb->devhdl,12<<2,devc->trig_fall);


       rc =usb_wr_reg(usb->devhdl,8<<2,0x0); 
//unsigned int usb_rd_data_req(libusb_device_handle *usbdevh,unsigned int base_addr,int length,unsigned int mode,unsigned char *buff,unsigned int timeout){
    //usb_rd_data_req(usb->devhdl,0x1,num_transfers*BUFSIZE,2,NULL,0);
    
    devc->num_transfers = 0;
    devc->submitted_transfers = 0;
    devc->rece_transfers  = 0;
    devc->send_total = num_transfers*BUFSIZE;

    // if(num_transfers >16){
    //     num_transfers = 16;
    // }
    //devc->cb_data = sdi;

    //std_session_send_df_header(sdi, LOG_PREFIX);
    //for (i = 0; i < 1; i++) {
    for (i = 0; i < num_transfers; i++) {

        size =  BUFSIZE;

        if (!(buf = g_try_malloc(BUFSIZE))) {
       //  if (!(buf = g_try_malloc(8*1024*1024))) {
            sr_err("%s: USB transfer buffer malloc failed.", __func__);
            return SR_ERR_MALLOC;
        }

        transfer = libusb_alloc_transfer(0);
        transfer->actual_length=0;
        libusb_fill_bulk_transfer(transfer, usb->devhdl, 0x82, buf, size,(libusb_transfer_cb_fn)receive_transfer, devc, time_out); //time_out
        if ((ret = libusb_submit_transfer(transfer)) != 0) {
            sr_err("%s: Failed to submit transfer: %s.",
                   __func__, libusb_error_name(ret));
            //libusb_cancel_transfer(transfer);
            libusb_free_transfer(transfer);
            g_free(buf);
            return SR_ERR;
        }
        else{
                sr_info("success   submit transfer");
                devc->transfers[i] = transfer;
                devc->num_transfers++;
                devc->submitted_transfers++;
        }

    }


       return SR_OK;


}

//static void remove_sources(struct PX_context *devc)
//{
//    int i;
//    sr_info("%s: remove fds from polling", __func__);
//    /* Remove fds from polling. */
//    for (i = 0; devc->usbfd[i] != -1; i++)
//        sr_source_remove(devc->usbfd[i]);
//
//    sr_info("lupfd num = %d",i);
//    g_free(devc->usbfd);
//}


/* Callback handling data */
static int receive_data2(int fd, int revents, const struct sr_dev_inst *sdi)
{
    struct PX_context *devc = sdi->priv;
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    double samples_elaspsed;
    uint64_t samples_to_send = 0, sending_now;
	int64_t time, elapsed;
    static uint16_t last_sample = 0;
    uint32_t cur_sample;
    uint64_t i;
    int completed = 0;
    struct drv_context *drvc;
        struct timeval tv;

	(void)fd;
	(void)revents;
    int ret = 0;
    uint32_t trigger_pos_real = 0;
    struct sr_usb_dev_inst *usb;
    usb = sdi->conn;
    //struct ctl_data cmd_data;
    tv.tv_sec = tv.tv_usec = 0;
    drvc = di->priv;
    libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &tv, &completed);
    
    //libusb_handle_events(drvc->sr_ctx->libusb_ctx);
    //libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &tv);


    //sr_info("devc->samples_counter = %d  ,devc->limit_samples = %d",devc->samples_counter,devc->limit_samples);
    if ((sdi->mode == LOGIC || devc->instant) && devc->limit_samples &&
        devc->samples_counter >= devc->limit_samples) {
        // if(devc->stop != TRUE){
        //     hw_dev_acquisition_stop(sdi, NULL);
        //     sr_dbg("Requested number of samples reached.");
        // }

        return TRUE;
    }

    if ((sdi->mode == LOGIC || devc->instant) && devc->limit_samples &&
        devc->samples_counter == 0) {
        if(devc->cmd_data.trig_out_validset == 0){
            ret = command_ctl_rddata(usb->devhdl, &(devc->cmd_data));
            if(ret == SR_OK){

                if(devc->cmd_data.sync_cur_sample > devc->trigger_pos_set){
                    cur_sample = devc->trigger_pos_set;


                }
                else{
                    cur_sample = devc->cmd_data.sync_cur_sample;

                }
                // sr_info("sync_cur_sample = %d",devc->cmd_data.sync_cur_sample);
                // sr_info("trig_out_validset = %d",devc->cmd_data.trig_out_validset);
                // sr_info("real_pos = %d",devc->cmd_data.real_pos);


                // sr_info("cur_sample = %d",cur_sample);
                //  sr_info("sample_limits = %d",devc->limit_samples);
                //devc->mstatus.trig_hit = 0x01;
                devc->mstatus.trig_hit = devc->cmd_data.trig_out_validset;
                devc->mstatus.vlen = devc->block_size;
                devc->mstatus.captured_cnt0 = cur_sample;
                devc->mstatus.captured_cnt1 = cur_sample >> 8;
                devc->mstatus.captured_cnt2 = cur_sample >> 16;
                devc->mstatus.captured_cnt3 = cur_sample >> 24;
                if(devc->op_mode == OP_STREAM && devc->is_loop==1){
                    
                }
                else {
                    if(devc->cmd_data.trig_out_validset){
                        devc->trigger_pos_set = devc->cmd_data.real_pos;
                        // sr_info(" devc->trig_one =  %8x",devc->trig_one);
                        // sr_info(" devc->trig_zero =  %8x",devc->trig_zero);
                        // sr_info(" devc->trig_fall =  %8x",devc->trig_fall);
                        // sr_info(" devc->trig_rise =  %8x",devc->trig_rise);
                        if(devc->trig_one| devc->trig_zero | devc->trig_fall | devc->trig_rise){
                            set_trigger_pos(sdi);
                            g_free(devc->trigger_pos);
                        }
                        
                    }

                }
                

            }
            else{
                //sr_info("command_ctl_rddata err = %d",ret);

            }
        }
        return TRUE;
    }
    // uint64_t sync_cur_sample;
    // uint32_t trig_out_validset;
    // uint32_t real_pos;



    // ret = usb_rd_reg(usb->devhdl,8192+(21<<2),&trigger_pos_real);

    // sr_info("reg rd state = %d trigger_pos_real = %d",ret,trigger_pos_real);
    // if(devc->stop == TRUE){
    //     sr_session_source_remove((gintptr) devc->channel);
    // }

    return TRUE;
}

static int hw_dev_acquisition_start(struct sr_dev_inst *sdi,
		void *cb_data)
{
    struct PX_context * devc = sdi->priv;

    (void)cb_data;
    struct sr_usb_dev_inst *usb;
    struct drv_context *drvc;
    const struct libusb_pollfd **lupfd;
    int i,rc;
     drvc = di->priv;
    if (sdi->status != SR_ST_ACTIVE)
        return SR_ERR_DEVICE_CLOSED;

    //devc->cb_data = cb_data;
	devc->samples_counter = 0;
    devc->pre_index = 0;
    devc->mstatus.captured_cnt0 = 0;
    devc->mstatus.captured_cnt1 = 0;
    devc->mstatus.captured_cnt2 = 0;
    devc->mstatus.captured_cnt3 = 0;
    devc->stop = FALSE;
    devc->samples_not_sent = 0;

    devc->trigger_stage = 0;
    usb = sdi->conn;
    devc->cb_data = sdi;
	/*
	 * Setting two channels connected by a pipe is a remnant from when the
	 * demo driver generated data in a thread, and collected and sent the
	 * data in the main program loop.
	 * They are kept here because it provides a convenient way of setting
	 * up a timeout-based polling mechanism.
	 */



    //if (!(devc->buf = g_try_malloc( BUFSIZE*sizeof(uint16_t)))) {
    //    sr_err("buf for receive_data malloc failed.");
    //    return FALSE;
    //}
//
  sr_dbg("start    acquisition.");
  //libusb_clear_halt(usb->devhdl,0x83);
    //sr_session_source_add_channel(devc->channel, G_IO_IN | G_IO_ERR,100, receive_data, sdi);

    

	/* Send header packet to the session bus. */
    //std_session_send_df_header(cb_data, LOG_PREFIX);
    

    // start_transfers(devc->cb_data);
    // sr_dbg("start_transfers");

    /* setup callback function for data transfer */
    // lupfd = libusb_get_pollfds(drvc->sr_ctx->libusb_ctx);
    // for (i = 0; lupfd[i]; i++);

    // sr_info("lupfd num = %d",i);
    
    // i=1;
    // if (!(devc->usbfd = g_try_malloc(sizeof(struct libusb_pollfd) * (i + 0))))
    // 	return SR_ERR;
    // for (i = 0; lupfd[i]; i++) {
    //     sr_source_add(lupfd[i]->fd, lupfd[i]->events,
    //               1000, receive_data2, sdi);
    //     devc->usbfd[i] = lupfd[i]->fd;
    //     break;
    // }
    // i=1;
    // devc->usbfd[i] = -1;
    // free(lupfd);


    // lupfd = libusb_get_pollfds(drvc->sr_ctx->libusb_ctx);
    // for (i = 0; lupfd[i] ;i++);
    // sr_info("lupfd num = %d",i);

    // //i=1;
    // if (!(devc->usbfd = g_try_malloc(sizeof(struct libusb_pollfd) * (i + 1)))){
    //     return SR_ERR;
    // }
    	
    // for (i = 0; lupfd[i];i++) {
    //     sr_source_add(lupfd[i]->fd, lupfd[i]->events,
    //               20, receive_data2, sdi);
    //     devc->usbfd[i] = lupfd[i]->fd;
    //     //break;
    // }
    // //i=1;
    // devc->usbfd[i] = -1;
    // free(lupfd);

    sr_session_source_add ((gintptr) devc->channel, G_IO_IN | G_IO_ERR,5, receive_data2, sdi);
    

    std_session_send_df_header(sdi, LOG_PREFIX);

    start_transfers(devc->cb_data);
    sr_dbg("start_transfers");


	return SR_OK;
}


static void finish_acquisition(struct sr_dev_inst *sdi){

    struct PX_context *const devc = sdi->priv;
    struct sr_datafeed_packet packet;


    struct sr_usb_dev_inst *usb;
    usb = sdi->conn;
    uint32_t trigger_pos_real;
    int ret;
    // if (devc->stop)
    //     return SR_OK;
    devc->stop = TRUE;
    
    //remove_sources(devc);
    
    //sr_session_source_remove_channel(devc->channel);
    // if(devc->buf != NULL){
    //    g_free(devc->buf);
    // }
    //abort_acquisition(devc);

    /* Send last packet. */

    packet.type = SR_DF_END;
    packet.status = SR_PKT_OK;
    ds_data_forward(sdi, &packet);

    

    //remove_sources(devc);
    sr_session_source_remove((gintptr) devc->channel);

    devc->num_transfers = 0;
	g_free(devc->transfers);

    sr_dbg("finish_acquisition");

    // libusb_clear_halt(usb->devhdl,0x82);
    //libusb_clear_halt(usb->devhdl,0x83);
    // ret = usb_rd_reg2(usb->devhdl,8192+(21<<2),&trigger_pos_real);
	
    // sr_info("ret = %d",ret);
    // if(ret==0){
    //     devc->trigger_pos_set = trigger_pos_real;
    //     set_trigger_pos(sdi);
    //     g_free(devc->trigger_pos);
        
    // }

        //devc->trigger_pos_set = trigger_pos_real;
        // set_trigger_pos(sdi);
        // g_free(devc->trigger_pos);
    
   //devc->stop = TRUE;
    



    // usb_wr_reg2(usb->devhdl,0<<2,5 | (1<<4));
    // usb_wr_reg2(usb->devhdl,8<<2,0xffffffff);
    // usb_wr_reg2(usb->devhdl,4<<2,0);


}
static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data;

    struct PX_context *const devc = sdi->priv;
    abort_acquisition(devc);
    sr_dbg("Stopping acquisition.");

	return SR_OK;
}

static int hw_dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg)
{
    (void)prg;

    if (sdi) {
        struct PX_context *const devc = sdi->priv;
        *status = devc->mstatus;
        return SR_OK;
    } else {
        return SR_ERR;
    }
}

SR_PRIV int sr_dslogic_option_value_to_code2(const struct sr_dev_inst *sdi, int config_id, const char *value)
{
    int num;
    int i;
    int n;
    struct PX_context *devc;

    assert(sdi);
    assert(sdi->priv);
    
    devc = sdi->priv;
 sr_info("sr_dslogic_option_value_to_code2");
    if (config_id == SR_CONF_CHANNEL_MODE)
    //if (1)
    {
         for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                if (devc->profile->dev_caps.channels & (1 << i)) 
                {
                    if (strcmp(channel_modes[i].descr, value) == 0)
                        return channel_modes[i].id;

                    if (i < ARRAY_SIZE(channel_mode_cn_map)){
                        if (channel_modes[i].id != channel_mode_cn_map[i].id)
                            assert(0);    
                        if (strcmp(channel_mode_cn_map[i].name, value) == 0)
                            return channel_modes[i].id;
                    }
                }
            }

        sr_err("Unkown lang text value:%s,config id:%d", value, config_id);
        return -1;
    }

    num = sizeof(lang_text_map) / sizeof(lang_text_map[0]);
    return sr_option_value_to_code(config_id, value, &lang_text_map[0], num);
}

SR_PRIV struct sr_dev_driver px_driver_test_info = {
    .name = "PX_Logic",
	.longname = "PX_Logic",
	.api_version = 1,
    .driver_type = DRIVER_TYPE_HARDWARE,
	.init = hw_init,
	.cleanup = hw_cleanup,
	.scan = hw_scan, 
    .dev_mode_list = hw_dev_mode_list,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = hw_dev_open,
	.dev_close = hw_dev_close,
    .dev_destroy = dev_destroy,
    .dev_status_get = hw_dev_status_get,
	.dev_acquisition_start = hw_dev_acquisition_start,
	.dev_acquisition_stop = hw_dev_acquisition_stop,
	.priv = NULL,
};
