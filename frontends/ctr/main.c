/*
 * MCA 最小 libretro 前端 —— 3DS（probe/pxt32-3ds-vita 测试包）
 *
 * 链接 core 的 libretro_ctr.a 静态库，职责只有测试需要的部分：
 *   - 顶部屏 2x 缩放（160x120 -> 320x240 居中），RGB565 直写 framebuffer
 *   - hid 8 键 -> RetroPad：A/B/十字键、START=游戏内菜单、SELECT=重置
 *   - L/R 肩键切换 sdmc:/mca 目录下的 .pxt64 卡带，Y 退回 Homebrew Launcher
 *   - ndsp 立体声 44100Hz 输出
 *   - 底屏 console 显示状态
 *   - START+SELECT 进暂停态卡带列表（底屏渲染）：上下选、A 装载、B 返回
 *
 * 契约对齐 docs/INPUT.md：RetroPad A(id8)=MakeCode A、B(id0)=MakeCode B、
 * START(3)=菜单、SELECT(2)=重置（重置上升沿由 core 在 retro_run 内处理）。
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>

#include "../../core/libretro.h"

#define CART_DIR "sdmc:/mca"
#define SCALE    2
/* 顶部屏 400x240（横向），游戏画面 320x240 居中 */
#define TOP_W    400
#define TOP_H    240
#define GAME_W   160
#define GAME_H   120
#define BLIT_X   ((TOP_W - GAME_W * SCALE) / 2)   /* 40 */
#define BLIT_Y   ((TOP_H - GAME_H * SCALE) / 2)   /* 0  */

#define MAX_CARTS 32

/* ------------------------------------------------------------------ */
/* libretro 前端回调                                                    */
/* ------------------------------------------------------------------ */

static void log_printf(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
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
    /* core 发出的 SET_* 一律接受：像素格式拿到 RGB565，描述符/控制器
     * 信息/空选项表照单全收 */
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

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data || w != GAME_W || h != GAME_H)
        return;
    const uint16_t *src = (const uint16_t *)data;
    unsigned sp = (unsigned)(pitch / 2);
    for (unsigned y = 0; y < h; y++)
        memcpy(g_frame + y * GAME_W, src + y * sp, GAME_W * 2);
}

/* ------------------------------------------------------------------ */
/* 音频（ndsp 双缓冲环）                                                */
/* ------------------------------------------------------------------ */

#define ABUF_FRAMES 2940   /* 50ms/缓冲 @44100 */
#define ABUF_COUNT  4

static ndspWaveBuf g_wb[ABUF_COUNT];
static int16_t    *g_abuf[ABUF_COUNT];
static int         g_aidx = 0;
static bool        g_audio_ok = false;

static void audio_init(void) {
    if (R_FAILED(ndspInit())) {
        printf("[mca] ndsp init failed, running without audio\n");
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    for (int i = 0; i < ABUF_COUNT; i++) {
        g_abuf[i] = (int16_t *)linearAlloc((size_t)ABUF_FRAMES * 4);
        memset(&g_wb[i], 0, sizeof(g_wb[i]));
        g_wb[i].data_vaddr = g_abuf[i];
        g_wb[i].status     = NDSP_WBUF_DONE;
    }
    g_audio_ok = true;
}

static void audio_exit(void) {
    if (!g_audio_ok)
        return;
    ndspExit();
    for (int i = 0; i < ABUF_COUNT; i++)
        if (g_abuf[i])
            linearFree(g_abuf[i]);
    g_audio_ok = false;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    if (!g_audio_ok)
        return frames;   /* 丢弃但假装全收，别让 VM 侧等待 */
    size_t written = 0;
    while (written < frames) {
        ndspWaveBuf *wb = &g_wb[g_aidx];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) {
            /* 环满：等 1ms，还满就丢剩余（测试包可接受轻微断音） */
            svcSleepThread(1000000ULL);
            if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
                break;
        }
        size_t n = frames - written;
        if (n > (size_t)ABUF_FRAMES)
            n = (size_t)ABUF_FRAMES;
        memcpy(g_abuf[g_aidx], data + written * 2, n * 4);
        /* nsamples 单位：PCM16 = halfword；立体声一帧 = 2 个 halfword */
        wb->nsamples = (u32)(n * 2);
        ndspChnWaveBufAdd(0, wb);
        g_aidx = (g_aidx + 1) % ABUF_COUNT;
        written += n;
    }
    return written;
}

/* ------------------------------------------------------------------ */
/* 输入                                                                */
/* ------------------------------------------------------------------ */

static void input_poll_cb(void) {
    hidScanInput();
}

static int16_t input_state_cb(unsigned port, unsigned device,
                              unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    u32 held = hidKeysHeld();
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        int16_t m = 0;
        if (held & KEY_A)      m |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
        if (held & KEY_B)      m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
        if (held & KEY_DUP)    m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (held & KEY_DDOWN)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        if (held & KEY_DLEFT)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
        if (held & KEY_DRIGHT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (held & KEY_START)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (held & KEY_SELECT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
        return m;
    }
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:      return (held & KEY_A) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return (held & KEY_B) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:     return (held & KEY_DUP) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (held & KEY_DDOWN) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (held & KEY_DLEFT) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (held & KEY_DRIGHT) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return (held & KEY_START) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (held & KEY_SELECT) ? 1 : 0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* 卡带管理                                                            */
/* ------------------------------------------------------------------ */

static char  g_cart_path[MAX_CARTS][256];
static int   g_ncarts  = 0;
static int   g_cartidx = -1;    /* 当前卡带下标，-1 = 未装载 */
static void *g_cart_data = NULL;
static long  g_cart_size = 0;

static void scan_carts(void) {
    g_ncarts = 0;
    DIR *d = opendir(CART_DIR);
    if (!d) {
        printf("[mca] cannot open " CART_DIR ", put .pxt64 files there\n");
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
    /* 文件名字典序排一下，切卡顺序稳定 */
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

static bool load_cart(int idx) {
    if (idx < 0 || idx >= g_ncarts)
        return false;
    FILE *f = fopen(g_cart_path[idx], "rb");
    if (!f) {
        printf("[mca] cannot open %s\n", g_cart_path[idx]);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        printf("[mca] read %s failed\n", g_cart_path[idx]);
        free(buf);
        fclose(f);
        return false;
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
        printf("[mca] load failed: %s\n", g_cart_path[idx]);
        free(buf);
        return false;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    g_cart_data = buf;
    g_cart_size = size;
    g_cartidx   = idx;
    printf("[mca] [%d/%d] %s (%ld B)\n", idx + 1, g_ncarts,
           g_cart_path[idx], size);
    return true;
}

static void switch_cart(int delta) {
    if (g_ncarts == 0)
        return;
    int next = (g_cartidx + delta + g_ncarts) % g_ncarts;
    load_cart(next);
}

/* ------------------------------------------------------------------ */
/* 卡带列表菜单（暂停态，底屏 console 渲染）                              */
/*                                                                      */
/* START+SELECT 同按进出；菜单态不跑 retro_run，VM 状态原地保留，          */
/* A 装载选中卡带（当前卡带则直接返回），B 返回游戏。                     */
/* ------------------------------------------------------------------ */

#define MENU_VIEW 22

static bool g_menu_open = false;
static int  g_menu_cur  = 0;
static int  g_menu_top  = 0;   /* 滚动窗口首行下标 */

/* 去掉目录前缀与 .pxt64 后缀的显示名 */
static void cart_disp(int idx, char *buf, size_t n) {
    snprintf(buf, n, "%s", g_cart_path[idx] + strlen(CART_DIR) + 1);
    size_t l = strlen(buf);
    if (l > 6 && strcmp(buf + l - 6, ".pxt64") == 0)
        buf[l - 6] = '\0';
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
    consoleClear();
    printf("== MCA carts %d/%d ==\n\n", g_menu_cur + 1, g_ncarts);
    for (int i = 0; i < MENU_VIEW; i++) {
        int idx = g_menu_top + i;
        if (idx >= g_ncarts)
            break;
        char nm[40];
        cart_disp(idx, nm, sizeof(nm));
        printf("%c%c %.35s\n",
               idx == g_menu_cur ? '>' : ' ',
               idx == g_cartidx ? '*' : ' ',
               nm);
    }
    printf("\nUP/DOWN=select  A=load  B=back\n");
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

static void print_status(void) {
    if (g_ncarts == 0) {
        printf("[mca] no .pxt64 in " CART_DIR "\n");
    } else if (g_cartidx >= 0) {
        char nm[64];
        cart_disp(g_cartidx, nm, sizeof(nm));
        printf("[mca] current %d/%d: %s\n", g_cartidx + 1, g_ncarts, nm);
    }
    printf("A/B=game keys  D-pad=move\n");
    printf("  START=ingame menu  SELECT=reset\n");
    printf("  START+SELECT=cart list  L/R=switch cart\n");
    printf("  Y=exit\n");
}

static void menu_leave(void) {
    g_menu_open = false;
    consoleClear();
    print_status();
}

/* ------------------------------------------------------------------ */
/* 视频（顶部屏直写 framebuffer）                                       */
/*                                                                      */
/* 3DS framebuffer 旋转存放：顶部屏 RGB565 缓冲 240x400，屏幕坐标        */
/* (x,y) 对应 fb[(239 - y) + x * 240]（devkitPro 示例通用公式）。        */
/* 若真机画面旋转/镜像，调这组宏即可。                                   */
/* ------------------------------------------------------------------ */

static void blit_frame(void) {
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int y = 0; y < GAME_H; y++) {
        const uint16_t *src = g_frame + y * GAME_W;
        for (int x = 0; x < GAME_W; x++) {
            u16 c = src[x];
            int sx = BLIT_X + x * SCALE;
            int sy = BLIT_Y + y * SCALE;
            for (int dy = 0; dy < SCALE; dy++)
                for (int dx = 0; dx < SCALE; dx++)
                    fb[(239 - (sy + dy)) + (sx + dx) * 240] = c;
        }
    }
}

static void clear_top(u16 color) __attribute__((unused));
static void clear_top(u16 color) {
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int i = 0; i < 240 * 400; i++)
        fb[i] = color;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    gfxInitDefault();
    gfxSet3D(false);
    /* RGB565 顶部屏 + 单缓冲（每帧整屏重绘，VSync 翻转无撕裂意义） */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    /* 底屏也必须关双缓冲：consoleInit 固定捕获一块 framebuffer 指针，
     * 双缓冲下每帧 swap 在两块缓冲间交替，console 永远只写其中一块，
     * 屏幕上就是两帧内容叠加的鬼影/乱码（3DS homebrew 经典坑） */
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    consoleInit(GFX_BOTTOM, NULL);
    /* 底屏文案一律 ASCII：3DS 系统共享字体是日文子集，简体字形缺失
     * 会渲染成豆腐块，真机上看起来就是另一种"乱码" */
    printf("MCA core test pack (pxt64)\n");
    printf("===========================\n");

    audio_init();

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(NULL);          /* 用 batch 接口 */
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    scan_carts();
    if (g_ncarts > 0)
        load_cart(0);
    print_status();

    u32 prev_held = 0;
    while (aptMainLoop()) {
    /* 菜单态不跑 retro_run（core 不会再调 input_poll_cb），必须每圈自己
     * 扫输入，否则 hidKeysHeld 永远是进菜单那一刻的缓存值，按键全失效 */
    hidScanInput();
    u32 held = hidKeysHeld();
    u32 down = held & ~prev_held;
    prev_held = held;

    if (down & KEY_Y)
        break;

    /* START+SELECT 同按（至少一键是新按下）= 菜单开关 */
    bool combo = (held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)
                 && (down & (KEY_START | KEY_SELECT)) != 0;

    if (g_menu_open) {
        if (combo || (down & KEY_B)) {
            menu_leave();
        } else if ((down & KEY_DOWN) && g_menu_cur < g_ncarts - 1) {
            g_menu_cur++;
            menu_clamp_top();
            menu_draw();
        } else if ((down & KEY_UP) && g_menu_cur > 0) {
            g_menu_cur--;
            menu_clamp_top();
            menu_draw();
        } else if (down & KEY_A) {
            int pick = g_menu_cur;
            menu_leave();
            if (pick != g_cartidx)
                load_cart(pick);
        }
        gspWaitForVBlank();
        continue;
    }

    if (combo) {
        menu_enter();
        gspWaitForVBlank();
        continue;
    }

    if (down & (KEY_L | KEY_R))
        switch_cart((down & KEY_L) ? -1 : 1);

    retro_run();
    blit_frame();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    }

    retro_unload_game();
    retro_deinit();
    free_cart();
    audio_exit();
    gfxExit();
    return 0;
}
