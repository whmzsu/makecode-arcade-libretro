// stub_core.c — 诊断用空核心（对照组的对照组）
//
// 目的：把"RetroArch + 一个静态核心"这条路本身，和"我们的核心"这件事分开。
//
//   ra_plain.nro   = RetroArch 本体，不带核心            （已知：能启动）
//   ra_stubcore.nro = RetroArch + 这个空核心              （本文件）
//   mca_boot.nro   = RetroArch + 我们的核心               （报障的那个）
//
// 判定：
//   ra_stubcore 也启动不了 -> 静态核心链接/注册机制本身有问题，与我们的代码无关
//   ra_stubcore 能启动     -> 机制没问题，问题在我们核心的目标代码
//
// 所以这个文件**只实现 libretro 必需的入口**，一行业务逻辑都没有：
// 不建线程、不分配大内存、不读文件、不碰 pthread。它必须简单到不可能崩，
// 否则会用自己的问题掩盖要查的问题。

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libretro.h"

static retro_environment_t env_cb;

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "MakeCode Arcade (stub)";
    info->library_version  = "diag";
    info->valid_extensions = "pxt64";
    info->need_fullpath    = false;
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 320;
    info->geometry.base_height  = 240;
    info->geometry.max_width    = 320;
    info->geometry.max_height   = 240;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 44100.0;
}

void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    /* 不注册任何核心选项、不发任何环境请求：本核心什么都不需要 */
    (void)env_cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)        { (void)cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)          { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb)              { (void)cb; }
void retro_set_input_state(retro_input_state_t cb)            { (void)cb; }

void retro_init(void)   { }
void retro_deinit(void) { }
void retro_reset(void)  { }

/* 每帧什么都不做，画面保持黑屏。前端只要拿到"返回了"就说明核心循环是活的 */
void retro_run(void) { }

bool retro_load_game(const struct retro_game_info *game)
{
    (void)game;
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

void retro_unload_game(void) { }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}
