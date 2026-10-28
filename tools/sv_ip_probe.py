#!/usr/bin/env python3
"""
sv_ip_probe.py - act like SystemView's IP Recorder against a target (or a
Renode UART socket): handshake, send START, capture the stream, decode the
standard packets and print a summary. Also saves the raw stream so it can be
opened later with SystemView "File > Load Data" (raw .bin).

Usage: sv_ip_probe.py [--host 127.0.0.1] [--port 19111] [--seconds 5] [--out capture.bin]
"""
import argparse, socket, sys, time
from collections import Counter

HOST_HELLO = b"SEGGER SystemView V3.32" + b"\0" * 9      # 32 bytes, like the app
CMD_START, CMD_STOP = 1, 2

NAMES = {0: "NOP", 1: "OVERFLOW", 2: "ISR_ENTER", 3: "ISR_EXIT", 4: "TASK_START_EXEC",
         5: "TASK_STOP_EXEC", 6: "TASK_START_READY", 7: "TASK_STOP_READY", 8: "TASK_CREATE",
         9: "TASK_INFO", 10: "TRACE_START", 11: "TRACE_STOP", 12: "SYSTIME_CYCLES",
         13: "SYSTIME_US", 14: "SYSDESC", 15: "MARK_START", 16: "MARK_STOP", 17: "IDLE",
         18: "ISR_TO_SCHEDULER", 19: "TIMER_ENTER", 20: "TIMER_EXIT", 21: "STACK_INFO",
         22: "MODULEDESC", 23: "DATA_SAMPLE", 24: "INIT", 25: "NAME_RESOURCE",
         26: "PRINT_FORMATTED", 27: "NUMMODULES", 28: "END_CALL", 29: "TASK_TERMINATE", 31: "EX"}
# Field layout of the fixed-structure packets (IDs < 24): u = varint U32, s = string
LAYOUT = {0: "", 1: "u", 2: "u", 3: "", 4: "u", 5: "", 6: "u", 7: "uu", 8: "u", 9: "uus",
          10: "", 11: "", 12: "u", 13: "uu", 14: "s", 15: "u", 16: "u", 17: "", 18: "",
          19: "u", 20: "", 21: "uuuu", 22: "uus", 23: "uu"}


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80:
            return v, i


def decode(b):
    i, n = 0, len(b)
    while i < n and b[i] == 0:              # 10-byte sync of NOPs
        i += 1
    out = []
    try:
        while i < n:
            eid = b[i]
            if eid < 24:
                i += 1
                fields = []
                for f in LAYOUT[eid]:
                    if f == "u":
                        v, i = varint(b, i); fields.append(v)
                    else:
                        ln = b[i]; i += 1
                        fields.append(b[i:i + ln].decode(errors="replace")); i += ln
            else:
                eid, i = varint(b, i)
                ln, i = varint(b, i)
                fields = [b[i:i + ln]]; i += ln
            if eid == 0:
                continue                       # NOPs carry no timestamp
            ts, i = varint(b, i)
            out.append((eid, fields, ts))
    except IndexError:
        pass                                   # capture ended mid-packet
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19111)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--out", default="capture.bin")
    ap.add_argument("--wait", type=float, default=0, help="seconds to let the target boot first")
    a = ap.parse_args()
    time.sleep(a.wait)

    deadline = time.time() + 60
    while True:
        try:
            s = socket.create_connection((a.host, a.port), timeout=5); break
        except OSError:
            if time.time() > deadline: sys.exit("no target listening")
            time.sleep(0.5)
    s.sendall(HOST_HELLO)
    hello = b""
    while len(hello) < 32:
        hello += s.recv(32 - len(hello))
    if not hello.startswith(b"SEGGER SystemView"):
        sys.exit("bad target hello: %s" % hello.hex(" "))
    print("target hello:", hello.split(b"\0")[0].decode())
    s.sendall(bytes([1, CMD_START]))           # IP framing: [len][cmd]
    data, t_end = bytearray(), time.time() + a.seconds
    s.settimeout(0.5)
    while time.time() < t_end:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
        except socket.timeout:
            pass
    s.sendall(bytes([1, CMD_STOP]))
    s.close()
    open(a.out, "wb").write(data)

    pk = decode(bytes(data))
    cnt = Counter(NAMES.get(e, "OS/USER %d" % e if e < 1024 else "USER %d" % e) for e, _, _ in pk)
    tasks = {f[0]: f[2] for e, f, _ in pk if e == 9}
    init = next((f for e, f, _ in pk if e == 24), None)
    sysdesc = [f[0] for e, f, _ in pk if e == 14]
    total_cycles = sum(ts for _, _, ts in pk)
    print("bytes: %d, packets decoded: %d" % (len(data), len(pk)))
    print("sysdesc:", sysdesc)
    print("tasks:", sorted(tasks.values()))
    print("span: %d timestamp ticks" % total_cycles)
    for k, v in cnt.most_common(14):
        print("  %-18s %d" % (k, v))


if __name__ == "__main__":
    main()
