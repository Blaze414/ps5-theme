# Build environment for ps5-theme (ps5-payload-sdk on Ubuntu 24.04 with LLVM 18).
FROM ubuntu:24.04 AS toolchain

ARG SDK_VERSION=v0.43

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bash ca-certificates clang-18 lld-18 llvm-18 make python3 unzip wget \
 && rm -rf /var/lib/apt/lists/*

RUN wget -q -O /tmp/sdk.zip \
      https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
 && unzip -q /tmp/sdk.zip -d /opt \
 && rm /tmp/sdk.zip

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

FROM toolchain AS build
WORKDIR /src
COPY . .
RUN make clean && make

FROM scratch AS artifact
COPY --from=build /src/ps5-theme.plugin /src/ps5-theme.elf /
