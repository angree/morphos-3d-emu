# SPDX-License-Identifier: MIT
"""Host-side self test: speaks the guest protocol (big-endian) to tglhost without QEMU.
Draws a clear + Gouraud triangle (immediate mode) + an RGB565 textured quad (arrays + DrawElements),
reads pixels back and checks them. Usage: python fakeguest.py [port]"""
import socket, struct, sys, re, os

HERE = os.path.dirname(os.path.abspath(__file__))
SL = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define SLOT_(\w+) (\d+)', open(os.path.join(HERE, '..', 'bridge', 'host', 'gen_slotnums.h')).read())}
port = int(sys.argv[1]) if len(sys.argv) > 1 else 7654
s = socket.create_connection(('127.0.0.1', port))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

def cmd(op, payload=b''):
    if len(payload) % 4: payload += b'\0' * (4 - len(payload) % 4)
    s.sendall(struct.pack('>II', op, len(payload)) + payload)

def reply():
    n = struct.unpack('>I', rx(4))[0]
    return rx(n)

def rx(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise SystemExit('closed')
        b += c
    return b

def gl(name, fmt='', *a):
    cmd(SL[name], struct.pack('>' + fmt, *a))

cmd(0xF001, struct.pack('>II32s', 0x54474C42, 1, b'fakeguest'))
assert struct.unpack('>I', reply())[0] == 1
cmd(0xF003, struct.pack('>IIII', 1, 320, 240, 0))
assert struct.unpack('>I', reply())[0] == 1
cmd(SL['GLGetString'], struct.pack('>I', 0x1F01)); print('RENDERER', reply().decode())
W, H = 320, 240
for frame in range(3):
    gl('GLClearColor', 'ffff', 0.1, 0.1, 0.3, 1.0)
    gl('GLClear', 'I', 0x4100)
    gl('GLMatrixMode', 'I', 0x1701); gl('GLLoadIdentity')
    gl('GLOrtho', 'dddddd', -1, 1, -1, 1, -1, 1)
    gl('GLMatrixMode', 'I', 0x1700); gl('GLLoadIdentity')
    gl('GLDisable', 'I', 0x0DE1)
    gl('GLBegin', 'I', 4)
    gl('GLColor3f', 'fff', 1, 0, 0); gl('GLVertex3f', 'fff', -0.9, -0.35, 0)
    gl('GLColor3f', 'fff', 0, 1, 0); gl('GLVertex3f', 'fff', -0.1, -0.35, 0)
    gl('GLColor3f', 'fff', 0, 0, 1); gl('GLVertex3f', 'fff', -0.5, 0.45, 0)
    gl('GLEnd')
    cmd(SL['GLGetError']); print('frame', frame, 'err after End', hex(struct.unpack('>I', reply())[0]))
    if frame == 0:
        cmd(SL['GLGenTextures'], struct.pack('>I', 1)); tex = struct.unpack('>I', reply())[0]
        gl('GLBindTexture', 'II', 0x0DE1, tex)
        gl('GLTexParameteri', 'IIi', 0x0DE1, 0x2801, 0x2600)
        gl('GLTexParameteri', 'IIi', 0x0DE1, 0x2800, 0x2600)
        pix = b''.join(struct.pack('>H', 0xF800 if ((x >> 3) ^ (y >> 3)) & 1 else 0xFFE0) for y in range(64) for x in range(64))
        cmd(SL['GLTexImage2D'], struct.pack('>11I', 0x0DE1, 0, 0x1907, 64, 64, 0, 0x1907, 0x8363, 1, 2, len(pix)) + pix)
    gl('GLColor3f', 'fff', 1, 1, 1)
    gl('GLEnable', 'I', 0x0DE1)
    gl('GLBindTexture', 'II', 0x0DE1, tex)
    gl('GLEnableClientState', 'I', 0x8074); gl('GLEnableClientState', 'I', 0x8078)
    verts = struct.pack('>12f', 0.1, 0.1, 0, 0.9, 0.1, 0, 0.9, 0.9, 0, 0.1, 0.9, 0)
    uvs = struct.pack('>8f', 0, 0, 2, 0, 2, 2, 0, 2)
    cmd(0xF008, struct.pack('>5I', 0, 3, 0x1406, 0, 4) + verts)
    cmd(0xF008, struct.pack('>5I', 3, 2, 0x1406, 0, 4) + uvs)
    idx = struct.pack('>6H', 0, 1, 2, 0, 2, 3)
    cmd(SL['GLDrawElements'], struct.pack('>4I', 4, 6, 0x1403, len(idx)) + idx)
    gl('GLDisableClientState', 'I', 0x8074); gl('GLDisableClientState', 'I', 0x8078)
    if frame == 2:
        for (x, y) in ((80, 120), (200, 160), (5, 5)):
            cmd(SL['GLReadPixels'], struct.pack('>6I', x, y, 1, 1, 0x1908, 0x1401))
            print('pixel', (x, y), tuple(reply()))
    cmd(0xF006, struct.pack('>I', 1 if frame == 2 else 0))
    r = reply()
    swapno, w, h = struct.unpack('>III', r[:12])
    print('swap', swapno, w, h, 'mirror bytes', len(r) - 12, 'first ARGB', tuple(r[12:16]) if len(r) > 12 else None)
cmd(SL['GLGetError']); print('glGetError', hex(struct.unpack('>I', reply())[0]))
cmd(0xF005); cmd(0xF009)
s.close()
print('fakeguest OK')
