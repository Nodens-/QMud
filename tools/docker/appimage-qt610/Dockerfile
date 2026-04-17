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
      flite \
      flite-devel \
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
      which \
      xz \
      zlib-devel \
      zip && \
    dnf clean all

RUN python3 -m pip install --no-cache-dir --upgrade pip setuptools wheel && \
    python3 -m pip install --no-cache-dir --upgrade aqtinstall

ARG QT_SERIES=6.10
ARG QT_ARCH=linux_gcc_64

RUN set -eux; \
    QT_VERSION="$(aqt list-qt linux desktop --spec "${QT_SERIES}" --latest-version)"; \
    aqt install-qt -O /opt/Qt linux desktop "${QT_VERSION}" "${QT_ARCH}" -m qtmultimedia; \
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
