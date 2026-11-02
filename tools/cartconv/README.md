# cartconv — 卡带转换器

把 MakeCode Arcade 保存的 `.png` 卡带转换成 `.pxt64`，供 libretro core 直接运行。

分发给玩家的是**免安装便携包**：解压后双击/拖拽即可，不需要装 Node.js、npm 或 Python。

## 为什么需要转换

`.png` 卡带里只有**项目源码**（`meta` + `source`，我用 4 张真实卡带验证过），
没有编译产物。要变成 core 能执行的 VM 字节码，必须现场过一遍 pxt 编译器
（`pxt build --hwvariant vm`）。这就是这个工具的重量来源——绕不开 node + pxt。

## 支持范围：别指望 100%

**已验证的只有 4 张真实卡带**（`png_examples/`）：ping、jumpy、falling duck、
cube land。覆盖 bpp=1/2、blocks/TS 两种编辑器、含中文源码、依赖 animation 与
color-coded-tilemap 的工程。

除此之外的卡带没有样本，也就没有保证。已知转不了的情况分四类，工具会主动识别
并说明原因：

| 情况 | 工具表现 | 说明 |
|---|---|---|
| 依赖社区扩展 `gh:user/repo` | 编译前拦下 | 需要联网下载，离线包做不到 |
| 本地没有的扩展名 | `install` 后核对，报出缺哪个 | 官方扩展离线可用，第三方不行 |
| 源码含二进制资源 | 编译前拦下 | 见下 |
| 卡带比编译器新 | 警告但继续编译 | 本地 pxt-arcade 4.2.1 |

### 二进制资源为什么转不了

新版 MakeCode（2023 之后的 assets 系统）会把用户上传的图片、音效作为独立文件
存进工程。这类文件按当前实现会用 `utf8` 写出，二进制内容被破坏，编译出的结果是
错的——而且**不会有任何报错**。所以宁可拦下也不能放行。

真遇到这种卡带，让作者在 MakeCode 里用 libretro 导出的 `.pxt64`，或者把工程分享
链接给出来用网页版编译。

### 版本兼容的方向性

4 张卡带里的 `meta.targetVersions` 都是 `target 1.3.44 / pxt 6.8.33`，
本地编译器是 `pxt-arcade 4.2.1 / pxt-core 13.2.1`——**新编译器跑旧卡带**成立。
反过来不成立：如果卡带用了 4.2.1 之后新增的接口，编译会失败，这时预检会给出
版本警告作为线索。

## 目录结构

```
tools/cartconv/
  cartconv.js          主程序（参数解析、批量、进度、调用编译器）
  cartdec.js           .png 卡带解码（PNG → 位流 → LZMA → 项目 JSON）
  cartenc.js           卡带编码（仅测试用，不进包）
  precheck.js          编译前预检（二进制资源 / 社区扩展 / 版本）
  launcher.c           Windows 拖拽入口（zig cc 编译成 cartconv.exe）
  fakebin/docker.cmd   docker 假程序（pxt 编译流程会尝试调 docker）
  keep_modules.txt     依赖白名单（实测生成，见下）
  prune_modules.py     按白名单裁剪**已有**目录（打包已改用正向复制，此脚本留作校验）
  package_cartconv.sh  打便携包
  package.json         版本号（打包脚本读取）
```

## 测试样本怎么造

`cartenc.js` 是 `cartdec.js` 的反向实现：工程对象 → `.png` 卡带。位流算法逐行
照搬 pxt 的 `Util.encodeBlobAsync`，只差 PNG 容器用 zlib 写。

它存在的意义是造故障样本——真实卡带覆盖不到那些失败路径：

```bash
# 解码 → 改一处 → 重新编码，闭环验证
node -e "
const {decodeCart}=require('./cartdec.js'), {encodeCart}=require('./cartenc.js');
(async()=>{
  const d=await decodeCart('../../png_examples/arcade-ping.png');
  const p=Object.assign({}, d.obj, {source: JSON.stringify(d.sources)});
  require('fs').writeFileSync('/tmp/rt.png', encodeCart(p,{width:512,height:424}).png);
})()"
```

已造过并验证过拦截效果的样本：二进制资源、`gh:` 扩展、本地缺扩展、TS 语法错误、
版本偏新、原样往返。


## 转换器本身也用 node 写

包里反正要带 node 运行时，所以主程序直接用 node，不再引入 Python 依赖——
玩家机器上大概率没有 Python。

卡带解码的那部分（`cartdec.js`）与 `tools/extract_png_meta.py` **逐字节等价**，
对 `png_examples/` 的 4 张真实卡带（含含中文的 falling-duck、bpp=2 的 jumpy）
输出完全一致。

## 性能

| 阶段 | 耗时 |
|---|---|
| 解码卡带 | < 0.1s |
| 复制模板 `pxt_modules` | ~1.3s |
| `pxt install`（补差量依赖） | 1~3s |
| `pxt build --hwvariant vm` | ~5s |
| **单张合计** | **约 10s** |

旧脚本 `png2pxt64.py` 每张要 29 秒（每次都完整 install）。现在的做法是
打包时预生成一份"模板工程"缓存（`pxt_modules` 4MB + `built/dockercross` 1MB），
每次转换复制一份再补差量。

## 依赖白名单怎么来的

`node_modules` 有 577 个包、405MB，但实际编译只用得到 60 个。
白名单不是猜的，是**实测**的：

```js
// 用 NODE_OPTIONS 劫持 Module._load，记录所有被 require 的模块
Module._load = function (request) { seen.add(request); return orig.apply(this, arguments); };
```

跑一次真实转换（用最复杂的 jumpy 卡带），得到 233 条模块请求、解析出 58 个包，
再加上 pxt-arcade / pxt-common-packages 两个"按路径读取、不 require"的目标包，
共 60 个。打包时**只复制这 60 个顶层包**，`node_modules` 从 405MB 降到 209MB，
压缩后 **85MB**。

白名单只管**顶层**；每个包内部的嵌套依赖（如 `pxt-common-packages/node_modules`）
整包复制，因为它们走各自的私有路径解析，不在顶层找。

**换 pxt 版本后要重新生成白名单**，否则可能漏包。重跑方法：

```bash
cat > trace_req.js <<'EOF'
const M = require('module'), fs = require('fs'), orig = M._load, seen = new Set();
M._load = function (r) { seen.add(r); return orig.apply(this, arguments); };
process.on('exit', () => fs.writeFileSync(process.env.TRACE_OUT + '.' + process.pid, [...seen].sort().join('\n')));
EOF
NODE_OPTIONS="--require $PWD/trace_req.js" TRACE_OUT=/tmp/req \
  node cartconv.js some-cart.png -o /tmp/out --force
# 再按包名解析 /tmp/req.* 即可
```

## 打包

```bash
tools/cartconv/package_cartconv.sh            # 完整打包并压缩
tools/cartconv/package_cartconv.sh --no-zip   # 只建目录（调试用）
```

产物：`dist/cartconv/makecode-cartconv-<ver>-windows-x64.zip`（约 85MB）

脚本会：
1. 用 zig cc 编译 `launcher.c`（需要 `../common tools/zig`）
2. 复制 `cartconv.js` / `cartdec.js` / `precheck.js` / `fakebin`
3. 复制 node 运行时（找 `NODE_EXE` 环境变量或本机 node）
4. 按白名单**正向复制** `node_modules`（只挑用到的 60 个包）
5. 预生成编译缓存（玩家首次使用就不用等 30 秒）
6. 生成 README 并压缩

旧目录不删除，而是改名成 `.prev-<时间戳>` 留作回滚——顺手避开"一次删上万
文件"的坑。确认新包没问题后可以手工清掉。

### 打包踩过的两个坑

- **白名单必须是 LF**。`keep_modules.txt` 如果用 Python 在 Windows 上生成，
  默认文本模式会把 `\n` 写成 `\r\n`，bash 读进来的包名尾部带 `\r`，
  `[ -d "$src" ]` 永远为假，结果 58 个包全部"不存在"。脚本里已加
  `${pkg%$'\r'}` 兜底。
- **临时目录别放 `/tmp`**。Git Bash 的 `mktemp -d` 返回 `/tmp/tmp.XXXX`，
  后续 `rm -rf` 会被路径校验判成 `embedded drive prefix is not allowed`。
  放在 `dist/` 下就没问题。

## 拖拽入口的几个坑（都踩过了）

- **控制台代码页**：必须 `SetConsoleOutputCP(65001)`，否则 node 输出的中文全乱码。
- **参数分隔**：拼命令行时每个参数前要加空格，否则
  `"a.exe""b.js""--x"` 会被 Windows 当成一个文件名，程序静默不启动。
- **暂停时机**：拖拽时窗口会随进程退出立刻关闭，必须 pause；
  但 cmd/管道里运行又不能 pause（否则批处理卡死）。
  判断依据是**父进程是不是 explorer.exe**，不能用 `GetConsoleProcessList`
  （Git Bash 下会误判成独占控制台）。
- **`-municode`**：mingw 下用 `wmain` 必须加，否则链接报 `undefined symbol: WinMain`。

## 与 png2pxt64.py 的关系

`tools/png2pxt64.py` 保留，是开发期的参考实现（Python + 直接调 pxt）。
两者产物**逐字节一致**，已用 ping 和 jumpy 验证。日常转换用 cartconv。
