FROM mate-slang:latest

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
    clangd-18 \
    && rm -rf /var/lib/apt/lists/*

ENV VIRTUAL_ENV=/opt/compiler-venv
ENV PATH="${VIRTUAL_ENV}/bin:/usr/local/bin:${PATH}"
ENV POETRY_HOME=/usr/local
ENV POETRY_VIRTUALENVS_CREATE=false
RUN curl -sSL https://install.python-poetry.org | python3 -

ENV CC=clang-18
ENV CXX=clang++-18

RUN python3 -m venv "${VIRTUAL_ENV}"
WORKDIR /tmp/poetry-install
COPY pyproject.toml poetry.lock ./
RUN poetry install --no-root

RUN git config --global --add safe.directory /workspace

CMD ["bash"]
