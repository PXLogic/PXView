/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * Solomon SSD1306 OLED controller protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Bit-level annotation 枚举 (0-9) ===== */
enum {
    ANN_BIT_DA = 0,
    ANN_BIT_RES,
    ANN_BIT_SL,
    ANN_BIT_CB,
    ANN_BIT_DC,
    ANN_BIT_PG,
    ANN_BIT_CO,
    ANN_BIT_MUX,
    ANN_BIT_PAR,
    ANN_BIT_LAST_BIT,
};

/* ===== Command-level annotation 枚举 (10-48) ===== */
enum {
    ANN_LC = 10,
    ANN_HC,
    ANN_DM,
    ANN_SCA,
    ANN_SPA,
    ANN_SFB,
    ANN_RHS,
    ANN_LHS,
    ANN_VRHS,
    ANN_VLHS,
    ANN_SS,
    ANN_AS,
    ANN_DSL,
    ANN_SCC,
    ANN_SCPU,
    ANN_MC0TS0,
    ANN_MCFFTS0,
    ANN_SVSA,
    ANN_DOR,
    ANN_DOI,
    ANN_ND,
    ANN_ID,
    ANN_SMR,
    ANN_DOFF,
    ANN_DON,
    ANN_PSA,
    ANN_CSU,
    ANN_CSD,
    ANN_SVO,
    ANN_DCR,
    ANN_ZI,
    ANN_SPP,
    ANN_SCPI,
    ANN_SVD,
    ANN_NOP,
    ANN_GR,
    ANN_DA,
    ANN_CB,
    ANN_LAST_CMD,
};

/* ===== Extra annotation 枚举 (49-51) ===== */
enum {
    ANN_BLOCK = ANN_LAST_CMD + 1,
    ANN_WARN,
    NUM_ANN,
};

/* ===== 状态枚举 ===== */
enum ssd1306_state {
    SSD1306_IDLE,
    SSD1306_GET_SLAVE_ADDR,
    SSD1306_WRITE_CONTROL_BYTE,
    SSD1306_SSD_COMMAND,
    SSD1306_SSD_DATA,
};

enum ssd1306_substate {
    SSD1306_SUB_COMMAND,
    SSD1306_SUB_PARAMETER,
    SSD1306_SUB_PARAMETER2,
    SSD1306_SUB_PARAMETER3,
    SSD1306_SUB_PARAMETER4,
    SSD1306_SUB_PARAMETER5,
    SSD1306_SUB_PARAMETER6,
};

/* ===== 命令表结构 ===== */
typedef struct {
    uint8_t cmd_byte;
    int ann_id;
    const char *texts[3];
    int has_param;
} ssd1306_cmd_entry;

static const ssd1306_cmd_entry ssd1306_cmds[] = {
    {0x00, ANN_LC,     {"Set Lower Column Start Address", "Set L Col Start", "LC"}, 0},
    {0x10, ANN_HC,     {"Set Higher Column Start Address", "Set H Col Start", "HC"}, 0},
    {0x20, ANN_DM,     {"Set Display Mode", "Set Dsp Md", "DM"}, 1},
    {0x21, ANN_SCA,    {"Set Column Address", "Set Col Adr", "CA"}, 1},
    {0x22, ANN_SPA,    {"Set Page Address", "Set Pg Adr", "PA"}, 1},
    {0x23, ANN_SFB,    {"Set Fade-out and Blinking", "Set FO Blnk", "FB"}, 1},
    {0x26, ANN_RHS,    {"Right horizontal scroll", "Right hor scr", "RHS"}, 1},
    {0x27, ANN_LHS,    {"Left horizontal scroll", "Left hor scr", "LHS"}, 1},
    {0x29, ANN_VRHS,   {"Vertical and right horizontal scroll", "Vert right hor scr", "VRHS"}, 1},
    {0x2A, ANN_VLHS,   {"Vertical and left horizontal scroll", "Vert left hor scr", "VLHS"}, 1},
    {0x2E, ANN_SS,     {"Stop scrolling", "Stsc", "SS"}, 0},
    {0x2F, ANN_AS,     {"Activate scrolling", "Acsc", "AS"}, 0},
    {0x40, ANN_DSL,    {"Display start line", "DSL", "DSL"}, 0},
    {0x81, ANN_SCC,    {"Set contrast control", "Set Ctr", "SC"}, 1},
    {0x8D, ANN_SCPU,   {"Set charge pump", "Set Ch pmp", "SP"}, 1},
    {0xA0, ANN_MC0TS0, {"Map col addr0 to seg0", "Map C0 to S0", "M00"}, 0},
    {0xA1, ANN_MCFFTS0,{"Map col addr7f to seg0", "Map C7f to S0", "M7f0"}, 0},
    {0xA3, ANN_SVSA,   {"Set vertical scroll area", "Set vert scr ar", "SVSA"}, 1},
    {0xA4, ANN_DOR,    {"Display on, resume to RAM", "Dis on, res RAM", "D1R"}, 0},
    {0xA5, ANN_DOI,    {"Display on, ignore RAM", "Dis on, ign RAM", "D1I"}, 0},
    {0xA6, ANN_ND,     {"Normal display", "Norm disp", "DN"}, 0},
    {0xA7, ANN_ID,     {"Inverse display", "Inv disp", "DI"}, 0},
    {0xA8, ANN_SMR,    {"Set multiplex ratio", "Set MUX rat", "MUX"}, 1},
    {0xAE, ANN_DOFF,   {"Display OFF", "Dis OFF", "DO"}, 0},
    {0xAF, ANN_DON,    {"Display ON", "Dis ON", "D1"}, 0},
    {0xB0, ANN_PSA,    {"Page start address", "Pg start", "PS"}, 0},
    {0xC0, ANN_CSU,    {"COM scan 0 to mux", "C scan upw", "SCU"}, 0},
    {0xC8, ANN_CSD,    {"COM scan mux to 0", "C scan dwd", "SCD"}, 0},
    {0xD3, ANN_SVO,    {"Set vertical offset", "Set vert ofs", "VO"}, 1},
    {0xD5, ANN_DCR,    {"Display clock ratio", "Clock ratio", "CR"}, 1},
    {0xD6, ANN_ZI,     {"Set zoom-in", "Zoom in", "ZI"}, 1},
    {0xD9, ANN_SPP,    {"Set precharge period", "Pre chrg", "PC"}, 1},
    {0xDA, ANN_SCPI,   {"Set COM pins", "COM pins", "CP"}, 1},
    {0xDB, ANN_SVD,    {"Set Vcomh deselect", "Vcomh desel", "VD"}, 1},
    {0xE3, ANN_NOP,    {"No operation", "NOP", "NOP"}, 0},
    {0xFF, -1,         {NULL, NULL, NULL}, 0}, /* sentinel */
};

/* ===== 私有数据结构 ===== */
typedef struct {
    enum ssd1306_state state;
    enum ssd1306_substate substate;
    int prevreg;
    char blockstring[512];
    uint64_t ss, es, ss_block, sscmd;
    int out_ann;
} ssd1306_state;

#define SSD1306_I2C_ADDRESS  0x3C
#define SSD1306_I2C_ADDRESS_2 0x3D

/* ===== 静态数据 ===== */
static const char *ssd1306_inputs[] = {"i2c", NULL};
static const char *ssd1306_tags[] = {"Display", "IC", NULL};

static const char *ssd1306_ann_labels[][3] = {
    /* 0-9: bit-level */
    {"", "bit_display_addressing", "Display Addressing bit"},
    {"", "bit_reserved", "Reserved bit"},
    {"", "bit_start_line", "Start Line bit"},
    {"", "bit_continuation", "Continuation bit"},
    {"", "bit_data_command", "Data / Command bit"},
    {"", "bit_page", "Page bit"},
    {"", "bit_column", "Column bit"},
    {"", "bit_mux", "mux bit"},
    {"", "bit_parameter", "Parameter bit"},
    {"", "bit_last", "Last bit"},
    /* 10-48: command-level */
    {"", "cmd_lowercolstart", "Set Lower Column Start Address"},
    {"", "cmd_highercolstart", "Set Higher Column Start Address"},
    {"", "cmd_displaymode", "Set Display Mode"},
    {"", "cmd_setcoladdress", "Set Column Address"},
    {"", "cmd_setpageaddress", "Set Page Address"},
    {"", "cmd_setfadeoutblinking", "Set Fade-out and Blinking"},
    {"", "cmd_righthorscroll", "Right horizontal scroll"},
    {"", "cmd_lefthorscroll", "Left horizontal scroll"},
    {"", "cmd_vertrighthorscroll", "Vertical and right horizontal scroll"},
    {"", "cmd_vertlefthorscroll", "Vertical and left horizontal scroll"},
    {"", "cmd_stopscrolling", "Stop scrolling"},
    {"", "cmd_activatescrolling", "Activate scrolling"},
    {"", "cmd_displaystartline", "Display start line"},
    {"", "cmd_setcontrast", "Set contrast control"},
    {"", "cmd_setchargepump", "Set charge pump"},
    {"", "cmd_mapcol0toseg0", "Map col addr0 to seg0"},
    {"", "cmd_mapcol127toseg0", "Map col addr7f to seg0"},
    {"", "cmd_setvertscrollarea", "Set vertical scroll area"},
    {"", "cmd_displayonresume", "Display on, resume to RAM"},
    {"", "cmd_displayonignore", "Display on, ignore RAM"},
    {"", "cmd_normaldisplay", "Normal display"},
    {"", "cmd_inversedisplay", "Inverse display"},
    {"", "cmd_setmultiplexratio", "Set multiplex ratio"},
    {"", "cmd_displayoff", "Display OFF"},
    {"", "cmd_displayon", "Display ON"},
    {"", "cmd_pgstartaddr", "Page start address"},
    {"", "cmd_comscanup", "COM scan 0 to mux"},
    {"", "cmd_comscandown", "COM scan mux to 0"},
    {"", "cmd_setverticaloffset", "Set vertical offset"},
    {"", "cmd_displayclockratio", "Display clock ratio"},
    {"", "cmd_zoomin", "Set zoom-in"},
    {"", "cmd_prechargeperiod", "Set precharge period"},
    {"", "cmd_setcompins", "Set COM pins"},
    {"", "cmd_setvcomhdeselect", "Set Vcomh deselect"},
    {"", "cmd_nop", "No operation"},
    {"", "cmd_gddram", "GDDRAM data write"},
    {"", "cmd_deviceaddress", "Device address"},
    {"", "cmd_controlbyte", "Control byte"},
    {"", "cmd_last", "Last command marker"},
    /* 49-50: extra */
    {"", "write_block", "Write block"},
    {"", "warning", "Warning"},
};

static const int ssd1306_row_bits_classes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1
};
static const int ssd1306_row_cmds_classes[] = {
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, -1
};
static const int ssd1306_row_blockdata_classes[] = {ANN_BLOCK, -1};
static const int ssd1306_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row ssd1306_ann_rows[] = {
    {"bits", "Bits", ssd1306_row_bits_classes, 10},
    {"cmds", "Commands", ssd1306_row_cmds_classes, 39},
    {"blockdata", "Block Data", ssd1306_row_blockdata_classes, 1},
    {"warnings", "Warnings", ssd1306_row_warnings_classes, 1},
};

/* ===== 辅助函数 ===== */
static const ssd1306_cmd_entry *ssd1306_find_cmd(uint8_t b)
{
    for (int i = 0; ssd1306_cmds[i].cmd_byte != 0xFF; i++) {
        if (ssd1306_cmds[i].cmd_byte == b)
            return &ssd1306_cmds[i];
    }
    return NULL;
}

static void ssd1306_output_block(struct srd_decoder_inst *di, ssd1306_state *s)
{
    if (s->blockstring[0] != '\0') {
        c_put(di, s->sscmd, s->es, s->out_ann, ANN_BLOCK, s->blockstring);
        s->blockstring[0] = '\0';
    }
}

static void ssd1306_handle_command(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t b, uint8_t param_byte, int is_param);

static void ssd1306_handle_par_0x00(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int val = param & 0xf;
    char buf[64];
    snprintf(buf, sizeof(buf), "Lwr col start addr= %d", val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_LC, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "= %d", val);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x10(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int val = param & 0xf;
    char buf[64];
    snprintf(buf, sizeof(buf), "Hghr col start addr= %d", val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_HC, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "= %d", val);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x20(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    static const char *am[] = {"hor. addr.", "vert. addr.", "page addr.", "invalid"};
    static const char *am2[] = {"HA", "VA", "PA", "IV"};
    int mode = param & 3;
    char buf[256];
    snprintf(buf, sizeof(buf), "Display mode: %s", am[mode]);
    c_put(di, s->ss, s->es, s->out_ann, ANN_DM, buf, am2[mode]);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": %s", am[mode]);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x21(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    char buf[64];
    if (s->substate == SSD1306_SUB_PARAMETER) {
        int sc = param & 0x7f;
        const char *res = (sc == 0) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "Start column: %d%s", sc, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SCA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " from %d%s ", sc, res);
        s->substate = SSD1306_SUB_PARAMETER2;
    } else if (s->substate == SSD1306_SUB_PARAMETER2) {
        int ec = param & 0x7f;
        const char *res = (ec == 0x7f) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "End column: %d%s", ec, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SCA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "to %d%s", ec, res);
        s->substate = SSD1306_SUB_COMMAND;
    }
}

static void ssd1306_handle_par_0x22(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    char buf[64];
    if (s->substate == SSD1306_SUB_PARAMETER) {
        int sp = param & 0x7;
        const char *res = (sp == 0) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "Start Page: %d%s", sp, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SPA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " from %d%s ", sp, res);
        s->substate = SSD1306_SUB_PARAMETER2;
    } else if (s->substate == SSD1306_SUB_PARAMETER2) {
        int ep = param & 0x7;
        const char *res = (ep == 0x7) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "End Page: %d%s", ep, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SPA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "to %d%s", ep, res);
        s->substate = SSD1306_SUB_COMMAND;
    }
}

static void ssd1306_handle_par_0x23(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    static const char *bf[] = {"no FA / blnk", "invalid", "fade-out", "blink"};
    int idx = (param >> 4) & 3;
    int fo = ((param & 0xf) << 3) + 8;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (%d frames)", bf[idx], fo);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SFB, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": %s, %d frames", bf[idx], fo);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x26(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    static const int iv_table[] = {5, 64, 128, 256, 3, 4, 25, 2};
    char buf[64];
    if (s->substate == SSD1306_SUB_PARAMETER) {
        s->substate = SSD1306_SUB_PARAMETER2;
    } else if (s->substate == SSD1306_SUB_PARAMETER2) {
        int sp = param & 0x7;
        snprintf(buf, sizeof(buf), "Start Page: %d", sp);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_RHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " from page %d, ", sp);
        s->substate = SSD1306_SUB_PARAMETER3;
    } else if (s->substate == SSD1306_SUB_PARAMETER3) {
        int iv = iv_table[param & 0x7];
        snprintf(buf, sizeof(buf), "Scroll Interval: %d", iv);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_RHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "time interval %d, ", iv);
        s->substate = SSD1306_SUB_PARAMETER4;
    } else if (s->substate == SSD1306_SUB_PARAMETER4) {
        int ep = param & 0x7;
        snprintf(buf, sizeof(buf), "End Page: %d", ep);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_RHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "to page %d", ep);
        s->substate = SSD1306_SUB_PARAMETER5;
    } else if (s->substate == SSD1306_SUB_PARAMETER5) {
        s->substate = SSD1306_SUB_PARAMETER6;
    } else if (s->substate == SSD1306_SUB_PARAMETER6) {
        s->substate = SSD1306_SUB_COMMAND;
    }
}

static void ssd1306_handle_par_0x29(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    static const int iv_table[] = {5, 64, 128, 256, 3, 4, 25, 2};
    char buf[64];
    if (s->substate == SSD1306_SUB_PARAMETER) {
        s->substate = SSD1306_SUB_PARAMETER2;
    } else if (s->substate == SSD1306_SUB_PARAMETER2) {
        int sp = param & 0x7;
        snprintf(buf, sizeof(buf), "Start Page: %d", sp);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_VRHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " from page %d, ", sp);
        s->substate = SSD1306_SUB_PARAMETER3;
    } else if (s->substate == SSD1306_SUB_PARAMETER3) {
        int iv = iv_table[param & 0x7];
        snprintf(buf, sizeof(buf), "Scroll Interval: %d", iv);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_VRHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "time interval %d, ", iv);
        s->substate = SSD1306_SUB_PARAMETER4;
    } else if (s->substate == SSD1306_SUB_PARAMETER4) {
        int ep = param & 0x7;
        snprintf(buf, sizeof(buf), "End Page: %d", ep);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_VRHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "to page %d", ep);
        s->substate = SSD1306_SUB_PARAMETER5;
    } else if (s->substate == SSD1306_SUB_PARAMETER5) {
        int vso = param & 0x3f;
        snprintf(buf, sizeof(buf), "Vert Scroll Ofs: %d", vso);
        c_put(di, s->ss, s->es, s->out_ann, ssd1306_find_cmd(s->prevreg) ? ssd1306_find_cmd(s->prevreg)->ann_id : ANN_VRHS, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " (vertical offset= %d rows)", vso);
        s->substate = SSD1306_SUB_COMMAND;
    }
}

static void ssd1306_handle_par_0x40(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int val = param & 0x3f;
    char buf[64];
    snprintf(buf, sizeof(buf), "Start line= %d", val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_DSL, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "= %d", val);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x81(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    char buf[64];
    const char *res = (param == 0x7f) ? " (reset)" : "";
    snprintf(buf, sizeof(buf), "Contrast= %d%s", param, res);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SCC, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " to %d%s ", param, res);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0x8d(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    const char *cp = (param & 4) ? "on" : "off";
    const char *res = (param & 4) ? "" : " (reset)";
    char buf[64];
    snprintf(buf, sizeof(buf), "Charge pump= %s", cp);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SCPU, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " to %s%s", cp, res);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xa3(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    char buf[64];
    if (s->substate == SSD1306_SUB_PARAMETER) {
        int tfr = param & 0x3f;
        const char *res = (tfr == 0) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "Top fixed rows: %d%s", tfr, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SVSA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ", top fixed rows: %d%s, ", tfr, res);
        s->substate = SSD1306_SUB_PARAMETER2;
    } else if (s->substate == SSD1306_SUB_PARAMETER2) {
        int sr = param & 0x7f;
        const char *res = (sr == 0x40) ? " (reset)" : "";
        snprintf(buf, sizeof(buf), "Scroll rows: %d%s", sr, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SVSA, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "scroll rows: %d%s", sr, res);
        s->substate = SSD1306_SUB_COMMAND;
    }
}

static void ssd1306_handle_par_0xa8(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int mux = (param & 0x3f) + 1;
    if (mux < 16) {
        char buf[64];
        snprintf(buf, sizeof(buf), "invalid multiplex ratio < 16 (%d)", mux);
        c_put(di, s->sscmd, s->es, s->out_ann, ANN_WARN, buf);
        s->blockstring[0] = '\0';
    } else {
        const char *res = (mux == 64) ? " (reset)" : "";
        char buf[64];
        snprintf(buf, sizeof(buf), "%d%s", mux, res);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SMR, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " to %d%s", mux, res);
    }
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xb0(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int val = param & 0x7;
    char buf[64];
    snprintf(buf, sizeof(buf), "Page start addr= %d", val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_PSA, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, "= %d", val);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xd3(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int vo = param & 0x3f;
    char buf[64];
    snprintf(buf, sizeof(buf), "Vertical offset = %d", vo);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SVO, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, " = %d", vo);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xd5(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int of = (param >> 4) & 0xf;
    int dr = (param & 0xf) + 1;
    const char *res = (of == 8) ? "(reset)" : "";
    char buf[256];
    snprintf(buf, sizeof(buf), "Freq=%d, div ratio=%d %s", of, dr, res);
    c_put(di, s->ss, s->es, s->out_ann, ANN_DCR, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": fOSC=%d, divide ratio=%d %s", of, dr, res);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xd6(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    const char *zo = (param & 1) ? "enable" : "disable (reset)";
    char buf[64];
    snprintf(buf, sizeof(buf), "Zoom-in: %s", zo);
    c_put(di, s->ss, s->es, s->out_ann, ANN_ZI, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": %s", zo);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xd9(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int p1 = param & 0xf;
    int p2 = (param >> 4) & 0xf;
    if (p1 == 0 || p2 == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "invalid precharge period = 0 (p1: %d, p2: %d)", p1, p2);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_WARN, buf);
        s->blockstring[0] = '\0';
    } else {
        const char *res1 = (p1 == 2) ? " (reset)" : "";
        const char *res2 = (p2 == 2) ? " (reset)" : "";
        char buf[256];
        snprintf(buf, sizeof(buf), "P1=%d%s, P2=%d%s", p1, res1, p2, res2);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SPP, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": P1=%d%s, P2=%d%s", p1, res1, p2, res2);
    }
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xda(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    const char *seq = (param & 0x20) ? "sequential" : "alternative";
    const char *lrm = (param & 0x10) ? "no " : "";
    char buf[256];
    snprintf(buf, sizeof(buf), "COM pins: %s, %s L/R remap", seq, lrm);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SCPI, buf);
    int pos = (int)strlen(s->blockstring);
    snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": %s, %s L/R remap", seq, lrm);
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_par_0xdb(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t param)
{
    int vc = (param >> 4) & 7;
    static const char *vcomh[] = {"0.65 Vcc", "", "0.77 Vcc (reset)", "0.83 Vcc"};
    if (vc != 0 && vc != 2 && vc != 3) {
        char buf[64];
        snprintf(buf, sizeof(buf), "invalid Vcomh deselect = 0x%02x", vc);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_WARN, buf);
        s->blockstring[0] = '\0';
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Vcomh = %s", vcomh[vc]);
        c_put(di, s->ss, s->es, s->out_ann, ANN_SVD, buf);
        int pos = (int)strlen(s->blockstring);
        snprintf(s->blockstring + pos, sizeof(s->blockstring) - pos, ": Vcomh = %s", vcomh[vc]);
    }
    s->substate = SSD1306_SUB_COMMAND;
}

static void ssd1306_handle_command(struct srd_decoder_inst *di,
    ssd1306_state *s, uint8_t b, uint8_t param_byte, int is_param)
{
    /* Normalize range commands */
    uint8_t b1 = b;
    if (b <= 0x0F) {
        b1 = b;
        b = 0x00;
    } else if (b >= 0x10 && b <= 0x1F) {
        b1 = b;
        b = 0x10;
    } else if (b >= 0x40 && b <= 0x7F) {
        b1 = b;
        b = 0x40;
    } else if (b >= 0xB0 && b <= 0xB7) {
        b1 = b;
        b = 0xB0;
    }

    const ssd1306_cmd_entry *entry = ssd1306_find_cmd(b);
    if (!entry)
        return;

    if (s->substate == SSD1306_SUB_COMMAND && !is_param) {
        /* New command */
        c_put(di, s->ss, s->es, s->out_ann, entry->ann_id, entry->texts[0], entry->texts[1], entry->texts[2]);
        snprintf(s->blockstring, sizeof(s->blockstring), "%s", entry->texts[0]);
        s->sscmd = s->ss_block;
        s->prevreg = b;
        if (entry->has_param)
            s->substate = SSD1306_SUB_PARAMETER;
        /* Handle range commands immediately */
        if (b == 0x00) ssd1306_handle_par_0x00(di, s, b1);
        else if (b == 0x10) ssd1306_handle_par_0x10(di, s, b1);
        else if (b == 0x40) ssd1306_handle_par_0x40(di, s, b1);
        else if (b == 0xB0) ssd1306_handle_par_0xb0(di, s, b1);
    } else if (is_param) {
        /* Parameter for current command */
        switch (b) {
        case 0x20: ssd1306_handle_par_0x20(di, s, param_byte); break;
        case 0x21: ssd1306_handle_par_0x21(di, s, param_byte); break;
        case 0x22: ssd1306_handle_par_0x22(di, s, param_byte); break;
        case 0x23: ssd1306_handle_par_0x23(di, s, param_byte); break;
        case 0x26: ssd1306_handle_par_0x26(di, s, param_byte); break;
        case 0x27: ssd1306_handle_par_0x26(di, s, param_byte); break; /* same as 0x26 */
        case 0x29: ssd1306_handle_par_0x29(di, s, param_byte); break;
        case 0x2A: ssd1306_handle_par_0x29(di, s, param_byte); break; /* same as 0x29 */
        case 0x81: ssd1306_handle_par_0x81(di, s, param_byte); break;
        case 0x8D: ssd1306_handle_par_0x8d(di, s, param_byte); break;
        case 0xA3: ssd1306_handle_par_0xa3(di, s, param_byte); break;
        case 0xA8: ssd1306_handle_par_0xa8(di, s, param_byte); break;
        case 0xD3: ssd1306_handle_par_0xd3(di, s, param_byte); break;
        case 0xD5: ssd1306_handle_par_0xd5(di, s, param_byte); break;
        case 0xD6: ssd1306_handle_par_0xd6(di, s, param_byte); break;
        case 0xD9: ssd1306_handle_par_0xd9(di, s, param_byte); break;
        case 0xDA: ssd1306_handle_par_0xda(di, s, param_byte); break;
        case 0xDB: ssd1306_handle_par_0xdb(di, s, param_byte); break;
        default: s->substate = SSD1306_SUB_COMMAND; break;
        }
    }

    if (s->substate == SSD1306_SUB_COMMAND)
        ssd1306_output_block(di, s);
}

/* ===== recv_proto ===== */
static void ssd1306_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ssd1306_state *s = (ssd1306_state *)c_decoder_get_private(di);
    if (!s) return;
    s->ss = start_sample;
    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "BITS") == 0) return;

    switch (s->state) {
    case SSD1306_IDLE:
        if (strcmp(cmd, "START") == 0) {
            s->state = SSD1306_GET_SLAVE_ADDR;
            s->ss_block = start_sample;
        }
        break;
    case SSD1306_GET_SLAVE_ADDR:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            if (databyte != SSD1306_I2C_ADDRESS && databyte != SSD1306_I2C_ADDRESS_2) {
                s->state = SSD1306_IDLE;
                return;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "Device address: 0x%02X", databyte);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_DA, buf);
            s->state = SSD1306_WRITE_CONTROL_BYTE;
        }
        break;
    case SSD1306_WRITE_CONTROL_BYTE:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Control byte = 0x%02X", databyte);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_CB, buf);
            if (databyte == 0x80)
                s->state = SSD1306_SSD_COMMAND;
            else if (databyte == 0x40)
                s->state = SSD1306_SSD_DATA;
            else
                s->state = SSD1306_IDLE;
            s->substate = SSD1306_SUB_COMMAND;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = SSD1306_IDLE;
        }
        break;
    case SSD1306_SSD_COMMAND:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->ss_block = start_sample;
            if (s->substate == SSD1306_SUB_COMMAND) {
                ssd1306_handle_command(di, s, databyte, 0, 0);
            } else {
                ssd1306_handle_command(di, s, s->prevreg, databyte, 1);
            }
            s->state = SSD1306_WRITE_CONTROL_BYTE;
        }
        break;
    case SSD1306_SSD_DATA:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "GDDRAM data: 0x%02X", databyte);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_GR, buf);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = SSD1306_IDLE;
        }
        break;
    }
}

/* ===== 生命周期回调 ===== */
static void ssd1306_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ssd1306_state)));
    }
    ssd1306_state *s = (ssd1306_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ssd1306_state));
    s->substate = SSD1306_SUB_COMMAND;
    s->prevreg = -1;
}

static void ssd1306_start(struct srd_decoder_inst *di)
{
    ssd1306_state *s = (ssd1306_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ssd1306");
}

static void ssd1306_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ssd1306_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器结构体 ===== */
struct srd_c_decoder ssd1306_c_decoder = {
    .id = "ssd1306_c",
    .name = "SSD1306(C)",
    .longname = "Solomon 1306",
    .desc = "Solomon SSD1306 OLED controller protocol.",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ssd1306_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ssd1306_ann_rows,
    .inputs = ssd1306_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ssd1306_tags,
    .num_tags = 2,
    .reset = ssd1306_reset,
    .start = ssd1306_start,
    .decode = ssd1306_decode,
    .destroy = ssd1306_destroy,
    .decode_upper = ssd1306_recv_proto,
    .state_size = 0,
};

/* ===== 导出函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ssd1306_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}