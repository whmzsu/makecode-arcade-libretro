// png_loader.cpp — .png 卡带解码：vendor 的 cart.c 薄封装
//
// 底层实现（同目录 cart.c / decompress.c，vendored 自 whmzsu/mcarcade-core，
// MIT）：PNG(miniz tinfl) → 低位隐写位流（magic 0x59347a7d）→ LZMA-alone →
// 工程 source 文件表。自写实现已在回环自测中验证格式认知一致，正式路径
// 统一走这份生产代码。
#include "png_loader.h"

#include "cart.h"

namespace mca {

bool arcade_png_load_project(const uint8_t* data, size_t size, CartProject& out) {
    cart_t cart;
    if (cart_load_png(data, size, &cart) != 0)
        return false;
    out.source.assign(cart.source, cart.source_len);
    out.is_source_json = cart.source_is_json != 0;
    cart_free(&cart);
    return true;
}

} // namespace mca
