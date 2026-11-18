# Add `makecode_arcade` to retroarch_cores.txt

This one-line PR makes dArkOS install the **MakeCode Arcade** libretro core during image build / core installation. It goes together with christianhaitian/retroarch-cores#12, and I'd like to explain how the two pieces fit together:

## How the two repos relate

- **`christianhaitian/retroarch-cores`** is the *distribution channel*: it stores the actual core binaries (`aarch64/<name>_libretro.so.zip` + the `.index` / `.index-extended` lists). PR christianhaitian/retroarch-cores#12 adds `makecode_arcade` there, following the existing `addcore` convention.
- **`retroarch_cores.txt` in this repo** is the *install list*: the comment on line 1 says cores are installed *from* the retroarch-cores repo, and this file decides **which** of them dArkOS actually installs.

So the logic is:

```
retroarch-cores repo (where binaries live)   ←  PR #12
        ↓  (download)
dArkOS retroarch_cores.txt (what gets installed)  ←  this PR
```

Neither file alone is enough: without #12 there is no binary to download; without this PR the binary sits in the distribution repo but is never installed by dArkOS. Merging both gives a working core out of the box.

## About the core

- **makecode_arcade** runs game ROMs built with [Microsoft MakeCode Arcade](https://arcade.makecode.com) (`.pxt64` cart format), rendered through RetroArch like any other libretro core. RGB565, 160x120 logical resolution.
- **64-bit only**: there is no 32-bit build (armhf is upstream TODO), so it only lands on aarch64 devices.
- Open source (MIT): https://github.com/whmzsu/makecode-arcade-libretro

## The change

One line, `makecode_arcade`, inserted in alphabetical order between `lowresnx` and `mame`:

```diff
 lowresnx
+makecode_arcade
 mame
```

Thanks for maintaining dArkOS!
