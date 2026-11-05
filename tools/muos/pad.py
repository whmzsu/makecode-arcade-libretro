"""通过 uinput 创建虚拟 Xbox360 手柄并发送按键，用于驱动 muOS 前端 UI。

用法：python3 pad.py UP DOWN A B ...
每个参数按一次（按下 120ms，松开后等 350ms）。
"""

import ctypes
import fcntl
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
UI_DEV_SETUP = 0x405C5503
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_ABS_SETUP = 0x401C5504

ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ = 0, 1, 2, 3, 4, 5
ABS_HAT0X, ABS_HAT0Y = 16, 17

KEYS = {
    "A": 0x130, "B": 0x131, "X": 0x133, "Y": 0x134,
    "L1": 0x136, "R1": 0x137,
    "SELECT": 0x13A, "START": 0x13B, "MODE": 0x13C,
}
HAT = {
    "UP": (ABS_HAT0Y, -1),
    "DOWN": (ABS_HAT0Y, 1),
    "LEFT": (ABS_HAT0X, -1),
    "RIGHT": (ABS_HAT0X, 1),
}


class InputAbsInfo(ctypes.Structure):
    _fields_ = [("value", ctypes.c_int32), ("minimum", ctypes.c_int32),
                ("maximum", ctypes.c_int32), ("fuzz", ctypes.c_int32),
                ("flat", ctypes.c_int32), ("resolution", ctypes.c_int32)]


class AbsSetup(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16), ("pad", ctypes.c_uint16),
                ("absinfo", InputAbsInfo)]


class UinputSetup(ctypes.Structure):
    _fields_ = [("bustype", ctypes.c_uint16), ("vendor", ctypes.c_uint16),
                ("product", ctypes.c_uint16), ("version", ctypes.c_uint16),
                ("name", ctypes.c_char * 80), ("ff_effects_max", ctypes.c_uint32)]


def main():
    seq = sys.argv[1:]
    fd = open("/dev/uinput", "wb", buffering=0)

    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    for code in KEYS.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for code in (ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)

    setup = UinputSetup(3, 0x045E, 0x028E, 0x0110, b"Microsoft X-Box 360 pad", 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, bytes(setup))

    for code, lo, hi in ((ABS_X, 0, 255), (ABS_Y, 0, 255), (ABS_Z, 0, 255),
                         (ABS_RX, 0, 255), (ABS_RY, 0, 255), (ABS_RZ, 0, 255),
                         (ABS_HAT0X, -1, 1), (ABS_HAT0Y, -1, 1)):
        fcntl.ioctl(fd, UI_ABS_SETUP, bytes(AbsSetup(code, 0, InputAbsInfo(0, lo, hi, 0, 0, 0))))

    fcntl.ioctl(fd, UI_DEV_CREATE, 0)
    time.sleep(1.5)

    def ev(etype, code, value):
        fd.write(struct.pack("@llHHi", 0, 0, etype, code, value))

    for step in seq:
        name = step.upper()
        if name in KEYS:
            ev(EV_KEY, KEYS[name], 1)
            ev(EV_SYN, 0, 0)
            time.sleep(0.12)
            ev(EV_KEY, KEYS[name], 0)
            ev(EV_SYN, 0, 0)
        elif name in HAT:
            code, val = HAT[name]
            ev(EV_ABS, code, val)
            ev(EV_SYN, 0, 0)
            time.sleep(0.12)
            ev(EV_ABS, code, 0)
            ev(EV_SYN, 0, 0)
        else:
            print("unknown:", step)
        time.sleep(0.35)

    time.sleep(0.4)
    fcntl.ioctl(fd, UI_DEV_DESTROY, 0)
    fd.close()
    print("sent:", " ".join(seq))


if __name__ == "__main__":
    main()
