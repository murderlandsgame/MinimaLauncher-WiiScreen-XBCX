/****************************************************************************
 * Copyright (C) 2012-2014
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
 ****************************************************************************/

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <ogcsys.h>
#include <unistd.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <ogc/machine/processor.h>
#include "apploader.h"
#include "memory.h"
#include "utils.h"
#include "disc.h"
#include "wdvd.h"
#include "gecko.h"
#include "defines.h"
#include "gc.hpp"
#include "fst.h"
#include "gameconfig_ssbb.h"
#include "gameconfig_kirby.h"
#include "patchcode.h"

#define FIFO_SIZE (256*1024)
#define STATE_GEOMETRY 1
#define STATE_TEXT 2

/* Boot Variables */
u32 GameIOS = 0;
u32 vmode_reg = 0;
GXRModeObj *vmode = NULL;

u32 AppEntrypoint = 0;

static void *xfb = NULL;
static GXRModeObj rmode, nextmode;
static sys_fontheader *fontdata;
static GXTexObj fonttex;
static int draw_state = STATE_GEOMETRY;
static int text_x;
static int text_y;
static int font_size;
static u32 text_color = 0xffffffff;
static int active_control = 0;

static void format_value_u16(char *buffer, void *data);
static void format_value_videomode(char *buffer, void *data);
static void format_value_tvmode(char *buffer, void *data);
static void format_value_xfbmode(char *buffer, void *data);

static void change_value_u16(u32 pressed, u32 held, void *data);
static void change_value_videomode(u32 pressed, u32 held, void *data);
static void change_value_tvmode(u32 pressed, u32 held, void *data);
static void change_value_xfbmode(u32 pressed, u32 held, void *data);

extern "C" {
extern void __exception_closeall();
}


const static struct _control {
    int x;
    int y;
    const char *label;
    void (*format_value)(char *buffer, void *data);
    void (*change_value)(u32 pressed, u32 held, void *data);
    void *data;
} controls[] = {
    { 200, 0, "Video mode: ", format_value_videomode, change_value_videomode, &nextmode, },
    { 200, 40, "TV mode: ", format_value_tvmode, change_value_tvmode, &nextmode.viTVMode, },
    { 60, 60, "FB width: ", format_value_u16, change_value_u16, &nextmode.fbWidth, },
    { 60, 80, "XFB mode: ", format_value_xfbmode, change_value_xfbmode, &nextmode.xfbMode, },
    { 360, 60, "EFB height: ", format_value_u16, change_value_u16, &nextmode.efbHeight, },
    { 360, 80, "XFB height: ", format_value_u16, change_value_u16, &nextmode.xfbHeight, },
    { 60, 120, "VI width: ", format_value_u16, change_value_u16, &nextmode.viWidth, },
    { 360, 120, "VI height: ", format_value_u16, change_value_u16, &nextmode.viHeight, },
    { 60, 140, "VI X origin: ", format_value_u16, change_value_u16, &nextmode.viXOrigin, },
    { 360, 140, "VI Y origin: ", format_value_u16, change_value_u16, &nextmode.viYOrigin, },
};
#define NUM_CONTROLS (sizeof(controls) / sizeof(struct _control))

struct _control_label {
    u32 value;
    char *label;
};

#define LABEL(l) { (u32)&l, #l }
const static struct _control_label videomode_labels[] = {
    LABEL(TVNtsc240Ds),
    LABEL(TVNtsc240DsAa),
    LABEL(TVNtsc240Int),
    LABEL(TVNtsc240IntAa),
    LABEL(TVNtsc480Int),
    LABEL(TVNtsc480IntDf),
    LABEL(TVNtsc480IntAa),
    LABEL(TVNtsc480Prog),
    LABEL(TVNtsc480ProgSoft),
    LABEL(TVNtsc480ProgAa),
    LABEL(TVMpal240Ds),
    LABEL(TVMpal240DsAa),
    LABEL(TVMpal240Int),
    LABEL(TVMpal240IntAa),
    LABEL(TVMpal480Int),
    LABEL(TVMpal480IntDf),
    LABEL(TVMpal480IntAa),
    LABEL(TVMpal480Prog),
    LABEL(TVMpal480ProgSoft),
    LABEL(TVMpal480ProgAa),
    LABEL(TVPal264Ds),
    LABEL(TVPal264DsAa),
    LABEL(TVPal264Int),
    LABEL(TVPal264IntAa),
    LABEL(TVPal528Int),
    LABEL(TVPal528IntDf),
    LABEL(TVPal524IntAa),
    LABEL(TVPal576IntDfScale),
    LABEL(TVPal528Prog),
    LABEL(TVPal528ProgSoft),
    LABEL(TVPal524ProgAa),
    LABEL(TVPal576ProgScale),
    LABEL(TVEurgb60Hz240Ds),
    LABEL(TVEurgb60Hz240DsAa),
    LABEL(TVEurgb60Hz240Int),
    LABEL(TVEurgb60Hz240IntAa),
    LABEL(TVEurgb60Hz480Int),
    LABEL(TVEurgb60Hz480IntDf),
    LABEL(TVEurgb60Hz480IntAa),
    LABEL(TVEurgb60Hz480Prog),
    LABEL(TVEurgb60Hz480ProgSoft),
    LABEL(TVEurgb60Hz480ProgAa),
    //LABEL(TV1080),
};
#define NUM_VIDEOMODES (sizeof(videomode_labels) / sizeof(struct _control_label))
#undef LABEL

#define LABEL(l) { l, #l }
const static struct _control_label tvmode_labels[] = {
    LABEL(VI_TVMODE_NTSC_INT),
    LABEL(VI_TVMODE_NTSC_DS),
    LABEL(VI_TVMODE_NTSC_PROG),
    LABEL(VI_TVMODE_PAL_INT),
    LABEL(VI_TVMODE_PAL_DS),
    LABEL(VI_TVMODE_PAL_PROG),
    LABEL(VI_TVMODE_EURGB60_INT),
    LABEL(VI_TVMODE_EURGB60_DS),
    LABEL(VI_TVMODE_EURGB60_PROG),
    LABEL(VI_TVMODE_MPAL_INT),
    LABEL(VI_TVMODE_MPAL_DS),
    LABEL(VI_TVMODE_MPAL_PROG),
    LABEL(VI_TVMODE_DEBUG_INT),
    LABEL(VI_TVMODE_DEBUG_PAL_INT),
    LABEL(VI_TVMODE_DEBUG_PAL_DS),
    //LABEL(VI_TVMODE_1080),
};
#define NUM_TVMODES (sizeof(tvmode_labels) / sizeof(struct _control_label))

const static struct _control_label xfbmode_labels[] = {
    LABEL(VI_XFBMODE_SF),
    LABEL(VI_XFBMODE_DF),
};
#define NUM_XFBMODES (sizeof(xfbmode_labels) / sizeof(struct _control_label))

#undef LABEL

static inline void set_text_pos(int x, int y)
{
    text_x = x;
    text_y = y;
}

static inline void set_text_size(int size)
{
    font_size = size;
}

static inline void set_text_color(u32 color)
{
    text_color = color;
}

static inline int adjustment_by_elapsed_abs(u64 now, u64 start)
{
    u32 elapsed_ms = diff_msec(start, now);
    if (elapsed_ms < 500) return 0;
    if (elapsed_ms < 1500) return elapsed_ms / 500;
    return 4 + (elapsed_ms - 1500) / 100;
}

static int adjustment_by_elapsed(u64 now, u64 start, u64 last)
{
    int total = adjustment_by_elapsed_abs(now, start);
    int prev = adjustment_by_elapsed_abs(last, start);
    return total - prev;
}

const char *label_from_value(const struct _control_label *labels,
                             int num_labels, u32 value)
{
    for (int i = 0; i < num_labels; i++) {
        if (labels[i].value == value)
            return labels[i].label;
    }
    return NULL;
}

static void change_value_label(const struct _control_label *labels, int num_labels,
                               u32 pressed, u32 held, u32 *value)
{
    int index = 0, adjustment = 0;

    if (pressed & WPAD_BUTTON_RIGHT) {
        adjustment = 1;
    } else if (pressed & WPAD_BUTTON_LEFT) {
        adjustment = -1;
    }
    if (adjustment == 0) return;

    for (index = 0; index < num_labels; index++) {
        if (labels[index].value == *value) {
            break;
        }
    }
    if (index < num_labels) { /* found */
        index += adjustment;
        if (index < 0) index = 0;
        else if (index >= num_labels) index = num_labels - 1;
    } else {
        index = 0;
    }

    *value = labels[index].value;
}

static void format_value_u16(char *buffer, void *data)
{
    sprintf(buffer, "% 4hd", *(u16*)data);
}

static void format_value_videomode(char *buffer, void *data)
{
    GXRModeObj *videomode = ((GXRModeObj*)(data));

    const char *label = NULL;
    for (int i = 0; i < NUM_VIDEOMODES; i++) {
        GXRModeObj *mode = ((GXRModeObj*)(((void*)videomode_labels[i].value)));
        // crash without this printf!
        //printf("Comparing %p with %p\n", mode, videomode);
        if (memcmp(mode, videomode, sizeof(GXRModeObj)) == 0) {
            label = videomode_labels[i].label;
            break;
        }
    }
    if (label) {
        strcpy(buffer, label);
    } else {
        strcpy(buffer, "Custom");
    }
}

static void format_value_tvmode(char *buffer, void *data)
{
    u32 tvmode = *(u32*)data;

    const char *label = label_from_value(tvmode_labels, NUM_TVMODES, tvmode);
    if (label) {
        strcpy(buffer, label);
    } else {
        sprintf(buffer, "Unknown (%d)", tvmode);
    }
}

static void format_value_xfbmode(char *buffer, void *data)
{
    u32 xfbmode = *(u32*)data;

    const char *label = label_from_value(xfbmode_labels, NUM_XFBMODES, xfbmode);
    if (label) {
        strcpy(buffer, label);
    } else {
        sprintf(buffer, "Unknown (%d)", xfbmode);
    }
}

static void change_value_u16(u32 pressed, u32 held, void *data)
{
    u16 *value = (u16*)data;
    int adjustment = 0;
    static u64 press_time, last_time;
    u64 now;

    now = gettime();
    if (pressed & WPAD_BUTTON_RIGHT) {
        adjustment = 1;
        press_time = last_time = now;
    } else if (pressed & WPAD_BUTTON_LEFT) {
        adjustment = -1;
        press_time = last_time = now;
    } else if (held & WPAD_BUTTON_RIGHT) {
        adjustment = adjustment_by_elapsed(now, press_time, last_time);
        last_time = now;
    } else if (held & WPAD_BUTTON_LEFT) {
        adjustment = -adjustment_by_elapsed(now, press_time, last_time);
        last_time = now;
    }
    *value += adjustment;
}

static void change_value_videomode(u32 pressed, u32 held, void *data)
{
    GXRModeObj *videomode = (GXRModeObj*)data, *mode;
    int index = 0, adjustment = 0;

    if (pressed & WPAD_BUTTON_RIGHT) {
        adjustment = 1;
    } else if (pressed & WPAD_BUTTON_LEFT) {
        adjustment = -1;
    }
    if (adjustment == 0) return;

    for (index = 0; index < NUM_VIDEOMODES; index++) {
        mode = ((GXRModeObj*)((void*)videomode_labels[index].value));
        if (memcmp(mode, videomode, sizeof(GXRModeObj)) == 0) {
            break;
        }
    }
    if (index < NUM_VIDEOMODES) { /* found */
        index += adjustment;
        if (index < 0) index = 0;
        else if (index >= NUM_VIDEOMODES) index = NUM_VIDEOMODES - 1;
    } else {
        index = 0;
    }

    memcpy(videomode, (void*)videomode_labels[index].value, sizeof(GXRModeObj));
}

static void change_value_tvmode(u32 pressed, u32 held, void *data)
{
    change_value_label(tvmode_labels, NUM_TVMODES, pressed, held, ((u32*)(data)));
}

static void change_value_xfbmode(u32 pressed, u32 held, void *data)
{
    change_value_label(xfbmode_labels, NUM_XFBMODES, pressed, held, ((u32*)(data)));
}

static void activate_font_texture()
{
    u32 texture_size;
    void *texels;

    texels = (void *)fontdata + fontdata->sheet_image;
    GX_InitTexObj(&fonttex, texels,
                  fontdata->sheet_width, fontdata->sheet_height,
                  fontdata->sheet_format, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&fonttex, GX_LINEAR, GX_LINEAR, 0., 0., 0.,
                     GX_TRUE, GX_TRUE, GX_ANISO_1);
    GX_LoadTexObj(&fonttex, GX_TEXMAP0);

    texture_size = GX_GetTexBufferSize(fontdata->sheet_width, fontdata->sheet_height,
                                       fontdata->sheet_format, GX_FALSE, 0);
    DCStoreRange(texels, texture_size);
    GX_InvalidateTexAll();
}

static void set_drawing_state(int state)
{
    if (draw_state == state) return;

    if (state == STATE_TEXT) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0);
        GX_SetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, 1, 1);

        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else if (state == STATE_GEOMETRY) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }

    draw_state = state;
}

static void draw_font_cell(int16_t x1, int16_t y1, uint32_t c, int16_t s1, int16_t t1)
{
    int16_t x2 = x1 + fontdata->cell_width * font_size / fontdata->cell_height;
    int16_t y2 = y1 - font_size;

    int16_t s2 = s1 + fontdata->cell_width;
    int16_t t2 = t1 + fontdata->cell_height;

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);

    GX_Position2s16(x1, y2);
    GX_Color1u32(c);
    GX_TexCoord2s16(s1, t1);

    GX_Position2s16(x2, y2);
    GX_Color1u32(c);
    GX_TexCoord2s16(s2, t1);

    GX_Position2s16(x2, y1);
    GX_Color1u32(c);
    GX_TexCoord2s16(s2, t2);

    GX_Position2s16(x1, y1);
    GX_Color1u32(c);
    GX_TexCoord2s16(s1, t2);

    GX_End();
}

static void setup_font()
{
    if (SYS_GetFontEncoding() == 0) {
        fontdata = (sys_fontheader*)(memalign(32, SYS_FONTSIZE_ANSI));
    } else {
        fontdata = (sys_fontheader*)(memalign(32, SYS_FONTSIZE_SJIS));
    }

    SYS_InitFont(fontdata);
    fontdata->sheet_image = (fontdata->sheet_image + 31) & ~31;
    activate_font_texture();

    font_size = fontdata->cell_height;
}

static void setup_gx()
{
    GXColor backgroundColor = {0, 0, 0, 255};
    void *fifoBuffer = NULL;
    Mtx mv;

    fifoBuffer = MEM_K0_TO_K1(memalign(32,FIFO_SIZE));
    memset(fifoBuffer, 0, FIFO_SIZE);

    GX_Init(fifoBuffer, FIFO_SIZE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCopyClear(backgroundColor, GX_MAX_Z24);

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0,
                   GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    guMtxIdentity(mv);
    guMtxTransApply(mv, mv, 0.4, 0.4, 0);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
}

static void setup_viewport()
{
    Mtx44 proj;
    u32 w, h;

    //w = rmode.fbWidth;
    //h = rmode.efbHeight;
    rmode.fbWidth = 1920;
    rmode.efbHeight = 1080;
    rmode.viWidth = 1920;
    rmode.viHeight = 1080;
    //rmode.xfbHeight = 1080;

	w = 1920;
	h = 1080;

    // matrix, t, b, l, r, n, f
    guOrtho(proj, 0, h, 0, w, 0, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

    GX_SetViewport(0, 0, w, h, 0, 1);

    f32 yscale = GX_GetYScaleFactor(h, rmode.xfbHeight);
    GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, w, h);
    GX_SetDispCopySrc(0, 0, w, h);
    GX_SetDispCopyDst(w, h);
    GX_SetCopyFilter(0, rmode.sample_pattern, GX_FALSE, rmode.vfilter);
}

static int process_string(const char *text, int should_draw)
{
    void *image;
    int32_t xpos, ypos, width, x;

    x = text_x;
    for (; *text != '\0'; text++) {
        char c = *text;
        if (c < fontdata->first_char) {
            continue;
        }
        SYS_GetFontTexture(c, &image, &xpos, &ypos, &width);
        if (should_draw) {
            draw_font_cell(x, text_y, text_color, xpos, ypos);
        }
        x += width * font_size / fontdata->cell_height;
    }

    return x - text_x;
}

static int draw_string(const char *text)
{
    return process_string(text, 1);
}

static int string_width(const char *text)
{
    return process_string(text, 0);
}

static void draw_controls()
{
    char buffer[64];

    int h = rmode.efbHeight;
    int base_y = (h - 160) / 2;
    for (int i = 0; i < NUM_CONTROLS; i++) {
        const struct _control *ctrl = &controls[i];
        int x, y;

        x = ctrl->x;
        y = base_y + ctrl->y;
        if (i == active_control) {
            set_text_color(0xffff00ff);
        } else {
            set_text_color(0xffffffff);
        }
        set_text_pos(x, y);
        set_text_size(18);
        if (ctrl->label) {
            x += draw_string(ctrl->label);
            set_text_pos(x, y);
        }
        ctrl->format_value(buffer, ctrl->data);
        x += draw_string(buffer);
    }
}

static void change_active_control(u32 pressed, u32 held)
{
    const struct _control *ctrl = &controls[active_control];

    if (!ctrl->change_value) return;

    ctrl->change_value(pressed, held, ctrl->data);
}

static void draw_corner_labels()
{
    char buffer[64];
    int w, h, text_w;

    w = rmode.fbWidth;
    h = rmode.efbHeight;
    set_text_size(16);
    set_text_color(0x0000ffff);

    set_text_pos(0, 16);
    draw_string("(0, 0)");

    sprintf(buffer, "(%d, 0)", w);
    text_w = string_width(buffer);
    set_text_pos(w - text_w, 16);
    draw_string(buffer);

    sprintf(buffer, "(%d, %d)", w, h);
    text_w = string_width(buffer);
    set_text_pos(w - text_w, h);
    draw_string(buffer);

    sprintf(buffer, "(0, %d)", h);
    text_w = string_width(buffer);
    set_text_pos(0, h);
    draw_string(buffer);
}

static void draw_help()
{
    int w, h, x, y;

    w = rmode.fbWidth;
    h = rmode.efbHeight;
    set_text_size(16);

    y = h - 10;
    x = w / 4;
    set_text_pos(x, y);
    set_text_color(0xc0c000ff);
    x += draw_string("A");
    set_text_pos(x, y);
    set_text_color(0xffffffff);
    x += draw_string(" - Toggle widescreen");

    x += 40;
    set_text_pos(x, y);
    set_text_color(0x00ff00ff);
    x += draw_string("1");
    set_text_pos(x, y);
    set_text_color(0xffffffff);
    x += draw_string(" - Apply");

    x += 40;
    set_text_pos(x, y);
    set_text_color(0xff0000ff);
    x += draw_string("2");
    set_text_pos(x, y);
    set_text_color(0xffffffff);
    x += draw_string(" - Reset");
}

static void draw_text()
{
    draw_corner_labels();
    draw_help();
    draw_controls();
}

static void draw_line(int x0, int y0, int x1, int y1)
{
    GX_Begin(GX_LINES, GX_VTXFMT0, 2);

    GX_Position2s16(x0, y0);
    GX_Color1u32(0xffffff40);
    GX_Position2s16(x1, y1);
    GX_Color4u8(255, 255, 255, 64);

    GX_End();
}

static void draw_background()
{
    u32 w, h;

    w = rmode.fbWidth;
    h = rmode.efbHeight;

    for (int x = 0; x < w; x += 16) {
        draw_line(x, 0, x, h);
    }
    draw_line(w - 1, 0, w - 1, h);

    for (int y = 0; y < h; y += 16) {
        draw_line(0, y, w, y);
    }
    draw_line(0, h - 1, w, h - 1);

    // Diagonal lines
    draw_line(0, 0, w, h);
    draw_line(w, 0, 0, h);
}

static void apply_settings()
{
    memcpy(&rmode, &nextmode, sizeof(nextmode));

    VIDEO_SetBlack(TRUE);
    VIDEO_Configure(&rmode);
    VIDEO_SetBlack(FALSE);

    VIDEO_Flush();

    VIDEO_WaitVSync();
    if (rmode.viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

    setup_viewport();
}

static void reset_settings()
{
    memcpy(&nextmode, &rmode, sizeof(nextmode));
}

static void toggle_widescreen()
{
    static bool widescreen = false;
    int max_width;

    widescreen = !widescreen;
    switch (nextmode.viTVMode >> 2) {
    case VI_DEBUG:
    case VI_NTSC:
        max_width = VI_MAX_WIDTH_NTSC;
        break;
    case VI_DEBUG_PAL:
    case VI_PAL:
        max_width = VI_MAX_WIDTH_PAL;
        break;
    case VI_MPAL:
        max_width = VI_MAX_WIDTH_MPAL;
        break;
    case VI_EURGB60:
        max_width = VI_MAX_WIDTH_EURGB60;
        break;
    }
    if (widescreen) {
        nextmode.viWidth = 1920;
    } else {
        nextmode.viWidth = 1920;
    }
    nextmode.viXOrigin = (max_width - nextmode.viWidth) / 2;
}

int main()
{
    #define RETURNTIDTHIS 0x0001000135474833 //change to hex of title id for the channel you are making of the compiled dol

	const GXRModeObj *vmode;

    nextmode.fbWidth = 1920;
    nextmode.efbHeight = 1080;
    nextmode.viHeight = 1080;
    nextmode.viWidth = 1920;
    nextmode.viXOrigin = 0;
    nextmode.viYOrigin = 0;


    // Initialise the video system
    VIDEO_Init();

    // This function initialises the attached controllers
    WPAD_Init();

    // Obtain the preferred video mode from the system
    // This will correspond to the settings in the Wii menu
    vmode = VIDEO_GetPreferredMode(NULL);
    memcpy(&rmode, vmode, sizeof(rmode));
    memcpy(&nextmode, vmode, sizeof(nextmode));

    // Allocate memory for the display in the uncached region
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&rmode));

    // Set up the video registers with the chosen mode
    VIDEO_Configure(&rmode);

    // Tell the video hardware where our display memory is
    VIDEO_SetNextFramebuffer(xfb);

    // Make the display visible
    VIDEO_SetBlack(FALSE);

    // Flush the video register changes to the hardware
    VIDEO_Flush();

    // Wait for Video setup to complete
    VIDEO_WaitVSync();
    if (rmode.viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

    setup_gx();
    setup_font();
    setup_viewport();

    while (1) {

        // Call WPAD_ScanPads each loop, this reads the latest controller states
        WPAD_ScanPads();

        // WPAD_ButtonsDown tells us which buttons were pressed in this loop
        // this is a "one shot" state which will not fire again until the button has been released
        u32 pressed = WPAD_ButtonsDown(0);
        u32 held = WPAD_ButtonsHeld(0);

        // We return to the launcher application via exit
        /*if (pressed & WPAD_BUTTON_HOME) exit(0);

        if (pressed & WPAD_BUTTON_UP) {
            active_control--;
        } else if (pressed & WPAD_BUTTON_DOWN) {
            active_control++;
        } else if (pressed & WPAD_BUTTON_1) {
            apply_settings();
			break;
        } else if (pressed & WPAD_BUTTON_2) {
            reset_settings();
        } else if (pressed & WPAD_BUTTON_A) {
            toggle_widescreen();
        }*/
        active_control %= NUM_CONTROLS;

        change_active_control(pressed, held);

        set_drawing_state(STATE_GEOMETRY);
        draw_background();

        set_drawing_state(STATE_TEXT);
        draw_text();

		//toggle_widescreen();
		apply_settings();


        GX_DrawDone();
        GX_CopyDisp(xfb, GX_TRUE);
        GX_Flush();

        // Wait for the next frame
        VIDEO_WaitVSync();
		break;
    }

	//if ( (*(u32*)(0xCD8005A0) >> 16 ) == 0xCAFE ) // Wii U
	//{
		/* vWii widescreen patch by tueidj */
		write32(0xd8006a0, 0x30000004), mask32(0xd8006a8, 0, 2);
	//}

	InitGecko();
	gprintf("MinimaLauncher v1.2 OC\n");
	VIDEO_Init();
	WPAD_Init();
	PAD_Init();

	/* Setup Low Memory */
	Disc_SetLowMemPre();

	/* Get Disc Status */
	WDVD_Init();
	u32 disc_check = 0;
	WDVD_GetCoverStatus(&disc_check);
	if(disc_check & 0x2)
	{
		/* Open up Disc */
		Disc_Open();
		if(Disc_IsGC() == 0)
		{
			WII_Initialize();
			gprintf("Booting GC Disc\n");
			/* Set Video Mode and Language */
			u8 region = ((u8*)Disc_ID)[3];
			u8 videoMode = 1; //PAL 576i
			if(region == 'E' || region == 'J')
				videoMode = 2; //NTSC 480i
			GC_SetVideoMode(videoMode);
			GC_SetLanguage();
			/* if DM is installed */
			DML_New_SetBootDiscOption();
			DML_New_WriteOptions();
			/* Set time */
			Disc_SetTime();
			/* NTSC-J Patch by FIX94 */
			if(region == 'J')
				*HW_PPCSPEED = 0x0002A9E0;
			/* Boot BC */
			WII_LaunchTitle(GC_BC);
		}
		else if(Disc_IsWii() == 0)
		{
			FILE *f = NULL;
			size_t fsize = 0;
			/* read in cheats */
			WDVD_ReadDiskId((u8*)Disc_ID);
			/*const DISC_INTERFACE *sd = &__io_wiisd;
			sd->startup();
			if(sd->isInserted())
				gprintf("sd inserted!\n");
			fatMountSimple("sd", sd);
			/* gameconfig */
			/*if((*(u32*)Disc_ID & 0xFFFFFF00) == 0x52534200)//rsb?01 brawl
				app_gameconfig_load((char*)Disc_ID, (u8*)gameconfig_ssbb, gameconfig_ssbb_size);
			else if(*(u32*)Disc_ID == 0x53554B45)//suke01 kirby
				app_gameconfig_load((char*)Disc_ID, (u8*)gameconfig_kirby, gameconfig_kirby_size);			
			else
			{
				f = fopen("sd:/gameconfig.txt", "rb");
				if(f != NULL)
				{
					fseek(f, 0, SEEK_END);
					fsize = ftell(f);
					rewind(f);
					u8 *gameconfig = (u8*)malloc(fsize);
					fread(gameconfig, fsize, 1, f);
					fclose(f);
					app_gameconfig_load((char*)Disc_ID, gameconfig, fsize);
					free(gameconfig);
				}
			}
			/* gct */
			/*char gamepath[22];
			if((*(u32*)Disc_ID & 0xFFFFFF00) == 0x52534200)//rsb?01 brawl
			{
				PAD_ScanPads();
				u32 pad_down = PAD_ButtonsDown(0) | PAD_ButtonsDown(1) | PAD_ButtonsDown(2) | PAD_ButtonsDown(3);
				WPAD_ScanPads();
				u32 wpad_down = WPAD_ButtonsDown(0) | WPAD_ButtonsDown(1) | WPAD_ButtonsDown(2) | WPAD_ButtonsDown(3);
				if ((wpad_down & WPAD_BUTTON_LEFT) || (pad_down & PAD_BUTTON_LEFT))
					sprintf(gamepath, "sd:/codes/ProjectM.gct");
				else if ((wpad_down & WPAD_BUTTON_RIGHT) || (pad_down & PAD_BUTTON_RIGHT))
					sprintf(gamepath, "sd:/codes/Minus.gct");
				else
					sprintf(gamepath, "sd:/codes/%.6s.gct", (char*)Disc_ID);
			}
			else
			{
				sprintf(gamepath, "sd:/codes/%.6s.gct", (char*)Disc_ID);
			}
			gprintf("%s\n", gamepath);
			f = fopen(gamepath, "rb");
			if(f != NULL)
			{
				gprintf("Opened gct\n");
				fseek(f, 0, SEEK_END);
				fsize = ftell(f);
				rewind(f);
				u8 *cheats = (u8*)malloc(fsize);
				fread(cheats, fsize, 1, f);
				fclose(f);
				ocarina_set_codes((void*)0x800022A8, (u8*)0x80003000, cheats, fsize);
				free(cheats);
			}
			fatUnmount("sd");
			sd->shutdown();*/
			/* Find our Partition */
			u32 offset = 0;
			Disc_FindPartition(&offset);
			WDVD_OpenPartition(offset, &GameIOS);
			gprintf("Using IOS: %i\n", GameIOS);
			WDVD_Close();
			if((*(u32*)Disc_ID & 0xFFFFFF00) == 0x52534200)
				IOS_ReloadIOS(58); //for SDHC support
			else
				IOS_ReloadIOS(GameIOS); //supports all other games
			/* Re-Init after IOS Reload */
			WDVD_Init();
			WDVD_ReadDiskId((u8*)Disc_ID);
			WDVD_OpenPartition(offset, &GameIOS);
			/* Run Apploader */
			AppEntrypoint = Apploader_Run();
			gprintf("Entrypoint: %08x\n", AppEntrypoint);
			WDVD_Close();
			/* Setup Low Memory */
			Disc_SetLowMem(GameIOS);
			/* Set an appropriate video mode */
			vmode = Disc_SelectVMode(&vmode_reg);
			Disc_SetVMode((GXRModeObj*)vmode, vmode_reg);
			/* Set time */
			Disc_SetTime();
			/* Shutdown IOS subsystems */
			u32 level = IRQ_Disable();
			__IOS_ShutdownSubsystems();
			__exception_closeall();
			/* Originally from tueidj - taken from NeoGamma (thx) */
			*(vu32*)0xCC003024 = 1;
            //apply_settings();
			/* Boot */
			if(hooktype == 0)				
			{
				asm volatile (
					"lis %r3, AppEntrypoint@h\n"
					"ori %r3, %r3, AppEntrypoint@l\n"
					"lwz %r3, 0(%r3)\n"
					"mtlr %r3\n"
					"blr\n"
				);
			}
			else{
				asm volatile (
					"lis %r3, AppEntrypoint@h\n"
					"ori %r3, %r3, AppEntrypoint@l\n"
					"lwz %r3, 0(%r3)\n"
					"mtlr %r3\n"
					"lis %r3, 0x8000\n"
					"ori %r3, %r3, 0x18A8\n"
					"mtctr %r3\n"
					"bctr\n"
				);
			}
			/* Fail */
			IRQ_Restore(level);
		}
	}
	/* Fail, init chan launching */
	WII_Initialize();
	/* goto HBC */
  /*WII_LaunchTitle(HBC_LULZ);
	WII_LaunchTitle(HBC_108);
	WII_LaunchTitle(HBC_JODI);
	WII_LaunchTitle(HBC_HAXX); */
 /* Fail, goto System Menu    */
	WII_LaunchTitle(RETURNTIDTHIS);
	return 0;
}

