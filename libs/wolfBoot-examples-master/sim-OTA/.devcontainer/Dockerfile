FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
  build-essential \
  git \
  autoconf \
  automake \
  libtool \
  libssl-dev \
  libgmp-dev \
  libjson-c-dev \
  pkg-config \
  cmake \
  ccache

WORKDIR /opt

# Clone IBM TPM2 Simulator
WORKDIR /opt
RUN git clone https://github.com/kgoldman/ibmswtpm2.git

# Build the TPM simulator
WORKDIR /opt/ibmswtpm2/src
RUN make -j$(nproc)

# Expose TPM ports
# 2321 = command port
# 2322 = platform port
EXPOSE 2321 2322

