"""SFTP host test client (paramiko), driven by test/host/sftp.sh.

usage: sftp.py <port> <keyfile> <workdir>

Against a host build of ssh_server.c linked to the vendored libssh, checks:
  - 3 sessions in a row, each: a pipelined get, a get with one 32 KB read in
    flight (like `sftp -R 1`), and a pipelined put of <workdir>/src.bin, every
    copy sha256-checked. paramiko closes the channel before the server does,
    which is the order that crashed the server before the close-order fix.
  - a 1 MB SSH_FXP_READ is answered short (261120 bytes, matching the file)
    and the session stays usable.
Prints one line per check and exits non-zero on the first failure. Rates are
printed for reference only: loopback has no real round trip, so they say
nothing about the console.
"""
import hashlib
import os
import sys
import time

import paramiko
from paramiko.sftp import CMD_DATA, CMD_READ
from paramiko.sftp_file import int64

port, keyf, work = int(sys.argv[1]), sys.argv[2], sys.argv[3]
src = os.path.join(work, "src.bin")
key = paramiko.Ed25519Key.from_private_key_file(keyf)


def connect():
    t = paramiko.Transport(("127.0.0.1", port))
    t.connect(username="test", pkey=key)
    return t, paramiko.SFTPClient.from_transport(t)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        sys.exit(1)


want = sha(src)
size = os.path.getsize(src)
for r in range(3):
    t, s = connect()
    dst = os.path.join(work, "get.bin")
    t0 = time.time()
    s.get(src, dst)
    dt = time.time() - t0
    check(sha(dst) == want, f"session {r}: pipelined get, {size / dt / 1e6:.0f} MB/s")

    f = s.open(src, "rb")
    h = hashlib.sha256()
    t0 = time.time()
    while True:
        b = f.read(32768)
        if not b:
            break
        h.update(b)
    dt = time.time() - t0
    f.close()
    check(h.hexdigest() == want, f"session {r}: one-at-a-time get, {size / dt / 1e6:.0f} MB/s")

    put = os.path.join(work, "put.bin")
    t0 = time.time()
    s.put(src, put)
    dt = time.time() - t0
    os.chmod(put, 0o644)  # the server creates it with the client's (absent) mode
    check(sha(put) == want, f"session {r}: pipelined put, {size / dt / 1e6:.0f} MB/s")
    s.close()
    t.close()
    time.sleep(0.5)  # let the server finish its side of the close

t, s = connect()
f = s.open(src, "rb")
tp, msg = s._request(CMD_READ, f.handle, int64(0), 1048576)
data = msg.get_string() if tp == CMD_DATA else b""
with open(src, "rb") as fh:
    head = fh.read(len(data))
check(len(data) == 261120 and data == head,
      f"1 MB read answered short: {len(data)} bytes, content matches")
check(s.stat(src).st_size == size, "session still usable after the oversized read")
t.close()
