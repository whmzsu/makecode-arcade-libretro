# ndk-build 配置（官方 buildbot 的 android-jni 模板要求）
#
# - c++_static：libc++ 静态链进 core，RetroArch 无需额外提供 libc++_shared.so
#   （不同 App 的 STL 版本差异会导致符号不匹配，官方其它 core 也是这么做的）
# - android-21：RetroArch Android 支持的最低 API（NDK r27 的下限即 21）
# - 只出 arm64-v8a：32 位 ABI 已整体移除。PXT64 镜像在 32 位 VM 下跑不了任何
#   游戏（官方编译器没有 32 位镜像输出，runtime 的 PXT32 补丁是上游 TODO），
#   见 README「只支持 64 位前端」。
APP_PLATFORM := android-21
APP_STL      := c++_static
APP_OPTIM    := release
APP_ABI      := arm64-v8a
