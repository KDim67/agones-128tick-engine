# syntax=docker/dockerfile:1.7@sha256:a57df69d0ea827fb7266491f2813635de6f17269be881f696fbfdf2d83dda33e

ARG GRPC_VERSION=v1.83.0
ARG AGONES_VERSION=v1.61.0

# Builder and runtime are both Debian 13: glibc and libstdc++ ABI must match
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libssl-dev ninja-build pkg-config \
 && rm -rf /var/lib/apt/lists/*
# Hardening for every stage, statically linked gRPC included
ENV CFLAGS="-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection" \
    CXXFLAGS="-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection" \
    LDFLAGS="-Wl,-z,relro,-z,now"

# gRPC pinned to the release whose bundled protobuf (7.35.1) matches the Agones SDK gencode
FROM toolchain AS grpc
ARG GRPC_VERSION
RUN git clone --depth 1 --branch "${GRPC_VERSION}" https://github.com/grpc/grpc /tmp/grpc \
 && git -C /tmp/grpc submodule update --init --depth 1 --jobs 8 \
      third_party/abseil-cpp third_party/cares/cares third_party/protobuf third_party/re2 third_party/zlib \
 && cmake -S /tmp/grpc -B /tmp/grpc/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/grpc \
      -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DgRPC_SSL_PROVIDER=package \
      -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
      -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
 && cmake --build /tmp/grpc/build \
 && cmake --install /tmp/grpc/build \
 && rm -rf /tmp/grpc

FROM grpc AS agones-sdk
ARG AGONES_VERSION
RUN git clone --depth 1 --branch "${AGONES_VERSION}" --filter=blob:none --sparse https://github.com/agones-dev/agones /tmp/agones \
 && git -C /tmp/agones sparse-checkout set sdks/cpp \
 && cmake -S /tmp/agones/sdks/cpp -B /tmp/agones/build -G Ninja -Wno-dev \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/grpc \
      -DCMAKE_INSTALL_PREFIX=/opt/agones \
 && cmake --build /tmp/agones/build \
 && cmake --install /tmp/agones/build \
 && rm -rf /tmp/agones

FROM agones-sdk AS build
WORKDIR /src
COPY CMakeLists.txt .
COPY include include
COPY src src
COPY tests tests
# FetchContent cache: source edits do not re-clone ENet
RUN --mount=type=cache,id=engine-fetchcontent,target=/src/build/_deps \
    cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/opt/grpc;/opt/agones" \
      -DENGINE_WITH_AGONES=ON \
      -DENGINE_BUILD_TOOLS=OFF \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure \
 && cmake --install build --strip --prefix /out --component runtime

# The harness never talks to an Agones sidecar, so it skips gRPC and builds in minutes on a cold cache
FROM toolchain AS bench-build
WORKDIR /src
COPY CMakeLists.txt .
COPY include include
COPY src src
COPY tests tests
COPY tools/loadgen tools/loadgen
RUN --mount=type=cache,id=engine-fetchcontent-bench,target=/src/build/_deps \
    cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DENGINE_WITH_AGONES=OFF \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure \
 && cmake --install build --strip --prefix /out --component bench

# Benchmark harness: servers, bots and netem share one loopback; run with --cap-add NET_ADMIN
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS bench
RUN apt-get update \
 && apt-get install -y --no-install-recommends iproute2 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=bench-build /out/bin/ /opt/bench/bin/
COPY --chmod=0755 tools/netem/impair.sh tools/bench/run_benchmark.sh /opt/bench/
ENV BIN_DIR=/opt/bench/bin REPORT_DIR=/reports
ENTRYPOINT ["/opt/bench/run_benchmark.sh"]

FROM gcr.io/distroless/cc-debian13:nonroot@sha256:e792ab3d241a468a4fd7519ddbbebe66b49b5f365771716ea688ad40b6c6f1c2 AS runtime
LABEL org.opencontainers.image.source="https://github.com/KDim67/agones-128tick-engine"
COPY --from=build /out/bin/game_server /app/game_server
USER 65532:65532
EXPOSE 7777/udp 9090/tcp
ENTRYPOINT ["/app/game_server"]
