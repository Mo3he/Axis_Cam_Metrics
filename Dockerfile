# Build both architectures from this single Dockerfile:
#   docker build --build-arg ARCH=aarch64 --target package -t metrics-aarch64 .
#   docker build --build-arg ARCH=armv7hf --target package -t metrics-armv7hf .
#
# Prefer ./build.sh, which does both and collects the .eap files in ./releases.
ARG ARCH=aarch64
ARG SDK_VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG SDK_REPO=axisecp
ARG SDK=acap-native-sdk
# Keep in sync with THIRD_PARTY_NOTICES.md.
ARG PAHO_VERSION=1.3.15

FROM ${SDK_REPO}/${SDK}:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS package
ARG ARCH
ARG PAHO_VERSION

# AXIS OS 13 does not ship libpaho, so it is linked statically. TLS uses the
# device's OpenSSL 3, present on both OS 12 and 13 with a real CA store.
# hadolint ignore=DL3008
RUN apt-get update && \
    apt-get install -y --no-install-recommends cmake && \
    rm -rf /var/lib/apt/lists/*

RUN curl -sSL -o /tmp/paho.tar.gz \
    "https://github.com/eclipse-paho/paho.mqtt.c/archive/refs/tags/v${PAHO_VERSION}.tar.gz" && \
    tar xzf /tmp/paho.tar.gz -C /tmp && \
    rm /tmp/paho.tar.gz

WORKDIR /tmp/paho.mqtt.c-${PAHO_VERSION}
RUN . /opt/axis/acapsdk/environment-setup* && \
    cmake -S . -B build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_FIND_ROOT_PATH="${SDKTARGETSYSROOT}" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DPAHO_BUILD_STATIC=TRUE -DPAHO_BUILD_SHARED=FALSE \
    -DPAHO_WITH_SSL=TRUE -DPAHO_ENABLE_TESTING=FALSE \
    -DPAHO_BUILD_SAMPLES=FALSE -DPAHO_HIGH_PERFORMANCE=TRUE && \
    cmake --build build -j"$(nproc)" && \
    cmake --install build --prefix "${SDKTARGETSYSROOT}/usr"

COPY app /opt/app/
COPY LICENSE /opt/app/LICENSE
WORKDIR /opt/app
RUN sed -i "s/\"BUILDARCH\"/\"${ARCH}\"/" manifest.json && \
    find . -name .DS_Store -delete && \
    . /opt/axis/acapsdk/environment-setup* && acap-build ./

FROM scratch
COPY --from=package /opt/app/*.eap /
