param(
    [Parameter(Mandatory = $true)][string]$QtDir,
    [Parameter(Mandatory = $true)][string]$CompilerDir,
    [Parameter(Mandatory = $true)][string]$SevenZipDll,
    [string]$BuildDir,
    [string]$CMake = 'cmake',
    [switch]$NoTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$BuildDir) { $BuildDir = Join-Path $root 'build-windows-qt' }
$qt = (Resolve-Path -LiteralPath $QtDir).Path
$compiler = (Resolve-Path -LiteralPath $CompilerDir).Path
$engine = (Resolve-Path -LiteralPath $SevenZipDll).Path
$source = Join-Path $root 'third_party\7zip\src'
if (!(Test-Path -LiteralPath (Join-Path $source 'CPP\7zip\Archive\IArchive.h'))) {
    $deps = Join-Path $root 'third_party\7zip'
    New-Item -ItemType Directory -Path $deps -Force | Out-Null
    $archive = Join-Path $deps '7z2604-src.tar.xz'
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri 'https://www.7-zip.org/a/7z2604-src.tar.xz' -OutFile $archive
    }
    $expected = '9691944c0fe0d01bb49373a704fb983fd33bc98b1738695179dfbf99ac1734f6'
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
        throw '7-Zip source checksum mismatch.'
    }
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    & tar -xJf $archive -C $source
    if ($LASTEXITCODE -ne 0) { throw '7-Zip source extraction failed.' }
}
$oldPath = $env:PATH
try {
    $env:PATH = (Join-Path $compiler 'bin') + ';' + (Join-Path $qt 'bin') + ';' + $oldPath
    $seven = Join-Path (Split-Path -Parent $engine) '7z.exe'
    $arguments = @('-S', $root, '-B', $BuildDir, '-G', 'MinGW Makefiles',
        "-DCMAKE_CXX_COMPILER=$compiler/bin/g++.exe", "-DSEVENZIP_SRC=$source",
        "-DCMAKE_PREFIX_PATH=$qt", '-DCMAKE_BUILD_TYPE=Release',
        "-DARCHIVER_TEST_ENGINE=$engine")
    if (Test-Path -LiteralPath $seven) { $arguments += "-DARCHIVER_TEST_7Z=$seven" }
    & $CMake @arguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & $CMake --build $BuildDir -j 6
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if (!(Test-Path -LiteralPath (Join-Path $BuildDir 'milkzip.exe'))) {
        throw 'GUI not built. Check that Qt Widgets and Concurrent are installed.'
    }
    Copy-Item -LiteralPath $engine -Destination (Join-Path $BuildDir '7z.dll')
    foreach ($dll in 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll') {
        Copy-Item -LiteralPath (Join-Path $compiler "bin\$dll") -Destination $BuildDir
    }
    & (Join-Path $qt 'bin\windeployqt.exe') --release --no-compiler-runtime --no-opengl-sw `
        --no-system-d3d-compiler --no-system-dxc-compiler --translations zh_CN `
        --skip-plugin-types 'generic,networkinformation,tls' (Join-Path $BuildDir 'milkzip.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
    if (!$NoTests) {
        $ctest = Join-Path (Split-Path -Parent (Get-Command $CMake).Source) 'ctest.exe'
        & $ctest --test-dir $BuildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    Write-Output (Join-Path $BuildDir 'milkzip.exe')
} finally {
    $env:PATH = $oldPath
}
