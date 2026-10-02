# SpaghettiKart online relay (the server behind "Host a game" / "Join a game" room codes).
# Builds straight from GitHub, so the build context can be just this folder:
#   cd tools/netplay && fly deploy          (Fly.io, see fly.toml)
#   docker build -t spaghetti-relay tools/netplay && docker run -d --restart unless-stopped -p 25564:25564 spaghetti-relay
FROM debian:bookworm-slim AS build
ARG REPO=https://github.com/Chrisbchick3n/SpaghettiKart
ARG BRANCH=netplay
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates git g++ cmake make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 --filter=blob:none --sparse -b "$BRANCH" "$REPO" . \
    && git sparse-checkout set src/enhancements/netplay tools/netplay
RUN cmake -S tools/netplay -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ./build/netplay-loopback-test > /dev/null 2>&1

FROM debian:bookworm-slim
COPY --from=build /src/build/spaghetti-netplay-relay /usr/local/bin/spaghetti-netplay-relay
EXPOSE 25564/tcp
CMD ["spaghetti-netplay-relay", "25564"]
