# QEMU machine ipq4019 -- multi-stage image.
#
#   docker build -t ipq4019 .
#   docker run --rm -it -v ~/dumps:/dumps:ro ipq4019
#   docker run --rm -it -p 8080:8080 -e NET=user \
#              -e HOSTFWD='tcp:0.0.0.0:8080-80' -v ~/dumps:/dumps:ro ipq4019
#
# Stage 1 has the toolchain and the QEMU source tree; stage 2 contains the one
# binary that matters plus the launcher, and nothing from the build.
#
# The machine itself is two files: hw-arm-ipq4019.c, dropped into the QEMU tree
# as hw/arm/ipq4019.c, and qemu-hooks.patch, which teaches Kconfig/meson about
# it and adds the external-abort diagnostics.  Nothing else in QEMU is touched.
#
# trixie rather than bookworm on purpose: QEMU 10.2 wants Meson >= 1.5 and
# bookworm ships 1.0, which would make the build download Meson from PyPI.
# slirp is compiled in so NET=user works without /dev/net/tun -- that is the
# only network mode Docker Desktop on Windows can use (see Windows.md).

ARG QEMU_VERSION=10.2.2
ARG DEBIAN_SUITE=trixie-slim

# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_SUITE} AS build
ARG QEMU_VERSION
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils patch flex bison git \
        ninja-build meson pkg-config python3 python3-venv python3-pip \
        libglib2.0-dev libpixman-1-dev libfdt-dev libslirp-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# A release tarball rather than a git clone: the machine only needs the source
# tree, not its history, and this keeps the layer small and pinned.
RUN curl -fsSLo qemu.tar.xz \
        "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" \
    && tar -xf qemu.tar.xz \
    && mv "qemu-${QEMU_VERSION}" qemu \
    && rm qemu.tar.xz

COPY qemu-hooks.patch /src/qemu-hooks.patch
COPY hw-arm-ipq4019.c /src/ipq4019.c

WORKDIR /src/qemu
RUN patch -p1 --forward < /src/qemu-hooks.patch \
    && cp /src/ipq4019.c hw/arm/ipq4019.c

# No "ninja install": it would drag 317 MiB of pc-bios/edk2 blobs into
# /opt/qemu/share, and this machine boots its own boot chain out of the die --
# it needs no firmware files at all.
RUN ./configure \
        --target-list=arm-softmmu \
        --enable-slirp \
        --disable-docs --disable-tools --disable-guest-agent \
        --disable-werror --disable-debug-info \
    && ninja -C build \
    && strip build/qemu-system-arm

# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_SUITE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        bash coreutils grep sed iproute2 python3-minimal \
        libglib2.0-0t64 libpixman-1-0 libfdt1 libslirp0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/qemu/build/qemu-system-arm /usr/local/bin/qemu-system-arm
ENV QEMU_BIN=/usr/local/bin/qemu-system-arm

# The launcher needs the entrypoint and the guest's DHCP client; the older
# gdb/QSEE helpers are not shipped because the machine skips the handover by
# itself now.
COPY packaging/entrypoint.sh packaging/dhcp_client.py /opt/ipq4019/
RUN chmod +x /opt/ipq4019/entrypoint.sh \
    && mkdir -p /dumps /var/log/ipq4019

LABEL org.opencontainers.image.title="QEMU machine ipq4019" \
      org.opencontainers.image.description="Qualcomm IPQ4018/4019 (ipq40xx) board: boots the SPI NOR dump from SBL1 through the bootloader to the guest OS, with no image patching" \
      org.opencontainers.image.source="https://github.com/"

WORKDIR /opt/ipq4019
VOLUME ["/dumps"]
EXPOSE 4444
ENTRYPOINT ["/opt/ipq4019/entrypoint.sh"]
