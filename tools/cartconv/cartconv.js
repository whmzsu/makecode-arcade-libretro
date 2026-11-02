#!/usr/bin/env node
'use strict';
/*
 * cartconv — MakeCode Arcade .png 卡带 → .pxt64 字节码镜像转换器
 *
 * 为什么需要这一步：MakeCode Arcade 保存的 .png 卡带里只有**项目源码**
 * （meta + source），没有编译产物，必须过一遍 pxt 编译器才能变成 libretro
 * core 能直接执行的 .pxt64。
 *
 * 用法：
 *   cartconv game.png                     单张
 *   cartconv a.png b.png c.png            多张
 *   cartconv carts/                       目录（递归找 *.png）
 *   cartconv carts/ -o out/               统一输出到 out/
 *   cartconv carts/ -o out/ --force       覆盖已有 .pxt64
 *
 * 把 .png 拖到 cartconv.exe 上等价于 cartconv <那些文件>。
 */

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const { decodeCart } = require('./cartdec.js');
const precheck = require('./precheck.js');

const VERSION = '1.1.0';

// ---------------------------------------------------------------- 工具函数

function die(msg, code) {
  console.error('\n[错误] ' + msg);
  process.exit(code === undefined ? 1 : code);
}

function rmrf(p) {
  try {
    fs.rmSync(p, { recursive: true, force: true });
  } catch (e) {
    /* 删不掉不致命 */
  }
}

function copyDir(src, dst) {
  fs.mkdirSync(dst, { recursive: true });
  fs.cpSync(src, dst, { recursive: true, force: true });
}

function humanSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

// ---------------------------------------------------------------- 运行时定位

/*
 * pxt 编译链（pxt-core + pxt-arcade + 依赖）的 node_modules 位置。
 * 便携包里在 <exe目录>/runtime/node_modules；开发仓库里在 tools/vm-build。
 */
function findPxtRoot(explicit) {
  const here = __dirname;
  const cands = [
    explicit,
    process.env.CART_CONV_PXT,
    path.join(here, 'runtime', 'node_modules'),
    path.join(here, '..', 'vm-build', 'node_modules'),
    path.join(here, '..', '..', 'tools', 'vm-build', 'node_modules'),
  ].filter(Boolean);

  for (const c of cands) {
    const pxtJs = path.join(c, 'pxt-core', 'built', 'pxt.js');
    const target = path.join(c, 'pxt-arcade', 'pxtarget.json');
    if (fs.existsSync(pxtJs) && fs.existsSync(target)) {
      return { root: c, pxtJs, target };
    }
  }
  return null;
}

/* 模板缓存：pxt_modules(4MB) + built/dockercross(1MB)，省掉每次 20 秒的 install */
function templateDir() {
  return path.join(__dirname, '.cache', 'template');
}

function templateReady() {
  const d = templateDir();
  return (
    fs.existsSync(path.join(d, 'pxt_modules', 'device')) &&
    fs.existsSync(path.join(d, 'built', 'dockercross', 'bld', 'all.tgz.b64'))
  );
}

/* 首次使用时生成模板：用一个极简工程跑完整流程，把依赖和 docker 占位产物留下来 */
function buildTemplate(pxt, log) {
  const tpl = templateDir();
  const work = path.join(pxt.root, 'pxt-arcade', '.cartconv_bootstrap');
  log('首次使用，正在准备编译环境（约 30 秒，只需一次）...');
  rmrf(work);
  fs.mkdirSync(work, { recursive: true });
  fs.writeFileSync(
    path.join(work, 'pxt.json'),
    JSON.stringify({ name: 'bootstrap', dependencies: { device: '*' }, files: ['main.ts'] }, null, 2)
  );
  fs.writeFileSync(path.join(work, 'main.ts'), 'game.splash("ok")\n');

  const env = pxtEnv();
  let r = spawnSync(process.execPath, [pxt.pxtJs, 'install', '--hwvariant', 'vm'], {
    cwd: work, env, encoding: 'utf8',
  });
  if (r.status !== 0 || !fs.existsSync(path.join(work, 'pxt_modules', 'device'))) {
    rmrf(work);
    die('准备编译环境失败（pxt install）\n' + tail(r.stdout) + tail(r.stderr));
  }

  // 第一次 build 会因缺 docker 交叉编译产物而失败，补个占位包再编一次
  r = spawnSync(process.execPath, [pxt.pxtJs, 'build', '--hwvariant', 'vm'], {
    cwd: work, env, encoding: 'utf8',
  });
  if (!fs.existsSync(path.join(work, 'built', 'binary.pxt64'))) {
    ensureDockerStub(work);
    r = spawnSync(process.execPath, [pxt.pxtJs, 'build', '--hwvariant', 'vm'], {
      cwd: work, env, encoding: 'utf8',
    });
  }
  if (!fs.existsSync(path.join(work, 'built', 'binary.pxt64'))) {
    rmrf(work);
    die('准备编译环境失败（pxt build）\n' + tail(r.stdout) + tail(r.stderr));
  }

  rmrf(tpl);
  copyDir(path.join(work, 'pxt_modules'), path.join(tpl, 'pxt_modules'));
  copyDir(path.join(work, 'built', 'dockercross'), path.join(tpl, 'built', 'dockercross'));
  rmrf(work);
  log('编译环境就绪');
}

/* pxt 在 --hwvariant vm 的最后一步会调 docker 交叉编译原生宿主，并读
 * built/dockercross/bld/all.tgz.b64。我们只要编译器产物，所以造个占位包。 */
function ensureDockerStub(projDir) {
  const bld = path.join(projDir, 'built', 'dockercross', 'bld');
  const dockercross = path.join(projDir, 'built', 'dockercross');
  if (!fs.existsSync(dockercross)) return false;
  fs.mkdirSync(bld, { recursive: true });
  const stub = path.join(bld, 'all.tgz.b64');
  if (!fs.existsSync(stub)) {
    // 138 字节：一个只含 stub.txt 的 tar.gz 的 base64
    fs.writeFileSync(stub, DOCKER_STUB_B64 + '\n', 'ascii');
  }
  return true;
}

// 与 pxt 期望的占位包等价的 tar.gz（内含一个 stub.txt）的 base64，
// 原样取自一次成功编译后的 built/dockercross/bld/all.tgz.b64
const DOCKER_STUB_B64 =
  'H4sIAH0152oC/+3RsQ2DQBAEwIupwhVYBA+uB0rwPxLlcxAhcpAszyR72mSDq22Z321tcaM+jaUcma6ZhtO9958yjPHq4wFLbdM3J+M/1fx/FwAAAAAAAAAAAPyaDY8Ex/kAKAAA';

function pxtEnv() {
  const env = Object.assign({}, process.env);
  env.PXT_FORCE_LOCAL = '1';
  env.PXT_NODOCKER = '1';
  // 便携包把 docker 假程序放在 fakebin/ 下（只有一个 docker.cmd）
  for (const cand of [path.join(__dirname, 'fakebin'), path.join(__dirname, '..', 'fakebin')]) {
    if (fs.existsSync(cand)) {
      env.PATH = cand + path.delimiter + (env.PATH || '');
      break;
    }
  }
  return env;
}

function tail(s, n) {
  if (!s) return '';
  const max = n || 1500;
  s = String(s).trim();
  return s.length > max ? s.slice(-max) : s;
}

/*
 * 去掉 pxt 输出里的固定噪音（版本横幅、工程路径、docker 提示）。
 * 这些对用户没有信息量，留着会把真正的错误挤出屏幕。
 */
function stripNoise(s) {
  return String(s || '')
    .split('\n')
    .filter(
      (l) =>
        !/^(Using target |setting hardware variant|building |\s+target: |\s+pxt-core: )/.test(l) &&
        !/^npm (WARN|notice)/.test(l)
    )
    .join('\n')
    .trim();
}

/*
 * 从编译器输出里挑出真正的错误行。
 *
 * pxt 的 stdout 开头是一堆横幅（target 版本、硬件变体、工程路径），
 * 直接取第一行会得到 "Using target arcade with build engine codal"，
 * 对用户毫无意义。真正的错误长这样：
 *   main.ts(212,5): error TS2322: Type '...' is not assignable to type 'number'.
 */
function pickErrorLine(s) {
  const lines = String(s || '')
    .split('\n')
    .map((l) => l.trim())
    .filter(Boolean);
  const pats = [
    /\(\d+,\d+\):\s*error\s+[A-Z]+\d+:/,
    /:\s*error\s+[A-Z]+\d+:/,
    /^error\b/i,
    /\berror\b/i,
  ];
  for (const re of pats) {
    const hit = lines.find((l) => re.test(l));
    if (hit) return hit;
  }
  return lines[0] || '';
}

// ---------------------------------------------------------------- 输入收集

function collectInputs(args, log) {
  const out = [];
  const seen = new Set();

  const addFile = (p) => {
    const abs = path.resolve(p);
    if (seen.has(abs)) return;
    seen.add(abs);
    out.push(abs);
  };

  const walk = (dir) => {
    let ents;
    try {
      ents = fs.readdirSync(dir, { withFileTypes: true });
    } catch (e) {
      log(`跳过无法读取的目录: ${dir}`);
      return;
    }
    for (const ent of ents) {
      const p = path.join(dir, ent.name);
      if (ent.isDirectory()) {
        if (ent.name === 'node_modules' || ent.name.startsWith('.')) continue;
        walk(p);
      } else if (/\.png$/i.test(ent.name)) {
        addFile(p);
      }
    }
  };

  for (const a of args) {
    let st;
    try {
      st = fs.statSync(a);
    } catch (e) {
      log(`跳过不存在的路径: ${a}`);
      continue;
    }
    if (st.isDirectory()) walk(path.resolve(a));
    else if (/\.png$/i.test(a)) addFile(a);
    else log(`跳过非 .png 文件: ${a}`);
  }
  return out;
}

// ---------------------------------------------------------------- 转换单张

async function convertOne(pxt, tpl, src, outPath, opts) {
  const started = Date.now();
  const dec = await decodeCart(src);

  // 编译前预检。注定失败的（二进制资源、需要联网的社区扩展）在这里就拦下，
  // 否则玩家看到的是一句没有信息量的 pxt 报错。
  const pre = precheck.run(dec, pxt.root);
  if (pre.fatal.length) {
    // 首行要能独立读懂 —— 失败清单只显示首行
    if (pre.fatal.length === 1) throw new Error('无法转换：' + pre.fatal[0]);
    throw new Error(
      '无法转换：' +
        pre.fatal[0] +
        `（另有 ${pre.fatal.length - 1} 项）\n  明细：\n` +
        pre.fatal.map((s) => '  · ' + s).join('\n')
    );
  }

  if (opts.verbose) {
    const names = Object.keys(dec.sources).sort();
    process.stdout.write(`      源文件 ${names.length} 个: ${names.join(', ')}\n`);
  }

  const work = path.join(pxt.root, 'pxt-arcade', '.cartconv_work');
  rmrf(work);
  fs.mkdirSync(work, { recursive: true });
  copyDir(path.join(tpl, 'pxt_modules'), path.join(work, 'pxt_modules'));
  copyDir(path.join(tpl, 'built', 'dockercross'), path.join(work, 'built', 'dockercross'));

  for (const name of Object.keys(dec.sources)) {
    const dst = path.join(work, name.replace(/\\/g, '/'));
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.writeFileSync(dst, dec.sources[name], 'utf8');
  }

  const env = pxtEnv();

  // 模板里只有基础依赖；卡带的 pxt.json 可能还声明了 animation /
  // color-coded-tilemap 等，必须按项目再 install 一次补齐。
  // 有了模板打底，这一步只是补差量，通常 1~2 秒。
  const inst = spawnSync(process.execPath, [pxt.pxtJs, 'install', '--hwvariant', 'vm'], {
    cwd: work, env, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, timeout: 180000,
  });

  // 不看 install 的退出码（pxt 有时非 0 但已装好、有时相反），直接核对目录。
  // 缺包是编译失败最常见的原因，先算出来，等 build 失败时用来归因。
  const missing = precheck.missingDeps(work, pre.deps);

  let r = spawnSync(process.execPath, [pxt.pxtJs, 'build', '--hwvariant', 'vm'], {
    cwd: work, env, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, timeout: 600000,
  });
  let image = path.join(work, 'built', 'binary.pxt64');
  if (!fs.existsSync(image) && ensureDockerStub(work)) {
    r = spawnSync(process.execPath, [pxt.pxtJs, 'build', '--hwvariant', 'vm'], {
      cwd: work, env, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, timeout: 600000,
    });
  }
  if (!fs.existsSync(image)) {
    let detail;
    if (missing.length) {
      // 缺扩展是压倒性的大概率原因，优先报它，别让玩家在 TS 报错里迷路
      detail =
        `缺少依赖扩展 ${missing.join(', ')}\n` +
        '  这些扩展本机没有，需要联网从 MakeCode 扩展库下载；\n' +
        '  离线环境下无法完成。\n' +
        (inst.stderr ? '  pxt install 输出：\n' + tail(inst.stderr, 600) : '');
    } else if (r.error && r.error.code === 'ETIMEDOUT') {
      detail = '编译超时（超过 10 分钟）\n  用 --verbose 可以看到卡在哪一步。';
    } else {
      const clean = {
        out: stripNoise(r.stdout),
        err: stripNoise(r.stderr),
      };
      const raw = opts.verbose
        ? tail(clean.out, 3000) + '\n' + tail(clean.err, 3000)
        : tail(clean.err, 900) || tail(clean.out, 900);
      detail = String(raw).trim() || '编译器没有给出错误信息\n  加 --verbose 重跑可以看到完整输出';
    }
    if (opts.keepProject) process.stderr.write(`      中间工程保留在: ${work}\n`);
    else rmrf(work);
    const head = missing.length ? String(detail).split('\n')[0] : pickErrorLine(detail);
    // 摘要那行不重复出现在明细里
    const body = String(detail).trim().split('\n');
    const rest = body[0] && body[0].trim() === String(head).trim() ? body.slice(1) : body;
    const err = new Error(
      '编译失败：' + (head || '原因未知').slice(0, 300) +
        (rest.length ? '\n' + rest.join('\n') : '')
    );
    // 预检的警告要跟着错误一起走：卡带比编译器新这类原因，
    // 只有和失败同时出现才有解释力
    err.warn = pre.warn;
    throw err;
  }

  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.copyFileSync(image, outPath);
  const size = fs.statSync(outPath).size;
  if (!opts.keepProject) rmrf(work);

  return {
    size,
    bpp: dec.bpp,
    files: Object.keys(dec.sources).length,
    ms: Date.now() - started,
    warn: pre.warn,
  };
}

// ---------------------------------------------------------------- 参数

function parseArgs(argv) {
  const opts = { inputs: [], outDir: null, force: false, verbose: false, keepProject: false, quiet: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-h' || a === '--help') opts.help = true;
    else if (a === '-V' || a === '--version') opts.version = true;
    else if (a === '-o' || a === '--out') opts.outDir = argv[++i];
    else if (a === '--force') opts.force = true;
    else if (a === '-v' || a === '--verbose') opts.verbose = true;
    else if (a === '--keep-project') opts.keepProject = true;
    else if (a === '-q' || a === '--quiet') opts.quiet = true;
    else if (a.startsWith('-')) die(`未知选项: ${a}（--help 查看用法）`);
    else opts.inputs.push(a);
  }
  return opts;
}

const HELP = `
cartconv ${VERSION} — MakeCode Arcade 卡带转换器（.png → .pxt64）

用法:
  cartconv <卡带.png | 目录> [...]           转换到卡带所在目录
  cartconv <输入...> -o <输出目录>           统一输出到指定目录

选项:
  -o, --out DIR      输出目录（默认与卡带同目录）
      --force        已存在同名 .pxt64 时也重新转换
  -v, --verbose      显示编译过程的详细信息
      --keep-project 保留中间工程（排查问题时用）
  -q, --quiet        只输出结果行
  -h, --help         显示本帮助
  -V, --version      显示版本

说明:
  * .png 卡带里存的是项目源码，需要现场编译成 VM 字节码，单张约 5~8 秒。
  * 目录会递归查找（跳过 node_modules 和隐藏目录）。
  * 把 .png 或文件夹直接拖到 cartconv.exe 上等同于命令行传参。
  * 转换结果可直接被 MakeCode Arcade libretro core 加载。
`;

// ---------------------------------------------------------------- 主流程

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help || process.argv.length <= 2) {
    process.stdout.write(HELP);
    return 0;
  }
  if (opts.version) {
    process.stdout.write(`cartconv ${VERSION}\n`);
    return 0;
  }

  const log = opts.quiet ? () => {} : (m) => process.stdout.write(m + '\n');

  const pxt = findPxtRoot();
  if (!pxt) {
    die(
      '找不到 pxt 编译环境。\n' +
        '  便携版：确认 runtime/node_modules 与 cartconv.js 在同一个目录下\n' +
        '  开发版：先跑 tools/vm-build/setup_toolchain.sh，或用 CART_CONV_PXT 指定 node_modules 路径'
    );
  }

  const files = collectInputs(opts.inputs, log);
  if (files.length === 0) die('没有找到任何 .png 卡带');

  if (!templateReady()) buildTemplate(pxt, log);
  const tpl = templateDir();

  log(`共 ${files.length} 张卡带待转换`);
  if (opts.outDir) {
    fs.mkdirSync(opts.outDir, { recursive: true });
    log(`输出目录: ${path.resolve(opts.outDir)}`);
  }
  log('');

  let ok = 0;
  let skip = 0;
  let fail = 0;
  const failures = [];
  const t0 = Date.now();
  const usedNames = new Set();

  for (let i = 0; i < files.length; i++) {
    const src = files[i];
    const base = path.basename(src, path.extname(src));
    const tag = `[${i + 1}/${files.length}] ${path.basename(src)}`;

    let outPath;
    if (opts.outDir) {
      let name = base;
      if (usedNames.has(name.toLowerCase())) {
        // 不同目录下的同名卡带：用父目录名区分，避免互相覆盖
        name = path.basename(path.dirname(src)) + '_' + base;
      }
      usedNames.add(name.toLowerCase());
      outPath = path.join(opts.outDir, name + '.pxt64');
    } else {
      outPath = path.join(path.dirname(src), base + '.pxt64');
    }

    if (!opts.force && fs.existsSync(outPath)) {
      log(`${tag}  跳过（已存在 ${path.basename(outPath)}，加 --force 覆盖）`);
      skip++;
      continue;
    }

    process.stdout.write(`${tag}  ...`);
    try {
      const r = await convertOne(pxt, tpl, src, outPath, opts);
      process.stdout.write(
        `\r${tag}  → ${path.basename(outPath)}  ${humanSize(r.size)}  ${(r.ms / 1000).toFixed(1)}s\n`
      );
      if (opts.verbose) {
        process.stdout.write(`      bpp=${r.bpp} 源文件 ${r.files} 个\n`);
      }
      for (const w of r.warn || []) process.stdout.write(`      注意: ${w}\n`);
      ok++;
    } catch (e) {
      process.stdout.write(`\r${tag}  失败\n`);
      console.error('      ' + String(e.message || e).replace(/\n/g, '\n      '));
      for (const w of e.warn || []) console.error('      注意: ' + w);
      failures.push({ file: src, error: String(e.message || e) });
      fail++;
    }
  }

  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  log('');
  log(`完成：成功 ${ok}，跳过 ${skip}，失败 ${fail}（用时 ${secs}s）`);
  if (fail > 0) {
    log('');
    log('失败清单：');
    for (const f of failures) log(`  ${path.basename(f.file)}: ${f.error.split('\n')[0]}`);
    return 1;
  }
  if (ok > 0 && !opts.quiet) log('\n把生成的 .pxt64 放进掌机，在 RetroArch 里加载即可。');
  return 0;
}

main()
  .then((code) => process.exit(code || 0))
  .catch((e) => {
    console.error('\n[异常] ' + (e && e.stack ? e.stack : e));
    process.exit(2);
  });
