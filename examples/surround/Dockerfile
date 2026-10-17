# Neko with Firefox + VLC on a 7.1 virtual output and two selectable stereo audio
# tracks: Dolby Pro Logic II and Dolby Headphone. Dolby Headphone runs the lakehp
# engine natively and reads its room filters from your DolbyHph.dll, mounted at
# runtime (see docker-compose.yaml) - the image itself contains no Dolby data.
#
# The neko image must be built from this branch first (it carries the selectable
# audio tracks), then this image on top of it, from the repository root:
#
#   ./build -y                    # ghcr.io/m1k1o/neko/base:latest
#   ./build -y -a firefox         # ghcr.io/m1k1o/neko/firefox:latest
#   docker build -f examples/surround/Dockerfile -t neko-surround .

ARG BASE_IMAGE=ghcr.io/m1k1o/neko/firefox:latest
ARG BUILD_IMAGE=debian:trixie-slim

#
# build the nekosurround GStreamer element (Pro Logic II encoder + lakehp engine)
FROM $BUILD_IMAGE AS surround
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; apt-get update; \
    apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base; \
    rm -rf /var/lib/apt/lists/*

COPY utils/surround /src
WORKDIR /src
RUN set -eux; \
    make plugin; \
    make test; \
    make install DESTDIR=/out

#
# runtime
FROM $BASE_IMAGE

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends vlc; \
    apt-get clean -y; \
    rm -rf /var/lib/apt/lists/* /var/cache/apt/*; \
    mkdir -p /etc/neko/dolby

COPY --from=surround /out/ /

COPY examples/surround/default.pa /etc/pulse/default.pa
COPY examples/surround/daemon.conf /etc/pulse/daemon.conf.d/neko-surround.conf
COPY examples/surround/supervisord.conf /etc/neko/supervisord/surround.conf
COPY examples/surround/run-app.sh /usr/lib/neko-surround/run-app

# Firefox and VLC only start when listed in NEKO_SURROUND_APPS (default: both)
RUN set -eux; \
    sed -i 's|^command=/usr/bin/firefox |command=/usr/lib/neko-surround/run-app firefox /usr/bin/firefox |' \
        /etc/neko/supervisord/firefox.conf; \
    grep -q 'run-app firefox' /etc/neko/supervisord/firefox.conf

ENV NEKO_SURROUND_APPS=firefox,vlc
COPY examples/surround/neko.yaml /etc/neko/neko.yaml

ENV VLC_MEDIA=/media
