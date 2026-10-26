# Build dependencies and configuration

The Docker build uses Ubuntu 24.04 and installs the following host packages:

- `bash`, `ca-certificates`, `curl`, `unzip`, `zip`, `xz-utils`, `tar`, `git`, `patch`
- `build-essential`, `cmake`, `ninja-build`, `meson`, `pkg-config`, `flex`, `bison`
- `clang`, `llvm`, `llvm-dev`, `glslang-tools`
- `python3`, `python3-capstone`, `python3-mako`, `python3-yaml`, `python3-packaging`
- `openjdk-17-jdk-headless`

These packages provide the shell, archive/network and Git tools, C/C++ build
toolchain, Mesa/Meson utilities, Python modules, and Java compiler/runtime used
by `build.sh`.

## Android SDK and NDK

The Dockerfile downloads the Android command-line tools from:

`https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip`

It accepts the SDK licenses and installs these SDK packages:

- `platform-tools`
- `platforms;android-36`
- `build-tools;36.0.0`
- `ndk;28.2.13676358` (NDK r28c)

Environment configuration:

| Variable | Value |
| --- | --- |
| `ANDROID_SDK_ROOT` | `/opt/android-sdk` |
| `ANDROID_HOME` | `/opt/android-sdk` |
| `ANDROID_NDK_HOME` | `/opt/android-sdk/ndk/28.2.13676358` |
| `BUILD_TOOLS` | `/opt/android-sdk/build-tools/36.0.0` |
| `ANDROID_JAR` | `/opt/android-sdk/platforms/android-36/android.jar` |

The Dockerfile also adds command-line tools, platform tools, and build-tools to
`PATH`. API 36 is used because `platforms;android-37` was not available from the
SDK repository when the Docker build was tested.

## Project inputs

`build.sh` additionally expects the project inputs copied into the image:

- A supported Factorio Linux ARM64 `.tar.xz` archive in `input/`
- `tools/`, `native/`, `patches/`, and `android-wrapper/`

At startup, `build.sh` checks the required executables, Python modules, Android
platform JAR, NDK toolchain, and Android build-tools (`aapt2`, `d8`, `zipalign`,
and `apksigner`). SDL 3.4.14 and the pinned Panfrost source revision are fetched
by the build script when needed.
