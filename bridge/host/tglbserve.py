# SPDX-License-Identifier: MIT
"""File server for the guest tool tglbget (GET from serve dir, PUT into the inbox dir).
   python tglbserve.py <serve_dir> <inbox_dir> [port=7660]
Listens on 127.0.0.1 only (the QEMU guest reaches it as 10.0.2.2). Names: [A-Za-z0-9._-] only."""
import os, re, socket, struct, sys, threading, time

serve, inbox = sys.argv[1], sys.argv[2]
port = int(sys.argv[3]) if len(sys.argv) > 3 else 7660
os.makedirs(inbox, exist_ok=True)
OK = re.compile(r'^[A-Za-z0-9._-]{1,64}$')

def log(*a):
    print(time.strftime('[%H:%M:%S]'), *a, flush=True)

def readline(c):
    b = b''
    while not b.endswith(b'\n') and len(b) < 300:
        x = c.recv(1)
        if not x:
            break
        b += x
    return b.decode('latin-1').strip()

def rx(c, n):
    b = bytearray()
    while len(b) < n:
        x = c.recv(min(65536, n - len(b)))
        if not x:
            raise IOError('short')
        b += x
    return bytes(b)

def handle(c):
    try:
        parts = readline(c).split()
        if len(parts) >= 2 and parts[0] == 'GET' and OK.match(parts[1]):
            p = os.path.join(serve, parts[1])
            if os.path.isfile(p):
                data = open(p, 'rb').read()
                c.sendall(struct.pack('>I', len(data)) + data)
                log('GET', parts[1], len(data))
            else:
                c.sendall(struct.pack('>I', 0xFFFFFFFF))
                log('GET', parts[1], 'MISSING')
        elif len(parts) == 3 and parts[0] == 'PUT' and OK.match(parts[1]):
            data = rx(c, int(parts[2]))
            open(os.path.join(inbox, parts[1]), 'wb').write(data)
            c.sendall(b'OK\n')
            log('PUT', parts[1], len(data))
        else:
            log('bad request', parts)
    except Exception as e:
        log('error', e)
    finally:
        c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(4)
log('tglbserve on 127.0.0.1:%d serve=%s inbox=%s' % (port, serve, inbox))
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
