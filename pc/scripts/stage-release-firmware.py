"""Stage the four desktop OTA inputs from an authenticated-by-digest public release."""
import argparse
import hashlib
from functools import lru_cache
import json
import re
import struct
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESTINATION = ROOT / 'pc/src-tauri/firmware/esp32-p4'
DOWNLOAD_ROOT = 'https://github.com/ai-maker-lab/HachimoDock/releases/download'
RELEASE_API = 'https://api.github.com/repos/ai-maker-lab/HachimoDock/releases'
PROFILES = {
    'wifi6-v1': ('firmware.bin', 'wifi6', 1, 199),
    'wifi6-v3': ('firmware-v3.bin', 'wifi6', 300, 399),
    'pico-v1': ('firmware-pico-v1.bin', 'pico', 1, 199),
    'pico-v3': ('firmware-pico-v3.bin', 'pico', 300, 399),
}
APPLICATION_OFFSET = 0x10000
MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
FACTORY_BYTES = 0x1780000


def verify(condition, message):
    if not condition:
        raise ValueError(message)


@lru_cache(maxsize=1)
def published_assets(tag):
    with urllib.request.urlopen(f'{RELEASE_API}/tags/{tag}', timeout=15) as response:
        release = json.loads(response.read(512 * 1024 + 1))
    verify(release['tag_name'] == tag and not release['draft'], 'Unexpected public release')
    return {asset['name']: asset['id'] for asset in release['assets']}


def download(name, size, tag):
    try:
        asset_id = published_assets(tag).get(name)
        if isinstance(asset_id, int) and asset_id > 0:
            request = urllib.request.Request(f'{RELEASE_API}/assets/{asset_id}',
                                             headers={'Accept': 'application/octet-stream'})
            with urllib.request.urlopen(request, timeout=25) as response:
                data = response.read(size + 1)
            if data.startswith(b'PK\x03\x04'):
                return data
    except (urllib.error.URLError, TimeoutError):
        pass
    with urllib.request.urlopen(f'{DOWNLOAD_ROOT}/{tag}/{name}', timeout=25) as response:
        return response.read(size + 1)


def extract(profile, release, local_assets, temporary):
    file, board, low, high = PROFILES[profile]
    version = release['desktopVersion']
    stem = f'HachimiDock_{version}_{profile}_FULL-FACTORY'
    archive = stem + '.zip'
    item, = (entry for entry in release['artifacts'] if entry['file'] == archive)
    app, = (entry for entry in release['firmware'] if entry['file'] == file)
    size = item['bytes']
    verify(0 < size <= MAX_ARCHIVE_BYTES, f'{profile}: invalid archive size')
    if local_assets:
        with (local_assets / archive).open('rb') as stream:
            data = stream.read(size + 1)
    else:
        data = download(archive, size, release['releaseTag'])
    verify(len(data) == size and hashlib.sha256(data).hexdigest() == item['sha256'],
           f'{profile}: factory package integrity check failed')
    archive_path = temporary / archive
    archive_path.write_bytes(data)
    with zipfile.ZipFile(archive_path) as package:
        members = package.infolist()
        matches = [member for member in members if member.filename == stem + '.bin']
        verify(len(matches) == 1 and matches[0].file_size == FACTORY_BYTES,
               f'{profile}: invalid factory image')
        length = app['bytes']
        verify(288 <= length <= 0x300000, f'{profile}: invalid application size')
        with package.open(matches[0]) as image:
            image.seek(APPLICATION_OFFSET)
            application = image.read(length)
    verify(len(application) == length and hashlib.sha256(application).hexdigest() == app['sha256'],
           f'{profile}: application integrity check failed')
    verify(application[0] == 0xE9 and struct.unpack_from('<H', application, 12) == (18,)
           and struct.unpack_from('<HH', application, 15) == (low, high)
           and application[48:80].split(b'\0')[0].decode('ascii') == release['firmwareVersion'] + '-' + board
           and app['boardModel'] == board and app['minChipRevision'] == low and app['maxChipRevision'] == high
           and hashlib.sha256(application[:-32]).digest() == application[-32:],
           f'{profile}: incorrect board, chip revision, version or image checksum')
    (temporary / file).write_bytes(application)
    return file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets-dir', type=Path, help='Use already downloaded factory ZIP files.')
    args = parser.parse_args()
    release = json.loads((ROOT / 'latest-release.json').read_text(encoding='utf-8'))
    version = release['desktopVersion']
    verify(re.fullmatch(r'\d+\.\d+\.\d+', version) and release['releaseTag'] == 'v' + version,
           'Invalid public release tag')
    DESTINATION.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=DESTINATION, prefix='.staging-') as directory:
        temporary = Path(directory)
        files = [extract(profile, release, args.assets_dir, temporary) for profile in PROFILES]
        for file in files:
            (temporary / file).replace(DESTINATION / file)
    print(f'Staged four verified OTA images for HachimiDock {version}')


if __name__ == '__main__':
    main()
