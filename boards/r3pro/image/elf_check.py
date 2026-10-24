#!/usr/bin/env python3
"""ELF analysis of a rootfs tree for the R3 Pro image recipe (check B5).

  elf_check.py removable ROOT PATH...    libraries only PATH... need (NEEDED
                                         closure), as change-list "rm" lines;
                                         the reasoning goes to stderr
  elf_check.py check ROOT [STOCKROOT]    check B5 (static part): every ELF is
                                         MIPS32 little-endian, its interpreter
                                         exists, every NEEDED library resolves
                                         inside ROOT. With STOCKROOT, problems
                                         an unchanged stock file already has in
                                         the stock tree are listed, not failed.

Library lookup follows the device's glibc loader: DT_RPATH/DT_RUNPATH
($ORIGIN expanded), then /lib and /usr/lib (the stock rootfs has no
ld.so.cache or ld.so.conf), symlinks resolved inside ROOT, and a candidate of
the wrong class/machine is skipped as ld.so does.
"""
import hashlib
import os
import struct
import sys

EM_MIPS = 8
ARCHS = {0x00000000: 'mips1', 0x10000000: 'mips2', 0x50000000: 'mips32', 0x70000000: 'mips32r2',
         0x90000000: 'mips32r6'}
OK_ARCHS = {'mips1', 'mips2', 'mips32', 'mips32r2'}  # what an XBurst X1000 runs
DEFAULT_LIB_DIRS = ['/lib', '/usr/lib']


def die(msg):
    sys.stderr.write('elf_check: %s\n' % msg)
    sys.exit(1)


def resolve(root, path, depth=0):
    """Absolute in-image path -> host path, following symlinks inside root; None if missing."""
    if depth > 40:
        return None
    parts = [p for p in path.split('/') if p]
    cur = ''
    for i, part in enumerate(parts):
        if part == '..':
            cur = os.path.dirname(cur)
            continue
        if part == '.':
            continue
        nxt = cur + '/' + part
        host = root + nxt
        if os.path.islink(host):
            target = os.readlink(host)
            base = target if target.startswith('/') else cur + '/' + target
            rest = '/'.join(parts[i + 1:])
            return resolve(root, base + ('/' + rest if rest else ''), depth + 1)
        if not os.path.lexists(host):
            return None
        cur = nxt
    return cur or '/'


class Elf:
    def __init__(self, data):
        self.data = data
        ident = data[:16]
        self.cls, self.endian = ident[4], ident[5]
        e = '<' if self.endian == 1 else '>'
        self.e = e
        if self.cls == 1:
            (self.type, self.machine, _, _, self.phoff, _, self.flags, _, self.phentsize,
             self.phnum) = struct.unpack_from(e + 'HHIIIIIHHH', data, 16)
        else:
            (self.type, self.machine, _, _, self.phoff, _, self.flags, _, self.phentsize,
             self.phnum) = struct.unpack_from(e + 'HHIQQQIHHH', data, 16)
        self.interp = None
        self.needed, self.soname, self.rpath = [], None, []
        if self.cls != 1:
            return
        loads, dyn = [], None
        for i in range(self.phnum):
            p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from(e + 'IIIIIIII', data,
                                                                                   self.phoff + i * self.phentsize)
            if p_type == 1:
                loads.append((p_vaddr, p_offset, p_filesz))
            elif p_type == 2:
                dyn = (p_offset, p_filesz)
            elif p_type == 3:
                self.interp = data[p_offset:p_offset + p_filesz].split(b'\0')[0].decode()
        if not dyn:
            return

        def v2o(v):
            for vaddr, off, sz in loads:
                if vaddr <= v < vaddr + sz:
                    return v - vaddr + off
            return None

        entries = []
        for i in range(dyn[1] // 8):
            tag, val = struct.unpack_from(e + 'iI', data, dyn[0] + i * 8)
            if tag == 0:
                break
            entries.append((tag, val))
        strtab = v2o(next((v for t, v in entries if t == 5), 0))
        if strtab is None:
            return

        def s(off):
            end = data.index(b'\0', strtab + off)
            return data[strtab + off:end].decode('utf-8', 'replace')

        for tag, val in entries:
            if tag == 1:
                self.needed.append(s(val))
            elif tag == 14:
                self.soname = s(val)
            elif tag in (15, 29):
                self.rpath += [p for p in s(val).split(':') if p]

    @property
    def arch(self):
        return ARCHS.get(self.flags & 0xf0000000, 'arch-0x%x' % (self.flags & 0xf0000000))

    def compatible(self):
        return self.cls == 1 and self.endian == 1 and self.machine == EM_MIPS and self.arch in OK_ARCHS


def scan(root):
    """{in-image path: Elf} for every regular ELF file."""
    out = {}
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            host = os.path.join(d, name)
            if os.path.islink(host) or not os.path.isfile(host):
                continue
            with open(host, 'rb') as f:
                if f.read(4) != b'\x7fELF':
                    continue
                f.seek(0)
                data = f.read()
            out['/' + os.path.relpath(host, root)] = Elf(data)
    return out


def find_lib(root, elf, path, name, elfs):
    if '/' in name:
        return resolve(root, name)
    dirs = [p.replace('$ORIGIN', os.path.dirname(path)).replace('${ORIGIN}', os.path.dirname(path))
            for p in elf.rpath] + DEFAULT_LIB_DIRS
    for d in dirs:
        r = resolve(root, d.rstrip('/') + '/' + name)
        if r and os.path.isfile(root + r):
            lib = elfs.get(r)
            if lib and lib.cls == elf.cls and lib.machine == elf.machine and lib.endian == elf.endian:
                return r
    return None


def problems(root, path, elf, elfs):
    out = []
    if not elf.compatible():
        out.append('not MIPS32 LE (class %d, data %d, machine %d, %s)' % (elf.cls, elf.endian, elf.machine,
                                                                          elf.arch))
        return out
    if elf.type == 1:  # ET_REL: kernel module / object file, nothing to load
        return out
    if elf.interp:
        r = resolve(root, elf.interp)
        if not r or not os.path.isfile(root + r):
            out.append('interpreter %s missing' % elf.interp)
    for n in elf.needed:
        if not find_lib(root, elf, path, n, elfs):
            out.append('NEEDED %s not found' % n)
    return out


def sha(root, path):
    with open(root + path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def cmd_check(root, stock=None):
    elfs = scan(root)
    stock_elfs = scan(stock) if stock else {}
    errors, known = [], []
    kinds = {}
    for path in sorted(elfs):
        elf = elfs[path]
        kind = {1: 'relocatable', 2: 'executables', 3: 'shared objects'}.get(elf.type, 'other')
        if elf.type == 2 and not elf.interp:
            kind = 'static executables'
        kinds[kind] = kinds.get(kind, 0) + 1
        p = problems(root, path, elf, elfs)
        if not p:
            continue
        same_as_stock = (path in stock_elfs and sha(root, path) == sha(stock, path) and
                         problems(stock, path, stock_elfs[path], stock_elfs) == p)
        (known if same_as_stock else errors).append('%s: %s' % (path, '; '.join(p)))
    print('  B5: %d ELF files (%s)' % (len(elfs), ', '.join('%d %s' % (v, k) for k, v in sorted(kinds.items()))))
    for k in known:
        print('  B5: stock, unchanged (not failed): ' + k)
    for e in errors:
        print('  B5: ERROR ' + e)
    return 1 if errors else 0


def cmd_removable(root, targets):
    elfs = scan(root)
    targets = ['/' + t.lstrip('/') for t in targets]
    for t in targets:
        if t not in elfs:
            die('%s is not an ELF file in the tree' % t)
    users = {}  # lib path -> set of ELF paths whose NEEDED resolves to it
    for path, elf in elfs.items():
        for n in elf.needed:
            r = find_lib(root, elf, path, n, elfs)
            if r:
                users.setdefault(r, set()).add(path)
    closure, todo = set(), list(targets)
    while todo:
        p = todo.pop()
        for n in elfs[p].needed:
            r = find_lib(root, elfs[p], p, n, elfs)
            if r and r not in closure and r not in targets:
                closure.add(r)
                todo.append(r)
    # Names a library goes by: its file name, its SONAME, every symlink to it.
    links = {}
    for d, _, files in os.walk(root):
        for name in files:
            host = os.path.join(d, name)
            if os.path.islink(host):
                r = resolve(root, '/' + os.path.relpath(host, root))
                if r:
                    links.setdefault(r, []).append('/' + os.path.relpath(host, root))
    removed = set(targets)
    report = []
    changed = True
    while changed:
        changed = False
        for lib in sorted(closure - removed):
            other = sorted(users.get(lib, set()) - removed)
            if other:
                continue
            removed.add(lib)
            changed = True
    # A library some other file names (dlopen, scripts, configs) stays, and
    # so does everything it needs (next round).
    names_of = {}
    for lib in sorted(removed - set(targets)):
        names = {os.path.basename(lib)} | {os.path.basename(l) for l in links.get(lib, [])}
        if elfs[lib].soname:
            names.add(elfs[lib].soname)
        names_of[lib] = names
    while True:
        keep = {}
        for d, _, files in os.walk(root):
            for name in files:
                host = os.path.join(d, name)
                p = '/' + os.path.relpath(host, root)
                if os.path.islink(host) or p in removed or not os.path.isfile(host):
                    continue
                with open(host, 'rb') as f:
                    data = f.read()
                for lib, names in names_of.items():
                    if lib in removed and lib not in keep:
                        hit = next((n for n in sorted(names) if n.encode() in data), None)
                        if hit:
                            keep[lib] = '%s names %s' % (p, hit)
        if not keep:
            break
        for lib, why in keep.items():
            removed.discard(lib)
            report.append('kept %s: %s' % (lib, why))
        # Re-run the closure rule: a kept library keeps what it needs.
        changed = True
        while changed:
            changed = False
            for lib in sorted(removed - set(targets)):
                if users.get(lib, set()) - removed:
                    removed.discard(lib)
                    report.append('kept %s: needed by a kept library' % lib)
                    changed = True
    lines = []
    for lib in sorted(removed):
        if lib in targets:
            continue
        who = ', '.join(sorted(users.get(lib, set())))
        lines.append((lib, 'NEEDED only by %s' % who))
        for l in sorted(links.get(lib, [])):
            lines.append((l, 'symlink to %s' % lib))
    for t in targets:
        for l in sorted(links.get(t, [])):
            lines.append((l, 'symlink to %s' % t))
    for lib in sorted(closure - removed):
        others = sorted(users.get(lib, set()) - removed)
        if others:
            report.append('kept %s: also NEEDED by %s%s' % (lib, ', '.join(others[:3]),
                                                            ' (+%d more)' % (len(others) - 3) if len(others) > 3 else ''))
    # Libraries nobody NEEDs that the removed binaries name (likely dlopen):
    # listed for review, never removed (a NEEDED analysis cannot prove them unused).
    named = []
    tdata = b''.join(open(root + t, 'rb').read() for t in targets)
    for path, elf in sorted(elfs.items()):
        if elf.type != 3 or path in removed or users.get(path) or not path.startswith(('/lib/', '/usr/lib/')):
            continue
        names = {os.path.basename(path)} | {os.path.basename(l) for l in links.get(path, [])}
        if elf.soname:
            names.add(elf.soname)
        if any(n.encode() in tdata for n in names):
            named.append(path)
    for r in report:
        sys.stderr.write('removable: %s\n' % r)
    for p in named:
        sys.stderr.write('removable: not removed, review: %s is NEEDED by nothing and named by %s '
                         '(dlopen?)\n' % (p, ', '.join(targets)))
    for path, why in lines:
        print('rm\t%s\t%s' % (path.lstrip('/'), why))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == 'check' and len(argv) <= 4:
        return cmd_check(argv[2].rstrip('/'), argv[3].rstrip('/') if len(argv) == 4 else None)
    if len(argv) >= 4 and argv[1] == 'removable':
        return cmd_removable(argv[2].rstrip('/'), argv[3:])
    die(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
