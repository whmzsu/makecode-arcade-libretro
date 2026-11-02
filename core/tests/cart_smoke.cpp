// cart_smoke.cpp — 用真实卡带验证 cart.c 解码管线（与 tools/extract_png_meta.py 对数）
// 用法: cart_smoke 游戏.png [更多.png ...]
#include "png_loader.h"

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s cart.png [...]\n", argv[0]); return 2; }
    int fails = 0;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { printf("%s: open failed\n", argv[i]); fails++; continue; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf((size_t)size);
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); fails++; continue; }
        fclose(f);

        mca::CartProject proj;
        if (mca::arcade_png_load_project(buf.data(), buf.size(), proj)) {
            printf("%s: OK  source=%zu bytes (%s)\n", argv[i], proj.source.size(),
                   proj.is_source_json ? "file-table" : "bare");
        } else {
            printf("%s: DECODE FAILED\n", argv[i]);
            fails++;
        }
    }
    return fails;
}
