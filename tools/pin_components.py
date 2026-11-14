# SPDX-License-Identifier: MIT
"""Makes the versions in an engine folder the ones Glasir is tested with.

Glasir installs the version of mpv, ReShade and DLSS 5's parts that
src/app/components.json names (src/app/components.h). To move to newer ones:

    Glasir.exe --setup --newest --no-dialogs      (on a copy of the app folder)
    ... play with the card: SDR and HDR, DLSS 5, frame generation ...
    python tools/pin_components.py --engine COPY\\engine

This writes components.json from engine\\components.ini, where Glasir keeps
each part's version, address and SHA-256 as it installed them. Build again,
and `Glasir.exe --setup` installs exactly these. `--print` shows the result
without writing it.
"""
import argparse
import configparser
import json
import os
import sys
import urllib.request

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ABOUT = ('The versions of the downloaded programs this Glasir was tested with (src/app/components.h). '
         "Written by tools/pin_components.py from an engine folder's components.ini.")
# The part's name in components.json: its section in components.ini and its
# keys' prefix there ('' for a section of its own).
PARTS = [('mpv', 'mpv', ''), ('reshade', 'reshade', ''), ('feeder', 'dlss5', 'feeder'), ('renodx', 'dlss5', 'renodx'),
         ('dlssnr', 'dlss5', 'dlssnr'), ('dlss', 'dlss5', 'dlss'),
         ('reshade_fxh', 'dlss5', 'reshade_fxh')]


def size_of(url):
    """The download's size as the server says it, or None."""
    request = urllib.request.Request(url, method='HEAD', headers={
        'User-Agent': 'Glasir-pin-components', 'Referer': 'https://reshade.me/'})
    try:
        with urllib.request.urlopen(request, timeout=30) as answer:
            length = answer.headers.get('Content-Length')
            return int(length) if length else None
    except OSError as e:
        print(f'  no size for {url}: {e}')
        return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--engine', default=os.path.join(REPO, 'out', 'Glasir', 'engine'))
    p.add_argument('--out', default=os.path.join(REPO, 'src', 'app', 'components.json'))
    p.add_argument('--print', action='store_true', help='show the result, write nothing')
    a = p.parse_args()

    ini = configparser.ConfigParser(interpolation=None)
    path = os.path.join(a.engine, 'components.ini')
    if not ini.read(path):
        print(f'{path} cannot be read')
        return 1
    try:
        with open(a.out, encoding='utf-8') as f:
            before = json.load(f)
    except (OSError, ValueError):
        before = {}

    pins = {'about': ABOUT}
    for part, section, prefix in PARTS:
        def key(what):
            return what if not prefix else prefix if what == 'version' else f'{prefix}_{what}'
        values = {what: ini.get(section, key(what), fallback='') for what in ('version', 'source', 'sha256')}
        if not all(values.values()):
            print(f'{part}: components.ini has no {" or ".join(w for w, v in values.items() if not v)} for it. '
                  f'Install it with this Glasir first (Glasir.exe --setup).')
            return 1
        pin = {'version': values['version'], 'url': values['source'], 'sha256': values['sha256'].lower()}
        old = before.get(part) if isinstance(before.get(part), dict) else {}
        size = old.get('size') if old.get('url') == pin['url'] else size_of(pin['url'])
        if size:
            pin['size'] = size
        pins[part] = pin
        changed = '' if old.get('version') == pin['version'] and old.get('sha256') == pin['sha256'] else \
            f"   (was {old.get('version', 'not pinned')})"
        print(f"{part:12} {pin['version']}{changed}")

    text = json.dumps(pins, indent=2) + '\n'
    if a.print:
        print(text)
    else:
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print(f'wrote {os.path.normpath(a.out)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
