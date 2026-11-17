/*
 * MCA 最小 libretro 前端 —— PS Vita（probe/pxt32-3ds-vita 测试包）
 *
 * 链接 core 的 libretro_vita.a 静态库，职责只有测试需要的部分：
 *   - sceDisplaySetFrameBuf 直写 RGBA 帧缓冲（960x544，游戏 4x 缩放居中）
 *   - sceCtrl 8 键 -> RetroPad：CROSS=A、CIRCLE=B、十字键、
 *     START=游戏内菜单、SELECT=重置，L/R 肩键切换卡带
 *   - START+SELECT 进暂停态卡带列表（内嵌 8x8 字体直绘）：上下选、
 *     CROSS 装载、CIRCLE 返回
 *   - sceAudioOut 立体声 44100Hz 输出（有声音时兼作帧节奏）
 *   - 卡带目录 ux0:/data/mca（.pxt64 卡带放这里）
 *
 * 契约对齐 docs/INPUT.md：RetroPad A(id8)=MakeCode A、B(id0)=MakeCode B、
 * START(3)=菜单、SELECT(2)=重置（重置上升沿由 core 在 retro_run 内处理）。
 *
 * 渲染不走 GXM/vita2d（vitasdk snapshot 无该库）：sceDisplaySetFrameBuf
 * 是 vitasdk samples 认可的 userland 用法，适合测试包。
 */
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>

#include "font8x8.h"
#include "../../core/libretro.h"

#define CART_DIR "ux0:/data/mca"
#define SCALE    4
#define SCREEN_W 960
#define SCREEN_H 544
#define GAME_W   160
#define GAME_H   120
#define BLIT_X   ((SCREEN_W - GAME_W * SCALE) / 2)   /* 160 */
#define BLIT_Y   ((SCREEN_H - GAME_H * SCALE) / 2)   /* 32  */

#define MAX_CARTS 32

/* ------------------------------------------------------------------ */
/* 日志（vpk 由 VitaShell 启动时 stdout 进其日志）                       */
/* ------------------------------------------------------------------ */

static void log_printf(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *lc = (struct retro_log_callback *)data;
        lc->log = log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = CART_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    default:
        return false;
    }
}

/* 最新一帧（video_cb 只在回调期间有效，必须拷走） */
static uint16_t g_frame[GAME_W * GAME_H];
static int      g_frames_last_run = 0;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data || w != GAME_W || h != GAME_H)
        return;
    const uint16_t *src = (const uint16_t *)data;
    unsigned sp = (unsigned)(pitch / 2);
    for (unsigned y = 0; y < h; y++)
        memcpy(g_frame + y * GAME_W, src + y * sp, GAME_W * 2);
}

/* ------------------------------------------------------------------ */
/* 音频（sceAudioOut：端口定长 1024 帧块，攒够一块阻塞出队；            */
/* 有声音时承担帧节奏）                                                 */
/* ------------------------------------------------------------------ */

#define ABLOCK_FRAMES 1024
static int     g_audio_port = -1;
static int     g_audio_ok   = 0;
static int16_t g_astage[ABLOCK_FRAMES * 2];   /* L/R 交错 */
static size_t  g_afill = 0;                   /* stage 里已攒的帧数 */

static void audio_init(void) {
    g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                       ABLOCK_FRAMES, 44100,
                                       SCE_AUDIO_OUT_MODE_STEREO);
    if (g_audio_port < 0) {
        printf("[mca] sceAudioOutOpenPort 失败 0x%x，无声运行\n", g_audio_port);
        return;
    }
    g_audio_ok = 1;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    if (!g_audio_ok)
        return frames;
    size_t written = 0;
    while (written < frames) {
        size_t space = ABLOCK_FRAMES - g_afill;
        size_t n = frames - written;
        if (n > space)
            n = space;
        memcpy(g_astage + g_afill * 2, data + written * 2, n * 4);
        g_afill += n;
        written += n;
        if (g_afill == ABLOCK_FRAMES) {
            /* 阻塞直到驱动收下这块 */
            if (sceAudioOutOutput(g_audio_port, g_astage) < 0)
                return written;
            g_afill = 0;
            g_frames_last_run += ABLOCK_FRAMES;
        }
    }
    return frames;
}

/* ------------------------------------------------------------------ */
/* 输入                                                                */
/* ------------------------------------------------------------------ */

static SceCtrlData g_pad;
static unsigned    g_pad_mask = 0;   /* 当前帧 RetroPad 位掩码 */

static void pad_to_mask(uint32_t btn) {
    unsigned m = 0;
    if (btn & SCE_CTRL_CROSS)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
    if (btn & SCE_CTRL_CIRCLE) m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    if (btn & SCE_CTRL_UP)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (btn & SCE_CTRL_DOWN)   m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (btn & SCE_CTRL_LEFT)   m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (btn & SCE_CTRL_RIGHT)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (btn & SCE_CTRL_START)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    if (btn & SCE_CTRL_SELECT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
    g_pad_mask = m;
}

static void input_poll_cb(void) {
    memset(&g_pad, 0, sizeof(g_pad));
    sceCtrlPeekBufferPositive(0, &g_pad, 1);
    pad_to_mask(g_pad.buttons);
}

static int16_t input_state_cb(unsigned port, unsigned device,
                              unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)g_pad_mask;
    return (g_pad_mask >> id) & 1;
}

/* ------------------------------------------------------------------ */
/* 卡带管理                                                            */
/* ------------------------------------------------------------------ */

static char  g_cart_path[MAX_CARTS][256];
static int   g_ncarts  = 0;
static int   g_cartidx = -1;
static void *g_cart_data = NULL;
static long  g_cart_size = 0;

static void scan_carts(void) {
    g_ncarts = 0;
    DIR *d = opendir(CART_DIR);
    if (!d) {
        printf("[mca] 打不开 " CART_DIR "，请把 .pxt64 卡带放进去\n");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_ncarts < MAX_CARTS) {
        size_t len = strlen(e->d_name);
        if (len > 6 && strcmp(e->d_name + len - 6, ".pxt64") == 0) {
            snprintf(g_cart_path[g_ncarts], sizeof(g_cart_path[0]),
                     CART_DIR "/%s", e->d_name);
            g_ncarts++;
        }
    }
    closedir(d);
    for (int i = 0; i < g_ncarts; i++)
        for (int j = i + 1; j < g_ncarts; j++)
            if (strcmp(g_cart_path[i], g_cart_path[j]) > 0) {
                char tmp[256];
                memcpy(tmp, g_cart_path[i], sizeof(tmp));
                memcpy(g_cart_path[i], g_cart_path[j], sizeof(tmp));
                memcpy(g_cart_path[j], tmp, sizeof(tmp));
            }
}

static void free_cart(void) {
    if (g_cart_data) {
        free(g_cart_data);
        g_cart_data = NULL;
    }
    g_cart_size = 0;
}

static int load_cart(int idx) {
    if (idx < 0 || idx >= g_ncarts)
        return 0;
    FILE *f = fopen(g_cart_path[idx], "rb");
    if (!f) {
        printf("[mca] 打不开 %s\n", g_cart_path[idx]);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        printf("[mca] 读 %s 失败\n", g_cart_path[idx]);
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    if (g_cartidx >= 0)
        retro_unload_game();
    free_cart();

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = g_cart_path[idx];
    info.data = buf;
    info.size = (size_t)size;
    if (!retro_load_game(&info)) {
        printf("[mca] 装载失败: %s\n", g_cart_path[idx]);
        free(buf);
        return 0;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    g_cart_data = buf;
    g_cart_size = size;
    g_cartidx   = idx;
    printf("[mca] [%d/%d] %s (%ld B)\n", idx + 1, g_ncarts,
           g_cart_path[idx], size);
    return 1;
}

static void switch_cart(int delta) {
    if (g_ncarts == 0)
        return;
    int next = (g_cartidx + delta + g_ncarts) % g_ncarts;
    load_cart(next);
}

/* ------------------------------------------------------------------ */
/* 视频（sceDisplaySetFrameBuf 直写 RGBA）                              */
/*                                                                      */
/* 像素格式 SCE_DISPLAY_PIXELFORMAT_A8B8G8R8：小端内存序 R,G,B,A，        */
/* u32 = 0xFF000000 | (b<<16) | (g<<8) | r。                             */
/* ------------------------------------------------------------------ */

static uint32_t g_fb[SCREEN_W * SCREEN_H];   /* 2MB 静态帧缓冲 */

static uint32_t rgb565_to_abgr(uint16_t c) {
    unsigned r = ((c >> 11) & 0x1F) << 3;   /* -> 8bit 近似（<<3|<<2 折叠） */
    unsigned g = ((c >> 5) & 0x3F) << 2;
    unsigned b = (c & 0x1F) << 3;
    r |= r >> 5; g |= g >> 6; b |= b >> 5;  /* 高位扩展，颜色更准 */
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void blit_frame(void) {
    for (int y = 0; y < GAME_H; y++) {
        const uint16_t *src = g_frame + y * GAME_W;
        uint32_t *row0 = g_fb + (BLIT_Y + y * SCALE) * SCREEN_W + BLIT_X;
        for (int dy = 0; dy < SCALE; dy++) {
            uint32_t *dst = row0 + dy * SCREEN_W;
            for (int x = 0; x < GAME_W; x++) {
                uint32_t c = rgb565_to_abgr(src[x]);
                dst[x * SCALE + 0] = c;
                dst[x * SCALE + 1] = c;
                dst[x * SCALE + 2] = c;
                dst[x * SCALE + 3] = c;
            }
        }
    }
}

static void clear_screen(uint32_t c) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        g_fb[i] = c;
}

/* ------------------------------------------------------------------ */
/* 卡带列表菜单（暂停态，font8x8 直绘到 g_fb）                           */
/*                                                                      */
/* START+SELECT 同按进出；菜单态不跑 retro_run，VM 状态原地保留，         */
/* CROSS 装载选中卡带（当前卡带则直接返回），CIRCLE 返回游戏。            */
/* ------------------------------------------------------------------ */

#define MENU_VIEW 22

static bool g_menu_open = false;
static int  g_menu_cur  = 0;
static int  g_menu_top  = 0;

/* 去掉目录前缀与 .pxt64 后缀的显示名 */
static void cart_disp(int idx, char *buf, size_t n) {
    snprintf(buf, n, "%s", g_cart_path[idx] + strlen(CART_DIR) + 1);
    size_t l = strlen(buf);
    if (l > 6 && strcmp(buf + l - 6, ".pxt64") == 0)
        buf[l - 6] = '\0';
}

/* 8x8 点阵文本：font8x8 每字节一行、bit0=最左列 */
static void draw_text(int x, int y, const char *s, uint32_t fg) {
    for (; *s; s++, x += 8) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E)
            ch = '?';
        const unsigned char *g = font8x8[ch - 0x20];
        for (int r = 0; r < 8; r++) {
            uint32_t *row = g_fb + (y + r) * SCREEN_W + x;
            for (int b = 0; b < 8; b++)
                if (g[r] & (1u << b))
                    row[b] = fg;
        }
    }
}

static void menu_clamp_top(void) {
    if (g_menu_top > g_menu_cur - 3)
        g_menu_top = g_menu_cur - 3;
    if (g_menu_top > g_ncarts - MENU_VIEW)
        g_menu_top = g_ncarts - MENU_VIEW;
    if (g_menu_top < 0)
        g_menu_top = 0;
}

static void menu_draw(void) {
    clear_screen(0xFF201414);   /* 深底（0xAABBGGRR） */
    char title[64];
    snprintf(title, sizeof(title), "MCA carts  %d/%d",
             g_menu_cur + 1, g_ncarts);
    draw_text(16, 16, title, 0xFFFFFFFF);
    for (int i = 0; i < MENU_VIEW; i++) {
        int idx = g_menu_top + i;
        if (idx >= g_ncarts)
            break;
        int y = 40 + i * 20;
        if (idx == g_menu_cur) {
            for (int r = 0; r < 16; r++) {
                uint32_t *row = g_fb + (y - 2 + r) * SCREEN_W + 8;
                for (int x = 0; x < SCREEN_W - 16; x++)
                    row[x] = 0xFF603018;   /* 高亮条 */
            }
        }
        char nm[64], line[80];
        cart_disp(idx, nm, sizeof(nm));
        snprintf(line, sizeof(line), "%c%c %s",
                 idx == g_menu_cur ? '>' : ' ',
                 idx == g_cartidx ? '*' : ' ',
                 nm);
        draw_text(16, y, line,
                  idx == g_menu_cur ? 0xFFFFFFFF : 0xFFB0B0B0);
    }
    draw_text(16, 40 + MENU_VIEW * 20 + 16,
              "UP/DOWN select   CROSS load   CIRCLE back",
              0xFF808080);
}

static void menu_enter(void) {
    if (g_ncarts == 0)
        return;
    g_menu_open = true;
    g_menu_cur  = (g_cartidx >= 0) ? g_cartidx : 0;
    g_menu_top  = 0;
    menu_clamp_top();
    menu_draw();
}

static void menu_leave(void) {
    g_menu_open = false;
    blit_frame();   /* 菜单盖掉了游戏画面，恢复 */
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    audio_init();

    printf("MCA core 测试包 (probe/pxt32-3ds-vita)\n");
    printf("=====================================\n");

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    scan_carts();
    printf("找到 %d 张卡带\n", g_ncarts);
    if (g_ncarts > 0)
        load_cart(0);
    else
        clear_screen(0xFF4040FF);   /* 红：没找到卡带 */
    printf("操作: CROSS=A CIRCLE=B 十字=方向\n");
    printf("      START=游戏内菜单 SELECT=重置\n");
    printf("      START+SELECT=卡带列表 L/R=换卡带\n");

    /* 帧缓冲挂上屏幕 */
    SceDisplayFrameBuf dfb;
    memset(&dfb, 0, sizeof(dfb));
    dfb.size        = sizeof(dfb);
    dfb.base        = g_fb;
    dfb.pitch       = SCREEN_W;
    dfb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    dfb.width       = SCREEN_W;
    dfb.height      = SCREEN_H;
    sceDisplaySetFrameBuf(&dfb, SCE_DISPLAY_SETBUF_NEXTFRAME);

    uint32_t prev_btn = 0;
    for (;;) {
        input_poll_cb();
        uint32_t down = g_pad.buttons & ~prev_btn;
        prev_btn = g_pad.buttons;

        /* START+SELECT 同按（至少一键是新按下）= 菜单开关 */
        uint32_t both = SCE_CTRL_START | SCE_CTRL_SELECT;
        bool combo = (g_pad.buttons & both) == both
                     && (down & both) != 0;

        if (g_menu_open) {
            if (combo || (down & SCE_CTRL_CIRCLE)) {
                menu_leave();
            } else if ((down & SCE_CTRL_DOWN) && g_menu_cur < g_ncarts - 1) {
                g_menu_cur++;
                menu_clamp_top();
                menu_draw();
            } else if ((down & SCE_CTRL_UP) && g_menu_cur > 0) {
                g_menu_cur--;
                menu_clamp_top();
                menu_draw();
            } else if (down & SCE_CTRL_CROSS) {
                int pick = g_menu_cur;
                menu_leave();
                if (pick != g_cartidx)
                    load_cart(pick);
            }
            sceDisplayWaitVblankStart();
            continue;
        }

        if (combo) {
            menu_enter();
            sceDisplayWaitVblankStart();
            continue;
        }

        if (down & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
            switch_cart((down & SCE_CTRL_LTRIGGER) ? -1 : 1);

        g_frames_last_run = 0;
        retro_run();
        blit_frame();
        /* 有音频输出时由阻塞的 sceAudioOutOutput 控节奏；无音频则 VBlank */
        if (g_frames_last_run == 0)
            sceDisplayWaitVblankStart();
    }
    return 0;   /* 不会到这：Vita 经 HOME 退出，系统回收 */
}
