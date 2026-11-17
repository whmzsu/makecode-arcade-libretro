# ndk-build 配置（官方 buildbot 的 android-jni 模板要求）
#
# - c++_static：libc++ 静态链进 core，RetroArch 无需额外提供 libc++_shared.so
#   （不同 App 的 STL 版本差异会导致符号不匹配，官方其它 core 也是这么做的）
# - android-21：RetroArch Android 支持的最低 API（NDK r27 的下限即 21）
# - 只出 arm64-v8a（发版 ABI）。armeabi-v7a 在 GitHub CI 有构建回归兜底
#   （A 方案 GC 旁路表进 main 后 PXT64 布局在 32 位平台可用，见
#   docs/PXT32-A-SCHEME.md 与 README「32 位与 64 位」），待真机验收后再进发版。
APP_PLATFORM := android-21
APP_STL      := c++_static
APP_OPTIM    := release
APP_ABI      := arm64-v8a
