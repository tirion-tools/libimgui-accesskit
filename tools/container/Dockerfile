# Gate toolchain (tools/container.sh). Debian trixie by digest; packages from a dated Debian snapshot
# at exact versions (apt verifies the signed Release files): CMake, Ninja, GCC 14, clang 19 (ASan,
# UBSan, libFuzzer), GoogleTest, Pango/cairo/fontconfig, libGL, and the Noto fonts the text tests shape
# with (Latin, Hebrew, Devanagari, colour emoji). SDL3, Dear ImGui and the AccessKit C release come from
# their upstream release URLs, checked against SHA-256 pins: SDL3 is built static into /usr/local,
# Dear ImGui's sources go to /opt/imgui (each build compiles them), and the accesskit-c release
# (prebuilt static library, header, CMake package) to /opt/accesskit-c.
FROM debian:trixie@sha256:9c6e381421d0f48d1b8133e573f33c185ee16cbf505eda9c4a8a4791b25ea8a6
ARG SNAPSHOT=20261101T000000Z
ARG SDL3_VERSION=3.4.2
ARG SDL3_SHA256=ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c
ARG IMGUI_VERSION=1.93.0-docking
ARG IMGUI_SHA256=746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5
ARG ACCESSKIT_C_VERSION=0.23.2
ARG ACCESSKIT_C_SHA256=c7fa84f8f34fca909f662375737a54f98b17cf9eae51fffd94ab3d9f662e3681
RUN rm -f /etc/apt/sources.list.d/* \
 && printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s trixie main\ndeb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s trixie-updates main\ndeb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s trixie-security main\n' \
      "$SNAPSHOT" "$SNAPSHOT" "$SNAPSHOT" > /etc/apt/sources.list \
 && apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      ca-certificates=20250419+deb13u1 curl=8.14.1-2+deb13u7 unzip=6.0-29+deb13u1 git=1:2.47.3-0+deb13u1 \
      cmake=3.31.6-2 ninja-build=1.12.1-1 pkgconf=1.8.1-4 gcc-14=14.2.0-19 g++-14=14.2.0-19 \
      clang-19=1:19.1.7-3+b1 libclang-rt-19-dev=1:19.1.7-3+b1 llvm-19=1:19.1.7-3+b1 libgtest-dev=1.16.0-1 \
      libpango1.0-dev=1.56.3-1 libcairo2-dev=1.18.4-1+b1 libfontconfig-dev=2.15.0-2.3 libgl-dev=1.7.0-1+b2 \
      fonts-noto-core=20201225-2 fonts-noto-mono=20201225-2 fonts-noto-color-emoji=2.051-0+deb13u1 >/dev/null \
 && rm -rf /var/lib/apt/lists/*
RUN set -e; cd /tmp \
 && curl -fsSL -o sdl.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz" \
 && echo "${SDL3_SHA256}  sdl.tar.gz" | sha256sum -c - \
 && tar xzf sdl.tar.gz \
 && CC=gcc-14 cmake -S "SDL3-${SDL3_VERSION}" -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON \
      -DSDL_SHARED=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
      -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF \
      -DSDL_POWER=OFF -DSDL_DIALOG=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGLES=OFF >/dev/null \
 && cmake --build sdl-build >/dev/null \
 && cmake --install sdl-build >/dev/null \
 && curl -fsSL -o imgui.tar.gz "https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz" \
 && echo "${IMGUI_SHA256}  imgui.tar.gz" | sha256sum -c - \
 && tar xzf imgui.tar.gz && mv "imgui-${IMGUI_VERSION}" /opt/imgui \
 && curl -fsSL -o accesskit.zip "https://github.com/AccessKit/accesskit-c/releases/download/${ACCESSKIT_C_VERSION}/accesskit-c-${ACCESSKIT_C_VERSION}.zip" \
 && echo "${ACCESSKIT_C_SHA256}  accesskit.zip" | sha256sum -c - \
 && unzip -q accesskit.zip "accesskit-c-${ACCESSKIT_C_VERSION}/include/*" \
      "accesskit-c-${ACCESSKIT_C_VERSION}/lib/linux/x86_64/static/*" "accesskit-c-${ACCESSKIT_C_VERSION}/accesskit*.cmake" \
      "accesskit-c-${ACCESSKIT_C_VERSION}/LICENSE*" \
 && mv "accesskit-c-${ACCESSKIT_C_VERSION}" /opt/accesskit-c \
 && rm -rf /tmp/*
ENV IMGUI_DIR=/opt/imgui CMAKE_PREFIX_PATH=/opt/accesskit-c
