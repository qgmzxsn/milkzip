#!/bin/sh
# Build the same 7-Zip + LZ4 core used on Windows for Android 8.0+.
# Requires CMake, make, and Android NDK r27c or newer. ABIS may select one ABI.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
  echo 'Set ANDROID_NDK_HOME to Android NDK r27c (27.2.12479018) or newer.' >&2
  exit 1
fi
case $(uname -s) in
  Darwin) host=darwin-x86_64; jobs=$(sysctl -n hw.ncpu) ;;
  Linux) host=linux-x86_64; jobs=$(nproc) ;;
  *) echo 'Run in Linux, macOS, or WSL.' >&2; exit 1 ;;
esac
pre="$ndk/toolchains/llvm/prebuilt/$host"
"$root/scripts/fetch-7zip.sh"
src="$root/third_party/7zip/src"
bundle="$src/CPP/7zip/Bundles/Format7zF"
for abi in ${ABIS:-arm64-v8a x86_64}; do
  case $abi in
    arm64-v8a) triple=aarch64-linux-android ;;
    x86_64) triple=x86_64-linux-android ;;
    *) echo "Unsupported ABI: $abi" >&2; exit 1 ;;
  esac
  # Generic C codec implementation avoids host assembler assumptions.
  make -C "$bundle" -j"$jobs" -f ../../cmpl_gcc.mak \
    CC="$pre/bin/${triple}26-clang" CXX="$pre/bin/${triple}26-clang++" \
    O="b/android-$abi" LFLAGS_STRIP= CFLAGS_WARN= \
    CFLAGS_WARN_WALL='-Wall -Wextra' LIB2='-lc++_shared' \
    LDFLAGS='-shared -fPIC -Wl,-soname,lib7z.so -Wl,-z,max-page-size=16384'
  build="$root/build-android/$abi"
  cmake -S "$root" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DSEVENZIP_SRC="$src" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build" -j"$jobs"
  pkg="$root/build-android/jni-package/$abi"
  mkdir -p "$pkg"
  cp "$build/libarchiver.so" "$build/libarchiver_jni.so" "$pkg/"
  cp "$bundle/b/android-$abi/7z.so" "$pkg/lib7z.so"
  cp "$pre/sysroot/usr/lib/$triple/libc++_shared.so" "$pkg/"
  "$pre/bin/llvm-strip" --strip-unneeded "$pkg/"*.so
  # Every LOAD segment must support Android's 16 KiB page-size devices.
  for lib in "$pkg/"*.so; do
    "$pre/bin/llvm-readelf" -lW "$lib" | awk '/LOAD/ { if ($NF != "0x4000" && $NF != "0x10000") bad=1 } END { exit bad }' || {
      echo "Not 16 KiB compatible: $lib" >&2; exit 1;
    }
  done
done
echo "Native libraries: $root/build-android/jni-package"
echo "Build APK: cd apps/android && ./gradlew assembleDebug"
