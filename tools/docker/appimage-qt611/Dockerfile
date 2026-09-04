FROM quay.io/fedora/fedora:44

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
      flite \
      flite-devel \
      freetype \
      gcc \
      gcc-c++ \
      git \
      glibc-langpack-en \
      libglvnd-devel \
      libxkbcommon \
      libxkbcommon-devel \
      lua \
      lua-devel \
      lua-json \
      lua-sec \
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
      speech-dispatcher \
      speech-dispatcher-devel \
      tar \
      unzip \
      vulkan-headers \
      which \
      xz \
      zlib-devel \
      zsync \
      zip && \
    dnf clean all

RUN python3 -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    python3 -m pip install --no-cache-dir --upgrade aqtinstall gcovr

RUN mkdir -p /etc/aqt && \
    printf '%s\n' \
      '[aqt]' \
      'baseurl: https://download.qt.io' \
      '' \
      '[requests]' \
      'hash_algorithm: sha256' \
      'INSECURE_NOT_FOR_PRODUCTION_ignore_hash: False' \
      '' \
      '[mirrors]' \
      'trusted_mirrors:' \
      '    https://download.qt.io' \
      'blacklist:' \
      'fallbacks:' \
      '    https://download.qt.io' \
      > /etc/aqt/settings.ini

ENV AQT_CONFIG=/etc/aqt/settings.ini

ARG QT_SERIES=6.11
ARG QT_ARCH=linux_gcc_64

RUN set -eux; \
    QT_VERSION="$(aqt list-qt linux desktop --spec "${QT_SERIES}" --latest-version)"; \
    aqt install-qt -O /opt/Qt linux desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia qttasktree; \
    ln -sfn "/opt/Qt/${QT_VERSION}" /opt/Qt/latest

RUN set -eux; \
    QT_PREFIX="/opt/Qt/latest/gcc_64"; \
    QT_VERSION="$(basename "$(readlink -f /opt/Qt/latest)")"; \
    git clone --depth 1 --branch "v${QT_VERSION}" https://github.com/qt/qtspeech.git /tmp/qtspeech; \
    "${QT_PREFIX}/bin/qt-cmake" -S /tmp/qtspeech -B /tmp/qtspeech-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${QT_PREFIX}" \
      -DFEATURE_flite=ON \
      -DFEATURE_speechd=ON \
      -DQT_BUILD_TESTS=OFF \
      -DQT_BUILD_EXAMPLES=OFF; \
    cmake --build /tmp/qtspeech-build -j"$(nproc)"; \
    cmake --install /tmp/qtspeech-build; \
    test -f "${QT_PREFIX}/plugins/texttospeech/libqtexttospeech_flite.so"; \
    test -f "${QT_PREFIX}/plugins/texttospeech/libqtexttospeech_speechd.so"; \
    rm -rf /tmp/qtspeech /tmp/qtspeech-build

ENV QMUD_APPIMAGE_QT_PREFIX=/opt/Qt/latest/gcc_64
ENV PATH=/opt/Qt/latest/gcc_64/bin:${PATH}

WORKDIR /work
