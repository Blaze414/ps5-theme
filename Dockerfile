# Build environment for ps5-theme (ps5-payload-sdk on Ubuntu 24.04 with LLVM 18).
#
# Every input that decides the output is pinned, so the same source gives the same files byte for byte, today or
# later (see docs/building.md#reproducible-builds): the base image by its digest, the compiler, linker and make by
# their exact versions (Ubuntu 24.04's own, which stay in the archive), and the SDK by its checksum.
FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac AS toolchain

ARG SDK_VERSION=v0.43
ARG SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb
ARG LLVM_VERSION=1:18.1.3-1ubuntu1
ARG MAKE_VERSION=4.3-4.1build2

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bash ca-certificates clang-18=${LLVM_VERSION} lld-18=${LLVM_VERSION} llvm-18=${LLVM_VERSION} \
      make=${MAKE_VERSION} python3 unzip wget \
 && rm -rf /var/lib/apt/lists/*

RUN wget -q -O /tmp/sdk.zip \
      https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
 && echo "${SDK_SHA256}  /tmp/sdk.zip" | sha256sum -c - \
 && unzip -q /tmp/sdk.zip -d /opt \
 && rm /tmp/sdk.zip

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

FROM toolchain AS build
WORKDIR /src
COPY . .
RUN make clean && make

FROM scratch AS artifact
COPY --from=build /src/ps5-theme.plugin /src/ps5-theme.elf /src/ps5-theme-stop.elf /
