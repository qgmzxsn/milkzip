#!/bin/sh
# Download 7-Zip 26.04 source. The source is not part of the MIT tree.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dest="$root/third_party/7zip"
src="$dest/src"
tarball="$dest/7z2604-src.tar.xz"
url="https://www.7-zip.org/a/7z2604-src.tar.xz"
sum="9691944c0fe0d01bb49373a704fb983fd33bc98b1738695179dfbf99ac1734f6"

mkdir -p "$dest"
if [ -f "$src/CPP/7zip/Bundles/Format7zF/makefile.gcc" ]; then
  exit 0
fi
if [ ! -f "$tarball" ]; then
  curl -fsSL -o "$tarball" "$url"
fi
if command -v sha256sum >/dev/null 2>&1; then
  echo "$sum  $tarball" | sha256sum -c -
else
  echo "$sum  $tarball" | shasum -a 256 -c -
fi
mkdir -p "$src"
tar -xJf "$tarball" -C "$src"
