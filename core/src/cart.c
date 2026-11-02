/*
 * cart.c —— MakeCode Arcade 卡带解析
 *
 * 三步：
 *   1. PNG 解码（IHDR / IDAT → zlib 解压 → 反滤波 → RGBA）
 *   2. 低位隐写还原（pxt encodeBlobAsync 的逆过程，magic 0x59347a7d）
 *   3. blob 若是 LZMA 则解压，再从工程 JSON 里取出 main.ts
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "cart.h"
#include "decompress.h"
#include "miniz.h"

#define STEGO_MAGIC 0x59347a7du
#define STEGO_HDR   36

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 把一段裸源码包成 {"main.ts":"..."} —— 让 JS 侧只有一条加载路径 */
static char *wrap_single_file(const char *name, const char *src, size_t len, size_t *out_len)
{
    size_t cap = len * 2 + strlen(name) + 32;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "{\"%s\":\"", name);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (o + 8 >= cap) {
            cap *= 2;
            char *n = (char *)realloc(out, cap);
            if (!n) { free(out); return NULL; }
            out = n;
        }
        switch (c) {
        case '"': out[o++] = '\\'; out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        default:
            if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
            else out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o++] = '}';
    out[o] = 0;
    *out_len = o;
    return out;
}

static void set_err(char *err, const char *fmt, ...)
{
    va_list ap;
    if (err[0] == 0) {
        va_start(ap, fmt);
        vsnprintf(err, 256, fmt, ap);
        va_end(ap);
    }
}

/* ---------------- PNG ---------------- */

typedef struct {
    int w, h;
    uint8_t *rgba;
} png_img_t;

static int png_decode(const uint8_t *d, size_t len, png_img_t *img, char *err)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (len < 8 || memcmp(d, sig, 8) != 0) {
        set_err(err, "not a PNG file");
        return -1;
    }

    size_t pos = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    int got_ihdr = 0;

    while (pos + 8 <= len) {
        uint32_t clen = rd32be(d + pos);
        const char *type = (const char *)(d + pos + 4);
        const uint8_t *body = d + pos + 8;
        if (pos + 12 + clen > len) break;

        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            w = (int)rd32be(body);
            h = (int)rd32be(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
            got_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = (idat_cap ? idat_cap * 2 : 65536) + clen;
                uint8_t *n = (uint8_t *)realloc(idat, ncap);
                if (!n) { free(idat); set_err(err, "out of memory (IDAT)"); return -1; }
                idat = n;
                idat_cap = ncap;
            }
            memcpy(idat + idat_len, body, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }

    if (!got_ihdr || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        free(idat);
        set_err(err, "bad IHDR");
        return -1;
    }
    if (depth != 8 || interlace != 0 || (ctype != 2 && ctype != 6)) {
        free(idat);
        set_err(err, "unsupported PNG (need 8-bit RGB/RGBA, non-interlaced)");
        return -1;
    }
    if (!idat_len) {
        free(idat);
        set_err(err, "no IDAT");
        return -1;
    }

    int ch = (ctype == 6) ? 4 : 3;
    size_t stride = (size_t)w * ch;
    size_t raw_size = (stride + 1) * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!raw || !rgba) {
        free(idat); free(raw); free(rgba);
        set_err(err, "out of memory (PNG)");
        return -1;
    }

    size_t got = tinfl_decompress_mem_to_mem(raw, raw_size, idat, idat_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    free(idat);
    if (got == (size_t)-1 || got < raw_size) {
        free(raw); free(rgba);
        set_err(err, "zlib inflate failed");
        return -1;
    }

    /* 反滤波 */
    uint8_t *prev = (uint8_t *)calloc(stride, 1);
    if (!prev) { free(raw); free(rgba); set_err(err, "oom"); return -1; }
    for (int y = 0; y < h; y++) {
        const uint8_t *line = raw + (stride + 1) * (size_t)y;
        uint8_t filter = line[0];
        const uint8_t *src = line + 1;
        uint8_t *cur = (uint8_t *)malloc(stride);
        if (!cur) { free(prev); free(raw); free(rgba); set_err(err, "oom"); return -1; }
        memcpy(cur, src, stride);
        for (size_t i = 0; i < stride; i++) {
            int a = (i >= (size_t)ch) ? cur[i - ch] : 0;
            int b = prev[i];
            int c = (i >= (size_t)ch) ? prev[i - ch] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: {
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                cur[i] = (uint8_t)(cur[i] + pred);
                break;
            }
            default:
                free(cur); free(prev); free(raw); free(rgba);
                set_err(err, "bad PNG filter");
                return -1;
            }
        }
        uint8_t *dst = rgba + (size_t)y * w * 4;
        if (ch == 4) {
            memcpy(dst, cur, stride);
        } else {
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = cur[x * 3 + 0];
                dst[x * 4 + 1] = cur[x * 3 + 1];
                dst[x * 4 + 2] = cur[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        memcpy(prev, cur, stride);
        free(cur);
    }
    free(prev);
    free(raw);

    img->w = w;
    img->h = h;
    img->rgba = rgba;
    return 0;
}

/* ---------------- 隐写位流 ---------------- */

typedef struct {
    const uint8_t *d;
    size_t ptr;
    size_t bytes;   /* 可用字节数（w*h*4） */
    int bpp;
} stego_t;

static int stego_read(stego_t *s, uint8_t *trg, size_t n)
{
    int shift = 0;
    size_t i = 0;
    unsigned acc = 0;
    unsigned mask = (1u << s->bpp) - 1u;
    while (i < n) {
        if (s->ptr >= s->bytes)
            return -1;
        acc |= (unsigned)(s->d[s->ptr++] & mask) << shift;
        if ((s->ptr & 3) == 3)
            s->ptr++;
        shift += s->bpp;
        if (shift >= 8) {
            trg[i++] = (uint8_t)(acc & 0xff);
            acc >>= 8;
            shift -= 8;
        }
    }
    return 0;
}

/* ---------------- 极简 JSON 字符串取值 ---------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 在 json 里找 "key" : "..."，反转义后返回（malloc，NUL 结尾）。找不到返回 NULL。 */
static char *json_get_string(const char *json, const char *key, size_t *out_len)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);

    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') { p++; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != '"') { p++; continue; }  /* 不是字符串（比如对象），跳过 */

        q++;
        size_t cap = strlen(q) + 1;
        char *out = (char *)malloc(cap);
        if (!out) return NULL;
        size_t o = 0;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1]) {
                q++;
                switch (*q) {
                case 'n': out[o++] = '\n'; q++; break;
                case 't': out[o++] = '\t'; q++; break;
                case 'r': out[o++] = '\r'; q++; break;
                case 'b': out[o++] = '\b'; q++; break;
                case 'f': out[o++] = '\f'; q++; break;
                case '/': out[o++] = '/'; q++; break;
                case '"': out[o++] = '"'; q++; break;
                case '\\': out[o++] = '\\'; q++; break;
                case 'u': {
                    int h0 = hexval(q[1]), h1 = hexval(q[2]), h2 = hexval(q[3]), h3 = hexval(q[4]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { q++; break; }
                    unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    q += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF && q[0] == '\\' && q[1] == 'u') {
                        int g0 = hexval(q[2]), g1 = hexval(q[3]), g2 = hexval(q[4]), g3 = hexval(q[5]);
                        if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                            unsigned lo = (unsigned)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                q += 6;
                            }
                        }
                    }
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[o++] = (char)(0xF0 | (cp >> 18));
                        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out[o++] = *q; q++; break;
                }
            } else {
                out[o++] = *q++;
            }
        }
        out[o] = 0;
        if (out_len) *out_len = o;
        return out;
    }
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

int cart_load_png(const unsigned char *buf, size_t len, cart_t *out)
{
    memset(out, 0, sizeof(*out));

    png_img_t img;
    img.w = img.h = 0;
    img.rgba = NULL;
    if (png_decode(buf, len, &img, out->error) != 0)
        return -1;

    size_t bytes = (size_t)img.w * img.h * 4;
    int bpp = (img.rgba[0] & 1) | ((img.rgba[1] & 1) << 1) | ((img.rgba[2] & 1) << 2);
    if (bpp == 0 || bpp > 4) {
        free(img.rgba);
        set_err(out->error, "invalid cartridge (bpp=%d)", bpp);
        return -1;
    }

    stego_t s = { img.rgba, 4, bytes, bpp };
    uint8_t hd[STEGO_HDR];
    if (stego_read(&s, hd, STEGO_HDR) != 0) {
        free(img.rgba);
        set_err(out->error, "cartridge truncated");
        return -1;
    }
    uint32_t magic = rd32le(hd);
    uint32_t blob_len = rd32le(hd + 4);
    uint32_t added = rd32le(hd + 8);
    if (magic != STEGO_MAGIC) {
        free(img.rgba);
        set_err(out->error, "not a MakeCode Arcade cartridge (magic 0x%08x)", magic);
        return -1;
    }
    if (blob_len == 0 || blob_len > (64u << 20)) {
        free(img.rgba);
        set_err(out->error, "bad blob length");
        return -1;
    }

    uint8_t *blob = (uint8_t *)malloc(blob_len);
    if (!blob) {
        free(img.rgba);
        set_err(out->error, "oom (blob)");
        return -1;
    }

    if (added > 0) {
        size_t orig_size = (size_t)(img.h - (int)added) * img.w;
        size_t img_cap = ((orig_size - 1) * 3 * (size_t)bpp) >> 3;
        if (img_cap <= STEGO_HDR) {
            free(blob); free(img.rgba);
            set_err(out->error, "bad capacity");
            return -1;
        }
        size_t tmp_len = img_cap - STEGO_HDR;
        if (tmp_len > blob_len) tmp_len = blob_len;
        if (stego_read(&s, blob, tmp_len) != 0) {
            free(blob); free(img.rgba);
            set_err(out->error, "cartridge truncated (body)");
            return -1;
        }
        size_t rest = blob_len - tmp_len;
        if (rest) {
            stego_t s2 = { img.rgba, orig_size * 4, bytes, 8 };
            if (stego_read(&s2, blob + tmp_len, rest) != 0) {
                free(blob); free(img.rgba);
                set_err(out->error, "cartridge truncated (tail)");
                return -1;
            }
        }
    } else {
        if (stego_read(&s, blob, blob_len) != 0) {
            free(blob); free(img.rgba);
            set_err(out->error, "cartridge truncated (body)");
            return -1;
        }
    }
    free(img.rgba);

    /* blob 若是 LZMA 则解压成文本 */
    char *text = NULL;
    if (blob[0] == '{') {
        text = (char *)malloc(blob_len + 1);
        if (!text) { free(blob); set_err(out->error, "oom (text)"); return -1; }
        memcpy(text, blob, blob_len);
        text[blob_len] = 0;
        free(blob);
    } else {
        size_t txt_len = 0;
        uint8_t *dec = NULL;
        if (arc_lzma_alone(blob, blob_len, &dec, &txt_len, out->error, sizeof(out->error)) != 0) {
            free(blob);
            return -1;
        }
        free(blob);
        text = (char *)dec;
    }

    /*
     * 工程 JSON -> source。
     * source 通常是一个"文件表" JSON 字符串（{"main.ts":..., "tilemap.g.jres":...}），
     * 原样转交给 JS 侧：由它决定哪些文件参与编译、哪些是资源。
     */
    size_t inner_len = 0;
    char *inner = json_get_string(text, "source", &inner_len);
    char *src = NULL;
    if (inner) {
        if (inner[0] == '{') {
            /* 已经是文件表对象，直接用 */
            src = inner;
            out->source_len = inner_len;
            out->source_is_json = 1;
        } else {
            /* 裸源码：包成只有 main.ts 的文件表，走同一条路径 */
            char *wrapped = wrap_single_file("main.ts", inner, inner_len, &inner_len);
            free(inner);
            if (!wrapped) { free(text); set_err(out->error, "oom (wrap)"); return -1; }
            src = wrapped;
            out->source_len = inner_len;
            out->source_is_json = 1;
        }
    }
    if (!src) {
        size_t n = 0;
        char *legacy = json_get_string(text, "main.ts", &n);
        if (legacy) {
            size_t wlen = 0;
            char *wrapped = wrap_single_file("main.ts", legacy, n, &wlen);
            free(legacy);
            src = wrapped;
            out->source_len = wlen;
            out->source_is_json = wrapped ? 1 : 0;
        }
    }
    free(text);

    if (!src) {
        set_err(out->error, "no source/main.ts in project");
        return -1;
    }
    out->source = src;
    return 0;
}

void cart_free(cart_t *c)
{
    if (!c) return;
    free(c->source);
    c->source = NULL;
    c->source_len = 0;
    c->source_is_json = 0;
}
