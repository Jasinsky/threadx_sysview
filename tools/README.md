# Host tools

All tools are plain Python 3 with no third-party packages.

| Tool | Purpose |
| --- | --- |
| `gen_sysview_desc.py` | generate the SystemView description file and the adapter's thread-parameter table |
| `sv_ip_probe.py` | headless SystemView IP-Recorder client: handshake, capture, decode, summary |
| `sv_renode_bridge.py` | relay for the UART transport (reassembles the target hello) |
| `trx_decode.py` | native ThreadX `.trx` → event list, statistics, Perfetto JSON |

The Renode-side probe (`renode/sv_rtt_probe.py`) runs inside Renode and is
documented in [`../renode/README.md`](../renode/README.md).

## `gen_sysview_desc.py`

```sh
python3 tools/gen_sysview_desc.py deps/threadx/common/inc/tx_trace.h deps/threadx/common/inc/tx_api.h \
  txsv/SYSVIEW_ThreadX_native.txt txsv/txsv_thread_params.h
```

Re-run on every ThreadX update and commit both outputs.

What it reads:

- every `#define TX_TRACE_<X> <n> /* I1 = ..., I2 = ... */` in `tx_trace.h`
  (event name, ID, parameter descriptions; first definition wins for IDs
  defined twice under `#ifdef`)
- the thread states `TX_READY` .. `TX_PRIORITY_CHANGE` from `tx_api.h`

What it writes:

- **`SYSVIEW_ThreadX_native.txt`**: one line per trace point,
  `<n + 32> tx_<x> <Param> = <format>, ...`, plus `Option ReversePriority`
  (ThreadX: lower number = higher priority), the `TaskState` names and two
  value-name tables, `WaitOption` and `ThreadState`.
- **`txsv_thread_params.h`**: for each trace ID with thread-pointer
  parameters, a bitmask (bit n = parameter I(n+1)). The adapter sends those as
  SystemView task IDs.

Parameter formats are picked from the description text:

| Description contains | Format |
| --- | --- |
| `wait option` | `%WaitOption` (`TX_NO_WAIT`, `TX_WAIT_FOREVER`, else number) |
| `thread ptr`, or is `thread` / `owning thread` / `next thread` / `current thread` | `%t` (task name) |
| is `previous_state` / `new_state` / `thread state` | `%ThreadState` |
| `ptr`, `flags`, `address`, `start`, or is `stack` | `%p` |
| anything else | `%u` |

### Description-file syntax pitfalls

SystemView V4.10b only reports `Warning: Failed to parse description file.`
with no line number. Rules learned the hard way:

- `NamedType` values are space-separated `value=Name` tokens. No commas and no
  spaces inside a name (`5=Queue_susp`, not `5=Queue susp,`). The first version
  of the file broke on exactly this.
- Don't call a `NamedType` `TaskState`; that word is also the keyword for the
  task-state line. The generator uses `ThreadState`.
- `*=%u` as the first token gives unlisted values a default format (as in
  SEGGER's Zephyr file).
- The `TaskState <mask> 0=Name, 1=Name ...` line *does* use commas and may
  contain spaces.
- Event IDs must be 32..511.

If parsing fails again, bisect: keep the header and a few event lines, check,
then add halves back.

## `sv_ip_probe.py`

Acts like SystemView's IP Recorder.

```sh
python3 tools/sv_ip_probe.py [--host 127.0.0.1] [--port 19111] [--seconds 5] \
                             [--out capture.bin] [--wait 0]
```

1. Connects (retries for 60 s), sends the 32-byte host hello, checks the target hello.
2. Sends START (`[1][0x01]`), records for `--seconds`, sends STOP, closes.
3. Saves the raw stream to `--out`. SystemView can open it with
   *File → Load Data*.
4. Decodes standard packets and prints: target hello, byte and packet count,
   system description, task names, timestamp span, top event counts. An
   `OVERFLOW` line means the target's RTT ring overflowed.

Works against `renode/sv_rtt_probe.py` (port 19111), against the UART
transport through the bridge (19111) or directly (19112).

`decode()` and `varint()` can be imported for ad-hoc checks, e.g. decoding the
parameters of one event ID from a capture:

```python
import sys; sys.path.insert(0, "tools")
from sv_ip_probe import decode, varint
events = decode(open("capture.bin", "rb").read())   # [(id, fields, delta_ts), ...]
```

For events ≥ 24, `fields[0]` holds the raw payload; parameters are varints.

## `sv_renode_bridge.py`

Only for the UART transport (`TXSV_COMM=uart`).

```sh
python3 tools/sv_renode_bridge.py [--listen 19111] [--target-host 127.0.0.1] [--target-port 19112]
```

SystemView reads the target hello with a single `recv()`, but Renode's UART
socket terminal delivers it a few bytes at a time. The bridge collects the
whole 32-byte hello, sends it in one write, drops any bytes before it, then
relays both directions unchanged. With the RTT probe this isn't needed: the
probe writes the hello in one call itself.

## `trx_decode.py`

```sh
python3 -I tools/trx_decode.py native_trace.trx [--stats] [--json out.json] [--hz 6e6]
```

Decodes a native ThreadX trace buffer (TraceX layout, `tx_trace.h`):

- header, object registry (names of threads, queues, ...), entries in
  chronological order (the buffer is circular; the header's current pointer
  marks the oldest entry)
- **execution intervals inferred** from the readiness events: native traces have
  no "started running" record. `THREAD_RESUME`/`SUSPEND` field I4 is
  `_tx_thread_execute_ptr` after the change, i.e. the scheduler's decision.
- `--stats`: JSON summary — capacity, decoded entries, object names, timestamp
  monotonicity, span, inferred switches, ISR events, top events, execution share
  per thread, last user events
- `--json`: Chrome Trace Event JSON; open in https://ui.perfetto.dev
- `--hz`: timestamp frequency (6 MHz in the demo)

The buffer has no loss counter. The demo counts wraps in the buffer-full
callback and reports the count in its final `USR_EVT_PHASE` user event.
