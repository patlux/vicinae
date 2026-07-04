{
  pkgs,
  lib,
}: let
  ecm6 = pkgs.kdePackages.extra-cmake-modules.overrideAttrs (old: {
    meta = (old.meta or {}) // {platforms = lib.platforms.unix;};
  });

  qtPackages = with pkgs.qt6; [
    qtbase
    qtdeclarative
    qtsvg
    qttools
    qtshadertools
  ];

  prefixPackages =
    qtPackages
    ++ [
      pkgs.kdePackages.qtkeychain
      ecm6
      pkgs.libqalculate
      pkgs.gmp
      pkgs.mpfr
      pkgs.icu76
      pkgs.openssl
    ];

  cmakePrefixPath = lib.makeSearchPathOutput "out" "" prefixPackages;
  pkgConfigPath = lib.makeSearchPathOutput "dev" "lib/pkgconfig" [
    pkgs.libqalculate
    pkgs.gmp
    pkgs.mpfr
    pkgs.icu76
    pkgs.openssl
    pkgs.kdePackages.qtkeychain
  ];
  qtPluginPath = lib.makeSearchPathOutput "out" "lib/qt-6/plugins" [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative
    pkgs.qt6.qtsvg
  ];
  qmlImportPath = lib.makeSearchPathOutput "out" "lib/qt-6/qml" [
    pkgs.qt6.qtdeclarative
  ];

  configure = pkgs.writeShellScriptBin "vicinae-configure" ''
    set -euo pipefail
    build_dir="''${VICINAE_BUILD_DIR:-build}"
    unset DEVELOPER_DIR SDKROOT
    sdkroot="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
    swiftc="$(/usr/bin/xcrun --find swiftc)"
    cmake -S . -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE="''${CMAKE_BUILD_TYPE:-Release}" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 \
      -DCMAKE_OSX_SYSROOT="$sdkroot" \
      -DCMAKE_Swift_COMPILER="$swiftc" \
      -DCMAKE_CXX_FLAGS="-I${pkgs.libqalculate.dev}/include -I${pkgs.gmp.dev}/include -I${pkgs.mpfr.dev}/include -I${pkgs.kdePackages.qtkeychain}/include" \
      -DCMAKE_EXE_LINKER_FLAGS="-L${pkgs.libqalculate}/lib -L${pkgs.gmp.out}/lib -L${pkgs.mpfr.out}/lib -L${pkgs.kdePackages.qtkeychain}/lib" \
      -DCMAKE_SHARED_LINKER_FLAGS="-L${pkgs.libqalculate}/lib -L${pkgs.gmp.out}/lib -L${pkgs.mpfr.out}/lib -L${pkgs.kdePackages.qtkeychain}/lib" \
      -DCMAKE_PREFIX_PATH="''${CMAKE_PREFIX_PATH}" \
      -DICU_ROOT="${pkgs.icu76}" \
      -DICU_INCLUDE_DIR="${pkgs.icu76.dev}/include" \
      -DICU_UC_LIBRARY="${pkgs.icu76}/lib/libicuuc.dylib" \
      -DOPENSSL_ROOT_DIR="${pkgs.openssl}" \
      -DOPENSSL_INCLUDE_DIR="${pkgs.openssl.dev}/include" \
      -DOPENSSL_CRYPTO_LIBRARY="${pkgs.openssl.out}/lib/libcrypto.dylib" \
      -DOPENSSL_SSL_LIBRARY="${pkgs.openssl.out}/lib/libssl.dylib" \
      -Dlibqalculate_DIR="${pkgs.libqalculate.dev}/lib/cmake/libqalculate" \
      -DGMP_INCLUDE_DIR="${pkgs.gmp.dev}/include" \
      -DGMP_LIBRARY="${pkgs.gmp.out}/lib/libgmp.dylib" \
      -DMPFR_INCLUDE_DIR="${pkgs.mpfr.dev}/include" \
      -DMPFR_LIBRARY="${pkgs.mpfr.out}/lib/libmpfr.dylib" \
      -DWAYLAND_LAYER_SHELL=OFF \
      -DUSE_SYSTEM_LAYER_SHELL=ON \
      -DUSE_SYSTEM_CMARK_GFM=OFF \
      -DUSE_SYSTEM_QT_KEYCHAIN=ON \
      -DUSE_SYSTEM_GLAZE=OFF \
      -DLIBQALCULATE_BACKEND=ON \
      -DTYPESCRIPT_EXTENSIONS=ON \
      -DINSTALL_BROWSER_NATIVE_HOST=OFF \
      -DINSTALL_NODE_MODULES=ON
  '';

  build = pkgs.writeShellScriptBin "vicinae-build" ''
    set -euo pipefail
    build_dir="''${VICINAE_BUILD_DIR:-build}"
    jobs="''${1:-$(sysctl -n hw.ncpu)}"
    cmake --build "$build_dir" --parallel "$jobs"
  '';

  run = pkgs.writeShellScriptBin "vicinae-run" ''
    set -euo pipefail
    build_dir="''${VICINAE_BUILD_DIR:-build}"
    exec "$build_dir/bin/vicinae" server --replace "$@"
  '';

  stop = pkgs.writeShellScriptBin "vicinae-stop" ''
    set -euo pipefail
    build_dir="''${VICINAE_BUILD_DIR:-build}"
    pkill -f "$PWD/$build_dir/bin/vicinae" 2>/dev/null || true
    pkill -f "$PWD/$build_dir/bin/vicinae-server" 2>/dev/null || true
  '';
in
  pkgs.mkShell {
    packages =
      prefixPackages
      ++ [
        pkgs.cmake
        pkgs.ninja
        pkgs.pkg-config
        pkgs.nodejs_22
        pkgs.ccache
        configure
        build
        run
        stop
      ];

    CMAKE_PREFIX_PATH = cmakePrefixPath;
    PKG_CONFIG_PATH = pkgConfigPath;
    QT_PLUGIN_PATH = qtPluginPath;
    QML_IMPORT_PATH = qmlImportPath;
    QML2_IMPORT_PATH = qmlImportPath;
    QTBASE = pkgs.qt6.qtbase;
    QTDECLARATIVE = pkgs.qt6.qtdeclarative;
    QTSVG = pkgs.qt6.qtsvg;
    ICU_ROOT = pkgs.icu76;
    OPENSSL_ROOT_DIR = pkgs.openssl;

    shellHook = ''
      unset DEVELOPER_DIR SDKROOT
      export CC=/usr/bin/cc
      export CXX=/usr/bin/c++
      export CMAKE_C_COMPILER=$CC
      export CMAKE_CXX_COMPILER=$CXX
      export CMAKE_GENERATOR=Ninja
      export CPATH="${pkgs.libqalculate.dev}/include:${pkgs.gmp.dev}/include:${pkgs.mpfr.dev}/include:${pkgs.kdePackages.qtkeychain}/include''${CPATH:+:$CPATH}"
      export LIBRARY_PATH="${pkgs.libqalculate}/lib:${pkgs.gmp.out}/lib:${pkgs.mpfr.out}/lib:${pkgs.kdePackages.qtkeychain}/lib''${LIBRARY_PATH:+:$LIBRARY_PATH}"
      export VICINAE_BUILD_DIR=''${VICINAE_BUILD_DIR:-build}

      echo "Vicinae macOS dev shell"
      echo "  vicinae-configure"
      echo "  vicinae-build"
      echo "  vicinae-run --open"
      echo "  build dir: $VICINAE_BUILD_DIR"
    '';
  }
