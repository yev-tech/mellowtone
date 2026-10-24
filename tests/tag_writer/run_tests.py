#!/usr/bin/env python3
"""Test matrix for src/library/tag_writer.c.

Usage: run_tests.py TOOL WORKDIR   (normally via run.sh / container.sh)

Fixtures are generated into WORKDIR/fixtures with ffmpeg, flac and mutagen.
Independent checks: mutagen (tags/pictures/frame encodings), a separate
python parser for the tag-region/audio-payload split (payload must be
byte-identical), ffmpeg-decoded PCM md5, `flac -t`, and the app's own reader
(src/library/metadata.c, linked into TOOL as `mdread`).
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

from mutagen.flac import FLAC
from mutagen.id3 import (ID3, APIC, COMM, PRIV, TALB, TCON, TDRC, TIT2, TPE1, TPE2, TPOS, TRCK, TXXX,
                         TYER, TDAT, UFID, ID3NoHeaderError)

TOOL = sys.argv[1]
WORK = sys.argv[2]
PLAIN_TOOL = TOOL + "_plain" if os.path.exists(TOOL + "_plain") else TOOL
FIX = os.path.join(WORK, "fixtures")
CASES = os.path.join(WORK, "cases")

results = []  # (name, ok, detail)


def sh(*cmd, check=True, **kw):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if check and p.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(cmd), p.stderr.decode(errors="replace")))
    return p


def tw(*args, tool=None, user=None):
    cmd = [tool or TOOL] + [str(a) for a in args]
    if user:
        cmd = ["setpriv", "--reuid=%d" % user, "--regid=%d" % user, "--clear-groups"] + cmd
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.decode().strip().splitlines()
    err = p.stderr.decode()
    if "ERROR: AddressSanitizer" in err or "runtime error:" in err:
        raise RuntimeError("sanitizer: " + err)
    return out[-1] if out else "", err


def mode_of(err):
    m = [l.split(":", 1)[1].strip() for l in err.splitlines() if l.startswith("MODE:")]
    return m[-1] if m else None


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def pcm_md5(path):
    p = sh("ffmpeg", "-v", "error", "-i", path, "-map", "0:a", "-f", "s16le", "-")
    return hashlib.md5(p.stdout).hexdigest()


def syncsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def region_len(path):
    """Independent (python) tag-region parser: returns (fmt, audio_offset, flac_blocks, id3prefix_len)."""
    with open(path, "rb") as f:
        d = f.read()
    pos = 0
    while d[pos:pos + 3] == b"ID3":
        pos += 10 + syncsafe(d[pos + 6:pos + 10]) + (10 if d[pos + 5] & 0x10 else 0)
    if d[pos:pos + 4] == b"fLaC":
        prefix = pos
        pos += 4
        blocks = []
        while True:
            t = d[pos] & 0x7F
            last = d[pos] & 0x80
            n = int.from_bytes(d[pos + 1:pos + 4], "big")
            blocks.append((t, d[pos + 4:pos + 4 + n], bool(last)))
            pos += 4 + n
            if last:
                break
        return "flac", pos, blocks, prefix
    if d[:3] == b"ID3":
        return "mp3", 10 + syncsafe(d[6:10]) + (10 if (d[3] == 4 and d[5] & 0x10) else 0), None, 0
    return "mp3", 0, None, 0


def payload(path):
    off = region_len(path)[1]
    with open(path, "rb") as f:
        f.seek(off)
        return f.read()


def mdread(path):
    p = sh(TOOL, "mdread", path)
    out = {}
    for line in p.stdout.decode().splitlines():
        k, _, v = line.partition("=")
        out[k] = v
    return out


def leftovers(d):
    return [n for n in os.listdir(d) if n.endswith(".tagtmp") or n.endswith(".tmp")]


class Case:
    def __init__(self, name):
        self.name = name
        self.errors = []

    def check(self, cond, what):
        if not cond:
            self.errors.append(what)
        return cond

    def eq(self, a, b, what):
        return self.check(a == b, "%s: got %r expected %r" % (what, a, b))


def run_case(name, fn):
    c = Case(name)
    t0 = time.time()
    try:
        fn(c)
    except Exception as e:  # noqa
        import traceback
        c.errors.append("exception: %s\n%s" % (e, traceback.format_exc()))
    dt = time.time() - t0
    results.append((name, not c.errors, "; ".join(c.errors), dt))
    print("%-58s %s (%.1fs)%s" % (name, "PASS" if not c.errors else "FAIL", dt,
                                  "" if not c.errors else "\n    " + "\n    ".join(c.errors)), flush=True)


def fresh(fixture, name=None):
    d = os.path.join(CASES, "%03d" % len(results))
    os.makedirs(d, exist_ok=True)
    dst = os.path.join(d, name or os.path.basename(fixture))
    shutil.copyfile(fixture, dst)
    return d, dst


# ---------------------------------------------------------------- fixtures

def flac_rebuild(path, blocks, prefix=b""):
    """Writes prefix + fLaC + blocks [(type, body)] + original audio."""
    audio = payload(path)
    out = bytearray(prefix + b"fLaC")
    for i, (t, body) in enumerate(blocks):
        out.append(t | (0x80 if i == len(blocks) - 1 else 0))
        out += len(body).to_bytes(3, "big") + body
    with open(path, "wb") as f:
        f.write(bytes(out) + audio)


def id3_bytes(frames, v2_version=3):
    tmp = os.path.join(FIX, "_id3tmp")
    open(tmp, "wb").close()
    t = ID3()
    for fr in frames:
        t.add(fr)
    t.save(tmp, v2_version=v2_version, padding=lambda info: 64)
    with open(tmp, "rb") as f:
        b = f.read()
    os.remove(tmp)
    return b


COVER = None
BIGCOVER = None


def make_fixtures():
    global COVER, BIGCOVER
    os.makedirs(FIX, exist_ok=True)
    j = lambda n: os.path.join(FIX, n)  # noqa
    sh("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-ac", "2", "-ar", "44100",
       j("tone.wav"))
    sh("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "anoisesrc=d=240:c=white:a=0.5", "-ac", "2", "-ar", "44100",
       j("noise.wav"))
    sh("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=300x300", "-frames:v", "1", j("cover.png"))
    sh("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "nullsrc=s=900x900,geq=random(1)*255:random(2)*255:128",
       "-frames:v", "1", j("bigcover.png"))
    COVER = open(j("cover.png"), "rb").read()
    BIGCOVER = open(j("bigcover.png"), "rb").read()

    # FLAC 1: tags with aliases/duplicates/lower-case keys, 8 KiB padding, + APPLICATION block.
    sh("flac", "-s", "-f", "--padding=8192", "-T", "TITLE=Old title", "-T", "ARTIST=Old A", "-T", "ARTIST=Old A2",
       "-T", "ALBUM ARTIST=Alias AA", "-T", "album=lower album", "-T", "TRACKNUMBER=1", "-T", "TOTALTRACKS=9",
       "-T", "COMMENT=keep me", "-T", "REPLAYGAIN_TRACK_GAIN=-3.10 dB", "-T", "DISKNUMBER=4",
       "-o", j("tagged.flac"), j("tone.wav"))
    _, _, blocks, _ = region_len(j("tagged.flac"))
    bl = [(t, b) for t, b, _ in blocks]
    bl.insert(1, (2, b"TEST" + bytes(range(40))))  # APPLICATION
    flac_rebuild(j("tagged.flac"), bl)

    # FLAC 2: no VORBIS_COMMENT, no padding.
    sh("flac", "-s", "-f", "--no-padding", "-o", j("novc_nopad.flac"), j("tone.wav"))
    _, _, blocks, _ = region_len(j("novc_nopad.flac"))
    flac_rebuild(j("novc_nopad.flac"), [(t, b) for t, b, _ in blocks if t not in (1, 4)])

    # FLAC 3: big PICTURE + small (16 byte) padding.
    sh("flac", "-s", "-f", "--padding=16", "--picture=" + j("bigcover.png"), "-T", "TITLE=pic", "-T",
       "ALBUMARTIST=Keep AA", "-o", j("picture_smallpad.flac"), j("tone.wav"))

    # FLAC 3b: 2 MB PICTURE between VORBIS_COMMENT and 8 KiB PADDING (in-place candidate).
    sh("flac", "-s", "-f", "--padding=8192", "--picture=" + j("bigcover.png"), "-T", "TITLE=pic", "-T",
       "ARTIST=Someone", "-o", j("picture_bigpad.flac"), j("tone.wav"))

    # FLAC 4: leading ID3v2.3 tag before fLaC (seen in the user's library) + picture.
    sh("flac", "-s", "-f", "--padding=512", "--picture=" + j("cover.png"), "-T", "TITLE=behind id3",
       "-o", j("id3prefix.flac"), j("tone.wav"))
    pre = id3_bytes([TIT2(encoding=1, text="ID3 prefix title"), TPE1(encoding=1, text="ID3 prefix artist")])
    with open(j("id3prefix.flac"), "rb") as f:
        body = f.read()
    with open(j("id3prefix.flac"), "wb") as f:
        f.write(pre + body)

    # FLAC 5: large (3 min pink noise) with picture.
    sh("flac", "-s", "-f", "--padding=4096", "--picture=" + j("cover.png"), "-T", "TITLE=large",
       "-o", j("large.flac"), j("noise.wav"))

    # MP3 0: no tag at all.
    sh("ffmpeg", "-v", "error", "-y", "-i", j("tone.wav"), "-c:a", "libmp3lame", "-b:a", "128k",
       "-id3v2_version", "0", "-write_id3v1", "0", j("notag.mp3"))
    assert open(j("notag.mp3"), "rb").read(3) != b"ID3"

    def mp3_with(name, frames, v2, v1=0):
        shutil.copyfile(j("notag.mp3"), j(name))
        t = ID3()
        for fr in frames:
            t.add(fr)
        t.save(j(name), v2_version=v2, v1=v1, padding=lambda info: 256)

    def common(enc):
        return [TIT2(encoding=enc, text="Old title"), TPE1(encoding=enc, text="Old artist"),
                TALB(encoding=enc, text="Old album"), TPE2(encoding=enc, text="Old AA"),
                TRCK(encoding=enc, text="3/12"), TPOS(encoding=enc, text="1/2"), TCON(encoding=enc, text="Rock"),
                COMM(encoding=enc, lang="eng", desc="", text="keep comment"),
                APIC(encoding=enc, mime="image/png", type=3, desc="cover", data=COVER),
                TXXX(encoding=enc, desc="MusicBrainz Album Id", text="old-album-id"),
                TXXX(encoding=enc, desc="Other Thing", text="keep txxx"),
                UFID(owner="http://musicbrainz.org", data=b"old-recording-id"),
                UFID(owner="http://example.org", data=b"keep-ufid"),
                PRIV(owner="keep.priv", data=b"\x00\x01\x02\xff")]

    # MP3 1: ID3v2.3, UTF-16 text (mutagen downgrades to UTF-16 for 2.3), APIC, TYER+TDAT.
    mp3_with("v23.mp3", common(1) + [TDRC(encoding=1, text="1999-01-02")], 3)
    # MP3 2: ID3v2.4, UTF-8, TDRC, MB TXXX with lower-case description.
    mp3_with("v24.mp3", common(3) + [TDRC(encoding=3, text="1999-01-02"),
                                     TXXX(encoding=3, desc="musicbrainz artist id", text="old-artist-id")], 4)
    # MP3 3: ID3v2.3 + ID3v1 trailer.
    mp3_with("v23_v1.mp3", common(1) + [TDRC(encoding=1, text="1998")], 3, v1=2)
    with open(j("v23_v1.mp3"), "rb") as f:
        assert f.read()[-128:-125] == b"TAG"

    # MP3 4: ID3v2.4 with a footer (crafted: flag 0x10, no padding, "3DI" footer).
    d = open(j("v24.mp3"), "rb").read()
    size = syncsafe(d[6:10])
    tag = d[10:10 + size].rstrip(b"\x00")
    hdr = bytearray(d[:10])
    hdr[5] |= 0x10
    hdr[6:10] = bytes([(len(tag) >> 21) & 0x7F, (len(tag) >> 14) & 0x7F, (len(tag) >> 7) & 0x7F, len(tag) & 0x7F])
    footer = b"3DI" + bytes(hdr[3:])
    with open(j("v24_footer.mp3"), "wb") as f:
        f.write(bytes(hdr) + tag + footer + d[10 + size:])

    # Unsupported MP3 variants.
    d2 = open(j("v23.mp3"), "rb").read()
    with open(j("v23_unsync.mp3"), "wb") as f:
        f.write(d2[:5] + bytes([d2[5] | 0x80]) + d2[6:])
    with open(j("v23_exthdr.mp3"), "wb") as f:
        f.write(d2[:5] + bytes([d2[5] | 0x40]) + d2[6:])
    body22 = b"TT2" + (6).to_bytes(3, "big") + b"\x00Hello"
    body22 += b"\x00" * 64
    hdr22 = b"ID3\x02\x00\x00" + bytes([(len(body22) >> 21) & 0x7F, (len(body22) >> 14) & 0x7F,
                                        (len(body22) >> 7) & 0x7F, len(body22) & 0x7F])
    with open(j("v22.mp3"), "wb") as f:
        f.write(hdr22 + body22 + open(j("notag.mp3"), "rb").read())

    # Malformed files.
    t = open(j("tagged.flac"), "rb").read()
    with open(j("truncated_meta.flac"), "wb") as f:
        f.write(t[:300])
    off = region_len(j("tagged.flac"))[1]
    with open(j("garbage_after_meta.flac"), "wb") as f:
        f.write(t[:off] + b"JUNK" + t[off:])
    with open(j("tagsize_too_big.mp3"), "wb") as f:
        f.write(b"ID3\x04\x00\x00\x7f\x7f\x7f\x7f" + open(j("notag.mp3"), "rb").read())


# ---------------------------------------------------------------- checks

FULL = {
    "ALBUM": "Kind of Blue (Legacy Edition)",
    "ALBUMARTIST": "Miles Davis",
    "ARTIST": "Miles Davis & John Coltrane",
    "TITLE": "So What",
    "TRACKNUMBER": "1",
    "TRACKTOTAL": "5",
    "DISCNUMBER": "1",
    "DISCTOTAL": "2",
    "DATE": "1959-08-17",
    "GENRE": "Jazz",
    "MUSICBRAINZ_ALBUMID": "8f3a5b6c-1111-2222-3333-444455556666",
    "MUSICBRAINZ_ALBUMARTISTID": "561d854a-6a28-4aa7-8c99-323e6ce46c2a",
    "MUSICBRAINZ_ARTISTID": "561d854a-6a28-4aa7-8c99-323e6ce46c2a",
    "MUSICBRAINZ_TRACKID": "0e4ef3e5-aaaa-bbbb-cccc-ddddeeeeffff",
    "MUSICBRAINZ_RELEASETRACKID": "1a2b3c4d-0000-1111-2222-333344445555",
}
UNI = {
    "TITLE": "שיר השירים — 東京 🎵 Ünïcödé",
    "ARTIST": "Ахмед & Søren 𝄞",
    "ALBUM": "אלבום 🎶",
    "ALBUMARTIST": "דוד ברוזה",
}


def args(fields):
    return ["%s=%s" % (k, v) for k, v in fields.items()]


def vc_list(path):
    f = FLAC(path)
    return list(f.tags) if f.tags is not None else []


def vc_get(path, key):
    return [v for k, v in vc_list(path) if k.upper() == key.upper()]


def check_audio_same(c, orig, new, decode=True):
    c.check(payload(orig) == payload(new), "audio payload bytes differ")
    if decode:
        c.eq(pcm_md5(new), pcm_md5(orig), "decoded PCM md5")
    if new.lower().endswith(".flac"):
        c.check(sh("flac", "-t", "-s", new, check=False).returncode == 0, "flac -t failed")


def check_md(c, path, exp, modes=("full", "text")):
    m = mdread(path)
    for mode in modes:
        for k, v in exp.items():
            c.eq(m.get(mode + "." + k), str(v), "metadata.c %s.%s" % (mode, k))


def check_flac_fields(c, path, fields):
    for k, v in fields.items():
        got = vc_get(path, k)
        c.eq(got, [v] if v else [], "VC " + k)


def flac_nonvc_blocks(path):
    return [(t, b) for t, b, _ in region_len(path)[2] if t not in (1, 4)]


def check_flac_structure(c, path):
    _, off, blocks, _ = region_len(path)
    c.eq(sum(1 for _, _, last in blocks if last), 1, "exactly one last-block flag")
    c.check(blocks[-1][2], "last block has last flag")
    c.eq(blocks[0][0], 0, "STREAMINFO first")
    c.check(sh("metaflac", "--list", path, check=False).returncode == 0, "metaflac --list")


# ---------------------------------------------------------------- cases

def case_flac_full(c):
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    bak = os.path.join(d, "bak.ctw")
    before_vc = vc_list(p)
    out, err = tw("apply", p, bak, *args(FULL))
    c.eq(out, "TAG_WRITE_OK", "apply")
    c.eq(mode_of(err), "IN_PLACE", "mode")
    c.check(os.path.getsize(bak) < 64 * 1024, "small v2 backup (%d)" % os.path.getsize(bak))
    check_flac_fields(c, p, FULL)
    for alias in ("ALBUM ARTIST", "TOTALTRACKS", "DISKNUMBER"):
        c.eq(vc_get(p, alias), [], "alias removed " + alias)
    c.eq(vc_get(p, "COMMENT"), ["keep me"], "COMMENT kept")
    c.eq(vc_get(p, "REPLAYGAIN_TRACK_GAIN"), ["-3.10 dB"], "RG kept")
    # Kept comments keep their relative order; replaced ones keep position.
    after = vc_list(p)
    c.eq([k for k, _ in after][:4], ["TITLE", "ARTIST", "ALBUMARTIST", "ALBUM"],
         "replacement keeps position (ALBUMARTIST takes the alias' slot)")
    c.eq(FLAC(p).tags.vendor, FLAC(orig).tags.vendor, "vendor kept")
    c.eq(flac_nonvc_blocks(p), flac_nonvc_blocks(orig), "STREAMINFO/APPLICATION/SEEKTABLE blocks byte-identical")
    c.eq(region_len(p)[1], region_len(orig)[1], "audio offset unchanged (fits in padding)")
    check_flac_structure(c, p)
    check_audio_same(c, orig, p)
    check_md(c, p, {"title": "So What", "artist": FULL["ARTIST"], "album": FULL["ALBUM"],
                    "albumartist": "Miles Davis", "genre": "Jazz", "track": 1, "disc": 1, "year": 1959})
    c.eq(leftovers(d), [], "no temp files")
    _ = before_vc
    out, _ = tw("restore", p, bak)
    c.eq(out, "TAG_WRITE_OK", "restore")
    c.eq(sha(p), sha(orig), "restore byte-identical")
    c.eq(leftovers(d), [], "no temp files after restore")


def case_flac_remove(c):
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    tw("apply", p, "-", *args(FULL))
    out, _ = tw("apply", p, "-", "GENRE=", "DATE=", "ALBUMARTIST=", "TRACKTOTAL=")
    c.eq(out, "TAG_WRITE_OK", "apply remove")
    for k in ("GENRE", "DATE", "ALBUMARTIST", "TRACKTOTAL"):
        c.eq(vc_get(p, k), [], "removed " + k)
    c.eq(vc_get(p, "TITLE"), ["So What"], "others kept")
    check_md(c, p, {"genre": "", "albumartist": "", "year": 0, "title": "So What"})
    check_audio_same(c, orig, p, decode=False)


def case_flac_novc_nopad(c):
    d, p = fresh(os.path.join(FIX, "novc_nopad.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    c.check(FLAC(orig).tags is None, "fixture has no VC")
    bak = os.path.join(d, "b.ctw")
    out, err = tw("apply", p, bak, *args(FULL))
    c.eq(out, "TAG_WRITE_OK", "apply")
    c.eq(mode_of(err), "FULL_COPY", "mode")
    c.eq(open(bak, "rb").read(12)[8:12], b"\x01\x00\x00\x00", "v1 backup")
    check_flac_fields(c, p, FULL)
    blocks = region_len(p)[2]
    c.eq([t for t, _, _ in blocks], [0, 4, 3, 1], "VC inserted after STREAMINFO, padding last")
    c.eq(len(blocks[-1][1]), 4096, "new padding 4 KiB")
    check_flac_structure(c, p)
    check_audio_same(c, orig, p)
    out, _ = tw("restore", p, bak)
    c.eq(out, "TAG_WRITE_OK", "restore")
    c.eq(sha(p), sha(orig), "restore byte-identical")


def case_flac_novc_remove_only(c):
    d, p = fresh(os.path.join(FIX, "novc_nopad.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    out, _ = tw("apply", p, "-", "TITLE=")
    c.eq(out, "TAG_WRITE_OK", "apply removal on file without VC")
    c.check(FLAC(p).tags is None, "no VC created for a pure removal")
    check_audio_same(c, orig, p, decode=False)


def case_flac_picture_grow(c):
    d, p = fresh(os.path.join(FIX, "picture_smallpad.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    bak = os.path.join(d, "b.ctw")
    f = dict(UNI)
    f["GENRE"] = "x" * 3000  # does not fit the 16-byte padding
    out, err = tw("apply", p, bak, *args(f))
    c.eq(out, "TAG_WRITE_OK", "apply")
    c.eq(mode_of(err), "FULL_COPY", "growth uses full copy")
    check_flac_fields(c, p, f)
    c.eq(vc_get(p, "TITLE"), [UNI["TITLE"]], "unicode title")
    c.eq([pp.data for pp in FLAC(p).pictures], [pp.data for pp in FLAC(orig).pictures], "pictures identical")
    c.eq(FLAC(p).pictures[0].data, BIGCOVER, "picture equals source image")
    c.check(region_len(p)[1] > region_len(orig)[1], "region grew")
    check_flac_structure(c, p)
    check_audio_same(c, orig, p)
    m = mdread(p)
    c.eq(m["full.picture"], str(len(BIGCOVER)), "metadata.c sees the picture")
    check_md(c, p, {"title": UNI["TITLE"], "artist": UNI["ARTIST"], "albumartist": UNI["ALBUMARTIST"]})
    out, _ = tw("restore", p, bak)
    c.eq(sha(p), sha(orig), "restore byte-identical")


def case_flac_id3prefix(c):
    d, p = fresh(os.path.join(FIX, "id3prefix.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    bak = os.path.join(d, "b.ctw")
    out, _ = tw("apply", p, bak, *args(FULL))
    c.eq(out, "TAG_WRITE_OK", "apply")
    pre = region_len(orig)[3]
    c.check(pre > 0, "fixture has prefix")
    c.eq(open(p, "rb").read(pre), open(orig, "rb").read(pre), "ID3 prefix byte-identical")
    check_flac_fields(c, p, FULL)
    c.eq(len(FLAC(p).pictures), 1, "picture kept")
    check_flac_structure(c, p)
    check_audio_same(c, orig, p)
    # Only the text walker: the app's full metadata_read() path (dr_flac with
    # onTell) cannot open ID3-prefixed FLAC at all -- pre-existing, see report.
    check_md(c, p, {"title": "So What", "albumartist": "Miles Davis", "track": 1, "year": 1959}, modes=("text",))
    c.eq(mdread(orig)["full.title"], "", "known reader quirk also on the untouched original")
    out, _ = tw("restore", p, bak)
    c.eq(sha(p), sha(orig), "restore byte-identical")


def case_flac_large(c):
    d, p = fresh(os.path.join(FIX, "large.flac"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    size = os.path.getsize(p)
    bak = os.path.join(d, "b.ctw")
    t0 = time.time()
    out, err = tw("apply", p, bak, *args(FULL), tool=PLAIN_TOOL)
    dt = time.time() - t0
    c.eq(out, "TAG_WRITE_OK", "apply")
    hwm = [l for l in err.splitlines() if l.startswith("VmHWM")]
    print("    large FLAC: %.1f MB in %.2fs (host tmpfs/disk), tool peak RSS %s" %
          (size / 1e6, dt, hwm[0].split(":", 1)[1].strip() if hwm else "?"))
    if hwm:
        c.check(int(hwm[0].split()[1]) < 8 * 1024, "peak RSS under 8 MiB")
    check_flac_fields(c, p, FULL)
    check_audio_same(c, orig, p)
    out, _ = tw("restore", p, bak, tool=PLAIN_TOOL)
    c.eq(sha(p), sha(orig), "restore byte-identical")


def id3_load(path):
    return ID3(path, translate=False)


def text_of(t, fid):
    fr = t.getall(fid)
    return [str(x) for f in fr for x in f.text]


def check_mp3_fields(c, path, fields, v23):
    t = id3_load(path)
    c.eq(t.version[:2], (2, 3) if v23 else (2, 4), "ID3 version")
    simple = {"ALBUM": "TALB", "ALBUMARTIST": "TPE2", "ARTIST": "TPE1", "TITLE": "TIT2", "GENRE": "TCON"}
    for k, fid in simple.items():
        if k in fields:
            c.eq(text_of(t, fid), [fields[k]] if fields[k] else [], fid)
            if fields[k]:
                c.eq(t.getall(fid)[0].encoding, 1 if v23 else 3, fid + " encoding")
    if "DATE" in fields:
        if v23:
            c.eq(text_of(t, "TYER"), [fields["DATE"][:4]] if fields["DATE"] else [], "TYER")
            c.eq(t.getall("TDAT") + t.getall("TDRC"), [], "TDAT/TDRC removed in v2.3")
        else:
            c.eq([str(x) for x in (t.getall("TDRC")[0].text if t.getall("TDRC") else [])],
                 [fields["DATE"]] if fields["DATE"] else [], "TDRC")
            c.eq(t.getall("TYER"), [], "no TYER in v2.4")
    txxx = {"MUSICBRAINZ_ALBUMID": "MusicBrainz Album Id", "MUSICBRAINZ_ALBUMARTISTID": "MusicBrainz Album Artist Id",
            "MUSICBRAINZ_ARTISTID": "MusicBrainz Artist Id", "MUSICBRAINZ_RELEASETRACKID": "MusicBrainz Release Track Id"}
    for k, desc in txxx.items():
        if k in fields:
            hits = [f for f in t.getall("TXXX") if f.desc.lower() == desc.lower()]
            c.eq([(f.desc, list(f.text)) for f in hits], [(desc, [fields[k]])] if fields[k] else [], "TXXX " + desc)
    if "MUSICBRAINZ_TRACKID" in fields:
        u = [f for f in t.getall("UFID") if f.owner == "http://musicbrainz.org"]
        v = fields["MUSICBRAINZ_TRACKID"]
        c.eq([f.data for f in u], [v.encode()] if v else [], "UFID")


def frames_except(path, drop):
    t = id3_load(path)
    out = {}
    for key, fr in t.items():
        if fr.FrameID in drop or key.split(":")[0] in drop:
            continue
        out[key] = repr(fr)
    return out


TOUCHED = {"TALB", "TPE2", "TPE1", "TIT2", "TRCK", "TPOS", "TCON", "TDRC", "TYER", "TDAT", "TIME", "TRDA"}


def mp3_case(fixture, v23, fields=FULL, check_v1=False):
    def fn(c):
        d, p = fresh(os.path.join(FIX, fixture))
        orig = p + ".orig"
        shutil.copyfile(p, orig)
        bak = os.path.join(d, "b.ctw")
        out, _ = tw("apply", p, bak, *args(fields))
        c.eq(out, "TAG_WRITE_OK", "apply")
        check_mp3_fields(c, p, fields, v23)
        if "TRACKNUMBER" in fields:
            c.eq(text_of(id3_load(p), "TRCK"), ["%s/%s" % (fields["TRACKNUMBER"], fields["TRACKTOTAL"])], "TRCK n/N")
            c.eq(text_of(id3_load(p), "TPOS"), ["%s/%s" % (fields["DISCNUMBER"], fields["DISCTOTAL"])], "TPOS n/N")
        try:
            ot = id3_load(orig)
        except ID3NoHeaderError:
            ot = None
        if ot is not None:
            # Everything not touched survives (APIC, COMM, other TXXX/UFID, PRIV).
            keep_o = {k: v for k, v in frames_except(orig, TOUCHED).items()
                      if not (k.startswith("TXXX:") and k[5:].lower().startswith("musicbrainz"))
                      and k != "UFID:http://musicbrainz.org"}
            keep_n = {k: v for k, v in frames_except(p, TOUCHED).items()
                      if not (k.startswith("TXXX:") and k[5:].lower().startswith("musicbrainz"))
                      and k != "UFID:http://musicbrainz.org"}
            c.eq(keep_n, keep_o, "untouched frames identical")
            if ot.getall("APIC"):
                c.eq(id3_load(p).getall("APIC")[0].data, COVER, "APIC data")
            if region_len(orig)[1] >= region_len(p)[1]:
                c.eq(region_len(p)[1], region_len(orig)[1], "tag size kept when it fits")
        if check_v1:
            c.eq(open(p, "rb").read()[-128:], open(orig, "rb").read()[-128:], "ID3v1 trailer identical")
        check_audio_same(c, orig, p)
        exp = {"title": fields.get("TITLE", ""), "artist": fields.get("ARTIST", ""),
               "albumartist": fields.get("ALBUMARTIST", ""), "album": fields.get("ALBUM", "")}
        if "TRACKNUMBER" in fields:
            exp.update({"track": 1, "disc": 1, "year": 1959, "genre": "Jazz"})
        check_md(c, p, exp)
        c.eq(leftovers(d), [], "no temp files")
        out, _ = tw("restore", p, bak)
        c.eq(out, "TAG_WRITE_OK", "restore")
        c.eq(sha(p), sha(orig), "restore byte-identical")
    return fn


def case_mp3_merge(c):
    d, p = fresh(os.path.join(FIX, "v24.mp3"))
    out, _ = tw("apply", p, "-", "TRACKTOTAL=15")
    c.eq(out, "TAG_WRITE_OK", "total only")
    c.eq(text_of(id3_load(p), "TRCK"), ["3/15"], "merged with existing number")
    out, _ = tw("apply", p, "-", "TRACKNUMBER=7")
    c.eq(text_of(id3_load(p), "TRCK"), ["7/15"], "merged with existing total")
    out, _ = tw("apply", p, "-", "TRACKTOTAL=")
    c.eq(text_of(id3_load(p), "TRCK"), ["7"], "total removed")
    out, _ = tw("apply", p, "-", "DISCNUMBER=")
    c.eq(out, "TAG_WRITE_OK", "disc number removed")
    c.eq(text_of(id3_load(p), "TPOS"), [], "TPOS removed with its kept total")
    h = sha(p)
    out, _ = tw("apply", p, "-", "DISCTOTAL=3")
    c.eq(out, "TAG_WRITE_BAD_ARGS", "total without number refused")
    out, _ = tw("apply", p, "-", "TRACKNUMBER=3/4")
    c.eq(out, "TAG_WRITE_BAD_ARGS", "number with slash refused for ID3")
    c.eq(sha(p), h, "untouched after refusals")
    check_md(c, p, {"track": 7, "disc": 0})


def case_mp3_v23_date_bad(c):
    d, p = fresh(os.path.join(FIX, "v23.mp3"))
    h = sha(p)
    out, _ = tw("apply", p, "-", "DATE=circa 1960")
    c.eq(out, "TAG_WRITE_BAD_ARGS", "v2.3 needs a 4-digit year")
    c.eq(sha(p), h, "untouched")


def case_mp3_remove(c):
    d, p = fresh(os.path.join(FIX, "v24.mp3"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    out, _ = tw("apply", p, "-", "ALBUMARTIST=", "MUSICBRAINZ_ALBUMID=", "MUSICBRAINZ_TRACKID=", "DATE=", "GENRE=",
                "MUSICBRAINZ_ARTISTID=")
    c.eq(out, "TAG_WRITE_OK", "apply")
    t = id3_load(p)
    c.eq(t.getall("TPE2") + t.getall("TDRC") + t.getall("TCON"), [], "text frames removed")
    c.eq([f.desc for f in t.getall("TXXX")], ["Other Thing"], "only non-MB TXXX left")
    c.eq([f.owner for f in t.getall("UFID")], ["http://example.org"], "only non-MB UFID left")
    c.eq(text_of(t, "TIT2"), ["Old title"], "title kept")
    check_audio_same(c, orig, p, decode=False)
    check_md(c, p, {"albumartist": "", "year": 0, "genre": "", "title": "Old title"})


def case_mp3_unicode(v23):
    def fn(c):
        d, p = fresh(os.path.join(FIX, "v23.mp3" if v23 else "v24.mp3"))
        orig = p + ".orig"
        shutil.copyfile(p, orig)
        f = dict(UNI)
        f["MUSICBRAINZ_ARTISTID"] = "ünïcode-ïd"
        out, _ = tw("apply", p, "-", *args(f))
        c.eq(out, "TAG_WRITE_OK", "apply")
        check_mp3_fields(c, p, f, v23)
        check_md(c, p, {"title": UNI["TITLE"], "artist": UNI["ARTIST"], "albumartist": UNI["ALBUMARTIST"]})
        check_audio_same(c, orig, p, decode=False)
    return fn


def case_mp3_grow(c):
    d, p = fresh(os.path.join(FIX, "v23.mp3"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    bak = os.path.join(d, "b.ctw")
    out, _ = tw("apply", p, bak, "GENRE=" + "g" * 5000)
    c.eq(out, "TAG_WRITE_OK", "apply")
    c.check(region_len(p)[1] > region_len(orig)[1], "tag grew")
    c.eq(text_of(id3_load(p), "TCON"), ["g" * 5000], "long value")
    check_audio_same(c, orig, p)
    out, _ = tw("restore", p, bak)
    c.eq(sha(p), sha(orig), "restore byte-identical")


def case_unsupported(c):
    for name, exp in (("v22.mp3", "TAG_WRITE_UNSUPPORTED"), ("v23_unsync.mp3", "TAG_WRITE_UNSUPPORTED"),
                      ("v23_exthdr.mp3", "TAG_WRITE_UNSUPPORTED"), ("truncated_meta.flac", "TAG_WRITE_PARSE_ERROR"),
                      ("garbage_after_meta.flac", "TAG_WRITE_PARSE_ERROR"),
                      ("tagsize_too_big.mp3", "TAG_WRITE_PARSE_ERROR")):
        d, p = fresh(os.path.join(FIX, name))
        h = sha(p)
        out, _ = tw("apply", p, os.path.join(d, "b.ctw"), "TITLE=x")
        c.eq(out, exp, name)
        c.eq(sha(p), h, name + " untouched")
        c.check(not os.path.exists(os.path.join(d, "b.ctw")), name + " no backup written")
        c.eq(leftovers(d), [], name + " no temp files")
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    link = os.path.join(d, "link.flac")
    os.symlink(p, link)
    out, _ = tw("apply", link, "-", "TITLE=x")
    c.eq(out, "TAG_WRITE_UNSUPPORTED", "symlink refused")
    os.mkdir(os.path.join(d, "dir.flac"))
    out, _ = tw("apply", os.path.join(d, "dir.flac"), "-", "TITLE=x")
    c.check(out in ("TAG_WRITE_UNSUPPORTED", "TAG_WRITE_IO_ERROR"), "directory refused: " + out)
    out, _ = tw("apply", os.path.join(d, "missing.flac"), "-", "TITLE=x")
    c.eq(out, "TAG_WRITE_IO_ERROR", "missing file")


def case_restore_negative(c):
    d, a = fresh(os.path.join(FIX, "tagged.flac"), "a.flac")
    bak = os.path.join(d, "a.ctw")
    out, _ = tw("apply", a, bak, *args(FULL))
    c.eq(out, "TAG_WRITE_OK", "apply")
    # Different file (different audio length).
    b = os.path.join(d, "b.flac")
    shutil.copyfile(os.path.join(FIX, "picture_smallpad.flac"), b)
    sh("flac", "-s", "-f", "--padding=100", "-o", b, os.path.join(FIX, "noise.wav"))
    h = sha(b)
    out, _ = tw("restore", b, bak)
    c.eq(out, "TAG_WRITE_VERIFY_FAILED", "mismatched audio length refused")
    c.eq(sha(b), h, "untouched")
    # Same length, one audio byte flipped.
    cfile = os.path.join(d, "c.flac")
    data = bytearray(open(a, "rb").read())
    data[-1000] ^= 0x01
    open(cfile, "wb").write(data)
    h = sha(cfile)
    out, _ = tw("restore", cfile, bak)
    c.eq(out, "TAG_WRITE_VERIFY_FAILED", "changed audio refused")
    c.eq(sha(cfile), h, "untouched")
    # Corrupt backup.
    bb = bytearray(open(bak, "rb").read())
    bb[60] ^= 0xFF
    cb = os.path.join(d, "corrupt.ctw")
    open(cb, "wb").write(bb)
    h = sha(a)
    out, _ = tw("restore", a, cb)
    c.eq(out, "TAG_WRITE_PARSE_ERROR", "corrupt backup refused")
    tb = os.path.join(d, "trunc.ctw")
    open(tb, "wb").write(bb[:40])
    out, _ = tw("restore", a, tb)
    c.eq(out, "TAG_WRITE_PARSE_ERROR", "truncated backup refused")
    c.eq(sha(a), h, "untouched")
    # FLAC backup onto an MP3.
    m = os.path.join(d, "m.mp3")
    shutil.copyfile(os.path.join(FIX, "v24.mp3"), m)
    h = sha(m)
    out, _ = tw("restore", m, bak)
    c.eq(out, "TAG_WRITE_BAD_ARGS", "format mismatch refused")
    c.eq(sha(m), h, "untouched")
    c.eq(leftovers(d), [], "no temp files")


def case_fail_injection(c):
    for fixture in ("tagged.flac", "v23.mp3", "notag.mp3"):
        for stage, exp in ((1, "TAG_WRITE_IO_ERROR"), (2, "TAG_WRITE_VERIFY_FAILED"), (3, "TAG_WRITE_IO_ERROR"),
                           (4, "TAG_WRITE_BACKUP_FAILED"), (5, "TAG_WRITE_IO_ERROR")):
            d, p = fresh(os.path.join(FIX, fixture))
            h = sha(p)
            bak = os.path.join(d, "b.ctw")
            open(bak, "wb").write(b"previous backup")
            grow = dict(FULL)
            grow["GENRE"] = "G" * 20000  # never fits: forces the full-copy mode
            out, err = tw("fail", stage, p, bak, *args(grow))
            tag = "%s stage %d" % (fixture, stage)
            c.eq(out, exp, tag)
            c.eq(sha(p), h, tag + " original untouched")
            c.eq(leftovers(d), [], tag + " no temp files")
            if stage != 3:
                c.eq(open(bak, "rb").read(), b"previous backup", tag + " previous backup untouched")
    # restore failing in verification leaves the current file alone
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    bak = os.path.join(d, "b.ctw")
    tw("apply", p, bak, *args(FULL))
    h = sha(p)
    out, _ = tw("failrestore", 2, p, bak)
    c.eq(out, "TAG_WRITE_VERIFY_FAILED", "restore stage 2")
    c.eq(sha(p), h, "restore failure leaves file")
    c.eq(leftovers(d), [], "no temp files")


def case_unwritable(c):
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    os.chmod(p, 0o644)
    os.chmod(d, 0o755)  # owned by root, tool runs as nobody
    h = sha(p)
    out, err = tw("apply", p, "-", *args(FULL), user=65534)
    c.eq(out, "TAG_WRITE_IO_ERROR", "unwritable dir")
    c.eq(sha(p), h, "original untouched")
    c.eq(leftovers(d), [], "no temp files")
    out, _ = tw("apply", p, "/nonexistent/dir/b.ctw", *args(FULL))
    c.eq(out, "TAG_WRITE_BACKUP_FAILED", "backup dir missing")
    c.eq(sha(p), h, "original untouched")
    c.eq(leftovers(d), [], "no temp files")


def case_footer(c):
    d, p = fresh(os.path.join(FIX, "v24_footer.mp3"))
    orig = p + ".orig"
    shutil.copyfile(p, orig)
    bak = os.path.join(d, "b.ctw")
    out, _ = tw("apply", p, bak, "TITLE=footer ok")
    c.eq(out, "TAG_WRITE_OK", "apply")
    c.eq(text_of(id3_load(p), "TIT2"), ["footer ok"], "title")
    c.eq(open(p, "rb").read(6)[5] & 0x10, 0, "footer flag dropped")
    check_audio_same(c, orig, p)
    out, _ = tw("restore", p, bak)
    c.eq(sha(p), sha(orig), "restore byte-identical")


def case_idempotent(c):
    d, p = fresh(os.path.join(FIX, "v23_v1.mp3"))
    tw("apply", p, "-", *args(FULL))
    h1 = sha(p)
    bak = os.path.join(d, "unused.ctw")
    out, err = tw("apply", p, bak, *args(FULL))
    c.eq(out, "TAG_WRITE_OK", "second apply")
    c.eq(mode_of(err), "NONE", "nothing to write")
    c.check(not os.path.exists(bak), "no backup created when nothing changes")
    c.eq(sha(p), h1, "second identical apply is byte-identical")
    d, f = fresh(os.path.join(FIX, "tagged.flac"))
    tw("apply", f, "-", *args(FULL))
    h1 = sha(f)
    tw("apply", f, "-", *args(FULL))
    c.eq(sha(f), h1, "FLAC second identical apply is byte-identical")


def v1_backup_bytes(orig_path, fmt):
    """Independently builds a version-1 backup (as the original code wrote it)."""
    d = open(orig_path, "rb").read()
    r = region_len(orig_path)[1]
    audio = d[r:]
    h = fnv64(audio)
    body = b"CTWBAK\r\n" + struct.pack("<IIQQQQ", 1, fmt, len(d), r, len(audio), h) + d[:r]
    return body + struct.pack("<Q", fnv64(body))


def fnv64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def inplace_case(fixture, fields, fmt):
    def fn(c):
        d, p = fresh(os.path.join(FIX, fixture))
        orig = p + ".orig"
        shutil.copyfile(p, orig)
        bak = os.path.join(d, "b.ctw")
        out, err = tw("apply", p, bak, *args(fields))
        c.eq(out, "TAG_WRITE_OK", "apply")
        c.eq(mode_of(err), "IN_PLACE", "mode")
        bsz = os.path.getsize(bak)
        print("    %s: file %d bytes, v2 backup %d bytes" % (fixture, os.path.getsize(p), bsz))
        c.check(bsz < 64 * 1024, "backup < 64 KiB (%d)" % bsz)
        c.eq(open(bak, "rb").read(12)[8:12], b"\x02\x00\x00\x00", "v2 backup")
        c.eq(os.path.getsize(p), os.path.getsize(orig), "size unchanged")
        c.eq(region_len(p)[1], region_len(orig)[1], "audio offset unchanged")
        check_audio_same(c, orig, p)
        if fmt == 1:
            check_flac_fields(c, p, fields)
            check_flac_structure(c, p)
            c.eq([x.data for x in FLAC(p).pictures], [x.data for x in FLAC(orig).pictures], "pictures identical")
            check_md(c, p, {"title": fields["TITLE"], "albumartist": fields["ALBUMARTIST"]})
        else:
            check_mp3_fields(c, p, fields, id3_load(orig).version[:2] == (2, 3))
            c.eq(id3_load(p).getall("APIC")[0].data, COVER, "APIC data")
            check_md(c, p, {"title": fields["TITLE"], "albumartist": fields["ALBUMARTIST"]})
        new_sha = sha(p)
        # v2 restore refuses a span that holds foreign bytes.
        data = bytearray(open(p, "rb").read())
        bk = open(bak, "rb").read()
        nsp = struct.unpack("<I", bk[56:60])[0]
        spans = [struct.unpack("<QQQ", bk[64 + 24 * i:88 + 24 * i]) for i in range(nsp)]
        total = sum(x[1] for x in spans)
        base = 64 + 24 * nsp
        print("    spans: %s" % [(o, l) for o, l, _ in spans])
        c.eq(len(bk), base + 2 * total + 8, "v2 backup size consistent")
        off, ln = spans[0][0], spans[0][1]
        ospan, nspan = bk[base:base + ln], bk[base + total:base + total + ln]
        k = next((i for i in range(ln) if ospan[i] == nspan[i]), None)
        if k is not None:
            alien = os.path.join(d, "alien" + os.path.splitext(p)[1])
            data2 = bytearray(data)
            data2[off + k] ^= 0x5A
            open(alien, "wb").write(data2)
            h = sha(alien)
            out, _ = tw("restore", alien, bak)
            c.eq(out, "TAG_WRITE_VERIFY_FAILED", "foreign span bytes refused")
            c.eq(sha(alien), h, "untouched")
        # changed audio refused
        aud = os.path.join(d, "aud" + os.path.splitext(p)[1])
        data3 = bytearray(data)
        data3[-10] ^= 1
        open(aud, "wb").write(data3)
        h = sha(aud)
        out, _ = tw("restore", aud, bak)
        c.eq(out, "TAG_WRITE_VERIFY_FAILED", "changed audio refused")
        c.eq(sha(aud), h, "untouched")
        c.eq(sha(p), new_sha, "tagged file untouched by the above")
        out, _ = tw("restore", p, bak)
        c.eq(out, "TAG_WRITE_OK", "restore")
        c.eq(sha(p), sha(orig), "restore byte-identical")
        out, _ = tw("restore", p, bak)
        c.eq(out, "TAG_WRITE_OK", "second restore: already restored")
        c.eq(sha(p), sha(orig), "still byte-identical")
        # A version-1 backup (old format) of the original still restores.
        shutil.copyfile(orig, p)
        tw("apply", p, "-", *args(fields))
        v1 = os.path.join(d, "v1.ctw")
        open(v1, "wb").write(v1_backup_bytes(orig, fmt))
        out, _ = tw("restore", p, v1)
        c.eq(out, "TAG_WRITE_OK", "v1 restore")
        c.eq(sha(p), sha(orig), "v1 restore byte-identical")
        # v2 backup after the file was later rewritten with a size change -> refused.
        tw("apply", p, bak, *args(fields))
        grow = {"GENRE": "Q" * 30000}
        out, err = tw("apply", p, "-", *args(grow))
        c.eq(mode_of(err), "FULL_COPY", "growth -> full copy")
        h = sha(p)
        out, _ = tw("restore", p, bak)
        c.eq(out, "TAG_WRITE_VERIFY_FAILED", "stale v2 backup refused after size change")
        c.eq(sha(p), h, "untouched")
        c.eq(leftovers(d), [], "no temp files")
    return fn


def inplace_fail_case(fixture, fields):
    def fn(c):
        for stage in (6, 7, 8, 4, 5):
            d, p = fresh(os.path.join(FIX, fixture))
            h = sha(p)
            bak = os.path.join(d, "b.ctw")
            out, err = tw("fail", stage, p, bak, *args(fields))
            tag = "stage %d" % stage
            if stage == 6:
                c.eq(out, "TAG_WRITE_IO_ERROR", tag)
                c.eq(sha(p), h, tag + " untouched")
            elif stage == 7:
                c.eq(out, "TAG_WRITE_IO_ERROR", tag)
                c.check(sha(p) != h, tag + " torn write reached the file")
                out, _ = tw("restore", p, bak)
                c.eq(out, "TAG_WRITE_OK", tag + " restore after torn write")
                c.eq(sha(p), h, tag + " restore byte-identical")
            elif stage == 8:
                c.eq(out, "TAG_WRITE_VERIFY_FAILED", tag)
                c.eq(sha(p), h, tag + " rolled back byte-identical")
            elif stage == 4:
                c.eq(out, "TAG_WRITE_BACKUP_FAILED", tag)
                c.eq(sha(p), h, tag + " untouched")
            else:
                c.eq(out, "TAG_WRITE_IO_ERROR", tag)
                c.eq(sha(p), h, tag + " untouched")
            c.eq(leftovers(d), [], tag + " no temp files")
    return fn


INPLACE_FIELDS = {"TITLE": "So What (in place)", "ALBUMARTIST": "Miles Davis", "ARTIST": "Miles Davis",
                  "ALBUM": "Kind of Blue", "TRACKNUMBER": "1", "TRACKTOTAL": "5", "DATE": "1959",
                  "MUSICBRAINZ_ALBUMID": FULL["MUSICBRAINZ_ALBUMID"], "MUSICBRAINZ_TRACKID": FULL["MUSICBRAINZ_TRACKID"]}


def case_unit(c):
    d, p = fresh(os.path.join(FIX, "tagged.flac"))
    h = sha(p)
    out, err = tw("unit", p)
    c.eq(out, "UNIT OK", "unit tests " + err)
    c.eq(sha(p), h, "untouched")


def main():
    if os.path.exists(CASES):
        shutil.rmtree(CASES)
    os.makedirs(CASES)
    t0 = time.time()
    make_fixtures()
    print("fixtures generated in %.1fs" % (time.time() - t0))
    run_case("unit: argument validation", case_unit)
    run_case("FLAC tagged+aliases+APPLICATION, full MB set, restore", case_flac_full)
    run_case("FLAC remove fields", case_flac_remove)
    run_case("FLAC no VORBIS_COMMENT, no padding, restore", case_flac_novc_nopad)
    run_case("FLAC no VORBIS_COMMENT, removal only", case_flac_novc_remove_only)
    run_case("FLAC big PICTURE, 16B padding, unicode, grows", case_flac_picture_grow)
    run_case("FLAC leading ID3v2 prefix, restore", case_flac_id3prefix)
    run_case("FLAC large (3 min noise), restore", case_flac_large)
    run_case("MP3 no tag -> new v2.4, restore", mp3_case("notag.mp3", False))
    run_case("MP3 ID3v2.3 UTF-16 + APIC, restore", mp3_case("v23.mp3", True))
    run_case("MP3 ID3v2.4 UTF-8, restore", mp3_case("v24.mp3", False))
    run_case("MP3 ID3v2.3 + ID3v1 trailer, restore", mp3_case("v23_v1.mp3", True, check_v1=True))
    run_case("MP3 ID3v2.4 with footer", case_footer)
    run_case("MP3 TRCK/TPOS merge rules", case_mp3_merge)
    run_case("MP3 v2.3 non-numeric DATE refused", case_mp3_v23_date_bad)
    run_case("MP3 remove fields", case_mp3_remove)
    run_case("MP3 v2.3 unicode (UTF-16 surrogates)", case_mp3_unicode(True))
    run_case("MP3 v2.4 unicode", case_mp3_unicode(False))
    run_case("MP3 tag grows past padding, restore", case_mp3_grow)
    run_case("unsupported / malformed inputs untouched", case_unsupported)
    run_case("restore refuses mismatched/corrupt backups", case_restore_negative)
    run_case("failure injection (5 stages x 3 files)", case_fail_injection)
    run_case("unwritable dir / backup dir missing", case_unwritable)
    run_case("idempotent re-apply", case_idempotent)
    run_case("in-place FLAC, 2 MB PICTURE before padding", inplace_case("picture_bigpad.flac", INPLACE_FIELDS, 1))
    run_case("in-place FLAC, tagged + APPLICATION", inplace_case("tagged.flac", INPLACE_FIELDS, 1))
    run_case("in-place MP3 ID3v2.3 + APIC", inplace_case("v23.mp3", INPLACE_FIELDS, 2))
    run_case("in-place MP3 ID3v2.4", inplace_case("v24.mp3", INPLACE_FIELDS, 2))
    run_case("in-place MP3 ID3v2.3 + ID3v1", inplace_case("v23_v1.mp3", INPLACE_FIELDS, 2))
    run_case("in-place failure injection FLAC (6,7,8,4,5)", inplace_fail_case("picture_bigpad.flac", INPLACE_FIELDS))
    run_case("in-place failure injection MP3 (6,7,8,4,5)", inplace_fail_case("v23.mp3", INPLACE_FIELDS))
    bad = [r for r in results if not r[1]]
    print("\n%d/%d cases passed" % (len(results) - len(bad), len(results)))
    sys.exit(1 if bad else 0)


main()
