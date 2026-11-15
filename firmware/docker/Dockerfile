# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# Build environment for the MSM8916 modem firmware (modem_proc tree, SCons, Hexagon tools 6.4.06.a).
# The Hexagon tools are not part of the image: they are mounted from the host as /hextools (see ../build.sh).
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
# The build scripts need Python 2.7; the Hexagon tools are 32-bit, hence the i386 libraries.
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
        python2.7 python2.7-minimal make perl file xxd binutils ca-certificates \
        libtinfo5 libncurses5 zlib1g less vim-tiny \
        libc6:i386 libstdc++6:i386 zlib1g:i386 libatomic1 libxml2 \
    && ln -sf /usr/bin/python2.7 /usr/bin/python \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work/modem_proc
CMD ["/bin/bash"]
