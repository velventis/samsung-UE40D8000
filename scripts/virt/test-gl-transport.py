#!/usr/bin/env python3
"""Exercise the real ARM helper protocol, high file offsets and image orientation."""
import mmap
import os
from pathlib import Path
import struct
import subprocess
import tempfile

project = Path.home() / 'samsung-UE40D8000'
runtime = project / 'work/virt-mesa/root'
def words(*a): return struct.pack('<' + 'I' * len(a), *a)
def command(op, *a): return words(op, len(a), *a)
def fbits(f): return struct.unpack('<I', struct.pack('<f', f))[0]
with tempfile.TemporaryFile() as physical, tempfile.TemporaryFile() as shared:
    physical.truncate(0xfffff000)
    shared.truncate(32 << 20)
    mem = mmap.mmap(shared.fileno(), 32 << 20)
    cp_r, cp_w = os.pipe()
    rp_r, rp_w = os.pipe()
    env = os.environ.copy()
    env.update(EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
               MESA_SHADER_CACHE_DISABLE='true', LP_NUM_THREADS='2',
               __EGL_VENDOR_LIBRARY_DIRS=str(runtime / 'usr/share/glvnd/egl_vendor.d'),
               LIBGL_DRIVERS_PATH=str(runtime / 'usr/lib/arm-linux-gnueabihf/dri'))
    proc = subprocess.Popen(['qemu-arm-static', '-L', str(runtime), '-E',
        'LD_LIBRARY_PATH=' + str(runtime / 'usr/lib/arm-linux-gnueabihf'),
        str(runtime / 'usr/bin/tvgl'), str(cp_r), str(rp_w), str(shared.fileno()), str(physical.fileno())],
        pass_fds=(cp_r, rp_w, shared.fileno(), physical.fileno()), env=env)
    os.close(cp_r); os.close(rp_w)
    def batch(data):
        mem[:len(data)] = data
        os.write(cp_w, words(len(data)))
        assert len(os.read(rp_r, 4)) == 4, 'renderer exited'
    width = int(os.environ.get('TV_TEST_WIDTH', '8'))
    height = int(os.environ.get('TV_TEST_HEIGHT', '8'))
    stride = width * 4
    pa = 0x974d86c0
    try:
        batch(command(1, 1) + command(3, 1, width, height, stride, pa) + command(5, 1, 1) +
              command(9, 1, fbits(.25), fbits(.5), fbits(.75), fbits(1)) +
              command(9, 3, 0x4000) + command(8))
        pixels = os.pread(physical.fileno(), stride * height, pa)
        assert pixels == bytes([191, 128, 64, 255]) * (width * height), pixels[:16]
        batch(command(9, 5, 0, 0, 1, 1) + command(9, 6, 0x0c11) +
              command(9, 1, fbits(1), fbits(0), fbits(0), fbits(1)) +
              command(9, 3, 0x4000) + command(8))
        pixels = os.pread(physical.fileno(), stride * height, pa)
        assert pixels[:4] == bytes([191, 128, 64, 255]), 'top row unexpectedly changed'
        bottom = (height - 1) * stride
        assert pixels[bottom:bottom+4] == bytes([0, 0, 255, 255]), 'GL bottom row was not inverted once for TV memory'
        assert pixels[bottom+4:bottom+8] == bytes([191, 128, 64, 255]), 'scissor damaged adjacent pixel'
        contexts = int(os.environ.get('TV_TEST_CONTEXTS', '1'))
        assert 1 <= contexts <= 63
        for ctx in range(2, contexts + 1):
            batch(command(1, ctx) + command(5, ctx, 1) +
                  command(9, 1, fbits(ctx / (contexts + 1)), fbits(.5), fbits(.75), fbits(1)) +
                  command(9, 3, 0x4000) + command(8))
            pixel = os.pread(physical.fileno(), 4, pa)
            expected = (191,128,round(255 * ctx / (contexts + 1)),255)
            assert all(abs(a-b) <= 1 for a,b in zip(pixel, expected)), (ctx,pixel,expected)
        if contexts > 1:
            print('Concurrent context draw/readback passed:', contexts, 'TV contexts plus EGL root')
            batch(command(5, 1, 1))
        cycles = int(os.environ.get('TV_TEST_CYCLES', '0'))
        rss = []
        for cycle in range(cycles):
            batch(command(4, 1) + command(2, 1) + command(1, 1) +
                  command(3, 1, width, height, stride, pa) + command(5, 1, 1) +
                  command(9, 1, fbits(.25), fbits(.5), fbits(.75), fbits(1)) +
                  command(9, 3, 0x4000) + command(8))
            assert os.pread(physical.fileno(), 4, pa) == bytes([191,128,64,255])
            status = Path('/proc') / str(proc.pid) / 'status'
            rss.append(int(next(line.split()[1] for line in status.read_text().splitlines()
                                if line.startswith('VmRSS:'))))
        if cycles >= 8:
            assert max(rss[4:]) - min(rss[4:]) < 32768, ('resource churn grew by >32 MiB', rss)
            print('Resource churn passed:', cycles, 'surface/context lifetimes; RSS KiB:', rss)
        print('GL protocol test passed:', width, 'x', height, 'shared descriptors, unaligned physical offset >2GiB, FBO clear, scissor, upright row readback')
    finally:
        os.close(cp_w); os.close(rp_r)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
