#!/usr/bin/env python3
"""UBIFS image reader/writer helper for the R3 Pro image recipe.

Reads a UBIFS image (mkfs.ubifs output, as in the official r3pro.upt) node by
node, checks every node CRC it touches, and gives the exact metadata of every
path: type, permission bits (incl. setuid/setgid/sticky), uid, gid, link
count, UBIFS inode flags, atime/ctime/mtime (sec + nsec), symlink target,
device number, extended attributes, and the SHA-256 of regular file data.

ubireader_extract_files is not used to unpack: it can only mknod device nodes
as real root, never sets symlink owners or times, and ignores xattrs. Here the
tree is written with the content only, and the metadata stays in a JSON
manifest; `apply` sets owner/mode/times on the work tree (container root) and
writes a mkfs.ubifs device table for the nodes a user namespace cannot
create; `fixup` then stamps the exact times (mkfs.ubifs takes ctime from the
host, which nobody can set) and a fixed UUID into the new image.

  ubifs_tree.py info    IMG                     superblock fields (JSON)
  ubifs_tree.py dump    IMG OUT.json            full manifest (verifies CRCs)
  ubifs_tree.py extract IMG DIR OUT.json        content into DIR + manifest
  ubifs_tree.py target  STOCK.json CHANGES DIR OUT.json SDE
                                                stock manifest + change list
                                                -> intended manifest; applies
                                                the changes' contents to DIR
  ubifs_tree.py apply   TARGET.json DIR DEVTABLE
                                                owner/mode/times on DIR, device
                                                table for c/b/p nodes
  ubifs_tree.py fixup   IMG TARGET.json UUIDHEX times + UUID into IMG, CRCs
  ubifs_tree.py compare TARGET.json OURS.json   check B4: every difference
"""
import hashlib
import json
import os
import shutil
import stat
import struct
import sys
import zlib

from lzallright import LZOCompressor

MAGIC = 0x06101831
CH_SZ = 24
INO, DATA, DENT, XENT, TRUN, PAD, SB, MST, REF, IDX, CS, ORPH = range(12)
BLOCK = 4096
NEW_FLAGS = 1  # UBIFS_COMPR_FL: mkfs.ubifs sets it on every inode when compression is on
ITYPE = {0: 'f', 1: 'd', 2: 'l', 3: 'b', 4: 'c', 5: 'p', 6: 's'}
IFMT = {'f': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK, 'b': stat.S_IFBLK,
        'c': stat.S_IFCHR, 'p': stat.S_IFIFO, 's': stat.S_IFSOCK}


def die(msg):
    sys.stderr.write('ubifs_tree: %s\n' % msg)
    sys.exit(1)


def ubifs_crc(buf):
    # crc32_le(0xFFFFFFFF, ...) without the final inversion
    return zlib.crc32(buf) ^ 0xFFFFFFFF


class Image:
    def __init__(self, path, writable=False):
        self.path = path
        self.f = open(path, 'r+b' if writable else 'rb')
        self.size = os.path.getsize(path)
        self.leb = 0  # LEB 0 offset 0 until the superblock is read
        sb = self.node_at(0, 0, SB)
        self.sb = parse_sb(sb)
        self.leb = self.sb['leb_size']
        if self.size % self.leb:
            die('%s: size %d is not a multiple of the LEB size' % (path, self.size))
        m1 = self.node_at(1, 0, MST)
        m2 = self.node_at(2, 0, MST)
        if m1[CH_SZ:] != m2[CH_SZ:]:  # the headers differ (sqnum, CRC)
            die('%s: the two master nodes differ (image was mounted?)' % path)
        (self.highest_inum, self.cmt_no, self.mst_flags, self.log_lnum, self.root_lnum,
         self.root_offs, self.root_len) = struct.unpack_from('<QQIIIII', m1, CH_SZ)

    def read(self, lnum, offs, n):
        self.f.seek(lnum * self.leb + offs)
        b = self.f.read(n)
        if len(b) != n:
            die('%s: short read at LEB %d:%d' % (self.path, lnum, offs))
        return b

    def node_at(self, lnum, offs, want=None):
        ch = self.read(lnum, offs, CH_SZ)
        magic, crc, sqnum, length, ntype = struct.unpack_from('<IIQIB', ch)
        if magic != MAGIC:
            die('%s: bad node magic at LEB %d:%d' % (self.path, lnum, offs))
        if want is not None and ntype != want:
            die('%s: node type %d at LEB %d:%d, expected %d' % (self.path, ntype, lnum, offs, want))
        buf = ch + self.read(lnum, offs + CH_SZ, length - CH_SZ)
        if ubifs_crc(buf[8:]) != crc:
            die('%s: CRC error in node type %d at LEB %d:%d' % (self.path, ntype, lnum, offs))
        return buf

    def scan(self):
        """Every node in LEBs 1.. (LEB 0 holds only the superblock), in on-media
        order: (lnum, offs, type, buf)."""
        for lnum in range(1, self.size // self.leb):
            offs = 0
            while offs + CH_SZ <= self.leb:
                ch = self.read(lnum, offs, CH_SZ)
                magic, _, _, length, ntype = struct.unpack_from('<IIQIB', ch)
                if magic != MAGIC:
                    break  # rest of the LEB is empty (0xFF)
                yield lnum, offs, ntype, self.node_at(lnum, offs)
                offs += (length + 7) & ~7

    def walk_index(self):
        """Leaf nodes reachable from the index root: list of (lnum, offs, buf)."""
        out = []
        stack = [(self.root_lnum, self.root_offs)]
        while stack:
            lnum, offs = stack.pop()
            buf = self.node_at(lnum, offs)
            ntype = buf[20]
            if ntype == IDX:
                child_cnt, level = struct.unpack_from('<HH', buf, CH_SZ)
                for i in range(child_cnt):
                    blnum, boffs, blen = struct.unpack_from('<III', buf, CH_SZ + 4 + i * 20)
                    stack.append((blnum, boffs))
            else:
                out.append((lnum, offs, buf))
        return out


SB_FIELDS = [  # (name, struct fmt, offset)
    ('key_hash', 'B', 26), ('key_fmt', 'B', 27), ('flags', 'I', 28), ('min_io_size', 'I', 32),
    ('leb_size', 'I', 36), ('leb_cnt', 'I', 40), ('max_leb_cnt', 'I', 44), ('max_bud_bytes', 'Q', 48),
    ('log_lebs', 'I', 56), ('lpt_lebs', 'I', 60), ('orph_lebs', 'I', 64), ('jhead_cnt', 'I', 68),
    ('fanout', 'I', 72), ('lsave_cnt', 'I', 76), ('fmt_version', 'I', 80), ('default_compr', 'H', 84),
    ('rp_uid', 'I', 88), ('rp_gid', 'I', 92), ('rp_size', 'Q', 96), ('time_gran', 'I', 104),
    ('ro_compat_version', 'I', 124), ('hash_algo', 'H', 256),
]


def parse_sb(buf):
    d = {}
    for name, fmt, off in SB_FIELDS:
        d[name] = struct.unpack_from('<' + fmt, buf, off)[0]
    d['uuid'] = buf[108:124].hex()
    d['hmac_zero'] = not any(buf[128:256])
    d['hash_mst_zero'] = not any(buf[258:322])
    d['padding_zero'] = not any(buf[322:4096]) and not any(buf[24:26]) and not any(buf[86:88])
    return d


def key_of(buf):
    k0, k1 = struct.unpack_from('<II', buf, CH_SZ)
    return k0, k1 >> 29, k1 & 0x1FFFFFFF


def decompress(ctype, size, data):
    if ctype == 0:
        out = data
    elif ctype == 1:
        out = LZOCompressor.decompress(data, output_size_hint=size)
    elif ctype == 2:
        out = zlib.decompress(data, -11)
    else:
        die('unsupported compression type %d' % ctype)
    if len(out) != size:
        die('decompressed %d bytes, expected %d' % (len(out), size))
    return out


INO_FMT = '<QQQQQIIIIIIIIIIIII'  # creat_sqnum .. xattr_names (offset 40)


def parse_ino(buf):
    (creat_sqnum, size, atime, ctime, mtime, atime_ns, ctime_ns, mtime_ns, nlink, uid, gid, mode, flags,
     data_len, xattr_cnt, xattr_size, _pad, xattr_names) = struct.unpack_from(INO_FMT, buf, 40)
    compr_type = struct.unpack_from('<H', buf, 132)[0]
    return {
        'size': size, 'atime': [atime, atime_ns], 'ctime': [ctime, ctime_ns], 'mtime': [mtime, mtime_ns],
        'nlink': nlink, 'uid': uid, 'gid': gid, 'mode': mode, 'flags': flags, 'xattr_cnt': xattr_cnt,
        'compr_type': compr_type, 'data': bytes(buf[160:160 + data_len]),
    }


def decode_dev(data):
    # union ubifs_dev_desc: le32 new_encode_dev() or le64 huge_encode_dev(),
    # the same encoding (the kernel decodes both with new_decode_dev()).
    if len(data) not in (4, 8):
        die('bad device descriptor length %d' % len(data))
    dev = struct.unpack('<I' if len(data) == 4 else '<Q', data)[0]
    if dev >> 32:
        die('device number 0x%x out of range' % dev)
    return [(dev & 0xfff00) >> 8, (dev & 0xff) | ((dev >> 12) & 0xfff00)]


def read_tree(img_path, content_dir=None):
    """Manifest {path: entry} of the image; with content_dir also writes the tree's
    content (regular files, directories, symlinks, hard links) there."""
    img = Image(img_path)
    inodes, data, dents, xents = {}, {}, {}, {}
    leaves = img.walk_index()
    for lnum, offs, buf in leaves:
        inum, ktype, kval = key_of(buf)
        ntype = buf[20]
        if ntype == INO:
            inodes[inum] = parse_ino(buf)
        elif ntype == DATA:
            size, ctype = struct.unpack_from('<IH', buf, 40)
            data.setdefault(inum, {})[kval] = decompress(ctype, size, bytes(buf[48:]))
        elif ntype in (DENT, XENT):
            tinum, _, dtype, nlen = struct.unpack_from('<QBBH', buf, 40)
            name = bytes(buf[56:56 + nlen]).decode('utf-8', 'surrogateescape')
            (dents if ntype == DENT else xents).setdefault(inum, []).append((name, tinum, dtype))
        else:
            die('unexpected leaf node type %d at LEB %d:%d' % (ntype, lnum, offs))
    # Every ino/data/dent node in the image must be the indexed one (an
    # mkfs.ubifs image has no journal): catches anything the index walk misses.
    counts = {}
    for _, _, ntype, _ in img.scan():
        counts[ntype] = counts.get(ntype, 0) + 1
    if counts.get(INO, 0) != len(inodes) or counts.get(DENT, 0) != sum(len(v) for v in dents.values()):
        die('%s: nodes outside the index (ino %d vs %d)' % (img_path, counts.get(INO, 0), len(inodes)))

    def content(inum):
        ino = inodes[inum]
        blocks = data.get(inum, {})
        out = bytearray()
        for b in range((ino['size'] + BLOCK - 1) // BLOCK):
            out += blocks.get(b, b'').ljust(BLOCK, b'\0')  # holes and short blocks read as zeros
        if any(b * BLOCK >= ino['size'] for b in blocks):
            die('%s: data beyond the end of inode %d' % (img_path, inum))
        return bytes(out[:ino['size']])

    def xattrs(inum):
        out = {}
        for name, tinum, _ in sorted(xents.get(inum, [])):
            out[name] = inodes[tinum]['data'].hex()
        return out

    entries = {}
    first_path = {}
    seen_dirs = set()

    def visit(inum, path):
        ino = inodes.get(inum)
        if ino is None:
            die('%s: dangling directory entry %s' % (img_path, path))
        fmt = stat.S_IFMT(ino['mode'])
        t = {v: k for k, v in IFMT.items()}.get(fmt)
        if t is None:
            die('%s: unknown mode %o for %s' % (img_path, ino['mode'], path))
        e = {'type': t, 'perm': '%04o' % stat.S_IMODE(ino['mode']), 'uid': ino['uid'], 'gid': ino['gid'],
             'nlink': ino['nlink'], 'flags': ino['flags'], 'atime': ino['atime'], 'ctime': ino['ctime'],
             'mtime': ino['mtime'], 'xattrs': xattrs(inum), 'inum': inum}
        if t == 'f':
            e['size'] = ino['size']
            if inum in first_path:
                e['hardlink'] = first_path[inum]
                e['sha256'] = entries[first_path[inum]]['sha256']
            else:
                c = content(inum)
                e['sha256'] = hashlib.sha256(c).hexdigest()
                first_path[inum] = path
                if content_dir is not None:
                    with open(os.path.join(content_dir, path), 'wb') as f:
                        f.write(c)
            if content_dir is not None and 'hardlink' in e:
                os.link(os.path.join(content_dir, e['hardlink']), os.path.join(content_dir, path))
        elif t == 'l':
            e['target'] = ino['data'].decode('utf-8', 'surrogateescape')
            if content_dir is not None:
                os.symlink(e['target'], os.path.join(content_dir, path))
        elif t in ('c', 'b'):
            e['dev'] = decode_dev(ino['data'])
        elif t == 'd':
            if inum in seen_dirs:
                die('%s: directory hard link at %s' % (img_path, path))
            seen_dirs.add(inum)
        entries[path] = e
        if t == 'd':
            if content_dir is not None and path != '.':
                os.mkdir(os.path.join(content_dir, path))
            for name, tinum, _ in sorted(dents.get(inum, [])):
                if '/' in name or name in ('.', '..', ''):
                    die('%s: bad name %r in %s' % (img_path, name, path))
                visit(tinum, name if path == '.' else path + '/' + name)

    if content_dir is not None:
        os.makedirs(content_dir, exist_ok=True)
    visit(1, '.')
    xattr_inodes = {t for v in xents.values() for _, t, _ in v}
    orphans = set(inodes) - {e['inum'] for e in entries.values()} - xattr_inodes
    if orphans:
        die('%s: %d inodes not reachable from / (e.g. %d)' % (img_path, len(orphans), min(orphans)))
    return img, entries


def save(obj, path):
    with open(path, 'w') as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write('\n')


def load(path):
    with open(path) as f:
        return json.load(f)


def cmd_info(img):
    print(json.dumps(Image(img).sb, indent=1, sort_keys=True))


def cmd_dump(img, out, content_dir=None):
    image, entries = read_tree(img, content_dir)
    save({'superblock': image.sb, 'entries': entries}, out)
    n = {}
    for e in entries.values():
        n[e['type']] = n.get(e['type'], 0) + 1
    print('ubifs_tree: %s: %d paths %s' % (os.path.basename(img), len(entries), json.dumps(n, sort_keys=True)))


# ---- intended tree --------------------------------------------------------------

def cmd_target(stock_json, changes, tree, out, sde):
    """Stock manifest + change list = the intended manifest; the change list's
    content is applied to the work tree. Change list lines (tab-separated):
      rm    PATH  REASON                   remove a stock file/symlink
      dir   PATH  PERM UID GID             new directory
      file  PATH  PERM UID GID SOURCE      new or replaced regular file; PERM, UID and
                                           GID "-" keep the replaced file's
      link  PATH  TARGET                   new symlink (root:root 0777)
    New and replaced entries get atime = ctime = mtime = SOURCE_DATE_EPOCH and
    the UBIFS inode flags mkfs.ubifs gives every inode (compression on)."""
    sde = int(sde)
    entries = load(stock_json)['entries']
    for e in entries.values():
        e.pop('inum', None)
    with open(changes) as f:
        lines = [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')]
    for line in lines:
        op, *a = line.split('\t')
        path = a[0]
        if path.startswith('/') or '..' in path.split('/'):
            die('bad path in change list: %s' % path)
        full = os.path.join(tree, path)
        times = {'atime': [sde, 0], 'ctime': [sde, 0], 'mtime': [sde, 0]}
        if op == 'rm':
            old = entries.get(path)
            if not old or old['type'] not in ('f', 'l'):
                die('rm %s: not a stock file or symlink' % path)
            if old.get('nlink', 1) != 1 and old['type'] == 'f':
                die('rm %s: hard-linked file, not supported' % path)
            del entries[path]
            os.unlink(full)
        elif op == 'dir':
            perm, uid, gid = a[1:4]
            if path in entries:
                die('dir %s: exists' % path)
            entries[path] = dict(type='d', perm=perm, uid=int(uid), gid=int(gid), flags=NEW_FLAGS, xattrs={},
                                 **times)
            os.mkdir(full)
        elif op == 'file':
            perm, uid, gid, src = a[1:5]
            old = entries.get(path)
            if old and old['type'] != 'f':
                die('file %s: replaces a %s' % (path, old['type']))
            if old and old.get('nlink', 1) != 1:
                die('file %s: replaces a hard-linked file' % path)
            parent = os.path.dirname(path) or '.'
            if entries.get(parent, {}).get('type') != 'd':
                die('file %s: no parent directory' % path)
            with open(src, 'rb') as f:
                c = f.read()
            if os.path.lexists(full):
                os.unlink(full)
            with open(full, 'wb') as f:
                f.write(c)
            if '-' in (perm, uid, gid) and not old:
                die('file %s: "-" needs a file to replace' % path)
            perm = old['perm'] if perm == '-' else perm
            uid = old['uid'] if uid == '-' else int(uid)
            gid = old['gid'] if gid == '-' else int(gid)
            entries[path] = dict(type='f', perm=perm, uid=uid, gid=gid, nlink=1, flags=NEW_FLAGS,
                                 xattrs={}, size=len(c), sha256=hashlib.sha256(c).hexdigest(), **times)
        elif op == 'link':
            if path in entries:
                die('link %s: exists' % path)
            entries[path] = dict(type='l', perm='0777', uid=0, gid=0, nlink=1, flags=NEW_FLAGS, xattrs={}, target=a[1],
                                 **times)
            os.symlink(a[1], full)
        else:
            die('unknown change %r' % op)
    save({'entries': entries}, out)


def cmd_apply(target_json, tree, devtable):
    """Owner, mode and times on the work tree (as root in the container), and
    a mkfs.ubifs device table for device nodes and FIFOs (a user namespace
    cannot mknod; mkfs.ubifs creates them from the table)."""
    entries = load(target_json)['entries']
    dev_lines = []
    for path in sorted(entries):
        e = entries[path]
        full = os.path.join(tree, path)
        t = e['type']
        if e['xattrs']:
            die('%s: extended attributes are not supported by this recipe' % path)
        if t in ('c', 'b', 'p'):
            if os.path.lexists(full):
                die('%s: device node already in the tree' % path)
            major, minor = e.get('dev', [0, 0])
            dev_lines.append('/%s %s %s %d %d %d %d - - -' % (path, t, e['perm'], e['uid'], e['gid'], major, minor))
            continue
        if t == 's':
            die('%s: sockets are not supported by mkfs.ubifs device tables' % path)
        st = os.lstat(full)
        if stat.S_IFMT(st.st_mode) != IFMT[t]:
            die('%s: tree has %o, manifest says %s' % (path, st.st_mode, t))
        os.lchown(full, e['uid'], e['gid'])  # before chmod: chown clears setuid/setgid
        if t != 'l':
            os.chmod(full, int(e['perm'], 8))
    # Times last, deepest first, so creating/chmodding children does not touch them.
    for path in sorted(entries, key=lambda p: (-p.count('/'), p), reverse=False):
        e = entries[path]
        if e['type'] in ('c', 'b', 'p'):
            continue
        a, m = e['atime'], e['mtime']
        os.utime(os.path.join(tree, path), ns=(a[0] * 10**9 + a[1], m[0] * 10**9 + m[1]), follow_symlinks=False)
    with open(devtable, 'w') as f:
        f.write(''.join(l + '\n' for l in dev_lines))
    print('ubifs_tree: metadata applied to %d paths, %d device-table entries' % (len(entries), len(dev_lines)))


def cmd_fixup(img_path, target_json, uuid_hex):
    """Exact atime/ctime/mtime (mkfs.ubifs copies ctime from the host, which
    cannot be set) and a fixed UUID (mkfs.ubifs picks a random one): both are
    plain fields of self-contained nodes, so each patched node only needs its
    CRC recomputed; no index or LPT data depends on them."""
    entries = load(target_json)['entries']
    _, ours = read_tree(img_path)
    want = {}
    for path, e in ours.items():
        t = entries.get(path)
        if t is None:
            continue  # reported by compare
        want[e['inum']] = (t['atime'], t['ctime'], t['mtime'])
    img = Image(img_path, writable=True)
    patched = 0
    for lnum, offs, ntype, buf in img.scan():
        if ntype != INO:
            continue
        inum = key_of(buf)[0]
        if inum not in want:
            continue  # xattr inodes, none expected
        (a, an), (c, cn), (m, mn) = want[inum]
        buf = bytearray(buf)
        struct.pack_into('<QQQIII', buf, 56, a, c, m, an, cn, mn)
        struct.pack_into('<I', buf, 4, ubifs_crc(bytes(buf[8:])))
        img.f.seek(lnum * img.leb + offs)
        img.f.write(buf)
        patched += 1
    sb = bytearray(img.node_at(0, 0, SB))
    sb[108:124] = bytes.fromhex(uuid_hex)
    struct.pack_into('<I', sb, 4, ubifs_crc(bytes(sb[8:])))
    img.f.seek(0)
    img.f.write(sb)
    # mtd-utils 2.3.0 writes an empty 64-byte node after the superblock (the
    # space a signature node takes in an authenticated image: node type 0,
    # zero payload) plus a padding node. Nothing reads LEB 0 beyond the
    # superblock unless the superblock says "authenticated", and the stock
    # image has erased flash (0xFF) there: make it the same.
    rest = img.read(0, 4096, img.leb - 4096)
    blanked = 'none'
    if rest != b'\xff' * len(rest):
        magic, _, _, length, ntype = struct.unpack_from('<IIQIB', rest)
        if magic != MAGIC or length != 64 or ntype != 0 or any(rest[24:64]):
            die('%s: unexpected data after the superblock' % img_path)
        img.node_at(0, 4096)  # CRC
        pad = rest[64:]
        pmagic, _, _, plen, ptype = struct.unpack_from('<IIQIB', pad)
        if pmagic != MAGIC or ptype != PAD or any(b != 0xff for b in pad[plen + struct.unpack_from('<I', pad, 24)[0]:]):
            die('%s: unexpected data after the superblock' % img_path)
        img.f.seek(4096)
        img.f.write(b'\xff' * len(rest))
        blanked = 'empty signature-space node + padding'
    img.f.close()
    print('ubifs_tree: fixed times of %d inodes, UUID %s, LEB 0 after the superblock erased (%s)' %
          (patched, uuid_hex, blanked))


DERIVED = {'inum'}  # inode numbers are assigned by mkfs.ubifs


def cmd_compare(target_json, ours_json):
    target = load(target_json)['entries']
    ours = load(ours_json)['entries']
    errors = []
    for path in sorted(set(target) | set(ours)):
        t, o = target.get(path), ours.get(path)
        if t is None:
            errors.append('%s: in the image but not intended' % path)
            continue
        if o is None:
            errors.append('%s: intended but missing from the image' % path)
            continue
        for k in sorted(set(t) | set(o)):
            if k in DERIVED:
                continue
            if t['type'] == 'd' and k in ('nlink', 'size'):
                continue  # directory link count and size follow from the children
            if t.get(k) != o.get(k):
                errors.append('%s: %s intended %r, image has %r' % (path, k, t.get(k), o.get(k)))
    for e in errors[:200]:
        print('  B4: ' + e)
    if len(errors) > 200:
        print('  B4: ... %d more' % (len(errors) - 200))
    print('ubifs_tree: compare: %d paths, %d differences' % (len(target), len(errors)))
    return 1 if errors else 0


def main(argv):
    if len(argv) < 2:
        die(__doc__)
    cmd, args = argv[1], argv[2:]
    if cmd == 'info' and len(args) == 1:
        cmd_info(*args)
    elif cmd == 'dump' and len(args) == 2:
        cmd_dump(*args)
    elif cmd == 'extract' and len(args) == 3:
        cmd_dump(args[0], args[2], args[1])
    elif cmd == 'target' and len(args) == 5:
        cmd_target(*args)
    elif cmd == 'apply' and len(args) == 3:
        cmd_apply(*args)
    elif cmd == 'fixup' and len(args) == 3:
        cmd_fixup(*args)
    elif cmd == 'compare' and len(args) == 2:
        return cmd_compare(*args)
    else:
        die(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
