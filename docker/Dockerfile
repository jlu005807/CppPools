FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake libsqlite3-dev sqlite3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DCPPPOOLS_USE_MYSQL=OFF -DCPPPOOLS_USE_REDIS=OFF \
    && cmake --build /build --target cpppools_server --parallel \
    && mkdir -p /seed \
    && sqlite3 /seed/cpppools.db \
       'CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);'

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends libsqlite3-0 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home --shell /usr/sbin/nologin cpppools \
    && mkdir -p /var/lib/cpppools /etc/cpppools \
    && chown -R cpppools:cpppools /var/lib/cpppools /etc/cpppools

COPY --from=build /build/bin/cpppools_server /usr/local/bin/cpppools_server
COPY --from=build --chown=cpppools:cpppools /seed/cpppools.db /var/lib/cpppools/cpppools.db
USER cpppools
WORKDIR /var/lib/cpppools
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/cpppools_server"]
CMD ["--config", "/etc/cpppools/config.json"]
