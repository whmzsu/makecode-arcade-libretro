tool69 / 192.168.1.69 —— MakeCode Arcade libretro 核心的工具机使用说明
最后更新：2026-11-22（目录硬切换，旧 mca-tools/mca-work/mca-data 已停用）
完整现场版：/opt/mca/README.md（本文件内容已并入）

【定位】常驻 Linux 验证节点，不是发版机。Windows 本机只负责编辑与 git，
所有交叉编译、arm64 行为回归、干净发行版构建都在这里跑。
这是共用桌面机（3 个会话、Samba/Ceph）：不改服务、不重启、编译一律 nice + -j2，
不要跑满 8 线程，也不要在这里做性能基准（qemu 下的 fps 没有参考价值，
吞吐只认 cubexx / rgb30c 真机）。

【铁律：路径只在 env.sh 定义一次】
  任何脚本或命令先 `source /opt/mca/env.sh`，之后只用变量，禁止写死 /opt/mca-* 路径。
  旧三目录（mca-tools / mca-work / mca-data，均在 /opt 下）已永久删除，无兼容软链。

【目录】（唯一项目根 /opt/mca，四层按变更速度分层）
  /opt/mca/env.sh          全部路径的唯一定义点（source 后拿到 PATH + 所有变量）
  /opt/mca/README.md           根目录唯一入口说明（完整使用与目录规则）
                               本文件已并入其中；现场以 /opt/mca/README.md 为准
  /opt/mca/src/            唯一源码树（git 主树，从 Windows git ls-files 同步）
  /opt/mca/toolchain/      稳定层：装好后基本不动
    bin/                   zig 0.17.0（清华 PyPI wheel）
    ndk -> android-ndk-r27c  Linux 版 NDK r27c
    android-sdk/           Android SDK
    venv/                  Python 3.12（zig wheel 本体）
    musl-cross/            aarch64-linux-musl-cross（musl.cc GCC 11.2.1，323MB，Lakka 用）
    relay/                 ctr/vita/nx 工具链 + RA 树 + fetch.sh
    sysroot-arm64/         Debian bookworm arm64 chroot（qemu-user-static + binfmt）
    sysroot-amd64/         Debian bookworm amd64 干净 rootfs（g++/make/ca-certificates）
    vm/                    完整 arm64 虚拟机镜像（TCG 软件模拟，异构客户机无法用 KVM）
  /opt/mca/work/           活跃层：全部可再生
    build/                 各目标构建工作区（ctr / vita / vm-measure / v0310 等）
    reports/               所有回归 / 矩阵 / 体检 / 每日报告
    fixtures/carts/        21 张 .pxt64 语料（第三方游戏内容，不要提交进仓库）
    scripts/               本目录的运维脚本（见下）
    tmp/                   一次性分析脚本
  /opt/mca/archive/        冷层：只进不出，带 MANIFEST.md（原路径→归档路径→日期→还原命令）
    <YYYYMMDD>/            按日期分卷，只 mv 不删

【脚本】（均在 $MCA_BIN，即 /opt/mca/work/scripts）
  regress_arm64.sh [帧数]  21 张语料 × 2 个核心（真机 0.3.3 vs 本地 zig 产物）
                            在 arm64 chroot 里逐张比对 FINAL-FRAME-HASH，
                            compare_regress.py 产出 report.md。
  ci_matrix.sh             A zig 发版矩阵 5 目标 + B 发行版工具链 CI 构型
                            （gcc x64/arm64、MinGW x64、ndk-build arm64）
                            + B2 Lakka musl 全静态（musl.cc 工具链，替代 CI 的
                            dockcross 镜像——本机 Docker Hub 不通）
                            + C package_release.sh 打 zip + verify_release.py 逐包体检。
  verify_release.py        发行包「装一遍再发」：按扩展名挑核心、查 ELF/PE 机器类型、
                            .info 版本 == kCoreVersion、NEEDED 白名单、glibc 基线、
                            retro_* 导出、Android 16KB 对齐、包里不得有 .pdb。
  toolhost_daily.sh        cron 每日 03:20：arm64 全量回归 + 两台在线掌机只读巡检 + 报告。
  compare_regress.py       回归结果比对（被 regress_arm64.sh 调用）。
  vm_boot_check.sh         完整 arm64 虚拟机启动检查。
  relay/fetch.sh           在 $MCA_RELAY 下逐 commit 增量拉取工具链 relay。

【关键结论（历史，仍有效）】
  * 21 张语料 × 600 帧：真机 0.3.3 与工具机 zig 产物逐张末帧哈希全一致；
    ping f93e4597… / arcade-cube-land 23210fb3… 与历史真机基准对齐；
    唯一异常是已知的 Strawberry_Slam panic（游戏自身 bug）。
  * x86_64（gcc 干净版 / zig 版）与 arm64 三路哈希同为 f93e4597… —— 跨架构语义等价。
  * 干净 Debian bookworm（g++ 12）编出的 .so 依赖 GLIBC_2.34；发版 zig 包钉在 2.17。
    两套 CI 的分工是必要的，别合并。
  * 修复过两个真 bug：set_controller_info 数组缺 NULL 终止哨兵；
    libretro_host.py 把变参声明成 c_char_p 导致 ctypes strlen 垃圾指针。

【Lakka / musl 全静态（2026-11-08 打通）】
  * 本机 Docker Hub 不通，改用 musl.cc 官方 aarch64-linux-musl-cross（GCC 11.2.1），
    build_musl.sh 自动识别 triplet，7 秒编完，产物零 NEEDED、导出 retro_* 25 个。
  * musl.cc 单连接仅 12~28KB/s；108MB 分 40 片、20 并发 ranged 下载约 7 分钟。
  * 功能等价已验证：arm64 chroot 里 dlopen 加载，21 卡 × 600 帧与 zig glibc 版全一致。
    **仍未在真实 Lakka 设备上跑过**（无该设备），这是唯一缺口。

【常用命令】
  ssh tool69                                  # 别名 tool69 / mcatool
  source /opt/mca/env.sh
  cd "$MCA_SRC" && ZIG=zig ANDROID_NDK_HOME="$MCA_NDK" \
      ./tools/build_target.sh aarch64-linux-gnu.2.17
  chroot "$MCA_SYSROOT_ARM64" /usr/bin/python3 /root/mca/tools/libretro_host.py \
      /root/mca/core-stage033.so /root/mca/carts/ping.pxt64 120 --turbo \
      --data-dir=/tmp/mca_data                 # 必须带 --data-dir
  "$MCA_BIN/regress_arm64.sh" 600            # 全量回归
  "$MCA_BIN/ci_matrix.sh"                    # 全矩阵 + 发行包体检

【同步源码（Windows 侧）】
  git ls-files > list && tar czf src.tgz -T list && scp src.tgz tool69:/opt/mca/
  # 到 tool69 后解到 /opt/mca/src/。必须包含 core/libretro.h（漏了报 not found）。
  # 网络：github.com、raw、jsdelivr、Docker Hub 不通；
  # api.github.com、pypi(清华)、npmmirror、apt(清华)、dl.google.com、cloud.debian.org 通。

【运维脚本同步约定（2026-11-22 补）】
  verify_release.py / check_pe.py 必须与 Windows 项目的 tools/ 保持同步：
    scp <win>/tools/verify_release.py <win>/tools/check_pe.py tool69:$MCA_BIN/
  verify_release.py 用 HERE/check_pe.py（脚本同目录），check_pe.py 缺失会导致
  windows-arm64 体检 FAIL（pefile 回退失败）。EXPECT 表以 Windows 版为准（12 平台）。
  src/tools/*.sh 从 git archive 解包后可能丢可执行位，构建前先 chmod +x。
