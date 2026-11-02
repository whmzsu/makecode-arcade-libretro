# ndk-build 配置（官方 buildbot 的 android-jni 模板要求）
#
# - c++_static：libc++ 静态链进 core，RetroArch 无需额外提供 libc++_shared.so
#   （不同 App 的 STL 版本差异会导致符号不匹配，官方其它 core 也是这么做的）
# - android-21：RetroArch Android 支持的最低 API（NDK r27 的下限即 21）
# - 只列实际验证过的 ABI；模板会用 APP_ABI=... 覆盖单个 ABI 逐次构建
APP_PLATFORM := android-21
APP_STL      := c++_static
APP_OPTIM    := release
APP_ABI      := arm64-v8a armeabi-v7a
