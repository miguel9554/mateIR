FROM custom-hdl-slang:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    curl \
    verilator \
    perl \
    flex \
    bison \
    gdb \
    graphviz \
    python3 \
    python3-pip \
    python3-venv \
    python-is-python3 \
    && rm -rf /var/lib/apt/lists/*

ENV POETRY_HOME=/usr/local
ENV POETRY_CACHE_DIR=/opt/pypoetry-cache
ENV POETRY_VIRTUALENVS_IN_PROJECT=false
RUN curl -sSL https://install.python-poetry.org | python3 -

ENV CC=clang-18
ENV CXX=clang++-18

WORKDIR /workspace

# Pre-install Python packages.
COPY pyproject.toml poetry.lock ./
RUN poetry install --no-root && \
    chmod -R a+rX /opt/pypoetry-cache

RUN git config --global --add safe.directory /workspace

CMD ["bash"]
