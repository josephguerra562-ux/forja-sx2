"""Export the working PS4 dependencies for hosted CI. Run with Python 3 in WSL."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openorbis', type=Path, default=Path('/opt/pacbrew/ps4/openorbis'))
    parser.add_argument('--sysroot', type=Path, default=Path('/root/dolphinps4-build/sysroot'))
    parser.add_argument('--mesa', type=Path, default=Path('/root/dolphinps4-build/build/mesa'))
    parser.add_argument('--windows-sdk', type=Path, default=Path('/mnt/c/OpenOrbis/PS4Toolchain'))
    parser.add_argument('--output', type=Path, default=repo / 'ps4/ci/build/ps4sx2-sdk-v2.tar.gz')
    parser.add_argument('--llvm-ar', default='llvm-ar-21', help='LLVM archiver for materializing thin libraries')
    args = parser.parse_args()
    if os.name == 'nt':
        parser.error('Run this exporter in WSL so the Linux SDK is accessible.')
    if args.output.exists():
        parser.error('Output already exists. Choose a new --output to preserve the existing snapshot.')

    files = {}

    def add(source, destination):
        source = Path(source)
        if not source.is_file():
            raise FileNotFoundError(source)
        files[destination] = source

    def tree(source, destination, pattern='*'):
        source = Path(source)
        if not source.is_dir():
            raise FileNotFoundError(source)
        for item in sorted(source.rglob(pattern)):
            if item.is_file():
                add(item, destination + '/' + item.relative_to(source).as_posix())

    for folder in ('include', 'cmake', 'usr/include'):
        tree(args.openorbis / folder, 'linux/openorbis/' + folder)
    # Static target libraries, CRT objects and the OpenOrbis import stubs used by this target.
    for folder in ('lib', 'usr/lib'):
        for pattern in ('*.a', '*.o'):
            tree(args.openorbis / folder, 'linux/openorbis/' + folder, pattern)
    for name in ('kernel', 'SceLibcInternal', 'SceNet', 'SceNetCtl', 'SceHttp', 'SceSsl',
                 'SceAudioOut', 'SceSysmodule', 'SceSystemService', 'SceUserService',
                 'SceVideoOut', 'ScePad'):
        add(args.openorbis / 'lib' / f'lib{name}.so', f'linux/openorbis/lib/lib{name}.so')
    add(args.openorbis / 'link.x', 'linux/openorbis/link.x')
    for name in ('ld.lld', 'llvm-ar', 'llvm-ranlib', 'llvm-strip', 'llvm-readelf', 'create-fself'):
        add(args.openorbis / 'bin' / name, 'linux/openorbis/bin/' + name)
    for folder in ('include', 'libc-overlay'):
        tree(args.sysroot / folder, 'linux/sysroot/' + folder)
    tree(args.sysroot / 'lib', 'linux/sysroot/lib', '*.a')
    tree(args.mesa, 'linux/mesa', '*.a')
    tree(repo / 'ps4/deps/prefix/include', 'linux/prefix/include')
    tree(repo / 'ps4/deps/prefix/lib', 'linux/prefix/lib', '*.a')

    for name in ('PkgTool.Core.exe', 'PkgTool.Core.dll', 'PkgTool.Core.runtimeconfig.json',
                 'LibOrbisPkg.Core.dll', 'LibOrbisPkg.dll', 'create-gp4.exe', 'LICENSE.txt', 'README.md'):
        add(args.windows_sdk / 'bin/windows' / name, 'windows/openorbis/bin/windows/' + name)
    for name in ('sce_sys/about/right.sprx', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
        add(args.windows_sdk / 'samples/graphics' / name, 'windows/openorbis/samples/graphics/' + name)
    for name in ('Gontserrat-Regular.ttf', 'OFL.txt'):
        add(args.windows_sdk / 'samples/font/assets/fonts' / name,
            'windows/openorbis/samples/font/assets/fonts/' + name)

    # Carry the notices available in the SDK and source trees alongside the binaries.
    notice_roots = {
        'openorbis': args.openorbis,
        'windows-packaging': args.windows_sdk / 'bin/windows',
        'libcxx': args.sysroot.parent / 'src/llvm-project-21.1.6.src/libcxx',
        'libcxxabi': args.sysroot.parent / 'src/llvm-project-21.1.6.src/libcxxabi',
        'mesa': args.sysroot.parent / 'src/mesa',
        'local-deps': repo / 'ps4/deps/src',
    }
    for label, root in notice_roots.items():
        if root.is_dir():
            print(f'Collecting {label} license notices...', flush=True)
            for directory, folders, names in os.walk(root):
                folders[:] = [name for name in folders if name not in
                              ('.git', 'build', 'test', 'tests', 'benchmarks', '__pycache__')]
                for name in names:
                    if name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                        item = Path(directory) / name
                        if item.is_file():
                            add(item, 'metadata/licenses/' + label + '/' + item.relative_to(root).as_posix())

    required = (
        'linux/sysroot/lib/libc++.a', 'linux/sysroot/lib/libc++abi.a',
        'linux/mesa/src/amd/vulkan/libvulkan_radeon.a',
        'linux/prefix/lib/libzstd.a', 'linux/prefix/lib/liblz4.a',
        'linux/openorbis/lib/crt1.o', 'linux/openorbis/usr/lib/libgnm.a',
    )
    for name in required:
        if name not in files:
            raise RuntimeError(f'Missing SDK build prerequisite: {name}')
    revision = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    manifest = {'format': 2, 'project_commit': revision, 'materialized_archives': [],
                'description': 'PS4SX2 development SDK snapshot; prebuilt PS4 dependencies', 'files': {}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(f'Packing {len(files)} SDK files into {args.output}', flush=True)
    # Dereference SDK tool aliases so the archive is portable and contains no symlinks.
    with tempfile.TemporaryDirectory(prefix='ps4-sdk-archives-') as temporary, \
            tarfile.open(args.output, 'w:gz', compresslevel=6, dereference=True) as tar:
        for index, (name, source) in enumerate(sorted(files.items())):
            if source.suffix == '.a':
                with source.open('rb') as stream:
                    thin = stream.read(8) == b'!<thin>\n'
                if thin:
                    # Meson thin archives only reference .o files in the original build tree.
                    # Append their members to a NEW regular archive, preserving duplicate names.
                    # Never mutate the developer's original library or unpack by member basename.
                    standalone = Path(temporary) / f'{index}.a'
                    print(f'Materializing {name}', flush=True)
                    subprocess.run([args.llvm_ar, 'qcLs', str(standalone), str(source.resolve())], check=True)
                    with standalone.open('rb') as stream:
                        if stream.read(8) != b'!<arch>\n':
                            raise RuntimeError(f'Failed to create standalone library: {name}')
                    source = standalone
                    manifest['materialized_archives'].append(name)
            with source.open('rb') as stream:
                manifest['files'][name] = hashlib.file_digest(stream, 'sha256').hexdigest()
            tar.add(source, arcname=name, recursive=False)
        data = json.dumps(manifest, indent=2).encode() + b'\n'
        info = tarfile.TarInfo('metadata/manifest.json')
        info.size = len(data)
        info.mode = 0o644
        tar.addfile(info, io.BytesIO(data))
    with args.output.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    args.output.with_suffix(args.output.suffix + '.sha256').write_text(
        f'{digest}  {args.output.name}\n', encoding='utf-8')
    print(f'Archive: {args.output}\nPS4_SDK_SHA256={digest}')
    print('Upload the archive to a stable HTTPS asset URL, then set PS4_SDK_URL in Actions variables.')


if __name__ == '__main__':
    main()
