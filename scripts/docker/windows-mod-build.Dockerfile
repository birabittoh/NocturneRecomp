# Cross-build environment for mods_src/* mods' Windows .dll from a
# non-Windows host, invoked from make_mods.py when the host can't build the
# "windows" target directly (i.e. isn't Windows itself, and doesn't already
# have clang-cl + an xwin sysroot on PATH). Uses clang-cl + lld-link against
# Microsoft's redistributable CRT/SDK, fetched via `xwin`, so the resulting
# .dll is MSVC-ABI compatible with the SDK's rex::runtime import library
# (built the same way scripts/build.py builds it: plain clang, MSVC target).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        wget gnupg ca-certificates python3 xz-utils lsb-release software-properties-common \
    && wget -qO- https://apt.llvm.org/llvm.sh | bash -s -- 20 \
    && apt-get install -y --no-install-recommends \
        ninja-build cmake clang-tools-20 lld-20 llvm-20 \
    && update-alternatives --install /usr/bin/clang-cl clang-cl /usr/bin/clang-cl-20 200 \
    && update-alternatives --install /usr/bin/lld-link lld-link /usr/bin/lld-link-20 200 \
    && update-alternatives --install /usr/bin/llvm-lib llvm-lib /usr/bin/llvm-lib-20 200 \
    && rm -rf /var/lib/apt/lists/*

# xwin fetches Microsoft's redistributable Windows SDK + CRT/STL headers and
# import libraries under their published redistribution terms, which
# `--accept-license` accepts on your behalf -- same mechanism cargo-xwin
# uses for Rust's MSVC cross target.
ARG XWIN_VERSION=0.6.5
RUN wget -qO /tmp/xwin.tar.gz \
        "https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
    && tar -xzf /tmp/xwin.tar.gz -C /tmp \
    && mv "/tmp/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl/xwin" /usr/local/bin/xwin \
    && rm -rf /tmp/xwin* \
    && xwin --accept-license splat --output /opt/xwin-cache

WORKDIR /workspace
