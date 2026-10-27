# Game server image. Build from the repository root:
#   docker build -f apps/server/Dockerfile -t rts-server .
#   docker run -p 27015:27015/udp -p 27016:27016 -v rts-data:/srv/rts/data rts-server
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DRTS_BUILD_CLIENT=OFF -DRTS_BUILD_EDITOR=OFF -DRTS_BUILD_TOOLS=OFF -DRTS_BUILD_HEADLESS=OFF -DRTS_BUILD_TESTS=OFF \
 && cmake --build build --target rts_server

FROM debian:bookworm-slim
RUN useradd --system --home /srv/rts rts && mkdir -p /srv/rts/data && chown rts /srv/rts/data
COPY --from=build /src/build/rts_server /usr/local/bin/rts_server
COPY apps/server/server.json /etc/rts/server.json
# Game data is optional for the server; it only makes saved replays point at real files.
COPY games /srv/rts/games
WORKDIR /srv/rts
USER rts
EXPOSE 27015/udp 27016/tcp
CMD ["rts_server", "--config", "/etc/rts/server.json", "--replays", "data/replays", "--ratings", "data/ratings.txt"]
