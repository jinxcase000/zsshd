"""TCP proxy for test/host/session_race.sh: holds each client->server chunk
for a short delay and forwards it together with whatever else the client sent
meanwhile, so packets the client writes back to back reach the server in one
read. Server->client bytes pass straight through.

usage: coalesce_proxy.py <listen_port> <server_port> <delay_ms>
Prints "ready" once listening; runs until killed."""
import select
import socket
import sys
import threading
import time


def pump_server_to_client(srv, cli):
    try:
        while True:
            b = srv.recv(65536)
            if not b:
                break
            cli.sendall(b)
    except OSError:
        pass
    for s in (cli, srv):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def pump_client_to_server(cli, srv, delay):
    try:
        while True:
            b = cli.recv(65536)
            if not b:
                break
            time.sleep(delay)
            while select.select([cli], [], [], 0)[0]:
                more = cli.recv(65536)
                if not more:
                    break
                b += more
            srv.sendall(b)
    except OSError:
        pass
    try:
        srv.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def handle(cli, server_port, delay):
    try:
        srv = socket.create_connection(("127.0.0.1", server_port))
    except OSError:
        cli.close()
        return
    for s in (cli, srv):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t = threading.Thread(target=pump_server_to_client, args=(srv, cli), daemon=True)
    t.start()
    pump_client_to_server(cli, srv, delay)
    t.join()
    cli.close()
    srv.close()


def main():
    listen_port, server_port = int(sys.argv[1]), int(sys.argv[2])
    delay = int(sys.argv[3]) / 1000.0
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", listen_port))
    ls.listen(16)
    print("ready", flush=True)
    while True:
        cli, _ = ls.accept()
        threading.Thread(target=handle, args=(cli, server_port, delay), daemon=True).start()


main()
