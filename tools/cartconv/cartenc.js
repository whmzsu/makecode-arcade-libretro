'use strict';
/*
 * 卡带编码器（.pxt64 之外的反向操作：工程 → .png 卡带）
 *
 * 用途：造测试样本。真实卡带样本只有 4 张，覆盖不到"带二进制资源""依赖社区
 * 扩展""版本更新"这些失败路径，没有样本就没法验证预检到底拦不拦得住。
 *
 * 算法与 pxt 的 Util.encodeBlobAsync 一致：
 *   像素最低 bpp 位承载数据，每 3 个通道跳过 1 个 alpha；
 *   第 1 个像素的低位存 bpp 本身，第 2 个像素起是 36 字节头（9 个小端 u32），
 *   之后是 LZMA 压缩过的工程 JSON。
 *
 * 只在开发时用，不进便携包。
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const IMAGE_MAGIC = 0x59347a7d;
const IMAGE_HEADER_SIZE = 36;

function loadLzma() {
  const cands = [
    'lzma',
    path.join(__dirname, 'runtime', 'node_modules', 'lzma'),
    path.join(__dirname, '..', 'vm-build', 'node_modules', 'lzma'),
  ];
  for (const c of cands) {
    try {
      return require(c);
    } catch (e) {
      /* next */
    }
  }
  throw new Error('缺少 lzma 模块');
}

/*
 * 位流写入。逐字照搬 pxt 的实现，包括它在数据耗尽时的处理方式 ——
 * 任何"优化"都会导致生成的卡带与官方工具不兼容。
 */
function encodeBlob(img, ptr, bpp, data) {
  let shift = 0;
  let dp = 0;
  let v = data[dp++];
  const bppMask = (1 << bpp) - 1;
  let keepGoing = true;
  while (keepGoing) {
    let bits = (v >> shift) & bppMask;
    const left = 8 - shift;
    if (left <= bpp) {
      if (dp >= data.length) {
        if (left === 0) break;
        keepGoing = false;
      }
      v = data[dp++];
      bits |= (v << left) & bppMask;
      shift = bpp - left;
    } else {
      shift += bpp;
    }
    img[ptr] = ((img[ptr] & ~bppMask) | bits) & 0xff;
    ptr++;
    if ((ptr & 3) === 3) img[ptr++] = 0xff; // alpha 不承载数据，补 0xff
  }
  return ptr;
}

function u32le(vals) {
  const b = Buffer.alloc(vals.length * 4);
  vals.forEach((v, i) => b.writeUInt32LE(v >>> 0, i * 4));
  return b;
}

/* RGBA → PNG（filter 全 0，不做行预测；只求能被自己解回来） */
function pngEncode(width, height, rgba) {
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    rgba.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }

  const table = [];
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  const crc32 = (buf) => {
    let c = 0xffffffff;
    for (const b of buf) c = table[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };

  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
  };

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/*
 * 工程对象 → .png 卡带
 *
 * project 形如 { meta: {...}, source: "<JSON 字符串>" }，与 decodeCart 返回的
 * obj 同构，方便"解码 → 改一处 → 重新编码"。
 */
function encodeCart(project, opts) {
  const options = opts || {};
  const lzma = loadLzma();

  const text = typeof project.source === 'string' ? JSON.stringify(project) : '';
  if (!text) throw new Error('project.source 必须是 JSON 字符串');
  const blob = Buffer.from(lzma.compress(text));

  const width = options.width || 512;
  let height = options.height || 512;

  // 选最小可行 bpp（上限 4，与 pxt 一致）
  const usable = (width * height - 1) * 3;
  let bpp = 1;
  while (bpp < 4 && usable * bpp < (IMAGE_HEADER_SIZE + blob.length) * 8) bpp++;

  // 还是放不下就加行（第二段用 bpp=8 从原画布末尾继续写）
  const imgCapacity = (usable * bpp) >> 3;
  const missing = IMAGE_HEADER_SIZE + blob.length - imgCapacity;
  const addedOffset = width * height * 4;
  let addedLines = 0;
  if (missing > 0) addedLines = Math.ceil(missing / (width * 3));

  const totalH = height + addedLines;
  const rgba = Buffer.alloc(width * totalH * 4, 0xff); // 白底

  const header = u32le([IMAGE_MAGIC, blob.length, addedLines, 0, 0, 0, 0, 0, 0]);
  if (header.length !== IMAGE_HEADER_SIZE) throw new Error('头部长度算错了');

  encodeBlob(rgba, 0, 1, [bpp]);
  let ptr = 4;
  ptr = encodeBlob(rgba, ptr, bpp, header);
  if (ptr & 3) throw new Error('头部没有 4 字节对齐，算法有误');

  if (addedLines === 0) {
    encodeBlob(rgba, ptr, bpp, blob);
  } else {
    const firstChunk = imgCapacity - IMAGE_HEADER_SIZE;
    encodeBlob(rgba, ptr, bpp, blob.subarray(0, firstChunk));
    encodeBlob(rgba, addedOffset, 8, blob.subarray(firstChunk));
  }

  // 剩余 alpha 补 0xff
  ptr |= 3;
  for (; ptr < rgba.length; ptr += 4) rgba[ptr] = 0xff;

  return { png: pngEncode(width, totalH, rgba), bpp, width, height: totalH, blobSize: blob.length };
}

/* 便捷入口：读卡带 → 改写 → 写新卡带 */
function rewrite(srcPng, dstPng, mutator) {
  const { decodeCart } = require('./cartdec.js');
  return decodeCart(srcPng).then((dec) => {
    const project = { meta: dec.obj.meta, source: JSON.stringify(dec.sources) };
    // 保留原 obj 的其他字段（cloudId 等在 meta 里）
    Object.assign(project, dec.obj, project);
    const sources = JSON.parse(project.source);
    const r = mutator({ meta: project.meta, sources });
    if (r && r.meta) project.meta = r.meta;
    if (r && r.sources) project.source = JSON.stringify(r.sources);
    const out = encodeCart(project);
    fs.writeFileSync(dstPng, out.png);
    return out;
  });
}

module.exports = { encodeCart, pngEncode, rewrite, encodeBlob, IMAGE_MAGIC };
