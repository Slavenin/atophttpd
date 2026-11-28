FROM ubuntu:26.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config libglib2.0-dev libncurses-dev zlib1g-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY --from=atophttpd_source / /src/
RUN test "$(sed -n 's/^#define[[:space:]]*ATOPVERS[[:space:]]*"\([^"]*\)".*/\1/p' atop/version.h)" = 2.13.0 \
    && make -j"$(nproc)" USE_TLS=0 SKIP_SUBMODULE_UPDATE=1

FROM ubuntu:26.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    zlib1g curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/log/atop
COPY --from=build /src/atophttpd /usr/local/bin/atophttpd
USER 65534:65534
EXPOSE 2867
ENTRYPOINT ["/usr/local/bin/atophttpd"]
CMD ["-a", "0.0.0.0", "-P", "/var/log/atop", "-H"]
