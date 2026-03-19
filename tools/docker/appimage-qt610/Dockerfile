FROM quay.io/fedora/fedora:43

ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN dnf -y upgrade && \
    dnf -y install \
      bash \
      ca-certificates \
      cmake \
      curl \
      file \
      findutils \
      fontconfig \
      freetype \
      gcc \
      gcc-c++ \
      git \
      glibc-langpack-en \
      libglvnd-devel \
      libxkbcommon \
      lua \
      lua-devel \
      lua-json \
      lua-lpeg \
      lua-socket \
      make \
      ninja-build \
      openssl \
      openssl-devel \
      patchelf \
      pkgconf-pkg-config \
      pulseaudio-libs \
      python3 \
      python3-pip \
      rsync \
      tar \
      unzip \
      which \
      xz \
      zlib-devel \
      zip && \
    dnf clean all

RUN python3 -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    python3 -m pip install --no-cache-dir --upgrade aqtinstall

ARG QT_VERSION=6.10.2
ARG QT_ARCH=linux_gcc_64

RUN aqt install-qt -O /opt/Qt linux desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia

ENV QMUD_APPIMAGE_QT_PREFIX=/opt/Qt/${QT_VERSION}/gcc_64
ENV PATH=/opt/Qt/${QT_VERSION}/gcc_64/bin:${PATH}

WORKDIR /work
