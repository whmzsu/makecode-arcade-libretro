tool69 / 192.168.1.69 —— MakeCode Arcade libretro 核心的工具机归档说明
最后更新：2026-11-07

【定位】常驻 Linux 验证节点，不是发版机。Windows 本机只负责编辑与 git，
所有交叉编译、arm64 行为回归、干净发行版构建都在这里跑。
这是共用桌面机（3 个会话、Samba/Ceph）：不改服务、不重启、编译一律 nice + -j2，
不要跑满 8 线程，也不要在这里做性能基准（qemu 下的 fps 没有参考价值，
吞吐只认 cubexx / rgb30c 真机）。

【目录】
  /opt/mca-tools/          zig 0.17.0（清华 PyPI wheel）、env.sh、Linux 版 NDK r27c
                            source /opt/mca-tools/env.sh 可拿到 PATH
  /opt/mca-data/tools/     aarch64-linux-musl-cross（musl.cc GCC 11.2.1，323MB）
                            Lakka 全静态核心用；env.sh 已把它加进 PATH
  /opt/mca-data/           /dev/nvme0n1p7 数据分区 153G：scratch / artifacts / vm / tools
                            （新增大文件、工具链、镜像一律放这里，别塞根分区）
  /opt/mca-work/repo/      全量源码（从 Windows git ls-files 同步，含 core/libretro.h）
  /opt/mca-work/mca/       冒烟素材：21 张卡带、真机核心 0.3.3、libretro_host.py
  /opt/mca-work/mca/carts/ 21 张 .pxt64 语料（第三方游戏内容，不要提交进仓库）
  /opt/mca-work/arm64-root/ Debian bookworm arm64 chroot（qemu-user-static + binfmt）
  /opt/mca-work/amd64-root/ Debian bookworm amd64 干净 rootfs（只装 g++/make/ca-certificates）
  /opt/mca-work/bin/       本目录的运维脚本（见下）
  /opt/mca-work/reports/   所有回归 / 矩阵 / 体检 / 每日报告
  /opt/mca-work/vm/        完整 arm64 虚拟机镜像 + vm_boot_check.sh（TCG 软件模拟，
                            x86 宿主的 /dev/kvm 无法加速异构客户机）

【脚本】
  regress_arm64.sh [帧数]  21 张语料 × 2 个核心（真机 0.3.3 vs 本地 zig 产物）
                            在 arm64 chroot 里逐张比对 FINAL-FRAME-HASH，
                            compare_regress.py 产出 report.md。
  ci_matrix.sh             A zig 发版矩阵 5 目标 + B 发行版工具链 CI 构型
                            （gcc x64/arm64、MinGW x64、ndk-build arm64）
                            + B2 Lakka musl 全静态（musl.cc 工具链，替代 CI 的
                            dockcross 镜像——本机 Docker Hub 不通）
                            + C package_release.sh 打 zip + verify_release.py 逐包体检（6 包）。
  verify_release.py        发行包「装一遍再发」：按扩展名挑核心、查 ELF/PE 机器类型、
                            .info 版本 == kCoreVersion、NEEDED 白名单、glibc 基线、
                            retro_* 导出、Android 16KB 对齐、包里不得有 .pdb。
  toolhost_daily.sh        cron 每日 03:20：arm64 全量回归 + 两台在线掌机只读巡检 + 报告。
  compare_regress.py       回归结果比对（被 regress_arm64.sh 调用）。

【关键结论（2026-11-07）】
  * 21 张语料 × 600 帧：真机 0.3.3 与工具机 zig 产物逐张末帧哈希全一致；
    ping f93e4597… / arcade-cube-land 23210fb3… 与历史真机基准对齐；
    唯一异常是已知的 Strawberry_Slam panic（游戏自身 bug）。
  * x86_64（gcc 干净版 / zig 版）与 arm64 三路哈希同为 f93e4597… —— 跨架构语义等价。
  * 干净 Debian bookworm（g++ 12）编出的 .so 依赖 GLIBC_2.34；发版 zig 包钉在 2.17。
    两套 CI 的分工是必要的，别合并。
  * 修复过两个真 bug（详见 docs 或当日工作日志）：
    1) set_controller_info 的 retro_controller_info 数组缺 NULL 终止哨兵，
       gcc 构型下前端/宿主越界读 -> SIGSEGV；
    2) libretro_host.py 把 retro_log_printf_t 的第一个变参声明成 c_char_p，
       ctypes 无条件 strlen 垃圾指针 -> SIGSEGV。

【Lakka / musl 全静态（2026-11-08 打通）】
  * 本机 Docker Hub 不通，改用 musl.cc 官方 aarch64-linux-musl-cross（GCC 11.2.1），
    tools/build_musl.sh 自动识别 triplet，7 秒编完，产物零 NEEDED、导出 retro_* 25 个。
  * musl.cc 单连接仅 12~28KB/s；108MB 分 40 片、20 并发 ranged 下载约 7 分钟
    （脚本 /tmp/pdl.sh 思路：curl -r 分段 + xargs -P）。以后重下照此办理。
  * 功能等价已验证：同一 .so 在 arm64 chroot（glibc Py3.11）里 dlopen 加载，
    ping 120 帧 f93e4597…，21 卡 × 600 帧逐张哈希与 zig glibc 版 21/21 全一致。
    **仍未在真实 Lakka 设备上跑过**（无该设备），这是唯一缺口。

【常用命令】
  ssh tool69                                  # 别名 tool69 / mcatool
  cd /opt/mca-work/repo && ZIG=zig ANDROID_NDK_HOME=/opt/mca-tools/ndk \
      ./tools/build_target.sh aarch64-linux-gnu.2.17
  chroot /opt/mca-work/arm64-root /usr/bin/python3 /root/mca/tools/libretro_host.py \
      /root/mca/core-stage033.so /root/mca/carts/ping.pxt64 120 --turbo \
      --data-dir=/tmp/mca_data                 # 必须带 --data-dir
  /opt/mca-work/bin/regress_arm64.sh 600      # 全量回归
  /opt/mca-work/bin/ci_matrix.sh              # 全矩阵 + 发行包体检

【同步源码（Windows 侧）】
  git ls-files > list && tar czf src.tgz -T list && scp src.tgz tool69:/opt/mca-work/
  # 必须包含 core/libretro.h（漏了会报 ../libretro.h not found）。
  # 微信/网络：github.com、raw、jsdelivr、Docker Hub 不通；
  # api.github.com、pypi(清华)、npmmirror、apt(清华)、dl.google.com、cloud.debian.org 通。
