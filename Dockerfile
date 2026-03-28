# Use Ubuntu 24.04 as base
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    ninja-build \
    curl \
    software-properties-common \
    clang-18 \
    libc++-18-dev \
    libc++abi-18-dev \
    verilator \
    perl \
    flex \
    bison \
    gdb \
    graphviz \
    && rm -rf /var/lib/apt/lists/*

# slang requires fmt >= 12.1 (see external/slang/external/CMakeLists.txt).
# Ubuntu 24.04 ships 10.x. Built with -fPIC so pyslang can link it into its .so.
RUN git clone --depth 1 --branch 12.1.0 https://github.com/fmtlib/fmt.git /tmp/fmt-src && \
    cmake -B /tmp/fmt-build -S /tmp/fmt-src \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DFMT_TEST=OFF -DFMT_DOC=OFF && \
    cmake --build /tmp/fmt-build -j$(nproc) && \
    cmake --install /tmp/fmt-build && \
    rm -rf /tmp/fmt-src /tmp/fmt-build

# Python 3.14 (Ubuntu ships 3.12; pyproject.toml requires >=3.14)
RUN add-apt-repository ppa:deadsnakes/ppa && \
    apt-get update && apt-get install -y \
    python3.14 \
    python3.14-dev \
    python3.14-venv \
    && rm -rf /var/lib/apt/lists/*

RUN python3.14 -m ensurepip --upgrade && \
    python3.14 -m pip install --upgrade pip

RUN curl -sSL https://install.python-poetry.org | python3.14 -
ENV PATH="/root/.local/bin:$PATH"

# Store the venv outside /workspace so the bind-mount doesn't hide it at runtime
RUN poetry config virtualenvs.in-project false

ENV CC=clang-18
ENV CXX=clang++-18

WORKDIR /workspace

# Build slang from the submodule source (uses system fmt, so no network needed here).
# Installs into /opt/slang — outside /workspace, never shadowed by the bind-mount.
COPY external/slang external/slang
COPY scripts/build_slang.sh scripts/build_slang.sh
RUN SLANG_INSTALL_PREFIX=/opt/slang bash scripts/build_slang.sh && rm -rf build/ external/slang

# Pre-install Python packages. pyslang has no cp314 wheel and must be compiled
# from source. CC/CXX are cleared because clang-18 fails cmake's FindThreads check.
COPY pyproject.toml poetry.lock ./
RUN poetry env use python3.14 && CC="" CXX="" poetry install --no-root

RUN git config --global --add safe.directory /workspace

CMD ["bash"]
