#!/bin/sh
# Link the shared Qt shell for 64-bit Windows.
# Needs a MinGW Qt 6.8 kit and a matching Linux Qt 6.8 (for moc). Neither is
# stored in this repo. If they are missing, this script stops.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
qt_win="${QT_WIN_PREFIX:-/tmp/qt-win/6.8.0/mingw_64}"
qt_host="${QT_HOST_PATH:-/tmp/qt-host/6.8.0/gcc_64}"

if ! command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then
  echo "blocker: x86_64-w64-mingw32-g++-posix is not installed." >&2
  echo "Qt 6.8's MinGW kit uses the POSIX thread model. The win32-thread g++ cannot be the one that links it." >&2
  exit 1
fi
if [ ! -f "$qt_win/lib/cmake/Qt6/Qt6Config.cmake" ] || [ ! -x "$qt_host/libexec/moc" ]; then
  echo "blocker: Windows Qt kit or Linux Qt 6.8 host tools are not installed." >&2
  echo "This machine's apt Qt is the Linux 6.4 kit and cannot be linked into a Windows exe." >&2
  echo "Download outside the repo, then re-run this script:" >&2
  echo "  aqt install-qt windows desktop 6.8.0 win64_mingw -O /tmp/qt-win" >&2
  echo "  aqt install-qt linux desktop 6.8.0 linux_gcc_64 -O /tmp/qt-host" >&2
  exit 1
fi

"$root/scripts/fetch-7zip.sh"
src="$root/third_party/7zip/src"
cmake -S "$root" -B "$root/build-windows-qt" \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64-posix.cmake" \
  -DSEVENZIP_SRC="$src" \
  -DCMAKE_BUILD_TYPE=Release \
  -DARCHIVER_WINDOWS_QT=ON \
  -DQT_HOST_PATH="$qt_host" \
  -DCMAKE_PREFIX_PATH="$qt_win" \
  -DCMAKE_FIND_ROOT_PATH="/usr/x86_64-w64-mingw32;$qt_win"
cmake --build "$root/build-windows-qt" -j"$(nproc)" --target milkzip

qtbin="$qt_win/bin"
stage="$root/build-windows-qt"
cp -f "$qtbin/Qt6Core.dll" "$qtbin/Qt6Gui.dll" "$qtbin/Qt6Widgets.dll" \
  "$qtbin/libstdc++-6.dll" "$qtbin/libgcc_s_seh-1.dll" "$qtbin/libwinpthread-1.dll" \
  "$stage/"
cp -f "$src/CPP/7zip/Bundles/Format7zF/b/mingw64/7z.dll" "$stage/7z.dll"
mkdir -p "$stage/platforms"
cp -f "$qt_win/plugins/platforms/qwindows.dll" "$stage/platforms/"

echo "Windows Qt shell: $stage/milkzip.exe"
echo "It loads libarchiver.dll and 7z.dll from the same directory."
