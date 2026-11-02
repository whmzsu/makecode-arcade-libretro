'use strict';
/*
 * cartdec.js — MakeCode Arcade .png 卡带解码（纯 node，无第三方依赖）
 *
 * 与 tools/extract_png_meta.py 完全等价：PNG 解码 → 位流提取 → LZMA 解压
 * → 项目 JSON。唯一的外部依赖是 lzma（node_modules/lzma，纯 JS 实现）。
 *
 * 格式要点（详见 docs/PNG-FORMAT.md）：
 *   - 卡带数据藏在像素最低位（bpp = 1~5，由前 3 个通道的最低位编码）
 *   - 前 36 字节是头：magic 0x59347A7D + payload 长度 + 附加行数
 *   - payload 是 lzma(alone) 压缩的 JSON，键为 source（文件名 → 文本）
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const MAGIC = 0x59347a7d;
const IMG_HEADER_SIZE = 36;
const PNG_SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

/* PNG → RGBA。只支持 8 位、非隔行（MakeCode 保存的卡带都满足）。 */
function pngToRgba(buf) {
  if (buf.length < 8 || !buf.subarray(0, 8).equals(PNG_SIG)) {
    throw new Error('not a PNG file');
  }
  let pos = 8;
  let ihdr = null;
  let plte = Buffer.alloc(0);
  let trns = Buffer.alloc(0);
  const idat = [];
  while (pos + 8 <= buf.length) {
    const length = buf.readUInt32BE(pos);
    const ctype = buf.subarray(pos + 4, pos + 8).toString('latin1');
    const body = buf.subarray(pos + 8, pos + 8 + length);
    pos += 12 + length;
    if (ctype === 'IHDR') {
      ihdr = {
        w: body.readUInt32BE(0),
        h: body.readUInt32BE(4),
        depth: body[8],
        color: body[9],
        comp: body[10],
        filt: body[11],
        interlace: body[12],
      };
    } else if (ctype === 'PLTE') {
      plte = body;
    } else if (ctype === 'tRNS') {
      trns = body;
    } else if (ctype === 'IDAT') {
      idat.push(body);
    } else if (ctype === 'IEND') {
      break;
    }
  }
  if (!ihdr) throw new Error('missing IHDR');
  if (ihdr.depth !== 8 || ihdr.interlace !== 0 || ihdr.comp !== 0 || ihdr.filt !== 0) {
    throw new Error(`unsupported PNG: depth=${ihdr.depth} interlace=${ihdr.interlace}`);
  }
  const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
  const channels = CHANNELS[ihdr.color];
  if (!channels) throw new Error(`unsupported color type ${ihdr.color}`);

  const { w, h } = ihdr;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * channels;
  const out = Buffer.alloc(w * h * 4);
  let prev = Buffer.alloc(stride);

  for (let y = 0; y < h; y++) {
    const base = y * (stride + 1);
    const f = raw[base];
    const line = Buffer.from(raw.subarray(base + 1, base + 1 + stride));
    if (f === 1) {
      for (let i = channels; i < stride; i++) line[i] = (line[i] + line[i - channels]) & 0xff;
    } else if (f === 2) {
      for (let i = 0; i < stride; i++) line[i] = (line[i] + prev[i]) & 0xff;
    } else if (f === 3) {
      for (let i = 0; i < stride; i++) {
        const a = i >= channels ? line[i - channels] : 0;
        line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff;
      }
    } else if (f === 4) {
      for (let i = 0; i < stride; i++) {
        const a = i >= channels ? line[i - channels] : 0;
        const c = i >= channels ? prev[i - channels] : 0;
        line[i] = (line[i] + paeth(a, prev[i], c)) & 0xff;
      }
    } else if (f !== 0) {
      throw new Error(`bad filter ${f} at row ${y}`);
    }
    prev = line;

    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      const i = x * channels;
      if (ihdr.color === 0) {
        out[o] = out[o + 1] = out[o + 2] = line[i];
        out[o + 3] = 255;
      } else if (ihdr.color === 2) {
        out[o] = line[i]; out[o + 1] = line[i + 1]; out[o + 2] = line[i + 2]; out[o + 3] = 255;
      } else if (ihdr.color === 3) {
        const idx = line[i];
        out[o] = plte[idx * 3]; out[o + 1] = plte[idx * 3 + 1]; out[o + 2] = plte[idx * 3 + 2];
        out[o + 3] = idx < trns.length ? trns[idx] : 255;
      } else if (ihdr.color === 4) {
        out[o] = out[o + 1] = out[o + 2] = line[i];
        out[o + 3] = line[i + 1];
      } else {
        out[o] = line[i]; out[o + 1] = line[i + 1]; out[o + 2] = line[i + 2]; out[o + 3] = line[i + 3];
      }
    }
  }
  return { w, h, rgba: out };
}

/* 位流解码：每个字节取低 bpp 位，每 3 个字节跳 1 个（Alpha 通道不承载数据）。 */
function blobDecode(d, ptr, bpp, tgrLen) {
  let shift = 0;
  let acc = 0;
  const mask = (1 << bpp) - 1;
  const out = Buffer.alloc(tgrLen);
  let i = 0;
  const n = d.length;
  while (i < tgrLen) {
    if (ptr >= n) throw new Error(`bitstream overrun at byte ${ptr}`);
    acc |= (d[ptr] & mask) << shift;
    ptr += 1;
    if ((ptr & 3) === 3) ptr += 1;
    shift += bpp;
    if (shift >= 8) {
      out[i++] = acc & 0xff;
      acc = 0;
      shift -= 8;
    }
  }
  return { ptr, out };
}

function extractPayload(w, h, d) {
  const bpp = (d[0] & 1) | ((d[1] & 1) << 1) | ((d[2] & 1) << 2);
  if (bpp < 1 || bpp > 5) {
    throw new Error('不是 MakeCode Arcade 卡带（像素最低位没有编码数据）');
  }
  let ptr = 4;
  const hdRes = blobDecode(d, ptr, bpp, IMG_HEADER_SIZE);
  ptr = hdRes.ptr;
  const hd = hdRes.out;
  const magic = hd.readUInt32LE(0);
  const retLen = hd.readUInt32LE(4);
  const addedLines = hd.readUInt32LE(8);
  if (magic !== MAGIC) {
    throw new Error(`bad magic 0x${magic.toString(16)} (expect 0x${MAGIC.toString(16)})`);
  }
  if (addedLines > 0) {
    const origSize = (h - addedLines) * w;
    const imgCap = ((origSize - 1) * 3 * bpp) >> 3;
    const r1 = blobDecode(d, ptr, bpp, imgCap - IMG_HEADER_SIZE);
    let res = r1.out;
    const r2 = blobDecode(d, origSize * 4, 8, retLen - res.length);
    res = Buffer.concat([res, r2.out]);
    return { bpp, payload: res };
  }
  const r = blobDecode(d, ptr, bpp, retLen);
  return { bpp, payload: r.out };
}

/*
 * lzma 解压（node-lzma 是回调 API，这里包成 Promise）。
 *
 * 注意：LZMA-JS 的 decompress 回调给的是**已经按 UTF-8 解码好的字符串**，
 * 不是原始字节。早期版本这里用 Buffer.from(result, 'binary') 又转了一次，
 * 纯 ASCII 的卡带看不出问题，含中文/emoji 的卡带会被打成乱码并让 JSON.parse
 * 报 "Bad control character"。直接用字符串即可。
 */
let _lzma = null;
function loadLzma() {
  if (_lzma) return _lzma;
  const cands = [
    'lzma', // 便携包：node_modules 就在旁边，node 能直接解析
    path.join(__dirname, 'runtime', 'node_modules', 'lzma'),
    path.join(__dirname, '..', 'vm-build', 'node_modules', 'lzma'),
    path.join(__dirname, '..', '..', 'tools', 'vm-build', 'node_modules', 'lzma'),
  ];
  for (const c of cands) {
    try {
      _lzma = require(c);
      return _lzma;
    } catch (e) {
      /* 继续找下一个 */
    }
  }
  throw new Error('缺少 lzma 模块（便携包缺失 runtime/node_modules/lzma）');
}

function lzmaDecompressText(buf) {
  const lzma = loadLzma();
  return new Promise((resolve, reject) => {
    lzma.decompress(new Uint8Array(buf), (result, error) => {
      if (error) reject(error);
      else resolve(typeof result === 'string' ? result : Buffer.from(result).toString('utf8'));
    });
  });
}

async function payloadToProject(res) {
  let content;
  let compressed = false;
  if (res[0] === 0x7b /* '{' */) {
    content = res.toString('utf8');
  } else {
    content = await lzmaDecompressText(res);
    compressed = true;
  }
  const obj = JSON.parse(content);
  const source = JSON.parse(obj.source);
  return { compressed, obj, source };
}

/* 完整链路：.png → { obj, sources, bpp } */
async function decodeCart(file) {
  const data = fs.readFileSync(file);
  const { w, h, rgba } = pngToRgba(data);
  const { bpp, payload } = extractPayload(w, h, rgba);
  const { compressed, obj, source } = await payloadToProject(payload);
  if (!source || Object.keys(source).length === 0) {
    throw new Error('卡带里没有源码文件');
  }
  return { obj, sources: source, bpp, compressed, pngSize: data.length, payloadSize: payload.length };
}

module.exports = { decodeCart, pngToRgba, extractPayload, payloadToProject, MAGIC };
