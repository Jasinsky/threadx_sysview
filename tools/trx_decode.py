#!/usr/bin/env python3
"""
trx_decode.py - decode a ThreadX native trace buffer (TraceX .trx layout,
common/inc/tx_trace.h) into (a) a normalized event list, (b) inferred
thread-execution intervals, and (c) Chrome Trace Event JSON for Perfetto.

Usage: trx_decode.py native_trace.trx [--json out.json] [--hz CPU_HZ] [--stats]

Semantics notes (see assessment doc):
  * RESUME/SUSPEND are readiness transitions, not execution. Field I4 is
    _tx_thread_execute_ptr *after* the transition, i.e. the scheduler's
    decision. Native traces have no "thread started running" record; the
    switch instant is inferred from I4 and from the running-thread field
    of subsequent events.
  * The buffer is circular with no loss counter; the header's current
    pointer marks the oldest entry. Wrap count must come from the
    full-notify callback (we emit it as a user event).
"""
import argparse, json, struct, sys

HDR = struct.Struct("<4I2H8I")          # TX_TRACE_HEADER (little endian)
OBJ_NAME = 32
ENTRY = struct.Struct("<8I")

OBJ_TYPES = {1: "thread", 2: "timer", 3: "queue", 4: "semaphore", 5: "mutex",
             6: "event_flags", 7: "block_pool", 8: "byte_pool"}

EVENT_NAMES = {
    1: "THREAD_RESUME", 2: "THREAD_SUSPEND", 3: "ISR_ENTER", 4: "ISR_EXIT",
    5: "TIME_SLICE", 6: "RUNNING",
    32: "tx_event_flags_get", 36: "tx_event_flags_set", 40: "tx_interrupt_control",
    52: "tx_mutex_get", 57: "tx_mutex_put", 68: "tx_queue_receive", 69: "tx_queue_send",
    83: "tx_semaphore_get", 88: "tx_semaphore_put", 100: "tx_thread_create",
    108: "tx_thread_priority_change", 112: "tx_thread_sleep",
    130: "tx_timer_create", 81: "tx_semaphore_create", 50: "tx_mutex_create",
    60: "tx_queue_create", 30: "tx_event_flags_create",
}
ISR_CTX, INIT_CTX = 0xFFFFFFFF, 0xF0F0F0F0


def decode(buf):
    (tid, mask, base, reg_start, _r1, name_sz, reg_end, buf_start, buf_end,
     buf_cur, *_rest) = HDR.unpack_from(buf, 0)
    if tid != 0x54585442:
        sys.exit("not a ThreadX trace buffer (bad TXTB id 0x%08x)" % tid)
    off = lambda p: p - base
    objects = {}
    entry_sz = 4 + 4 * 3 + name_sz
    p = off(reg_start)
    while p + entry_sz <= off(reg_end):
        avail, otype, _a, _b, ptr, p1, p2 = struct.unpack_from("<4B3I", buf, p)
        name = buf[p + 16:p + 16 + name_sz].split(b"\0")[0].decode(errors="replace")
        if not avail and otype in OBJ_TYPES:
            objects[ptr] = {"type": OBJ_TYPES[otype], "name": name, "p1": p1, "p2": p2}
        p += entry_sz
    n = (off(buf_end) - off(buf_start)) // ENTRY.size
    first = (off(buf_cur) - off(buf_start)) // ENTRY.size
    events = []
    for k in range(n):                       # oldest -> newest
        i = (first + k) % n
        th, prio, eid, ts, i1, i2, i3, i4 = ENTRY.unpack_from(buf, off(buf_start) + i * ENTRY.size)
        if th == 0 or eid == 0xFFFFFFFF:     # never written
            continue
        events.append(dict(thread=th, prio=prio, id=eid, ts=ts, info=(i1, i2, i3, i4)))
    return dict(mask=mask, objects=objects, events=events, capacity=n)


def unwrap_timestamps(events, mask):
    hi, last, out = 0, None, 0
    for e in events:
        t = e["ts"] & mask
        if last is not None and t < last:
            hi += mask + 1                   # timestamp wrap
        last = t
        e["t"] = hi + t
    return events


def infer_execution(events):
    """Return [(thread_ptr, t_start, t_end)] from readiness events + running field."""
    runs, cur, since = [], None, None
    for e in events:
        ctx = e["thread"]
        if ctx not in (ISR_CTX, INIT_CTX) and ctx != cur:
            if cur is not None:
                runs.append((cur, since, e["t"]))
            cur, since = ctx, e["t"]
        if e["id"] in (1, 2):                # scheduler decision in I4
            nxt = e["info"][3]
            if nxt and nxt != cur and ctx != ISR_CTX:
                if cur is not None:
                    runs.append((cur, since, e["t"]))
                cur, since = nxt, e["t"]
    if cur is not None and events:
        runs.append((cur, since, events[-1]["t"]))
    return [r for r in runs if r[2] > r[1]]


def name_of(objs, ptr):
    o = objs.get(ptr)
    return o["name"] if o else "0x%08x" % ptr


def to_chrome_json(d, runs, hz):
    us = lambda t: t * 1e6 / hz
    out, objs = [], d["objects"]
    tids = {p: i + 1 for i, p in enumerate(sorted({r[0] for r in runs}))}
    out.append({"ph": "M", "pid": 1, "name": "process_name", "args": {"name": "ThreadX target"}})
    for p, tid in tids.items():
        out.append({"ph": "M", "pid": 1, "tid": tid, "name": "thread_name", "args": {"name": name_of(objs, p)}})
    out.append({"ph": "M", "pid": 1, "tid": 999, "name": "thread_name", "args": {"name": "ISR"}})
    for p, a, b in runs:
        out.append({"ph": "X", "pid": 1, "tid": tids[p], "name": "running", "ts": us(a), "dur": us(b - a)})
    for e in d["events"]:
        tid = 999 if e["thread"] == ISR_CTX else tids.get(e["thread"], 0)
        nm = EVENT_NAMES.get(e["id"], "user_%d" % (e["id"] - 4096) if e["id"] >= 4096 else "evt_%d" % e["id"])
        args = {"I%d" % (k + 1): "0x%08x" % v for k, v in enumerate(e["info"])}
        if e["info"][0] in objs:
            args["object"] = objs[e["info"][0]]["name"]
        out.append({"ph": "i", "s": "t", "pid": 1, "tid": tid, "name": nm, "ts": us(e["t"]), "args": args})
    return {"traceEvents": out, "displayTimeUnit": "ns"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trx")
    ap.add_argument("--json")
    ap.add_argument("--hz", type=float, default=6e6)
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()
    d = decode(open(a.trx, "rb").read())
    unwrap_timestamps(d["events"], d["mask"])
    runs = infer_execution(d["events"])
    if a.json:
        json.dump(to_chrome_json(d, runs, a.hz), open(a.json, "w"))
    if a.stats:
        from collections import Counter
        ev = d["events"]
        mono = all(ev[i]["t"] <= ev[i + 1]["t"] for i in range(len(ev) - 1))
        threads = {p: o["name"] for p, o in d["objects"].items() if o["type"] == "thread"}
        exec_t = Counter()
        for p, s, e in runs:
            exec_t[name_of(d["objects"], p)] += e - s
        print(json.dumps({
            "capacity": d["capacity"], "decoded": len(ev),
            "objects": {o["name"]: o["type"] for o in d["objects"].values()},
            "monotonic": mono, "span_cycles": ev[-1]["t"] - ev[0]["t"] if ev else 0,
            "switches_inferred": len(runs),
            "isr_events": sum(1 for e in ev if e["thread"] == ISR_CTX),
            "top_events": {EVENT_NAMES.get(k, str(k)): v for k, v in Counter(e["id"] for e in ev).most_common(8)},
            "exec_share": {k: round(v / max(1, sum(exec_t.values())), 3) for k, v in exec_t.most_common()},
            "user_events": [(e["id"] - 4096, e["info"][:2]) for e in ev if e["id"] >= 4096][-3:],
        }, indent=1))


if __name__ == "__main__":
    main()
