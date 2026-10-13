import hashlib, subprocess, sys, zlib, os
src, out = sys.argv[1], sys.argv[2]
fw = open(os.path.join(src, "info.txt")).read().split()[0]
secret = "SHWJUH:85a045ae-2296-484c-b457-ede832fcfbe1-646390a3-105e-40aa-85f6-da3086c70111"
pw = hashlib.sha1(secret.encode()).hexdigest()
valid = open(os.path.join(src, "validinfo.txt")).read()
xk = fw.encode()
for name in ["Image", "rootfs.img", "appext.img", "exe.img"]:
    d = open(os.path.join(src, name + ".sec"), "rb").read()
    siglen = int(d[-4:-1])
    body = d[:len(d) - 4 - siglen]
    aes = subprocess.run(["openssl", "enc", "-d", "-aes-128-cbc", "-md", "md5", "-nopad", "-pass", "pass:" + pw],
                         input=body, capture_output=True, check=True).stdout
    pad = aes[-1]
    if 1 <= pad <= 16 and aes[-pad:] == bytes([pad]) * pad:
        aes = aes[:-pad]
    k = (xk * (len(aes) // len(xk) + 1))[:len(aes)]
    plain = (int.from_bytes(aes, "big") ^ int.from_bytes(k, "big")).to_bytes(len(aes), "big")
    open(os.path.join(out, name), "wb").write(plain)
    crc = "%08x" % (zlib.crc32(plain) & 0xffffffff)
    want = valid.split("_" + name + "_")[1][:8]
    print(f"{name:12} size={len(plain):>10} crc={crc} expected={want} {'OK' if crc == want else 'MISMATCH'}")
