#!/usr/bin/env python3
# Downloads the freely licensed test images of manifest.json into
# tests/output/images/<set>/, once, and checks every file's SHA-1. Single
# slices of BBBC006, which is published only as zips of about 800 MB per
# focal plane, are cut out of the zips with HTTP range requests (the
# offsets are in the manifest). Nothing here is committed to the
# repository; the licences are in manifest.json.
#
#   tests/images/fetch.py           everything
#   tests/images/fetch.py NAME...   only these sets
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request
import zlib

here = os.path.dirname(os.path.abspath(__file__))
dest = os.path.join(os.path.dirname(here), 'output', 'images')
UA = 'gimp-fusion-tests/0.1 (test images)'


def get(url, rng=None, tries=4):
    for t in range(tries):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': UA})
            if rng:
                req.add_header('Range', 'bytes=%d-%d' % rng)
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except OSError as e:
            if t == tries - 1:
                raise
            print('  retrying %s (%s)' % (url, e))
            time.sleep(2 + 3 * t)


def zip_member(f):
    """one member of a remote zip, from its local header on"""
    off, size = f['zip_member_header_offset'], f['zip_member_compressed_bytes']
    head = get(f['container_zip_url'], (off, off + 29))
    sig, _, _, method, _, _, _, csize, _, nlen, xlen = struct.unpack('<IHHHHHIIIHH', head)
    if sig != 0x04034b50:
        raise ValueError('no zip entry at the offset')
    start = off + 30 + nlen + xlen
    data = get(f['container_zip_url'], (start, start + size - 1))
    return zlib.decompress(data, -15) if method == 8 else data


def main():
    m = json.load(open(os.path.join(here, 'manifest.json')))
    want = set(sys.argv[1:])
    bad = 0
    for s in m['sets']:
        if want and s['name'] not in want:
            continue
        d = os.path.join(dest, s['name'])
        os.makedirs(d, exist_ok=True)
        entries = [(f, '%02d' % i) for i, f in enumerate(s['files'])] + \
                  [(r, r['role']) for r in s.get('reference_results', [])]
        for f, stem in entries:
            ext = '.tif' if 'zip_member' in f else '.jpg'
            path = os.path.join(d, stem + ext)
            if os.path.exists(path) and hashlib.sha1(open(path, 'rb').read()).hexdigest() == f['sha1']:
                continue
            data = zip_member(f) if 'zip_member' in f else get(f['download_url'])
            sha = hashlib.sha1(data).hexdigest()
            if sha != f['sha1']:
                print('BAD  %s/%s: SHA-1 %s, expected %s' % (s['name'], stem, sha, f['sha1']))
                bad += 1
                continue
            with open(path + '.tmp', 'wb') as o:
                o.write(data)
            os.replace(path + '.tmp', path)
            print('got  %s/%s%s (%d bytes)' % (s['name'], stem, ext, len(data)))
    sys.exit(1 if bad else 0)


main()
