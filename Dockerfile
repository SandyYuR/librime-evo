FROM debian:12.15-slim

RUN DEBIAN_FRONTEND=noninteractive apt-get update \
  && apt-get install -y --no-install-recommends \
  ca-certificates \
  git \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libboost-dev \
  libboost-regex-dev \
  libboost-locale-dev \
  libgtest-dev \
  libyaml-cpp-dev \
  libleveldb-dev \
  libmarisa-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /librime
COPY . .

# Git metadata is excluded from the image; pass the release label separately.
ARG LUAJIT_RELVER=ROLLING
RUN printf '%s\n' "$LUAJIT_RELVER" > plugins/lua/thirdparty/luajit/.relver

RUN bash install-plugins.sh \
  rime/librime-charcode \
  lotem/librime-octagram \
  rime/librime-predict

RUN cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE:STRING=Release \
  -DENABLE_LOGGING:BOOL=ON \
  -DBUILD_TEST:BOOL=ON \
  -DBUILD_STATIC:BOOL=OFF \
  -DBUILD_SHARED_LIBS:BOOL=ON
RUN cmake --build build -j2

RUN ctest --test-dir build --output-on-failure
