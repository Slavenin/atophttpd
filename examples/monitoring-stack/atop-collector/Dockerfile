FROM ubuntu:26.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang llvm libelf-dev zlib1g-dev libglib2.0-dev \
    libncurses-dev pkg-config flex bison python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src/atop
COPY --from=atophttpd_source /atop /src/atop/
RUN test "$(sed -n 's/^#define[[:space:]]*ATOPVERS[[:space:]]*"\([^"]*\)".*/\1/p' version.h)" = 2.13.0 \
    && make -j"$(nproc)" atop
WORKDIR /src/netatop
COPY --from=netatop_source /Makefile /src/netatop/
COPY --from=netatop_source /*.c /src/netatop/
COPY --from=netatop_source /*.h /src/netatop/
COPY --from=netatop_source /libbpf /src/netatop/libbpf
COPY --from=netatop_source /bpftool /src/netatop/bpftool
COPY --from=netatop_source /vmlinux /src/netatop/vmlinux
COPY patch_netatop.py /tmp/patch_netatop.py
RUN python3 /tmp/patch_netatop.py && make -j"$(nproc)" CC='gcc -std=gnu11' netatop

FROM ubuntu:26.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 libglib2.0-0t64 libncursesw6 zlib1g libelf1t64 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/log/atop
COPY --from=build /src/atop/atop /usr/local/bin/atop
COPY --from=build /src/netatop/netatop /usr/local/bin/netatop
COPY supervisor.py health.py /usr/local/lib/atop-collector/
ENTRYPOINT ["python3", "/usr/local/lib/atop-collector/supervisor.py"]
