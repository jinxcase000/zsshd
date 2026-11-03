"""Keep SSH key exchanges in flight against 127.0.0.1:PORT for SECONDS
seconds from THREADS threads, for test/host/standdown.sh. Each connection
sends its banner and KEXINIT, waits a random 0 to 1.5 s, then sends
KEX_ECDH_INIT (curve25519), on which the server draws random bytes for its
own key share and signs; then it reads the reply and drops the connection.
So whenever the server is stood down, sessions are part way through a key
exchange and draw from the RNG while it tears down.
Usage: kex_flood.py PORT THREADS SECONDS"""
import os
import random
import socket
import struct
import sys
import threading
import time

port, nthreads, seconds = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
end = time.monotonic() + seconds


def string(b):
    return struct.pack(">I", len(b)) + b


def packet(payload):
    # Unencrypted binary packet: length, padding length, payload, padding;
    # the whole multiple of 8, padding at least 4 bytes.
    pad = 8 - (5 + len(payload)) % 8
    if pad < 4:
        pad += 8
    body = bytes([pad]) + payload + os.urandom(pad)
    return struct.pack(">I", len(body)) + body


KEXINIT = packet(
    bytes([20]) + os.urandom(16)
    + string(b"curve25519-sha256,curve25519-sha256@libssh.org")
    + string(b"rsa-sha2-256,rsa-sha2-512")
    + string(b"aes128-ctr") + string(b"aes128-ctr")
    + string(b"hmac-sha2-256") + string(b"hmac-sha2-256")
    + string(b"none") + string(b"none")
    + string(b"") + string(b"")
    + b"\x00" + struct.pack(">I", 0))


def one():
    s = socket.create_connection(("127.0.0.1", port), 1)
    try:
        s.settimeout(3)
        s.sendall(b"SSH-2.0-standdown\r\n" + KEXINIT)
        s.recv(4096)  # the server's banner (and usually its KEXINIT)
        time.sleep(random.uniform(0, 1.5))
        s.sendall(packet(bytes([30]) + string(os.urandom(32))))
        s.recv(4096)
    finally:
        s.close()


def flood():
    while time.monotonic() < end:
        try:
            one()
        except OSError:
            time.sleep(0.01)


threads = [threading.Thread(target=flood) for _ in range(nthreads)]
for t in threads:
    t.start()
for t in threads:
    t.join()
