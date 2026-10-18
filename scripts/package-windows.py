#!/usr/bin/env python3
"""Deploy the Qt app and produce portable/installer/source release assets.

Run from a Windows environment with Qt's matching MinGW bin directory on PATH.
The output version directory must not exist, to avoid shipping stale files.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def zip_tree(folder, output):
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(folder.rglob('*')):
            if path.is_file():
                archive.write(path, Path(folder.name) / path.relative_to(folder))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'qt-dir', 'compiler-dir', 'sevenzip-dll', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--makensis', type=Path)
    parser.add_argument('--packages-only', action='store_true',
                        help='Repackage an already deployed, matching version directory')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    version = re.search(r'#define ARCHIVER_VERSION "([\d.]+)"',
                        (repo / 'core/include/archiver.h').read_text()).group(1)
    build, qt, compiler, engine, output = (x.resolve() for x in (
        args.build_dir, args.qt_dir, args.compiler_dir, args.sevenzip_dll, args.output_dir))
    output.mkdir(parents=True, exist_ok=True)
    stage = output / f'milkzip-{version}-windows-x64'
    if stage.exists() and not args.packages_only:
        raise SystemExit(f'Refusing to reuse an existing release directory: {stage}')
    if args.packages_only:
        for name in ('milkzip.exe', 'libarchiver.dll'):
            if (stage / name).read_bytes() != (build / name).read_bytes():
                raise SystemExit('Stage differs from the build; deploy a new release directory first')
    else:
        deploy(repo, build, qt, compiler, engine, stage)

    portable = output / (stage.name + '-portable.zip')
    zip_tree(stage, portable)
    source = output / f'milkzip-{version}-source.zip'
    git = ['git', '-c', 'safe.directory=' + repo.as_posix()]
    names = subprocess.check_output([*git, 'ls-files', '--cached', '--others',
                                     '--exclude-standard', '-z'], cwd=repo).decode().split('\0')
    with zipfile.ZipFile(source, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(set(filter(None, names))):
            path = (repo / name).resolve()
            if not path.is_relative_to(repo) or not path.is_file():
                raise SystemExit(f'Invalid source file: {name}')
            archive.write(path, f'milkzip-{version}-source/' + name)
    assets = [portable, source]
    if args.makensis:
        metadata = build / 'release-meta'
        metadata.mkdir(exist_ok=True)
        file_list = metadata / 'uninstall-files.nsh'
        files = [p.relative_to(stage) for p in stage.rglob('*') if p.is_file()]
        dirs = sorted((p.relative_to(stage) for p in stage.rglob('*') if p.is_dir()),
                      key=lambda p: len(p.parts), reverse=True)
        lines = [f'Delete "$INSTDIR\\{str(p)}"' for p in files]
        lines += [f'RMDir "$INSTDIR\\{str(p)}"' for p in dirs]
        file_list.write_text('\n'.join(lines) + '\n', encoding='utf-8-sig')
        installer = output / (stage.name + '-setup.exe')
        run([args.makensis.resolve(), '-V2', '-WX', '-INPUTCHARSET', 'UTF8',
             '-DVERSION=' + version, '-DSTAGE=' + str(stage), '-DOUTPUT=' + str(installer),
             '-DAPP_ICON=' + str(repo / 'assets/milkzip.ico'),
             '-DUNINSTALL_LIST=' + str(file_list), repo / 'packaging/windows/installer.nsi'])
        assets.append(installer)
    checksums = ''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n'
                        for p in assets)
    (output / f'milkzip-{version}-SHA256SUMS.txt').write_text(checksums, encoding='ascii')
    for asset in assets:
        print(f'{asset.name}: {asset.stat().st_size / 1024 / 1024:.2f} MiB')


def deploy(repo, build, qt, compiler, engine, stage):
    stage.mkdir()
    for name in ('milkzip.exe', 'libarchiver.dll'):
        shutil.copy2(build / name, stage / name)
    shutil.copy2(engine, stage / '7z.dll')
    env = {**os.environ, 'PATH': str(compiler / 'bin') + os.pathsep +
           str(qt / 'bin') + os.pathsep + os.environ.get('PATH', '')}
    run([qt / 'bin/windeployqt.exe', '--release', '--no-compiler-runtime',
         '--no-opengl-sw', '--no-system-d3d-compiler', '--no-system-dxc-compiler',
         '--skip-plugin-types', 'generic,networkinformation,tls',
         '--translations', 'zh_CN', stage / 'milkzip.exe'], env=env)
    for name in ('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
        shutil.copy2(compiler / 'bin' / name, stage / name)
    (stage / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n', encoding='utf-8')
    shutil.copy2(repo / 'LICENSE', stage / 'LICENSE')
    shutil.copy2(repo / 'THIRD-PARTY-NOTICES.md', stage / 'THIRD-PARTY-NOTICES.md')
    shutil.copytree(repo / 'LICENSES', stage / 'LICENSES')
    for component in ('gcc', 'mingw-w64', 'winpthreads'):
        shutil.copytree(compiler / 'licenses' / component, stage / 'LICENSES/MinGW' / component)
    for name in ('qtbase-6.8.3.spdx.json', 'qttranslations-6.8.3.spdx.json'):
        shutil.copy2(qt / 'sbom' / name, stage / 'LICENSES/Qt' / name)
    guide = (repo / 'packaging/windows/README-zh-CN.txt').read_text(encoding='utf-8')
    (stage / 'README.txt').write_text(guide, encoding='utf-8-sig')

if __name__ == '__main__':
    main()
