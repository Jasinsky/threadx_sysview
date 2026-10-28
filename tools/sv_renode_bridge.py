#!/usr/bin/env python3
"""
sv_renode_bridge.py - TCP relay between the SystemView IP Recorder and the
Renode UART socket terminal.

Why: SystemView reads the target's 32-byte hello with a single recv() and
gives up ("Failed to receive Hello message") if it gets fewer bytes. Renode
writes UART output to its socket a few bytes at a time, so the hello arrives
split over several TCP segments. The bridge collects the whole hello, sends
it in one write, then relays both directions unchanged. It also drops any
stale bytes that precede the hello.

    renode renode/threadx_systemview.resc         # target on :19112
    python3 tools/sv_renode_bridge.py             # SystemView -> :19111
"""
import argparse, socket, sys, threading, time

HELLO_PREFIX = b"SEGGER SystemView V"
HELLO_SIZE = 32


def connect_target(host, port, timeout=30):
    deadline = time.time() + timeout
    while True:
        try:
            return socket.create_connection((host, port))
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.5)


def host_to_target(h, t):
    try:
        while True:
            d = h.recv(4096)
            if not d:
                break
            t.sendall(d)
    except OSError:
        pass
    for s in (h, t):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def target_to_host(t, h):
    buf = b""
    try:
        while True:                             # assemble the hello first
            d = t.recv(4096)
            if not d:
                return
            buf += d
            i = buf.find(HELLO_PREFIX)
            if i < 0:
                buf = buf[-len(HELLO_PREFIX):]  # keep a possible partial prefix
                continue
            if len(buf) - i >= HELLO_SIZE:
                h.sendall(buf[i:])              # hello + whatever followed it
                break
        while True:
            d = t.recv(65536)
            if not d:
                return
            h.sendall(d)
    except OSError:
        pass
    finally:
        for s in (h, t):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, default=19111, help="port SystemView connects to")
    ap.add_argument("--target-host", default="127.0.0.1")
    ap.add_argument("--target-port", type=int, default=19112, help="Renode socket terminal port")
    a = ap.parse_args()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.listen))
    srv.listen(1)
    print("bridge: SystemView -> 127.0.0.1:%d -> Renode %s:%d"
          % (a.listen, a.target_host, a.target_port), flush=True)
    while True:
        h, peer = srv.accept()
        h.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            t = connect_target(a.target_host, a.target_port)
        except OSError as e:
            print("bridge: cannot reach Renode: %s" % e, file=sys.stderr, flush=True)
            h.close()
            continue
        print("bridge: session from %s:%d" % peer, flush=True)
        th = [threading.Thread(target=host_to_target, args=(h, t), daemon=True),
              threading.Thread(target=target_to_host, args=(t, h), daemon=True)]
        for x in th:
            x.start()
        for x in th:
            x.join()
        h.close()
        t.close()
        print("bridge: session closed", flush=True)


if __name__ == "__main__":
    main()
