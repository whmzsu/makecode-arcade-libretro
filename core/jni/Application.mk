# ndk-build 配置（官方 buildbot 的 android-jni 模板要求）
#
# - c++_static：libc++ 静态链进 core，RetroArch 无需额外提供 libc++_shared.so
#   （不同 App 的 STL 版本差异会导致符号不匹配，官方其它 core 也是这么做的）
# - android-21：RetroArch Android 支持的最低 API（NDK r27 的下限即 21）
# - APP_ABI 默认 arm64-v8a（本地 ndk-build 的口径）；buildbot 的
#   android-jni 模板按 job 传 APP_ABI=armeabi-v7a / arm64-v8a，命令行变量
#   覆盖此处的默认值，两个 ABI 都已进 GitHub 发版矩阵与收录矩阵。
#   32 位走 A 方案 GC 旁路表，见 docs/PXT32-A-SCHEME.md。
APP_PLATFORM := android-21
APP_STL      := c++_static
APP_OPTIM    := release
APP_ABI      := arm64-v8a
