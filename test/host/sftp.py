"""SFTP host test client (paramiko), driven by test/host/sftp.sh.

usage: sftp.py <port> <keyfile> <workdir> [nohardlinks]

Against a host build of ssh_server.c linked to the vendored libssh, checks:
  - 3 sessions in a row, each: a pipelined get, a get with one 32 KB read in
    flight (like `sftp -R 1`), and a pipelined put of <workdir>/src.bin, every
    copy sha256-checked. paramiko closes the channel before the server does,
    which is the order that crashed the server before the close-order fix.
  - a 1 MB SSH_FXP_READ is answered short (261120 bytes, matching the file)
    and the session stays usable.
  - rename (plain rename onto an existing name refused, posix-rename replaces
    it), fstat on an open handle, fsetstat (chmod and utime through the handle,
    then stat), and hardlink@openssh.com (offered in SSH_FXP_VERSION; a raw
    extended request makes a real link).
  - the same rename and hardlink checks in <workdir>/nolink, where the
    server's link() reports success without creating anything: the rename
    still moves the file (never deletes it) and hardlink answers an error.
    These all run, then the script exits non-zero if any failed.
With nohardlinks (a server built with SFTP_NO_HARDLINKS, as for the PS5) only
the rename checks run, plus: hardlink@openssh.com is not in the server's
SSH_FXP_VERSION extensions and answers SSH_FX_OP_UNSUPPORTED, making no name.
Prints one line per check and exits non-zero on the first failure. Rates are
printed for reference only: loopback has no real round trip, so they say
nothing about the console.
"""
import hashlib
import os
import struct
import sys
import time

import paramiko
from paramiko.message import Message
from paramiko.sftp import (CMD_DATA, CMD_EXTENDED, CMD_INIT, CMD_READ,
                           CMD_STATUS, CMD_VERSION)
from paramiko.sftp_file import int64

SSH_FX_OP_UNSUPPORTED = 8
port, keyf, work = int(sys.argv[1]), sys.argv[2], sys.argv[3]
nohardlinks = len(sys.argv) > 4 and sys.argv[4] == "nohardlinks"
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


def ext_status(s, name, *args):
    """Send extended request `name`; return its status reply (code, message).
    paramiko's _request() reads and raises on the status itself, so the reply
    is read here instead."""
    num = s._async_request(type(None), CMD_EXTENDED, name, *args)
    tp, data = s._read_packet()
    msg = Message(data)
    rid = msg.get_int()
    with s._lock:
        s._expecting.pop(rid, None)
    if tp != CMD_STATUS or rid != num:
        raise IOError(f"reply type {tp} id {rid}, want a status for {num}")
    return msg.get_int(), msg.get_text()


def server_extensions():
    """The extension names in the server's SSH_FXP_VERSION (paramiko drops
    them), from an INIT on a connection of its own (the server takes one
    channel per connection)."""
    t = paramiko.Transport(("127.0.0.1", port))
    t.connect(username="test", pkey=key)
    try:
        return _version_extensions(t.open_session())
    finally:
        t.close()


def _version_extensions(ch):
    """Send SSH_FXP_INIT on channel ch; return the reply's extension names."""
    ch.settimeout(20)
    ch.invoke_subsystem("sftp")
    ch.sendall(struct.pack(">IBI", 5, CMD_INIT, 3))

    def recv(n):
        b = b""
        while len(b) < n:
            got = ch.recv(n - len(b))
            if not got:
                raise EOFError("channel closed in SSH_FXP_VERSION")
            b += got
        return b

    body = recv(struct.unpack(">I", recv(4))[0])
    ch.close()
    if body[0] != CMD_VERSION:
        raise IOError(f"reply type {body[0]} to INIT")
    m, names = Message(body[5:]), []
    while m.get_remainder():
        names.append(m.get_text())
        m.get_text()  # the extension's data
    return names


want = sha(src)
size = os.path.getsize(src)
for r in range(0 if nohardlinks else 3):
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

if not nohardlinks:
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

# --- rename, fstat, fsetstat, posix-rename, hardlink --------------------
bad = []


def soft(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        bad.append(what)


def attempt(what, fn):
    """Run fn() -> (ok, detail); an exception is a FAIL, not a crash."""
    try:
        ok, detail = fn()
    except Exception as e:  # server refused or dropped the request
        ok, detail = False, f"{type(e).__name__}: {e}"
    soft(ok, f"{what}: {detail}")


t, s = connect()
ops = os.path.join(work, "ops")
os.makedirs(ops, exist_ok=True)
a, b, c = (os.path.join(ops, n) for n in ("a", "b", "c"))
link = os.path.join(ops, "link")
for pth in (a, b, c, link):
    if os.path.lexists(pth):
        os.remove(pth)


def wr(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


def rd(path):
    with open(path, "rb") as fh:
        return fh.read()


def t_rename():
    wr(a, b"alpha")
    s.rename(a, b)
    return (not os.path.exists(a) and rd(b) == b"alpha"), "a -> b, old name gone, content kept"


def t_rename_refused():
    wr(a, b"new-a")
    try:
        s.rename(a, b)
    except IOError:
        refused = True
    else:
        refused = False
    return (refused and rd(a) == b"new-a" and rd(b) == b"alpha"), \
        "plain rename onto an existing name refused, both files untouched"


def t_posix_rename():
    s.posix_rename(a, b)
    return (not os.path.exists(a) and rd(b) == b"new-a"), "posix-rename onto an existing name replaces it"


def t_fstat():
    data = os.urandom(12345)
    f = s.open(c, "wb")
    os.chmod(c, 0o644)  # the server creates it with the client's (absent) mode
    f.write(data)
    f.flush()
    sz = f.stat().st_size
    f.close()
    return (sz == len(data) and rd(c) == data), f"fstat on open handle: size {sz} == {len(data)}"


def t_fsetstat():
    f = s.open(c, "rb")
    f.chmod(0o600)
    f.utime((1000000000, 1100000000))
    f.close()
    st = s.stat(c)
    ho = os.stat(c)
    ok = ((st.st_mode & 0o777) == 0o600 and (ho.st_mode & 0o777) == 0o600
          and st.st_atime == 1000000000 and st.st_mtime == 1100000000
          and int(ho.st_mtime) == 1100000000)
    return ok, f"chmod 0600 + utime via handle, then stat: mode {st.st_mode & 0o777:o} mtime {st.st_mtime}"


def t_hardlink():
    code, _ = ext_status(s, "hardlink@openssh.com", c, link)
    ok = (code == 0 and rd(link) == rd(c) and os.stat(link).st_ino == os.stat(c).st_ino
          and os.stat(c).st_nlink == 2)
    return ok, f"hardlink@openssh.com status {code}, same content, same inode, nlink 2"


# In <workdir>/nolink the server's link() returns 0 and creates nothing (sftp.sh
# sets ZSSHD_TEST_LINK_NOOP_DIR; test/host/link_fault.c). A plain rename must
# still move the file, never delete it.
nl = os.path.join(work, "nolink")
os.makedirs(nl, exist_ok=True)
na, nb, nc_, nlink = (os.path.join(nl, n) for n in ("a", "b", "c", "link"))
for pth in (na, nb, nc_, nlink):
    if os.path.lexists(pth):
        os.remove(pth)


def t_nolink_rename():
    wr(na, b"alpha")
    try:
        s.rename(na, nb)
        st = "OK"
    except IOError as e:
        st = f"refused ({e})"
    kept = os.path.exists(nb) and rd(nb) == b"alpha"
    return (not os.path.exists(na) and kept), \
        f"rename answered {st}; a gone {not os.path.exists(na)}, b holds the data {kept}"


def t_nolink_rename_refused():
    wr(na, b"new-a")
    try:
        s.rename(na, nb)
    except IOError:
        refused = True
    else:
        refused = False
    return (refused and os.path.exists(na) and rd(na) == b"new-a" and rd(nb) == b"alpha"), \
        f"rename onto an existing name refused {refused}, both files untouched"


def t_nolink_hardlink():
    wr(nc_, b"gamma")
    code, text = ext_status(s, "hardlink@openssh.com", nc_, nlink)
    answer, refused = f"status {code} ({text})", code != 0
    return (refused and not os.path.lexists(nlink) and rd(nc_) == b"gamma"), \
        f"hardlink@openssh.com answered {answer} (want an error), no link, c kept"


def t_hardlink_not_offered():
    names = server_extensions()
    return ("hardlink@openssh.com" not in names and "posix-rename@openssh.com" in names), \
        f"SSH_FXP_VERSION extensions {names}: no hardlink@openssh.com, posix-rename kept"


def t_hardlink_offered():
    names = server_extensions()
    return ("hardlink@openssh.com" in names and "posix-rename@openssh.com" in names), \
        f"SSH_FXP_VERSION extensions {names}: hardlink and posix-rename offered"


def t_hardlink_unsupported():
    wr(c, b"gamma")
    code, text = ext_status(s, "hardlink@openssh.com", c, link)
    return (code == SSH_FX_OP_UNSUPPORTED and not os.path.lexists(link) and rd(c) == b"gamma"), \
        f"hardlink@openssh.com answered status {code} ({text}), want {SSH_FX_OP_UNSUPPORTED}; no link, c kept"


attempt("rename", t_rename)
attempt("rename refused", t_rename_refused)
attempt("posix-rename", t_posix_rename)
if nohardlinks:
    attempt("no hardlinks: hardlink not offered", t_hardlink_not_offered)
    attempt("no hardlinks: hardlink unsupported", t_hardlink_unsupported)
else:
    attempt("fstat", t_fstat)
    attempt("fsetstat", t_fsetstat)
    attempt("hardlink offered", t_hardlink_offered)
    attempt("hardlink", t_hardlink)
    attempt("rename, link() a no-op", t_nolink_rename)
    attempt("rename refused, link() a no-op", t_nolink_rename_refused)
    attempt("hardlink, link() a no-op", t_nolink_hardlink)
try:
    t.close()
except Exception:
    pass
if bad:
    sys.exit(1)
