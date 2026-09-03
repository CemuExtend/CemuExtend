# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS cemu-extend-base

ARG DEBIAN_FRONTEND=noninteractive

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg/archives \
    VCPKG_DOWNLOADS=/root/.cache/vcpkg/downloads

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        file \
        freeglut3-dev \
        git \
        libasound2-dev \
        libbluetooth-dev \
        libclang-rt-dev \
        libdbus-1-dev \
        libdrm-dev \
        libegl1-mesa-dev \
        libgcrypt20-dev \
        libgbm-dev \
        libgl1-mesa-dev \
        libglu1-mesa-dev \
        libgtk-3-dev \
        libpulse-dev \
        libnss3-dev \
        libsecret-1-dev \
        libssl-dev \
        libsystemd-dev \
        libtool \
        libudev-dev \
        libusb-1.0-0-dev \
        libwayland-dev \
        libx11-dev \
        libx11-xcb-dev \
        libxcursor-dev \
        libxext-dev \
        libxfixes-dev \
        libxi-dev \
        libxinerama-dev \
        libxkbcommon-dev \
        libxrandr-dev \
        libxrender-dev \
        libxtst-dev \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        unzip \
        wayland-protocols \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_DOWNLOADS"

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        autoconf \
        autoconf-archive \
        automake \
        libglm-dev \
    && rm -rf /var/lib/apt/lists/*

# CEF OSR smoke tests need a display server even though no native browser
# window is created. Keep it in a separate layer so the large toolchain layer
# remains reusable.
RUN apt-get update \
    && apt-get install --no-install-recommends -y xauth xvfb \
    && rm -rf /var/lib/apt/lists/*

FROM cemu-extend-base AS dev

WORKDIR /workspace/CemuExtend
COPY . .
RUN test -f dependencies/vcpkg/bootstrap-vcpkg.sh \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics

CMD ["bash"]

FROM cemu-extend-base AS build

ARG BUILD_TYPE=Release
ARG GIT_HASH=unknown
ARG CEMU_EXTEND_COMMIT_HASH=unknown
ARG SOURCE_FINGERPRINT=unknown
ARG CEMU_FRONTEND=cef
ARG CEMU_OVERLAY_BACKEND=
# Preserve the CMake/Ninja cache by default. Set CLEAN_BUILD=1 only when a
# dependency, toolchain, or configuration change requires a full rebuild.
ARG CLEAN_BUILD=0

WORKDIR /workspace/CemuExtend

# The source is a bind mount, so source changes do not create a multi-gigabyte
# image layer. The writable mount is discarded after the artifact is copied
# out, while CMake/Ninja state is retained in the cache mount.
RUN --mount=type=bind,source=.,target=/workspace/CemuExtend,rw \
    --mount=type=cache,id=cemu-extend-vcpkg,target=/root/.cache/vcpkg/archives,sharing=locked \
    --mount=type=cache,id=cemu-extend-vcpkg-downloads,target=/root/.cache/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=cemu-extend-cef,target=/root/.cache/cemu-cef,sharing=locked \
    --mount=type=cache,id=cemu-extend-cmake,target=/workspace/CemuExtend/build/docker,sharing=locked \
    test -n "${SOURCE_FINGERPRINT}" \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cef_root_arg="" \
    && if [ "${CEMU_FRONTEND}" = "cef" ] || [ "${CEMU_FRONTEND}" = "webview" ]; then \
        CEMU_CEF_DOWNLOAD_DIR=/root/.cache/cemu-cef/downloads \
            CEF_ROOT=/root/.cache/cemu-cef/sdk \
            bash ./scripts/fetch-cef.sh; \
        cef_root_arg="-DCEF_ROOT=/root/.cache/cemu-cef/sdk"; \
    fi \
    && for attempt in 1 2 3; do \
        cmake -S . -B build/docker \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
            -DGIT_HASH=${GIT_HASH} \
            -DCEMU_EXTEND_COMMIT_HASH=${CEMU_EXTEND_COMMIT_HASH} \
            -DENABLE_VCPKG=ON \
            -DCEMU_FRONTEND=${CEMU_FRONTEND} \
            ${cef_root_arg} \
            ${CEMU_OVERLAY_BACKEND:+-DCEMU_OVERLAY_BACKEND=${CEMU_OVERLAY_BACKEND}} \
			-DCEMU_RUNTIME_OUTPUT_DIRECTORY=/workspace/CemuExtend/build/docker/bin \
			-DCEMU_STAGE_CEF_RUNTIME=OFF \
            -DALLOW_PORTABLE=OFF \
            -DVCPKG_INSTALL_OPTIONS=--clean-after-build \
        && break; \
        if [ "$attempt" -eq 3 ]; then exit 1; fi; \
        echo "CMake configure failed (attempt $attempt/3); retrying in 5 seconds..."; \
        sleep 5; \
    done \
    && if [ "${CLEAN_BUILD}" = "1" ]; then \
        cmake --build build/docker --clean-first --parallel; \
    else \
        cmake --build build/docker --parallel; \
    fi \
    && ctest --test-dir build/docker --output-on-failure --label-exclude cef-runtime \
	&& build_type_lower="$(printf '%s' "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')" \
	&& cp "build/docker/bin/Cemu_${build_type_lower}" "/Cemu_${build_type_lower}"

CMD ["bash"]

# The normal developer build exports only the executable. The persistent
# runtime already installed in result/bin is reused by docker-build.sh.
FROM scratch AS build-linux-binary-artifact

COPY --from=build /Cemu_release /Cemu_release

# Runtime packaging is intentionally separate from compilation so ordinary
# source edits never copy and export the multi-gigabyte CEF distribution.
FROM build AS build-linux-bundle

ARG CEMU_FRONTEND=cef

WORKDIR /workspace/CemuExtend

COPY cmake/CefVersion.cmake ./cmake/CefVersion.cmake
COPY scripts/fetch-cef.sh ./scripts/fetch-cef.sh
COPY tools/Cemu-runtime-launcher.sh ./tools/Cemu-runtime-launcher.sh
COPY tools/bundle-linux-runtime.sh ./tools/bundle-linux-runtime.sh

RUN --mount=type=cache,id=cemu-extend-cef,target=/root/.cache/cemu-cef,sharing=locked \
	--mount=type=cache,id=cemu-extend-cmake,target=/workspace/CemuExtend/build/docker,sharing=locked \
	if [ "${CEMU_FRONTEND}" = "headless" ]; then \
		mkdir -p /Cemu_release.bundle \
		&& cp /Cemu_release /Cemu_release.bundle/Cemu_release; \
	else \
		mkdir -p /cemu-bundle-input \
		&& cp /Cemu_release /cemu-bundle-input/Cemu_release \
		&& if [ "${CEMU_FRONTEND}" = "cef" ] || [ "${CEMU_FRONTEND}" = "webview" ]; then \
			CEMU_CEF_DOWNLOAD_DIR=/root/.cache/cemu-cef/downloads \
				CEF_ROOT=/root/.cache/cemu-cef/sdk \
				bash ./scripts/fetch-cef.sh \
			&& cp -a /root/.cache/cemu-cef/sdk/Release/. /cemu-bundle-input/ \
			&& cp -a /root/.cache/cemu-cef/sdk/Resources/. /cemu-bundle-input/ \
			&& mkdir -p /cemu-bundle-input/cef-swiftshader \
			&& mv /cemu-bundle-input/libvulkan.so.1 \
				/cemu-bundle-input/cef-swiftshader/libvulkan.so.1 \
			&& cp /root/.cache/cemu-cef/sdk/LICENSE.txt /cemu-bundle-input/CEF-LICENSE.txt \
			&& if [ -f /root/.cache/cemu-cef/sdk/README.txt ]; then \
				cp /root/.cache/cemu-cef/sdk/README.txt /cemu-bundle-input/CEF-README.txt; \
			fi \
			&& cp build/docker/src/webview/cef_overlay_osr_smoke_tests \
				/cemu-bundle-input/cef_overlay_osr_smoke_tests \
			&& CEMU_CEF_NO_SANDBOX=1 CEMU_CEF_OSR_SMOKE=1 \
				xvfb-run -a -s "-screen 0 1280x720x24" \
				/cemu-bundle-input/cef_overlay_osr_smoke_tests \
			&& rm /cemu-bundle-input/cef_overlay_osr_smoke_tests; \
		fi \
		&& tools/bundle-linux-runtime.sh \
			/cemu-bundle-input/Cemu_release /Cemu_release.bundle; \
	fi

# Export the Linux bundle without loading the multi-gigabyte build image into
# Docker's image store. docker-build.sh uses the local exporter for this stage.
FROM scratch AS build-linux-artifact

COPY --from=build-linux-bundle /Cemu_release.bundle /Cemu_release.bundle

# AppImage packaging reuses the explicitly requested full runtime stage.
FROM build-linux-bundle AS appimage

ARG CEMU_APPIMAGE_ARCH=X64

WORKDIR /workspace/CemuExtend

COPY dist/linux ./dist/linux
RUN mkdir -p bin \
	&& cp -a /Cemu_release.bundle/. bin/ \
	&& rm -f bin/Cemu_release \
	&& mv bin/.Cemu_release.bin bin/Cemu_release \
	&& rm -rf bin/.cemu-runtime

RUN bash dist/linux/appimage.sh "${CEMU_APPIMAGE_ARCH}"

CMD ["bash"]

# Cross-compile the 64-bit Windows executable from a Linux container. Keep this
# toolchain separate from the native Linux image: vcpkg's MinGW packages and
# CMake state are not compatible with the native x64-linux build caches.
FROM ubuntu:24.04 AS cemu-extend-windows-base

ARG DEBIAN_FRONTEND=noninteractive

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg-mingw/archives \
    VCPKG_DOWNLOADS=/root/.cache/vcpkg-mingw/downloads \
    WINEARCH=win64 \
    WINEDEBUG=-all \
    WINEPREFIX=/root/.wine-cemu-extend

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        autoconf \
        autoconf-archive \
        automake \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        file \
        g++-mingw-w64-x86-64-posix \
        gcc-mingw-w64-x86-64-posix \
        git \
        libtool \
        mingw-w64-tools \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        unzip \
        wine \
        xauth \
        xvfb \
        zip \
    && ln -sf /usr/bin/x86_64-w64-mingw32-gcc-posix /usr/local/bin/x86_64-w64-mingw32-gcc \
    && ln -sf /usr/bin/x86_64-w64-mingw32-g++-posix /usr/local/bin/x86_64-w64-mingw32-g++ \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_DOWNLOADS"

FROM cemu-extend-windows-base AS build-windows

ARG BUILD_TYPE=Release
ARG GIT_HASH=unknown
ARG CEMU_EXTEND_COMMIT_HASH=unknown
ARG SOURCE_FINGERPRINT=unknown
ARG ENABLE_WXWIDGETS=ON

WORKDIR /workspace/CemuExtend

RUN --mount=type=bind,source=.,target=/workspace/CemuExtend,rw \
    --mount=type=cache,id=cemu-extend-vcpkg-mingw,target=/root/.cache/vcpkg-mingw/archives,sharing=locked \
    --mount=type=cache,id=cemu-extend-vcpkg-mingw-downloads,target=/root/.cache/vcpkg-mingw/downloads,sharing=locked \
    --mount=type=cache,id=cemu-extend-cmake-mingw-v2,target=/workspace/CemuExtend/build/docker-windows,sharing=locked \
    test -n "${SOURCE_FINGERPRINT}" \
    && bash ./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && cmake -S . -B build/docker-windows \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
            -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/wine \
            -DGIT_HASH=${GIT_HASH} \
            -DCEMU_EXTEND_COMMIT_HASH=${CEMU_EXTEND_COMMIT_HASH} \
            -DENABLE_VCPKG=ON \
			-DCEMU_FRONTEND=wx \
			-DCEMU_OVERLAY_BACKEND=imgui \
            -DALLOW_PORTABLE=OFF \
            -DBUILD_TESTING=ON \
            -DVCPKG_TARGET_TRIPLET=x64-mingw-cemu-static \
            -DVCPKG_HOST_TRIPLET=x64-linux \
            -DVCPKG_OVERLAY_TRIPLETS=/workspace/CemuExtend/cmake/triplets \
            -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=/workspace/CemuExtend/cmake/toolchains/mingw-w64-x86_64.cmake \
            -DVCPKG_APPLOCAL_DEPS=OFF \
            -DVCPKG_INSTALL_OPTIONS=--clean-after-build \
    && cmake --build build/docker-windows --parallel \
    && xvfb-run -a sh -c 'timeout --signal=TERM --kill-after=10s 60s wineboot --init; status=$?; wineserver -k; [ "$status" -eq 0 ] || [ "$status" -eq 124 ]' \
    && xvfb-run -a ctest --test-dir build/docker-windows --output-on-failure \
    && build_type_lower="$(printf '%s' "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')" \
    && windows_executable="bin/Cemu_${build_type_lower}.exe" \
    && file "${windows_executable}" | grep -Eq 'PE32\+ executable.*x86-64' \
    && ! x86_64-w64-mingw32-objdump -p "${windows_executable}" \
        | grep -Eqi 'DLL Name: (libgcc_s[^ ]*|libstdc\+\+[^ ]*|libwinpthread[^ ]*)\.dll' \
	&& cp "${windows_executable}" /Cemu_release.exe

CMD ["bash"]

# Keep the exported Windows image small so docker-build.sh can create a
# temporary extraction container without unpacking the complete toolchain and
# Wine prefix.
FROM scratch AS build-windows-artifact

COPY --from=build-windows /Cemu_release.exe /Cemu_release.exe

CMD ["/Cemu_release.exe"]

# Runnable desktop image. The build stage intentionally remains separate so
# CI can keep extracting the bare binary, while this stage supplies the GTK,
# CEF, graphics, input, and audio runtime required to launch it.
FROM cemu-extend-base AS runtime

ARG CEMU_FRONTEND=cef

COPY --from=build-linux-bundle /Cemu_release.bundle /opt/cemu
COPY bin/resources /opt/cemu/resources
COPY bin/gameProfiles /opt/cemu/gameProfiles
COPY dist/network_services.xml /opt/cemu/network_services.xml

RUN mkdir -p \
        /home/cemu/.cache/Cemu \
        /home/cemu/.config/Cemu \
        /home/cemu/.local/share/Cemu \
        /tmp/cemu-runtime \
    && chmod 1777 /tmp/cemu-runtime

ENV HOME=/home/cemu \
    XDG_CACHE_HOME=/home/cemu/.cache \
    XDG_CONFIG_HOME=/home/cemu/.config \
    XDG_DATA_HOME=/home/cemu/.local/share \
    XDG_RUNTIME_DIR=/tmp/cemu-runtime \
    MESA_SHADER_CACHE_DIR=/home/cemu/.cache/Cemu/mesa_shader_cache \
    GDK_BACKEND=wayland,x11

WORKDIR /opt/cemu
ENTRYPOINT ["/opt/cemu/Cemu_release"]
