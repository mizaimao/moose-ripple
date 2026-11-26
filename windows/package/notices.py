#!/usr/bin/env python3
# Third-party notices for the Windows Vista and Windows 7 display driver package.
#
#   notices.py --kmd windows/kmd --mesa SRC BUILD [--mesa SRC BUILD ...]
#              --toolchain LLVM_MINGW --licenses DIR --out THIRD-PARTY-NOTICES.txt
#
# What the package's binaries are built from decides what is printed:
# - every source and header file that goes into viogpu_d3d10.dll, taken from each Mesa build
#   directory's own records (ninja's input list for the DLL and its header dependencies);
# - every file of the kernel driver's source folder;
# - the licence files of the C and C++ runtime that llvm-mingw links into the DLLs.
# The copyright and licence comments of those files are collected, sorted and printed once
# each. DIR holds the licence files that are in neither tree (make-release.sh fetches them):
# libcxx.txt, libcxxabi.txt, libunwind.txt, compiler-rt.txt (LLVM's LICENSE.TXT of each) and
# blake3.txt.
#
# The script also lists, on standard error, every file whose notice names a copyleft licence,
# so that a new one cannot slip into a package unseen. Bison's parser skeleton is expected
# there: its notice carries the exception that lets the generated parser be used under any
# licence.
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

TARGET = 'src/gallium/targets/d3d10umd/viogpu_d3d10.dll'

NEED = re.compile(
    r'copyright\s*(?:©|\(c\)|[0-9]|\(C\)|\(?[A-Z].*20[0-9][0-9])'
    r'|^\s*(?:Copyright|SPDX-FileCopyrightText)\s*[:(]'
    r'|Permission is (?:hereby )?granted|Permission to use, copy|redistribution and use'
    r'|licensed under|public domain|as a special exception|SPDX-License-Identifier'
    r'|all rights reserved|\(c\)\s*\d{4}', re.I | re.M)
COMMENT = re.compile(r'/\*([\s\S]*?)\*/|((?:^[ \t]*//[^\n]*(?:\n|$))+)|((?:^[ \t]*[#;][^\n]*(?:\n|$))+)', re.M)
COPYLEFT = re.compile(r'gnu (?:lesser|library|general|affero)|\bL?GPL\b|mozilla public|\bMPL\b', re.I)
GRANT = re.compile(r'^(?:Permission is hereby granted|Permission to use, copy'
                   r'|Redistribution and use in source and binary forms)', re.I | re.M)


def read(path):
    data = path.read_bytes()
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return data.decode('utf-16', errors='replace')
    return data.decode('utf-8', errors='replace')


def strip_comment(block):
    lines = [re.sub(r'^\s*\*+(?: |$)', '', line).rstrip() for line in block.strip().splitlines()]
    lines = [line for line in lines if not re.fullmatch(r'\s*[=*#;/-]{3,}\s*', line)]
    return '\n'.join(lines).strip()


def notices_of(text):
    found = []
    for m in COMMENT.finditer(text):
        block = m[1] or m[2] or m[3] or ''
        if m[2]:
            block = re.sub(r'^[ \t]*// ?', '', block, flags=re.M)
        elif m[3]:
            block = re.sub(r'^[ \t]*[#;]+ ?', '', block, flags=re.M)
        block = strip_comment(block)
        if NEED.search(block):
            found.append(block)
    return found


def mesa_files(src, build):
    """Every file under src or build that goes into the DLL."""
    def ninja(*args):
        return subprocess.check_output(['ninja', '-C', str(build), '-t', *args], text=True).splitlines()
    inputs = ninja('inputs', TARGET)
    objects = {p for p in inputs if p.endswith(('.obj', '.o'))}
    files = set()
    take = False
    for line in ninja('deps'):
        if not line.startswith(' '):
            take = line.split(': #deps ')[0] in objects
        elif take:
            files.add((build / line.strip()).resolve())
    for name in inputs:
        if not name.endswith(('.obj', '.o', '.a', '.dll', '.py', '.build')):
            files.add((build / name).resolve())
    src, build = src.resolve(), build.resolve()
    result = {}
    for p in files:
        if not p.is_file():
            continue
        if p.is_relative_to(src):
            result['mesa/' + str(p.relative_to(src))] = p
        elif p.is_relative_to(build):
            result['mesa/generated/' + str(p.relative_to(build))] = p
    return result


def section(heading, body):
    return heading + '\n' + '-' * len(heading) + '\n\n' + body.strip() + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kmd', type=Path, required=True)
    ap.add_argument('--mesa', type=Path, nargs=2, action='append', required=True, metavar=('SRC', 'BUILD'))
    ap.add_argument('--toolchain', type=Path, required=True)
    ap.add_argument('--licenses', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()

    files = {}
    for src, build in args.mesa:
        files.update(mesa_files(src, build))
    mesa_count = len(files)
    kmd = args.kmd.resolve()
    for p in sorted(kmd.rglob('*')):
        if p.is_file() and p.suffix.lower() in ('.c', '.cpp', '.h', '.rc', '.inx', '.ver'):
            files['kmd/' + str(p.relative_to(kmd))] = p

    notices = collections.defaultdict(set)
    copyleft = collections.defaultdict(list)
    for label, p in sorted(files.items()):
        for block in notices_of(read(p)):
            notices[block].add(label)
            if COPYLEFT.search(block):
                copyleft[label].append(' '.join(block.split())[:100])

    # A notice is a heading (who holds the copyright) and usually a grant (the licence
    # paragraphs). Notices with the same grant are printed together, the grant once.
    groups = collections.defaultdict(dict)
    grants = {}
    flat = lambda text: ' '.join(text.split())
    for block, labels in sorted(notices.items(), key=lambda item: min(item[1])):
        m = GRANT.search(block)
        if m and m.start() > 0:
            head, grant = block[:m.start()].strip(), block[m.start():].strip()
        else:
            head, grant = block, ''
        key = flat(grant) if grant else ('alone', flat(head))
        grants.setdefault(key, grant)
        seen = groups[key].get(flat(head))
        if seen:
            seen[1].update(labels)
        else:
            groups[key][flat(head)] = (head, set(labels))
    by_file = []
    for key, heads in groups.items():
        for head, labels in heads.values():
            by_file.append('[' + min(labels) + ']\n' + head)
        if grants[key]:
            by_file.append(grants[key])

    mesa_src = args.mesa[0][0]
    share = args.toolchain / 'x86_64-w64-mingw32/share/mingw32'
    official = lambda p: p.read_text(errors='replace').replace('\r\n', '\n').strip()
    spdx = lambda p: official(p).split('License-Text:', 1)[1]

    out = ["""Third-party notices for the moose-ripple display driver for Windows Vista and Windows 7

viogpu3d.sys, the kernel driver, is derived from the viogpu3d driver of the
virtio-win project (kvm-guest-drivers-windows, BSD-3-Clause):

    https://github.com/virtio-win/kvm-guest-drivers-windows
    https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943

It is linked with the display miniport library of Microsoft's Windows
Driver Kit 7.1.

viogpu_d3d10.dll and viogpu_d3d10.x86.dll, the user-mode driver for 64-bit
and 32-bit programs, are Mesa 24.3.4 (MIT) with this project's changes. They
contain Direct3D 9 driver files from Yttrium's Mesa fork and from
vmlab-win3d (both MIT), and statically linked C and C++ runtime code from
MinGW-w64 and LLVM (llvm-mingw).

    https://archive.mesa3d.org/mesa-24.3.4.tar.xz
    https://github.com/arehnman/virtio-win-mesa
    https://github.com/VMLabDev/vmlab-win3d
    https://github.com/mstorsjo/llvm-mingw

This project's source, with every change to the above:

    https://github.com/mizaimao/moose-ripple/tree/windows/windows

The sections below reproduce the licence and copyright notices of those
sources. A source path in brackets names a file that carries the notice
after it; a licence paragraph shared by several notices is printed once,
after them. The project's own licence is in LICENSE.txt.
"""]
    out.append(section('virtio-win (kernel driver)', official(kmd / 'LICENSE')))
    out.append(section('virtio-win virtio library (kernel driver)', official(kmd / 'VirtIO/LICENSE')))
    out.append(section('Mesa component licences', official(mesa_src / 'docs/license.rst')))
    out.append(section('Mesa MIT licence', spdx(mesa_src / 'licenses/MIT')))
    out.append(section('Apache License 2.0 (Khronos headers and others in Mesa)', spdx(mesa_src / 'licenses/Apache-2.0')))
    out.append(section('Boost Software License 1.0', spdx(mesa_src / 'licenses/BSL-1.0')))
    out.append(section('SGI Free Software License B, version 2.0', spdx(mesa_src / 'licenses/SGI-B-2.0')))
    out.append(section('BLAKE3 (in Mesa, src/util/blake3)', official(args.licenses / 'blake3.txt')))
    out.append(section('MinGW-w64 runtime', official(share / 'COPYING.MinGW-w64-runtime.txt')))
    first = True
    for project in ('libcxx', 'libcxxabi', 'libunwind', 'compiler-rt'):
        body = official(args.licenses / (project + '.txt'))
        pieces = body.split('Legacy LLVM', 1)
        if first or len(pieces) != 2:
            out.append(section('LLVM ' + project, body))
        else:
            out.append(section('LLVM ' + project + ' (the Apache 2.0 terms with LLVM exceptions as printed above)',
                               'Legacy LLVM' + pieces[1]))
        first = False
    out.append(section('Notices by source file', '\n\n'.join(by_file)))
    text = '\n'.join(out).replace('\r', '')
    args.out.write_bytes(text.replace('\n', '\r\n').encode('utf-8'))

    print(f'notices.py: {mesa_count} Mesa files, {len(files) - mesa_count} kernel driver files, '
          f'{len(notices)} notices, {len(text.splitlines())} lines', file=sys.stderr)
    for label, lines in sorted(copyleft.items()):
        print(f'notices.py: copyleft named in {label}: {lines[0]}', file=sys.stderr)
    unexpected = [label for label in copyleft if not re.search(r'bison|\.tab\.|_parse\.|parser\.', label, re.I)]
    if unexpected:
        print('notices.py: a copyleft licence is named outside a Bison parser: ' + ', '.join(sorted(unexpected)),
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
