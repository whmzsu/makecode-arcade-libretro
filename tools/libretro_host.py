#!/usr/bin/env python3
"""libretro_host.py — 最小 libretro 宿主（ctypes 动态加载 core）

为什么需要它：core_test.exe 是**静态链接**的假前端，它验证不了"前端用
dlopen 加载 .dll/.so 再逐符号调用"这条真实路径 —— 而真机上出问题的
恰恰是这条路径（缺导出符号、装载顺序、线程/栈环境差异）。

用法：
  python tools/libretro_host.py <core.dll|.so> <content> [帧数] [选项…]

选项：
  --bmp=路径            把最后一帧存成 BMP
  --data-dir=路径       存档 / mca_trace.log 的落盘目录（默认 out/libretro_host_saves）
  --opt=k=v             预设核心选项（本核心目前没有选项，保留给通用调试）
  --press=A             从第 5 帧起按住该物理键（A/B/X/Y/L/R/SELECT/START/UP/…）
  --press-at=N          改用第 N 帧按下（测 reset 用：--press=SELECT --press-at=60）
  --seq=A@120,B@160     按脚本依次按多个键（每个键按 20 帧），测"重置后是否还活着"
  --switch=PATH         跑完帧后 unload -> 加载第二张卡带 -> 再跑一半帧数（测换游戏）
  --dlclose             结束前 dlclose 核心库（模拟前端关闭游戏/退出，测卸载崩溃）
  --release=N           第 N 帧松开
  --bitmask             让 GET_INPUT_BITMASKS 返回 true（走位掩码轮询路径）
  --dry                 只加载、发环境、读描述符，不跑帧循环

行为尽量贴近 RetroArch：
  * 按 RetroArch 的顺序解析 25 个必需符号（缺一个就等价于真机加载失败）
  * 回 ENV_SET_VARIABLES / GET_VARIABLE / GET_INPUT_BITMASKS
  * 解析并打印 SET_INPUT_DESCRIPTORS / SET_CONTROLLER_INFO（按键可配置性的关键）
  * init → get_system_info → load_game → get_system_av_info → run 循环

退出码：0 正常；2 符号缺失；3 装载失败；4 帧循环中途异常
"""

import ctypes
import os
import struct
import sys
import time

# ── 存档 / trace 目录 ────────────────────────────────────────────────────
# core 会用 GET_SAVE_DIRECTORY 返回的目录写 settings、高分记录和 mca_trace.log。
# 默认落在 <项目>/out/ 下，**不要在项目根目录生成文件**；需要隔离时用
# --data-dir=PATH 覆盖（多个用例并行跑时各给一个目录）。
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(PROJECT_ROOT, "out", "libretro_host_saves")
# env 回调交出去的是裸指针，必须持有 bytes 引用，否则会被 GC 回收
DATA_DIR_B = DATA_DIR.encode("utf-8")

# ── libretro 常量 ────────────────────────────────────────────────────────
ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_SET_INPUT_DESCRIPTORS = 11
ENV_GET_VARIABLE = 15
ENV_SET_VARIABLES = 16
ENV_GET_VARIABLE_UPDATE = 17
ENV_SET_SUPPORT_NO_GAME = 18
ENV_GET_LOG_INTERFACE = 27
ENV_GET_SAVE_DIRECTORY = 31
ENV_SET_CONTROLLER_INFO = 35
ENV_GET_INPUT_BITMASKS = 51 | 0x10000        # | RETRO_ENVIRONMENT_EXPERIMENTAL

PIXEL_FORMAT_0RGB1555 = 0
PIXEL_FORMAT_XRGB8888 = 1
PIXEL_FORMAT_RGB565 = 2

LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG = 3, 4, 1, 2

# RetroPad 按键 id（libretro.h 的 RETRO_DEVICE_ID_JOYPAD_*）
JOYPAD = {
    "B": 0, "Y": 1, "SELECT": 2, "START": 3,
    "UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7,
    "A": 8, "X": 9, "L": 10, "R": 11, "L2": 12, "R2": 13,
    "L3": 14, "R3": 15,
}
JOYPAD_NAMES = {v: k for k, v in JOYPAD.items()}
ID_JOYPAD_MASK = 256

# RetroArch 的 runloop_init_libretro_symbols() 逐个解析这 25 个符号
REQUIRED = [
    "retro_init", "retro_deinit", "retro_api_version", "retro_get_system_info",
    "retro_get_system_av_info", "retro_set_environment", "retro_set_video_refresh",
    "retro_set_audio_sample", "retro_set_audio_sample_batch", "retro_set_input_poll",
    "retro_set_input_state", "retro_set_controller_port_device", "retro_reset",
    "retro_run", "retro_load_game", "retro_load_game_special", "retro_unload_game",
    "retro_get_region", "retro_serialize_size", "retro_serialize",
    "retro_unserialize", "retro_cheat_reset", "retro_cheat_set",
    "retro_get_memory_data", "retro_get_memory_size",
]


class retro_system_info(ctypes.Structure):
    _fields_ = [("library_name", ctypes.c_char_p),
                ("library_version", ctypes.c_char_p),
                ("valid_extensions", ctypes.c_char_p),
                ("need_fullpath", ctypes.c_bool),
                ("block_extract", ctypes.c_bool)]


class retro_game_geometry(ctypes.Structure):
    _fields_ = [("base_width", ctypes.c_uint), ("base_height", ctypes.c_uint),
                ("max_width", ctypes.c_uint), ("max_height", ctypes.c_uint),
                ("aspect_ratio", ctypes.c_float)]


class retro_system_timing(ctypes.Structure):
    _fields_ = [("fps", ctypes.c_double), ("sample_rate", ctypes.c_double)]


class retro_system_av_info(ctypes.Structure):
    _fields_ = [("geometry", retro_game_geometry), ("timing", retro_system_timing)]


class retro_game_info(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


class retro_input_descriptor(ctypes.Structure):
    _fields_ = [("port", ctypes.c_uint), ("device", ctypes.c_uint),
                ("index", ctypes.c_uint), ("id", ctypes.c_uint),
                ("description", ctypes.c_char_p)]


class retro_controller_description(ctypes.Structure):
    _fields_ = [("desc", ctypes.c_char_p), ("id", ctypes.c_uint)]


class retro_controller_info(ctypes.Structure):
    _fields_ = [("types", ctypes.POINTER(retro_controller_description)),
                ("num_types", ctypes.c_uint)]


class retro_variable(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("value", ctypes.c_char_p)]


ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint,
                            ctypes.c_uint, ctypes.c_size_t)
AUDIO_CB = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AUDIO_BATCH_CB = ctypes.CFUNCTYPE(ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t)
POLL_CB = ctypes.CFUNCTYPE(None)
STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint,
                            ctypes.c_uint, ctypes.c_uint)
# retro_log_printf_t 是真变参；Win64/x64 SysV 都按寄存器传前几个参数，
# 多声明一个 c_char_p 正好能读到第一个变参（我们的核心只会用 "%s" + 1 个串）。
LOG_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p)


class LogCallbackStruct(ctypes.Structure):
    _fields_ = [("log", LOG_CB)]


stats = {"video_frames": 0, "nonblack": 0, "w": 0, "h": 0, "audio_frames": 0,
         "audio_peak": 0, "audio_loud": 0, "audio_peak_win": 0,
         "last_frame": None, "descriptors": [], "port_types": [],
         "vars": {}, "vars_read": []}
_keepalive = []          # 回调对象/字符串必须保活，否则被 GC 后前端调用会崩
LOG_PREFIX = {LOG_ERROR: "[E]", LOG_WARN: "[W]", LOG_INFO: "[I]", LOG_DEBUG: "[D]"}

OPTIONS = {}             # 核心选项当前值（key -> value）
BITMASK = False          # GET_INPUT_BITMASKS 的回答
HELD = set()             # 当前按住的按键 id


def say(msg):
    print(msg, flush=True)


def fmt_text(fmt, arg1):
    """核心只会用 "%s" + 一个串，这里手工展开（真变参在 ctypes 里读不全）。"""
    f = (fmt or b"").decode("utf8", "replace")
    a = (arg1 or b"").decode("utf8", "replace")
    return f.replace("%s", a, 1) if "%s" in f else f


def env_cb(cmd, data):
    if cmd == ENV_GET_LOG_INTERFACE:
        cb = LogCallbackStruct()
        cb.log = LOG_CB(lambda level, fmt, arg1:
                        (say(f"  {LOG_PREFIX.get(level, '?')} [core] "
                             f"{fmt_text(fmt, arg1)}"), True)[1])
        _keepalive.append(cb)
        ctypes.cast(data, ctypes.POINTER(LogCallbackStruct))[0] = cb
        return True

    if cmd == ENV_SET_PIXEL_FORMAT:
        fmt = ctypes.cast(data, ctypes.POINTER(ctypes.c_int))[0]
        say(f"  [host] SET_PIXEL_FORMAT = {fmt} "
            f"({ {0: '0RGB1555', 1: 'XRGB8888', 2: 'RGB565'}.get(fmt, '?') })")
        return fmt in (PIXEL_FORMAT_0RGB1555, PIXEL_FORMAT_XRGB8888, PIXEL_FORMAT_RGB565)

    if cmd == ENV_SET_VARIABLES:
        arr = ctypes.cast(data, ctypes.POINTER(retro_variable))
        say("  [host] SET_VARIABLES（核心选项）:")
        i = 0
        while arr[i].key is not None and i < 64:
            key = arr[i].key.decode("utf8", "replace")
            spec = (arr[i].value or b"").decode("utf8", "replace")
            label, _, choices = spec.partition(";")
            vals = [x.strip() for x in choices.strip().split("|") if x.strip()]
            stats["vars"][key] = (label.strip(), vals)
            OPTIONS.setdefault(key, vals[0] if vals else "")
            say(f"           {key:<16} \"{label.strip()}\"  可选: {', '.join(vals)}"
                f"   当前: {OPTIONS[key]}")
            i += 1
        return True

    if cmd == ENV_GET_VARIABLE:
        pv = ctypes.cast(data, ctypes.POINTER(retro_variable))
        key = pv.contents.key.decode("utf8", "replace") if pv.contents.key else ""
        if key not in OPTIONS:
            return False
        buf = ctypes.create_string_buffer(OPTIONS[key].encode())
        _keepalive.append(buf)
        pv.contents.value = ctypes.cast(buf, ctypes.c_char_p)
        stats["vars_read"].append((key, OPTIONS[key]))
        return True

    if cmd == ENV_GET_VARIABLE_UPDATE:
        return False                     # 不给热更新（要测的话单独加）

    if cmd == ENV_GET_INPUT_BITMASKS:
        say(f"  [host] GET_INPUT_BITMASKS -> {BITMASK}")
        return BITMASK

    if cmd == ENV_SET_INPUT_DESCRIPTORS:
        arr = ctypes.cast(data, ctypes.POINTER(retro_input_descriptor))
        descs = []
        i = 0
        while i < 64 and arr[i].description is not None:
            d = arr[i]
            descs.append((d.port, d.device, d.index, d.id,
                          d.description.decode("utf8", "replace")))
            i += 1
        stats["descriptors"] = descs
        say(f"  [host] SET_INPUT_DESCRIPTORS：{len(descs)} 条（Controls 页会显示这些）")
        for port, dev, idx, id_, desc in descs:
            say(f"           port={port} id={id_:>3} {JOYPAD_NAMES.get(id_, '?'):<7} -> {desc}")
        return True

    if cmd == ENV_SET_CONTROLLER_INFO:
        arr = ctypes.cast(data, ctypes.POINTER(retro_controller_info))
        types = []
        port = 0
        while port < 8 and arr[port].types:
            names = [(arr[port].types[j].desc or b"").decode("utf8", "replace")
                     for j in range(arr[port].num_types)]
            types.append(names)
            say(f"  [host] SET_CONTROLLER_INFO port {port}: {', '.join(names)}")
            port += 1
        stats["port_types"] = types
        return True

    if cmd == ENV_SET_SUPPORT_NO_GAME:
        return True
    if cmd == ENV_GET_SAVE_DIRECTORY:
        ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = DATA_DIR_B
        return True
    if cmd == ENV_GET_SYSTEM_DIRECTORY:
        ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = DATA_DIR_B
        return True
    return False


def video_cb(data, width, height, pitch):
    if not data or not width or not height:
        return
    # 每 15 帧采样一次非黑像素比，记录峰值（首帧通常是黑的，不代表没画面）
    if stats["video_frames"] % 15 == 0:
        stats["w"], stats["h"] = width, height
        pixels = ctypes.cast(data, ctypes.POINTER(ctypes.c_uint16))
        n = width * height
        dark = sum(1 for i in range(n) if pixels[i] & 0xF800 == 0 and pixels[i] & 0x07E0 == 0
                   and pixels[i] & 0x001F == 0)
        stats["nonblack"] = max(stats["nonblack"], 100 - dark * 100 // n)
    stats["last_frame"] = (bytes(ctypes.string_at(data, width * height * 2)), width, height)
    stats["video_frames"] += 1


def write_bmp565(path, frame):
    """把 RGB565 帧存成 24 位 BMP（与 core_test.exe 的产物同格式，便于比对）。"""
    raw, w, h = frame
    row = (w * 3 + 3) & ~3
    pixels = bytearray(row * h)
    for y in range(h):
        base = (h - 1 - y) * row          # BMP 自下而上
        for x in range(w):
            v = raw[(y * w + x) * 2] | (raw[(y * w + x) * 2 + 1] << 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            pixels[base + x * 3 + 0] = (b * 255) // 31
            pixels[base + x * 3 + 1] = (g * 255) // 63
            pixels[base + x * 3 + 2] = (r * 255) // 31
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + bytes(pixels))
    say(f"   [host] 末帧已存: {path} ({w}x{h})")


def audio_batch_cb(buf, frames):
    stats["audio_frames"] += frames
    # 采样峰值：判断"重启后还有没有声音"的唯一客观依据（全零 = 静音 / dac 丢了）。
    # 逐点扫太慢，每 4 个点取一个，够用且不影响节拍。
    n = frames * 2
    peak = 0
    for i in range(0, n, 4):
        v = buf[i]
        if v < 0:
            v = -v
        if v > peak:
            peak = v
    if peak > stats["audio_peak"]:
        stats["audio_peak"] = peak
    if peak > stats["audio_peak_win"]:
        stats["audio_peak_win"] = peak
    if peak >= 256:
        stats["audio_loud"] += 1
    return frames


def audio_cb(left, right):
    return None


def input_poll_cb():
    return None


def input_state_cb(port, device, index, id_):
    if id_ == ID_JOYPAD_MASK:
        m = 0
        for b in HELD:
            m |= (1 << b)
        return ctypes.c_int16(m & 0xFFFF).value
    return 1 if id_ in HELD else 0


def parse_args(argv):
    """把 --opt k=v / --press=A 这类选项摘出来，返回剩余位置参数。"""
    global BITMASK
    pos = []
    cfg = {"bmp": None, "press": None, "press_at": 4, "release": None, "dry": False,
           "seq": None, "switch": None, "dlclose": False, "data_dir": None}
    for a in argv:
        if a.startswith("--bmp="):
            cfg["bmp"] = a.split("=", 1)[1]
        elif a.startswith("--data-dir="):
            cfg["data_dir"] = a.split("=", 1)[1]   # 存档/trace 目录（默认 out/libretro_host_saves）
        elif a.startswith("--opt=") or a.startswith("--opt"):
            kv = a.split("=", 1)[1] if "=" in a else ""
            if "=" in kv:
                k, v = kv.split("=", 1)
                OPTIONS[k.strip()] = v.strip()
        elif a.startswith("--press="):
            cfg["press"] = a.split("=", 1)[1].upper()
        elif a.startswith("--press-at="):
            cfg["press_at"] = a.split("=", 1)[1]   # 可为 "60" 或 "60,100,140"
        elif a.startswith("--seq="):
            cfg["seq"] = a.split("=", 1)[1]        # "A@120,SELECT@200"
        elif a.startswith("--release="):
            cfg["release"] = int(a.split("=", 1)[1])
        elif a == "--bitmask":
            BITMASK = True
        elif a.startswith("--switch="):
            cfg["switch"] = a.split("=", 1)[1]     # 换游戏：第二张卡带路径
        elif a == "--dlclose":
            cfg["dlclose"] = True                  # 模拟前端关闭内容后卸载核心库
        elif a == "--dry":
            cfg["dry"] = True
        else:
            pos.append(a)
    return pos, cfg


def main():
    pos, cfg = parse_args(sys.argv[1:])
    if len(pos) < 2:
        say(__doc__)
        return 2
    core_path, content_path = pos[0], pos[1]
    frames = int(pos[2]) if len(pos) > 2 else 180

    # 存档/trace 目录：默认 out/libretro_host_saves，避免污染项目根目录
    global DATA_DIR, DATA_DIR_B
    if cfg["data_dir"]:
        DATA_DIR = os.path.abspath(cfg["data_dir"])
        DATA_DIR_B = DATA_DIR.encode("utf-8")
    os.makedirs(DATA_DIR, exist_ok=True)
    say(f"== 存档/trace 目录：{DATA_DIR}")

    # --seq=KEY@frame,...：多键时序脚本，用于"重置后游戏是否还能响应输入"
    seq_events = {}
    if cfg["seq"]:
        for item in str(cfg["seq"]).split(","):
            item = item.strip()
            if not item:
                continue
            if "@" not in item:
                say(f"[FAIL] --seq 格式应为 KEY@frame，收到 {item!r}")
                return 2
            kname, kframe = item.split("@", 1)
            kname = kname.strip().upper()
            if kname not in JOYPAD:
                say(f"[FAIL] --seq 未知按键 {kname}")
                return 2
            kid = JOYPAD[kname]
            f = int(kframe)
            seq_events.setdefault(f, []).append((kid, True))
            seq_events.setdefault(f + 20, []).append((kid, False))
        say("== 输入脚本：" + ", ".join(
            f"frame {f} {'按' if d else '松'} {n}"
            for f in sorted(seq_events)
            for n, d in [(next((k for k, v in JOYPAD.items() if v == kid), "?"), down)
                         for kid, down in seq_events[f]]))

    press_id = None
    if cfg["press"]:
        if cfg["press"] not in JOYPAD:
            say(f"[FAIL] 未知按键 {cfg['press']}，可选: {', '.join(JOYPAD)}")
            return 2
        press_id = JOYPAD[cfg["press"]]
        say(f"== 输入：第 {cfg['press_at']} 帧按下 {cfg['press']}（id={press_id}，"
            f"每次保持 20 帧）"
            f"{'，第 %d 帧强制松开' % cfg['release'] if cfg['release'] else ''}")

    say(f"== 1. 加载 core: {core_path}")
    lib = ctypes.CDLL(core_path)

    say("== 2. 解析 25 个必需符号（等价于 RetroArch 的 dlsym 序列）")
    missing = []
    for name in REQUIRED:
        if not hasattr(lib, name):
            missing.append(name)
    if missing:
        say(f"   [FAIL] 缺 {len(missing)} 个: {', '.join(missing)}")
        return 2
    say(f"   OK（{len(REQUIRED)}/{len(REQUIRED)}）")

    lib.retro_set_environment.argtypes = [ENV_CB]
    lib.retro_set_video_refresh.argtypes = [VIDEO_CB]
    lib.retro_set_audio_sample.argtypes = [AUDIO_CB]
    lib.retro_set_audio_sample_batch.argtypes = [AUDIO_BATCH_CB]
    lib.retro_set_input_poll.argtypes = [POLL_CB]
    lib.retro_set_input_state.argtypes = [STATE_CB]
    lib.retro_get_system_info.argtypes = [ctypes.POINTER(retro_system_info)]
    lib.retro_get_system_av_info.argtypes = [ctypes.POINTER(retro_system_av_info)]
    lib.retro_load_game.argtypes = [ctypes.POINTER(retro_game_info)]
    lib.retro_load_game.restype = ctypes.c_bool
    lib.retro_api_version.restype = ctypes.c_uint
    lib.retro_set_controller_port_device.argtypes = [ctypes.c_uint, ctypes.c_uint]

    say("== 3. 前端回调注入")
    cbs = [ENV_CB(env_cb), VIDEO_CB(video_cb), AUDIO_CB(audio_cb),
           AUDIO_BATCH_CB(audio_batch_cb), POLL_CB(input_poll_cb),
           STATE_CB(input_state_cb)]
    _keepalive.extend(cbs)
    (env, video, audio, audio_batch, poll, state) = cbs
    lib.retro_set_environment(env)
    lib.retro_set_video_refresh(video)
    lib.retro_set_audio_sample(audio)
    lib.retro_set_audio_sample_batch(audio_batch)
    lib.retro_set_input_poll(poll)
    lib.retro_set_input_state(state)
    say(f"   [host] retro_api_version() = {lib.retro_api_version()}")

    say("== 4. retro_init + get_system_info")
    lib.retro_init()
    info = retro_system_info()
    lib.retro_get_system_info(ctypes.byref(info))
    say(f"   [host] {info.library_name.decode()} {info.library_version.decode()} "
        f"ext={info.valid_extensions.decode()} need_fullpath={info.need_fullpath}")

    say(f"== 5. 读内容: {content_path}")
    with open(content_path, "rb") as f:
        raw = f.read()
    buf = ctypes.create_string_buffer(raw, len(raw))   # 可写且 16 字节对齐
    say(f"   [host] {len(raw)} 字节")

    say("== 6. retro_set_controller_port_device(0, JOYPAD=1)")
    lib.retro_set_controller_port_device(0, 1)

    say("== 7. retro_load_game")
    gi = retro_game_info(path=content_path.encode(), data=ctypes.cast(buf, ctypes.c_void_p),
                         size=len(raw), meta=None)
    ok = lib.retro_load_game(ctypes.byref(gi))
    say(f"   [host] load_game -> {'OK' if ok else 'FAIL'}")
    if not ok:
        return 3

    say("== 8. retro_get_system_av_info")
    av = retro_system_av_info()
    lib.retro_get_system_av_info(ctypes.byref(av))
    say(f"   [host] {av.geometry.base_width}x{av.geometry.base_height} "
        f"fps={av.timing.fps:.2f} sr={av.timing.sample_rate:.0f}")

    # ---- 按键可配置性自检（不需要跑游戏）----
    say("== 9. 输入配置自检")
    problems = []
    if not stats["descriptors"]:
        problems.append("核心没有发 SET_INPUT_DESCRIPTORS（RetroArch 的 Controls 页会是空的）")
    if not stats["port_types"]:
        problems.append("核心没有发 SET_CONTROLLER_INFO（Controls 页的 Device Type 不可选）")
    ids = [d[3] for d in stats["descriptors"]]
    for need, name in [(0, "B"), (8, "A"), (4, "UP"), (5, "DOWN"), (6, "LEFT"), (7, "RIGHT"),
                       (2, "SELECT"), (3, "START")]:
        if need not in ids:
            problems.append(f"描述符缺 {name}（id={need}）")
    # 一个 RetroPad 键只能有一条描述：重复条目会让 Controls 页出现
    # 两条同名/同键的行（曾出现「两条 Menu、没有 Reset」）
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        names = ", ".join(JOYPAD_NAMES.get(i, str(i)) for i in sorted(dup))
        problems.append(f"描述符里同一个键出现多条：{names}")
    # 菜单与重置必须各恰好一条，且标签能区分（前端另有自己的 "Menu Toggle"）
    menus = [d for d in stats["descriptors"] if "Game Menu" in d[4]]
    resets = [d for d in stats["descriptors"] if "Reset" in d[4]]
    if len(menus) != 1:
        problems.append(f"「游戏内菜单」应恰好 1 条，实际 {len(menus)} 条")
    if len(resets) != 1:
        problems.append(f"「重置」应恰好 1 条，实际 {len(resets)} 条")
    # 本核心刻意不带核心选项：所有输入设置都在 Controls 页
    if stats["vars"]:
        names = ", ".join(stats["vars"])
        problems.append(f"核心不该再有核心选项，却发了：{names}")
    if problems:
        for p in problems:
            say(f"   [FAIL] {p}")
    else:
        say(f"   OK：{len(stats['descriptors'])} 条描述符 / "
            f"{len(stats['vars'])} 个选项 / 端口类型 {len(stats['port_types'])} 组")

    if cfg["dry"]:
        lib.retro_unload_game()
        lib.retro_deinit()
        say("== --dry：跳过帧循环")
        return 0 if not problems else 5

    def run_frames(n, tag):
        t_start = time.time()
        for i in range(n):
            if press_id is not None:
                pf = {int(x) for x in str(cfg["press_at"]).split(",")}
                if i in pf:
                    HELD.add(press_id)
                    say(f"   [host] frame {i}: 按下 {cfg['press']}")
                elif (i - 20) in pf:
                    HELD.discard(press_id)
                    say(f"   [host] frame {i}: 松开 {cfg['press']}")
            for kid, down in seq_events.get(i, []):
                if down:
                    HELD.add(kid)
                else:
                    HELD.discard(kid)
            if press_id is not None and cfg["release"] and i == cfg["release"]:
                HELD.discard(press_id)
                say(f"   [host] frame {i}: 松开 {cfg['press']}")
            lib.retro_run()
            if (i + 1) % 30 == 0:
                say(f"   [{tag}] frame {i + 1}: video={stats['video_frames']} "
                    f"audio_frames={stats['audio_frames']} "
                    f"audio_peak(近30帧)={stats['audio_peak_win']}")
                stats["audio_peak_win"] = 0
            time.sleep(0.016)
        return time.time() - t_start

    say(f"== 10. 帧循环（{frames} 帧，16ms 节拍）")
    try:
        dt = run_frames(frames, "game1")
    except Exception as exc:                             # noqa: BLE001
        say(f"   [FAIL] 帧循环异常 @ {stats['video_frames']} 帧: {exc}")
        return 4

    say(f"== 11. 统计：video={stats['video_frames']} 帧, "
        f"画面非黑峰值 {stats['nonblack']}%, audio={stats['audio_frames']} 采样帧, "
        f"音频峰值 {stats['audio_peak']}（有声采样帧 {stats['audio_loud']}）, "
        f"{frames / dt:.1f} fps 实际节拍")
    if cfg["bmp"] and stats["last_frame"]:
        write_bmp565(cfg["bmp"], stats["last_frame"])

    # ---- 12. 换游戏：unload -> load 下一张卡带（前端「加载内容」走的正是这条路）----
    # --switch=a,b,c 可以连着换多张（真机上玩家会反复切游戏，每次都是一次
    # unload+load；中间任何一次没把 VM 停干净，下一次就会踩到野状态）。
    if cfg["switch"]:
        for k, path2 in enumerate(str(cfg["switch"]).split(","), start=1):
            path2 = path2.strip()
            if not path2:
                continue
            say(f"== 12.{k} 换游戏：retro_unload_game -> 加载 {path2}")
            lib.retro_unload_game()
            raw2 = open(path2, "rb").read()
            buf2 = ctypes.create_string_buffer(raw2, len(raw2))
            _keepalive.append(buf2)
            gi2 = retro_game_info(path=path2.encode(),
                                  data=ctypes.cast(buf2, ctypes.c_void_p),
                                  size=len(raw2), meta=None)
            ok2 = lib.retro_load_game(ctypes.byref(gi2))
            say(f"   [host] load_game(第 {k} 张) -> {'OK' if ok2 else 'FAIL'}")
            if not ok2:
                return 6
            v0 = stats["video_frames"]
            try:
                run_frames(max(30, frames // 2), f"game{k + 1}")
            except Exception as exc:                     # noqa: BLE001
                say(f"   [FAIL] 换游戏后帧循环异常: {exc}")
                return 4
            got = stats["video_frames"] - v0
            say(f"   [host] 第 {k + 1} 张卡带产出 {got} 帧"
                f"{'（正常）' if got > 0 else '（[FAIL] 没有画面）'}")
            if got <= 0:
                return 7

    lib.retro_unload_game()
    lib.retro_deinit()
    say("== 完成 unload + deinit")

    # ---- 13. dlclose：模拟前端关闭游戏后卸载核心库 ----
    # 这是"关闭游戏导致 RetroArch 崩溃"的真实路径：核心库被 unmap 时若 VM 线程
    # 还在跑里面的代码，就是 SIGSEGV。正常情况下 unload 已经 join 过线程，安全。
    if cfg["dlclose"]:
        say("== 13. dlclose(核心库) — 若 VM 线程没停干净就会在这里崩")
        import _ctypes
        # POSIX 是 dlclose，Windows 是 FreeLibrary；两者等价于前端卸载核心库
        if hasattr(_ctypes, "dlclose"):
            _ctypes.dlclose(lib._handle)
        else:
            _ctypes.FreeLibrary(lib._handle)
        say("   [host] dlclose OK（随后立即退出，避免解释器重复释放）")
        sys.stdout.flush()
        os._exit(0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
