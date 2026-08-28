{
  description = "CemuExtend Nix build environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    # Cemu expects the complete Dear ImGui source tree, not only installed
    # headers.  Keep it as an explicit input so the flake also works when the
    # git submodules in a checkout have not been initialized yet.
    imgui-src = {
      url = "github:ocornut/imgui/v1.91.3";
      flake = false;
    };

    # Fetch this flake's own git submodules (including libcemuextend) rather
    # than pinning them to a separate, manually-updated input revision.
    self.submodules = true;
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      imgui-src,
      ...
    }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      lib = pkgs.lib;

      cefLinuxX64Archive = pkgs.fetchurl {
        url = "https://cef-builds.spotifycdn.com/cef_binary_151.3.24%2Bg2384915%2Bchromium-151.0.7922.174_linux64.tar.bz2";
        hash = "sha256-mV+1f+a5r56hhKmDvIeM6pzFiV0+0HEGWh1K140Oo68=";
      };
      cefLinuxX64 = pkgs.runCommand "cef-151.3.24-linux64" { nativeBuildInputs = [ pkgs.bzip2 ]; } ''
        mkdir -p "$out"
        tar -xjf ${cefLinuxX64Archive} --strip-components=1 -C "$out"
      '';

      # Keep the Nix source small and reproducible.  Dependencies that are
      # supplied by Nixpkgs are deliberately omitted and linked in
      # preConfigure below where Cemu expects a vendored source tree.
      commonSourceFiles = lib.fileset.difference ./src (
        lib.fileset.unions [
          ./src/gui/wxgui
          ./src/webview
        ]
      );
      sourceFor =
        frontend:
        lib.fileset.toSource {
          root = ./.;
          fileset = lib.fileset.unions (
            [
              ./CMakeLists.txt
              ./bin/gameProfiles/default
              ./bin/resources
              ./cmake
              ./dependencies/DirectX_2010
              ./dependencies/gamemode
              ./dependencies/ih264d
              (lib.fileset.maybeMissing ./dependencies/libcemuextend)
              ./dist
              commonSourceFiles
            ]
            ++ lib.optionals (frontend == "cef") [
              ./src/webview
            ]
            ++ lib.optionals (frontend == "wx") [
              ./src/gui/wxgui
            ]
          );
        };

      # glslang's CMake package refers to SPIRV-Tools-opt before it is
      # otherwise loaded.  This is the same initialization used by the
      # cemu_pinkd reference flake.
      devCmakeInit = pkgs.writeText "cemu-extend-nix-init.cmake" ''
        if(NOT TARGET SPIRV-Tools-opt)
          find_package(SPIRV-Tools-opt REQUIRED)
        endif()
      '';

      makeCemu =
        frontend:
        pkgs.stdenv.mkDerivation {
          pname = "cemu-extend-${frontend}";
          version = "2.0-${self.shortRev or "dirty"}";
          src = sourceFor frontend;

          nativeBuildInputs =
            with pkgs;
            [
              addDriverRunpath
              cmake
              nasm
              ninja
              pkg-config
              wayland-scanner
            ]
            ++ lib.optionals (frontend != "headless") [
              wrapGAppsHook3
            ]
            ++ lib.optionals (frontend == "wx") [
              wxwidgets_3_3
            ];

          buildInputs =
            with pkgs;
            [
              bluez
              boost
              cubeb
              curl
              fmt_12
              glm
              glslang
              hidapi
              libGL
              libGLU
              libpng
              libusb1
              libxrender
              libzip
              openssl
              pugixml
              rapidjson
              sdl3
              spirv-tools
              vulkan-headers
              vulkan-loader
              wayland
              wayland-protocols
              libx11
              libxrender
              zarchive
              zlib
              zstd
            ]
            ++ lib.optionals (frontend != "headless") [
              gtk3
            ]
            ++ lib.optionals (frontend == "wx") [
              wxwidgets_3_3
            ];

          cmakeFlags = [
            (lib.cmakeBool "ALLOW_PORTABLE" false)
            (lib.cmakeBool "ENABLE_FERAL_GAMEMODE" true)
            (lib.cmakeBool "ENABLE_VCPKG" false)
            (lib.cmakeFeature "CEMU_FRONTEND" frontend)
            (lib.cmakeFeature "CEF_ROOT" (if frontend == "cef" then toString cefLinuxX64 else ""))
            (lib.cmakeFeature "CMAKE_PROJECT_INCLUDE" (toString devCmakeInit))
            (lib.cmakeFeature "EMULATOR_VERSION_MAJOR" "2")
            (lib.cmakeFeature "EMULATOR_VERSION_MINOR" "0")
          ];

          preConfigure = ''
            rm -rf dependencies/imgui dependencies/Vulkan-Headers
            ln -s ${imgui-src} dependencies/imgui
            ln -s ${pkgs.vulkan-headers} dependencies/Vulkan-Headers

            substituteInPlace dependencies/gamemode/lib/gamemode_client.h \
              --replace-fail "libgamemode.so.0" \
              "${pkgs.gamemode.lib}/lib/libgamemode.so.0"
          '';

          installPhase = ''
            runHook preInstall

            ${
              if frontend == "cef" then
                ''
                  install -d $out/lib/cemu-extend $out/bin
                  cp -a ../bin/. $out/lib/cemu-extend/
                  ln -s ../lib/cemu-extend/Cemu_release $out/bin/Cemu_release
                  ln -s Cemu_release $out/bin/cemu
                ''
              else
                ''
                  install -Dm755 ../bin/Cemu_release $out/bin/Cemu_release
                  ln -s Cemu_release $out/bin/cemu
                ''
            }

            install -d $out/share/Cemu
            cp -r ../bin/resources ../bin/gameProfiles $out/share/Cemu/

            install -d $out/share/applications
            substitute ../dist/linux/info.cemu.Cemu.desktop \
              $out/share/applications/info.cemu.Cemu.desktop \
              --replace-fail "Exec=Cemu" "Exec=$out/bin/cemu"

            install -Dm644 ../dist/linux/info.cemu.Cemu.metainfo.xml \
              $out/share/metainfo/info.cemu.Cemu.metainfo.xml
            install -Dm644 ../src/resource/logo_icon.png \
              $out/share/icons/hicolor/128x128/apps/info.cemu.Cemu.png

            runHook postInstall
          '';

          preFixup = lib.optionalString (frontend != "headless") ''
            gappsWrapperArgs+=(
              --prefix LD_LIBRARY_PATH : "${lib.makeLibraryPath [ pkgs.vulkan-loader ]}"
            )
          '';

          strictDeps = true;
          enableParallelBuilding = true;

          meta = {
            description = "CemuExtend Wii U emulator build for NixOS";
            homepage = "https://github.com/CemuExtend/CemuExtend";
            license = lib.licenses.mpl20;
            mainProgram = "cemu";
            platforms = [ system ];
          };
        };
      cemuCef = makeCemu "cef";
      cemuWx = makeCemu "wx";
      cemuHeadless = makeCemu "headless";
    in
    {
      packages.${system} = {
        default = cemuCef;
        cemu-extend = cemuCef;
        cemu-extend-cef = cemuCef;
        cemu-extend-webview = cemuCef;
        cemu-extend-wx = cemuWx;
        cemu-extend-headless = cemuHeadless;
      };

      apps.${system}.default = {
        type = "app";
        program = lib.getExe cemuCef;
        meta.description = "Run CemuExtend";
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [
          cemuCef
          cemuWx
        ];
        packages = [ pkgs.git ];
        CMAKE_PROJECT_INCLUDE = devCmakeInit;

        shellHook = ''
          echo "CemuExtend Nix development shell"
          echo "Configure with: cmake -S . -B build/nix -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_VCPKG=OFF -DALLOW_PORTABLE=OFF -DCEMU_FRONTEND=cef"
        '';
      };

      checks.${system} = {
        default = cemuCef;
        frontend-wx = cemuWx;
        frontend-headless = cemuHeadless;
      };
      formatter.${system} = pkgs.nixfmt;
    };
}
