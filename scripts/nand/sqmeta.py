# SquashFS 4 metadata from a partial image: every data/fragment block
import zlib, struct
UNIT = 1 << 20

def superblock(img):
    v = struct.unpack_from('<IIIIIHHHHHHQ', img, 0)
    t = struct.unpack_from('<QQQQQQQ', img, 40)
    return dict(inodes=v[1], bs=v[3], frags=v[4], root=v[11], bytes_used=t[0],
                id=t[1], xattr=t[2], inode=t[3], dir=t[4], frag=t[5], export=t[6])

def meta_stream(img, start, end):
    """decompress metadata blocks in [start, end): bytes and block offsets"""
    out = bytearray(); offs = {}; pos = start
    while pos < end:
        h = struct.unpack_from('<H', img, pos)[0]
        n = h & 0x7fff
        raw = img[pos + 2:pos + 2 + n]
        offs[pos - start] = len(out)
        out += raw if h & 0x8000 else zlib.decompress(raw)
        pos += 2 + n
    return bytes(out), offs

def blocks(img, sb):
    """(start, size, compressed) of all data and fragment blocks"""
    res = []
    ino, _ = meta_stream(img, sb['inode'], sb['dir'])
    bs = sb['bs']; p = 0
    while p < len(ino):
        t = struct.unpack_from('<H', ino, p)[0]
        if t == 1: p += 16 + 16
        elif t == 2 or t == 9:
            if t == 2:
                start, frag, off, size = struct.unpack_from('<IIII', ino, p + 16); q = p + 32
            else:
                start, size, sparse, nl, frag, off, xa = struct.unpack_from('<QQQIIII', ino, p + 16); q = p + 56
            n = size // bs if frag != 0xffffffff else (size + bs - 1) // bs
            pos = start
            for i in range(n):
                s = struct.unpack_from('<I', ino, q + 4 * i)[0]
                ln = s & 0xffffff
                if ln: res.append((pos, ln, not s & (1 << 24)))
                pos += ln
            p = q + 4 * n
        elif t == 3 or t == 10:
            nl, sz = struct.unpack_from('<II', ino, p + 16); p += 24 + sz + (4 if t == 10 else 0)
        elif t == 8:
            nl, fsz, sblk, par, ic, off, xa = struct.unpack_from('<IIIIHHI', ino, p + 16)
            p += 40
            for i in range(ic):
                ns = struct.unpack_from('<I', ino, p + 8)[0]; p += 12 + ns + 1
        elif t in (4, 5): p += 24
        elif t in (6, 7): p += 20
        elif t in (11, 12): p += 28
        elif t in (13, 14): p += 24
        else: raise ValueError('inode type %d at %d' % (t, p))
    # fragment table: index of u64 pointers to metadata blocks of 16-byte entries
    nidx = (sb['frags'] * 16 + 8191) // 8192
    for i in range(nidx):
        mb = struct.unpack_from('<Q', img, sb['frag'] + 8 * i)[0]
        h = struct.unpack_from('<H', img, mb)[0]; raw = img[mb + 2:mb + 2 + (h & 0x7fff)]
        ent = raw if h & 0x8000 else zlib.decompress(raw)
        for k in range(0, len(ent), 16):
            st, sz, _ = struct.unpack_from('<QII', ent, k)
            ln = sz & 0xffffff
            if ln: res.append((st, ln, not sz & (1 << 24)))
    return res
