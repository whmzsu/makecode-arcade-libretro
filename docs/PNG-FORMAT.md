# PNG 卡带格式规格（MakeCode Arcade 项目文件）

来源：以 jan4984 原型的 Rust 实现为准（`loadPNG`），
其注释标明移植自 microsoft/pxt 的 `pxtlib/util.ts decodeBlobAsync`。
core 内的 C++ 实现（core/src/png_loader.cpp）与本工具（tools/extract_png_meta.py）
均按本规格编写。

## 总体结构

卡带 = 一张普通 PNG 截图，项目数据隐写编码在**像素数据的低位比特**里。
载荷是一段 JSON（可能经 LZMA 压缩），形如：

```json
{ "source": "{ \"main.ts\": \"<TS 源码>\", ... }", ... }
```

`source` 字段本身又是一个 JSON 字符串，键为项目内文件名（至少 main.ts），
值为文本源码。

## 解码流程

### 1. PNG → RGBA 像素

正常解码 PNG 得到 8bit RGBA 像素序列 `d`（pxt 保存的截图一般为
RGB/RGBA/调色板之一，统一转 RGBA 处理）。

### 2. 探测 bpp

```c
bpp = (d[0] & 1) | ((d[1] & 1) << 1) | ((d[2] & 1) << 2);
```

合法范围 1..5，`bpp == 0` 或 `> 5` 视为不是卡带文件。

### 3. 位流解码（decode 函数）

从像素字节流中以 `bpp` bit 为单位做 **LSB-first** 打包读取，逢 alpha 字节跳过：

```c
// ptr 从 4 开始（跳过 bpp 探测用的前 3 字节 + 对齐）
mask = (1 << bpp) - 1;
while (i < target_len) {
    acc |= (d[ptr] & mask) << shift;
    ptr++;
    if ((ptr & 3) == 3) ptr++;   // 跳过 RGBA 的 alpha 字节（下标 3,7,11,...）
    shift += bpp;
    if (shift >= 8) { out.push(acc & 0xff); i++; acc = 0; shift -= 8; }
}
```

### 4. 头部（36 字节）

从 ptr=4 解出 IMG_HEADER_SIZE=36 字节，按小端 u32 解释：

| 偏移 | 含义 |
|---|---|
| 0  | magic，必须等于 `0x59347a7d` |
| 4  | 载荷总长 ret_len |
| 8  | added_lines（截图后期"加高"追加的数据行数，通常为 0） |

### 5. 载荷提取

- `added_lines == 0`：从头部之后继续按 bpp 解出 `ret_len` 字节。
- `added_lines > 0`：原尺寸区域容量 `img_cap = (orig_size - 1) * 3 * bpp >> 3`
  （orig_size = (height - added_lines) * width），
  先解 `img_cap - 36` 字节，再从字节偏移 `orig_size * 4` 处以 **8bpp**
  解出剩余 `ret_len - 已解长度` 字节并拼接。
  （此分支仅见于截图被编辑软件加高的情况，需用真实样本验证。）

### 6. 载荷 → JSON

- 载荷首字节为 `{`：本身就是 JSON 文本。
- 否则：**LZMA 压缩**（LZMA alone 格式，对应 xz-utils 的 `.lzma`、
  Rust `lzma_rs`、Python `lzma.FORMAT_ALONE`），解压后为 JSON 文本。

## 验证状态

- 自写 Python 实现（tools/ 下两脚本）与合成卡带回环自测通过。
- C 实现直接采用 whmzsu 姊妹 core 的 `cart.c`（已在 QuickJS/Hermes 两个
  生产 core 上跑通真实卡带），实现细节与本文档一致，且额外覆盖：

### cart.c 补充的实测细节（本文档初版未覆盖）

1. **bpp 合法范围**：实测卡带 bpp 落在 1..4（cart.c 拒绝 >4；jan4984
   的 Rust 版放宽到 5）。以 cart.c 的 1..4 为准。
2. **source 字段三种形态**：
   - JSON 文件表字符串（主流）：`{"main.ts":..., "images.g.ts":...}`；
   - 裸源码字符串（少数）：需包成只含 main.ts 的文件表；
   - 老卡带顶层直接有 `"main.ts"` 键（legacy）。
   注意**不能只取 main.ts**——`.g.ts`/`.jres` 生成资源与主源码必须
   一起参与编译，否则缺符号。
3. **added_lines > 0 分支**：cart.c 已完整实现（原尺寸区按 bpp 解到
   容量上限，超出部分从 `orig_size*4` 字节偏移以 8bpp 解出拼接）。

## 验证工具

```bash
python tools/extract_png_meta.py game.png            # 打印格式信息
python tools/extract_png_meta.py game.png --dump-ts  # 导出 main.ts
```

## TODO / 待验证

- [ ] added_lines > 0 分支用真实样本验证
- [ ] `source` 中除 main.ts 外的其他文件（扩展名、依赖库 ts）如何携带
- [ ] pxt `decodeBlobAsync` 原始实现与 Rust 移植逐行比对（以 pxt 为准）
