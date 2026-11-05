"""最小 libretro 生命周期测试：定位 RetroArch 退出段错误。

用法: python3 mini_lifecycle.py <core.so> [cart.pxt64]

阶段：
  1) dlopen + retro_set_environment + retro_init + retro_deinit + dlclose
  2) （给卡带时）再跑 load_game -> 1 帧 -> unload -> deinit -> dlclose
每一步打印，崩在哪一步一目了然。
"""

import ctypes
import os
import sys

ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AUDIO_CB = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t)
POLL_CB = ctypes.CFUNCTYPE(None)
STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)


class GameInfo(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


def say(msg):
    print("[mini] " + msg, flush=True)


def main():
    core = sys.argv[1]
    cart = sys.argv[2] if len(sys.argv) > 2 else None

    lib = ctypes.CDLL(core)
    say("dlopen OK")

    def env(cmd, data):
        # 只回 false：不提供任何服务，测核心是否在缺服务时安全
        return False

    envcb = ENV_CB(env)
    videocb = VIDEO_CB(lambda d, w, h, p: None)
    audiocb = AUDIO_CB(lambda d, f: f)
    pollcb = POLL_CB(lambda: None)
    statecb = STATE_CB(lambda p, d, i, id_: 0)

    lib.retro_set_environment(envcb)
    lib.retro_set_video_refresh(videocb)
    lib.retro_set_audio_sample_batch(audiocb)
    lib.retro_set_input_poll(pollcb)
    lib.retro_set_input_state(statecb)
    say("set_* OK")

    lib.retro_init()
    say("retro_init OK")
    lib.retro_deinit()
    say("retro_deinit OK")

    if cart:
        raw = open(cart, "rb").read()
        buf = ctypes.create_string_buffer(raw, len(raw))
        gi = GameInfo(cart.encode(), ctypes.cast(buf, ctypes.c_void_p), len(raw), None)
        ok = lib.retro_load_game(ctypes.byref(gi))
        say("retro_load_game = %s" % bool(ok))
        if ok:
            lib.retro_run()
            say("retro_run 1 frame OK")
            lib.retro_unload_game()
            say("retro_unload_game OK")
            lib.retro_deinit()
            say("retro_deinit(after unload) OK")

    say("reached end, releasing handle")
    # 显式 dlclose：若崩在这里，说明是核心库卸载路径
    import _ctypes
    handle = lib._handle
    del lib
    _ctypes.dlclose(handle)
    say("dlclose OK")


if __name__ == "__main__":
    main()
