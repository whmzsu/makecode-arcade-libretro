/*
 * cache.c —— 编译缓存（png 卡带 → binary.js 的产物落盘）
 *
 * 为什么值得缓存：一张卡带每次开游戏都要重跑 pxtc（桌面 4~5 秒，掌机更久），
 * 但同一份工程编译出来的 binary.js 是逐字节确定的（实测与 PC 端 node 产物 md5 相同）。
 * 所以把产物按「源码内容 + 编译器指纹」存进存档目录，下次直接读盘。
 *
 * 文件格式（32 字节头 + 载荷）：
 *   0   'M','C','C','1'   魔数 + 格式版本
 *   4   u32 payload_len   载荷字节数
 *   8   u32 payload_crc32 校验截断/写坏
 *   12  char build_id[20] 内嵌编译器指纹：pxt 载荷或胶水一变就自动失效
 *   32  payload           binary.js 原文
 *
 * 写入走「<键>.tmp → 改名」，进程被杀不会留下半个文件；读到不合格的一律当未命中并重写。
 *
 * 路径：前端给的是 UTF-8（RetroArch 在中文目录下也是如此），Windows 上要转宽字符
 * 才能打开——中文存档路径很常见，用 ANSI 版 fopen 会直接失败。
 */
#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

#define PATH_CAP      1024
#define HDR_SIZE      32
#define ID_SIZE       20
#define CACHE_MAGIC   "MCC1"

/* 单份产物上限（现在最大 2.6MB）。超了就当文件坏了，别拿它去 malloc。 */
#define MAX_PAYLOAD   (64u << 20)

/* ---------------- 小工具 ---------------- */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc32_of(const void *data, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* 拼 "dir/name"。分隔符统一用 '/'，Windows 的 CRT 也认。 */
static int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* ---------------- 文件操作（Windows 走宽字符） ---------------- */

#if defined(_WIN32)

static int to_wide(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

/* write=1 打开写，0 打开读 */
static FILE *xfopen(const char *path, int write)
{
    wchar_t w[PATH_CAP];
    if (!to_wide(path, w, PATH_CAP)) return NULL;
    return _wfopen(w, write ? L"wb" : L"rb");
}

static int xremove(const char *path)
{
    wchar_t w[PATH_CAP];
    return to_wide(path, w, PATH_CAP) ? _wremove(w) : -1;
}

static int xrename(const char *from, const char *to)
{
    wchar_t a[PATH_CAP], b[PATH_CAP];
    if (!to_wide(from, a, PATH_CAP) || !to_wide(to, b, PATH_CAP)) return -1;
    return _wrename(a, b);
}

static int xmkdir(const char *path)
{
    wchar_t w[PATH_CAP];
    return to_wide(path, w, PATH_CAP) ? _wmkdir(w) : -1;
}

#else

static FILE *xfopen(const char *path, int write)
{
    return fopen(path, write ? "wb" : "rb");
}

static int xremove(const char *path) { return remove(path); }
static int xrename(const char *from, const char *to) { return rename(from, to); }

static int xmkdir(const char *path)
{
    return mkdir(path, 0755);   /* 已存在算成功，见下面的调用点 */
}

#endif

/* ---------------- 目录遍历（只给淘汰用） ---------------- */

typedef struct {
    long long stamp;     /* 越大越新：POSIX 是 mtime 秒，Windows 是 FILETIME */
    char name[24];       /* "0123456789abcdef.bin" */
} cache_entry;

/* 只认自己写出来的名字：16 位小写十六进制 + ".bin" */
#if !defined(_WIN32)
static int is_cache_name(const char *s)
{
    static const char hex[] = "0123456789abcdef";
    if (strlen(s) != 20) return 0;
    for (int i = 0; i < 16; i++)
        if (!strchr(hex, s[i])) return 0;
    return strcmp(s + 16, ".bin") == 0;
}
#endif

#if defined(_WIN32)

static int entry_from_win(const WIN32_FIND_DATAW *fd, cache_entry *e)
{
    const wchar_t *s = fd->cFileName;
    for (int i = 0; i < 16; i++)
        if (!((s[i] >= L'0' && s[i] <= L'9') || (s[i] >= L'a' && s[i] <= L'f'))) return 0;
    if (s[16] != L'.' || s[17] != L'b' || s[18] != L'i' || s[19] != L'n' || s[20] != 0) return 0;
    for (int i = 0; i < 21; i++) e->name[i] = (char)s[i];
    e->stamp = ((long long)fd->ftLastWriteTime.dwHighDateTime << 32) |
               (long long)fd->ftLastWriteTime.dwLowDateTime;
    return 1;
}

static int list_entries(const char *dir, cache_entry **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    wchar_t wdir[PATH_CAP], pat[PATH_CAP];
    if (!to_wide(dir, wdir, PATH_CAP)) return -1;
    if (_snwprintf(pat, PATH_CAP, L"%s\\*", wdir) < 0) return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    cache_entry *v = NULL;
    size_t n = 0, cap = 0;
    do {
        cache_entry e;
        if (!entry_from_win(&fd, &e)) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            cache_entry *nv = (cache_entry *)realloc(v, nc * sizeof(*v));
            if (!nv) break;
            v = nv;
            cap = nc;
        }
        v[n++] = e;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    *out = v;
    *count = n;
    return 0;
}

#else

static int list_entries(const char *dir, cache_entry **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;

    cache_entry *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!is_cache_name(de->d_name)) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            cache_entry *nv = (cache_entry *)realloc(v, nc * sizeof(*v));
            if (!nv) break;
            v = nv;
            cap = nc;
        }
        snprintf(v[n].name, sizeof(v[n].name), "%s", de->d_name);
        char p[PATH_CAP];
        struct stat st;
        v[n].stamp = (join_path(p, sizeof(p), dir, de->d_name) == 0 && stat(p, &st) == 0)
                     ? (long long)st.st_mtime : 0;
        n++;
    }
    closedir(d);

    *out = v;
    *count = n;
    return 0;
}

#endif

static int by_stamp(const void *a, const void *b)
{
    const cache_entry *x = (const cache_entry *)a;
    const cache_entry *y = (const cache_entry *)b;
    if (x->stamp != y->stamp) return x->stamp < y->stamp ? -1 : 1;
    return strcmp(x->name, y->name);
}

void cache_prune(const char *dir)
{
    cache_entry *v = NULL;
    size_t n = 0;
    if (!dir || list_entries(dir, &v, &n) != 0) return;
    if (n <= CACHE_MAX_ENTRIES) { free(v); return; }

    /* 旧的排前面，删掉多出来的部分 */
    qsort(v, n, sizeof(*v), by_stamp);
    for (size_t i = 0; i < n - CACHE_MAX_ENTRIES; i++) {
        char p[PATH_CAP];
        if (join_path(p, sizeof(p), dir, v[i].name) == 0) xremove(p);
    }
    free(v);
}

/* 逐层建目录（玩家可能把存档目录删了，或者从没存过档）。已存在的层级忽略失败。 */
static void mkdir_parents(const char *path)
{
    char buf[PATH_CAP];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char save = *p;
        *p = '\0';
        xmkdir(buf);
        *p = save;
    }
    xmkdir(buf);
}

/* ---------------- 对外接口 ---------------- */

void cache_key(const char *src, size_t len, const char *build_id, char out[17])
{
    uint64_t h = fnv1a(1469598103934665603ull, src, len);
    /* 编译器指纹也算进去：换编译器时旧产物不会被错用，各代各存一份 */
    if (build_id && build_id[0])
        h = fnv1a(h, build_id, strlen(build_id));
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

int cache_load(const char *dir, const char *key, const char *build_id,
               char **out, size_t *out_len)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!dir || !key) return 1;

    char name[32], path[PATH_CAP];
    snprintf(name, sizeof(name), "%s.bin", key);
    if (join_path(path, sizeof(path), dir, name) != 0) return 1;

    FILE *f = xfopen(path, 0);
    if (!f) return 1;                 /* 没有就是没有，正常的未命中 */

    unsigned char hdr[HDR_SIZE];
    if (fread(hdr, 1, HDR_SIZE, f) != HDR_SIZE) { fclose(f); return -1; }
    if (memcmp(hdr, CACHE_MAGIC, 4) != 0) { fclose(f); return -1; }

    uint32_t plen = rd32(hdr + 4);
    uint32_t crc = rd32(hdr + 8);
    char bid[ID_SIZE];
    memcpy(bid, hdr + 12, ID_SIZE - 1);
    bid[ID_SIZE - 1] = '\0';

    if (plen == 0 || plen > MAX_PAYLOAD) { fclose(f); return -1; }
    if (build_id && build_id[0] && strcmp(bid, build_id) != 0) { fclose(f); return -1; }

    /* 文件长度必须刚好等于头 + 载荷，否则是写了一半的残缺文件 */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsz = ftell(f);
    if (fsz != (long)(HDR_SIZE + plen)) { fclose(f); return -1; }
    if (fseek(f, HDR_SIZE, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)plen + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, plen, f) != plen) { fclose(f); free(buf); return -1; }
    fclose(f);

    if (crc32_of(buf, plen) != crc) { free(buf); return -1; }
    buf[plen] = '\0';
    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = plen;
    return 0;
}

int cache_save(const char *dir, const char *key, const char *build_id,
               const char *src, size_t len)
{
    if (!dir || !key || !src || len == 0 || len > MAX_PAYLOAD) return -1;

    /* 存档目录一般由前端保证存在，但玩家可能删过，缺了就逐层补上 */
    mkdir_parents(dir);

    char name[32], tmp[PATH_CAP], dst[PATH_CAP];
    snprintf(name, sizeof(name), "%s.tmp", key);
    if (join_path(tmp, sizeof(tmp), dir, name) != 0) return -1;
    snprintf(name, sizeof(name), "%s.bin", key);
    if (join_path(dst, sizeof(dst), dir, name) != 0) return -1;

    unsigned char hdr[HDR_SIZE];
    memcpy(hdr, CACHE_MAGIC, 4);
    wr32(hdr + 4, (uint32_t)len);
    wr32(hdr + 8, crc32_of(src, len));
    memset(hdr + 12, 0, ID_SIZE);
    if (build_id && build_id[0])
        snprintf((char *)hdr + 12, ID_SIZE, "%s", build_id);

    FILE *f = xfopen(tmp, 1);
    if (!f) return -1;
    int ok = fwrite(hdr, 1, HDR_SIZE, f) == HDR_SIZE
          && fwrite(src, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { xremove(tmp); return -1; }

    xremove(dst);                     /* Windows 的 rename 不覆盖已有文件 */
    if (xrename(tmp, dst) != 0) { xremove(tmp); return -1; }

    cache_prune(dir);
    return 0;
}
