# source this file for the MCA toolchain
# Single source of truth for all paths under /opt/mca.
# Hard cutover 2026-11-22: no legacy MCA_TOOLS/MCA_MUSL aliases, no compat symlinks.
# Repo backup: tools/tool69/env.sh; authoritative copy: /opt/mca/env.sh on tool69.
MCA_ROOT=/opt/mca
export MCA_ROOT
export MCA_SRC=$MCA_ROOT/src
export MCA_TOOLCHAIN=$MCA_ROOT/toolchain
export MCA_WORK=$MCA_ROOT/work
export MCA_ARCHIVE=$MCA_ROOT/archive

# derived paths (scripts reference these, never hardcode /opt/mca-*)
export MCA_BIN=$MCA_WORK/scripts
export MCA_REPORTS=$MCA_WORK/reports
export MCA_FIXTURES=$MCA_WORK/fixtures/carts
export MCA_NDK=$MCA_TOOLCHAIN/ndk
export MCA_MUSL_ROOT=$MCA_TOOLCHAIN/musl-cross
export MCA_SYSROOT_ARM64=$MCA_TOOLCHAIN/sysroot-arm64
export MCA_SYSROOT_AMD64=$MCA_TOOLCHAIN/sysroot-amd64
export MCA_VM=$MCA_TOOLCHAIN/vm
export MCA_RELAY=$MCA_TOOLCHAIN/relay

# release layer: official artifacts (local built vs downloaded from GitHub CI)
export MCA_RELEASE=$MCA_WORK/release
export MCA_RELEASE_LOCAL=$MCA_RELEASE/local
export MCA_RELEASE_CI=$MCA_RELEASE/ci

# build layer: workspace + intermediate/test versions only
export MCA_BUILD=$MCA_WORK/build
export MCA_BUILD_WORKSPACE=$MCA_BUILD/workspace
export MCA_BUILD_TEST=$MCA_BUILD/test

# version retention policy (prune_versions.sh)
export MCA_KEEP_RELEASE=5
export MCA_KEEP_TEST=3

# frontend bundle build parallelism (build_frontend_bundles.sh)
# 8 logical cores on host: use 6, leave 2 for system/sessions; override per-run if needed.
export MCA_BUILD_JOBS=${MCA_BUILD_JOBS:-6}

export PATH="$MCA_MUSL_ROOT/bin:$MCA_TOOLCHAIN/bin:$PATH"

# Android SDK / JDK
export ANDROID_HOME=$MCA_TOOLCHAIN/android-sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export PATH=$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$JAVA_HOME/bin:$PATH

export ZIG=$MCA_TOOLCHAIN/bin/zig
