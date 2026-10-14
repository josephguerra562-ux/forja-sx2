"""Download a maintainer-provided SDK snapshot, verify it, and extract one host component."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--component', required=True, choices=('linux', 'windows'))
    args = parser.parse_args()
    url = os.environ.get('PS4_SDK_URL', '')
    digest = os.environ.get('PS4_SDK_SHA256', '').lower()
    if not url.startswith('https://') or not re.fullmatch(r'[0-9a-f]{64}', digest):
        parser.error('Set repository Actions variables PS4_SDK_URL (public HTTPS archive) and '
                     'PS4_SDK_SHA256 (64 hex characters). See README: GitHub Actions.')
    if args.destination.exists():
        parser.error('SDK destination must not exist; use a fresh runner temporary directory.')

    with tempfile.TemporaryDirectory(prefix='ps4-sdk-download-') as tmp:
        archive = Path(tmp) / 'sdk.tar.gz'
        request = urllib.request.Request(url, headers={'User-Agent': 'PS4SX2-CI'})
        checksum = hashlib.sha256()
        with urllib.request.urlopen(request, timeout=120) as response, archive.open('wb') as out:
            if not response.url.startswith('https://'):
                raise RuntimeError('SDK download redirected away from HTTPS.')
            while chunk := response.read(1024 * 1024):
                checksum.update(chunk)
                out.write(chunk)
        if checksum.hexdigest() != digest:
            raise RuntimeError('SDK SHA-256 mismatch; refusing to extract or execute it.')

        args.destination.mkdir(parents=True)
        count = 0
        with tarfile.open(archive, 'r:gz') as tar:
            for member in tar:
                path = PurePosixPath(member.name)
                if (path.is_absolute() or '..' in path.parts or '\\' in member.name
                        or ':' in member.name or not path.parts):
                    raise RuntimeError(f'Unsafe SDK archive path: {member.name}')
                # Exporter only produces regular files. Never extract links or devices.
                if not member.isfile():
                    raise RuntimeError(f'Unsupported SDK archive entry: {member.name}')
                if path.parts[0] not in (args.component, 'metadata'):
                    continue
                target = args.destination.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, target.open('xb') as out:
                    shutil.copyfileobj(source, out)
                if os.name != 'nt':
                    target.chmod(member.mode & 0o777)
                count += 1
        required = ('linux/openorbis/cmake/ps4.cmake' if args.component == 'linux'
                    else 'windows/openorbis/bin/windows/PkgTool.Core.exe')
        if not (args.destination / required).is_file():
            raise RuntimeError(f'SDK snapshot is missing {required}')
        print(f'Restored {args.component} SDK: {count} files; SHA256 {digest}')


if __name__ == '__main__':
    main()
