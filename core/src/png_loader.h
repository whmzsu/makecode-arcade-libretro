// png_loader.h — .png 卡带（MakeCode Arcade 项目文件）解码
//
// M1 起：格式规格见 docs/PNG-FORMAT.md；实现直接 vendor 自
// whmzsu/mcarcade-core|mcarcade-libretro 的 cart.c / decompress.c
// （已在两个生产 core 上验证），本模块只做 C++ 侧薄封装。
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace mca {

// PNG 文件字节 → 工程源码表。
// 成功时 project_source_out 为 JSON 文件表（{"main.ts":"...", "...":"..."}），
// 少数老卡带为裸源码（用 is_source_json 区分）。
struct CartProject {
    std::string source;
    bool is_source_json = false;
};

bool arcade_png_load_project(const uint8_t* data, size_t size, CartProject& out);

} // namespace mca
