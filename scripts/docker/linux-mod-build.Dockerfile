# Cross-build environment for mods_src/* mods' Linux .so, invoked from
# make_mods.py on a non-Linux host. A mod only links rex::runtime (a
# prebuilt shared lib inside the SDK) via headers already bundled under
# sdk/include, so unlike the actual runtime build (see
# .github/workflows/_build.yml) this doesn't need GTK/audio/etc. dev
# packages -- just enough to invoke clang++/cmake/ninja.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        wget gnupg ca-certificates lsb-release software-properties-common python3 \
    && wget -qO- https://apt.llvm.org/llvm.sh | bash -s -- 20 \
    && apt-get install -y --no-install-recommends ninja-build cmake \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 200 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 200 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
