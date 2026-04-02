FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    ninja-build \
    clang-18 \
    libc++-18-dev \
    libc++abi-18-dev \
    && rm -rf /var/lib/apt/lists/*

# slang requires fmt >= 12.1 (Ubuntu 24.04 ships 10.x).
# Built with -fPIC so pyslang can link it into its .so.
RUN git clone --depth 1 --branch 12.1.0 https://github.com/fmtlib/fmt.git /tmp/fmt-src && \
    cmake -B /tmp/fmt-build -S /tmp/fmt-src \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DFMT_TEST=OFF -DFMT_DOC=OFF && \
    cmake --build /tmp/fmt-build -j$(nproc) && \
    cmake --install /tmp/fmt-build && \
    rm -rf /tmp/fmt-src /tmp/fmt-build && \
    echo '/usr/local/lib' > /etc/ld.so.conf.d/local.conf && \
    ldconfig

ENV CC=clang-18
ENV CXX=clang++-18

COPY external/slang external/slang
COPY scripts/build_slang.sh scripts/build_slang.sh
RUN SLANG_INSTALL_PREFIX=/opt/slang bash scripts/build_slang.sh && rm -rf build/ external/slang
