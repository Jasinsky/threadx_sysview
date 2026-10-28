# Renode: live SystemView without hardware

The goal is to run the **same firmware as a J-Link build** and look at it in
the real SystemView app. Renode has no J-Link, so `sv_rtt_probe.py` plays the
probe's role from inside Renode.

| File | Role |
| --- | --- |
| `threadx_m4.repl` | platform description |
| `threadx_systemview.resc` | default: firmware + RTT probe on `127.0.0.1:19111` |
| `threadx_systemview_uart.resc` | alternative: UART transport (`-DTXSV_COMM=uart`) |
| `sv_rtt_probe.py` | Renode-side J-Link stand-in |
| `board_pl011.c` | `board_init()` (DWT on, UART up) + polled PL011 driver |

## Platform (`threadx_m4.repl`)

A generic Cortex-M4 laid out like upstream ThreadX's
`ports/cortex_m4/gnu/example_build` (its linker script, vector table and
startup are used unmodified):

| Peripheral | Address | Notes |
| --- | --- | --- |
| flash | `0x00000000`, 4 MB | |
| SRAM | `0x20000000`, 8 MB | `SEGGER_SYSVIEW_SetRAMBase(0x20000000)` |
| NVIC | `0xE000E000` | SysTick 6 MHz, matches the example's `SYSTEM_CLOCK` |
| DWT | `0xE0001000` | 6 MHz `CYCCNT`, the SystemView timestamp |
| PL011 UART0 | `0x40004000`, IRQ 5 | only used by the UART transport |

Timestamps come from the DWT model, so the SystemView timeline is in emulated
time however fast Renode runs on the host.

## Running

```sh
renode renode/threadx_systemview.resc              # from the repository root
# SystemView: Target > Recorder Configuration > IP, 127.0.0.1:19111, F5
python3 tools/sv_ip_probe.py --port 19111 --seconds 10    # or headless
```

- A portable Renode works the same way: `/path/to/renode_1.17.0-portable/renode renode/threadx_systemview.resc`.
- The `.resc` takes `$elf` (default `build-renode/demo`) and `$port` (default 19111).
- Headless in the background, keep stdin open or Renode exits at EOF:
  `sleep 600 | renode --disable-gui --console -e "include @renode/threadx_systemview.resc"`.
- The probe logs to the Renode log: `sv_rtt_probe: host connected from ...`,
  `host disconnected after N bytes`.

## The RTT probe (`sv_rtt_probe.py`)

### What a J-Link does, and what the script does

| J-Link + SystemView | `sv_rtt_probe.py` |
| --- | --- |
| finds the RTT control block in RAM (scan for `"SEGGER RTT"` or a given address) | looks up `_SEGGER_RTT` in the loaded ELF, then checks `acID == "SEGGER RTT"` |
| picks the channel named `"SysView"` | same: walks `aUp[]` / `aDown[]` for the name |
| reads up-buffer `[RdOff, WrOff)` over SWD while the CPU runs, writes `RdOff` back | same, through Renode's system bus from a host thread; the CPU is never stopped or hooked |
| writes host commands into the down-buffer, then `WrOff` | same |
| hands the stream to the SystemView app | serves SystemView's **IP Recorder** protocol on TCP |

The target side is the normal RTT contract: SystemView writes events with
`SEGGER_RTT_WriteSkipNoLock` (mode `NO_BLOCK_SKIP`: drop the event if the ring
is full and report an overflow later), and polls the down-buffer in
`SEGGER_SYSVIEW_IsStarted()`, which the adapter calls from every hook. No
firmware code moves data: the trace and transport sources are the ones a
J-Link build uses. (The Renode build still links `board_pl011.c` for
`board_init()`, which enables the DWT counter and, unused here, the UART.)

### Session lifecycle

1. SystemView connects and sends its 32-byte hello.
2. The probe resolves `_SEGGER_RTT` and the `SysView` channels again (so `reset`
   or a newly loaded ELF is picked up), waiting until the target has run
   `SEGGER_RTT_Init` / `SEGGER_SYSVIEW_Init`.
3. It discards whatever was in the up-buffer, so the session starts clean.
4. It sends the target hello (`SEGGER SystemView V3.32.00`, padded to 32 bytes)
   in **one** write. SystemView reads the hello with a single `recv()`.
5. Loop, every `poll_ms` (wall clock): parse host frames `[len][bytes]` into the
   down-buffer (waiting for free space if needed), then drain the up-buffer to
   the socket until it is empty.
6. On disconnect it writes STOP (`0x02`) into the down-buffer, so a host that
   vanished without STOP doesn't leave the target recording into a full ring.

The next connection starts again at step 1; the target's START then resends
INIT, system description and task list.

### Parameters

```
include @renode/sv_rtt_probe.py
sv_rtt_probe <port=19111> <poll_ms=1> <version="3.32">
```

`version` only affects the hello string. It runs in Renode's IronPython 2.7.

### Why not Renode's bundled `segger-rtt.py`

Renode ships `scripts/single-node/segger-rtt.py` (used by e.g.
`tests/platforms/Renesas_RA8M1.robot`). It **replaces RTT function calls**:
CPU hooks on `SEGGER_RTT_WriteNoLock`, `ReadNoLock`, `HasKey`, `PutChar`
emulate the call in Python and return with `PC = LR`. It works for a terminal,
but not for SystemView:

- SystemView writes with `SEGGER_RTT_WriteSkipNoLock` and
  `WriteWithOverwriteNoLock`, which it doesn't hook.
- Its `ReadNoLock` hook ignores the channel number and returns console input,
  so SystemView would read keystrokes as host commands.
- The RTT ring is bypassed: no buffer fill, no overflow, and trace writes cost
  nothing on the target. That is the opposite of J-Link-like behaviour.

### Differences from a real J-Link that remain

- **Drain rate** is host polling speed against emulated time, not SWD speed.
  How often the ring overflows therefore depends on how busy the PC is and how
  fast Renode runs (about 0.5× real time for this demo on the test machine).
- **Not deterministic.** Two runs give different byte streams.
- **Snapshot timing.** Memory is read while the CPU thread executes. RTT is
  designed for this (data is written before `WrOff`), so it is safe, but the
  point at which a read happens is arbitrary.

All three go away with virtual-time polling, below.

## Using Renode virtual time for the probe

Today the probe is a host thread that wakes every 1 ms of *wall-clock* time.
Renode can instead call code at points in *virtual* (emulated) time. Moving the
drain there would give:

### Benefits

1. **Reproducible captures.** With a fixed quantum and the same ELF, the bytes
   drained at each virtual instant are identical on every run. A capture
   becomes a golden file: CI (e.g. `renode-test` / Robot) can diff event
   order, counts and emulated-µs timing against it, and a change in kernel
   behaviour shows up as a diff, not as noise.
2. **A probe model you can tune to your hardware.** Give the probe a byte budget
   per virtual millisecond. Set it to what your J-Link actually sustains (measure
   on the board; SystemView shows the data rate) and the overflow behaviour of
   `SEGGER_SYSVIEW_RTT_BUFFER_SIZE` in emulation matches the board. That lets
   you size the RTT buffer and find "too many events per ms" before hardware
   exists. With wall-clock polling the buffer-size answer changes with PC load.
3. **Independent of host speed.** Running Renode faster or slower (other load,
   `SetAdvanceImmediately`, a debugger attached, a slow CI runner) no longer
   changes what the target sees.
4. **Consistent memory snapshots.** At a sync point every CPU is stopped at a
   quantum boundary, so `WrOff` and the ring contents are read from one state
   instead of racing the executing CPU.
5. **Optional lossless mode for CI.** A virtual-time hook may block: if the host
   side can't keep up, emulation waits instead of the ring overflowing. A real
   probe can't do that, so it must stay a separate, explicit mode, but it is
   handy when a test wants every event.
6. **Deterministic host commands.** Inject START at a fixed virtual time (or use
   `TXSV_AUTOSTART`) instead of whenever someone presses F5, so the recording
   always begins at the same instruction.

### How it would look

- Register a periodic callback in virtual time: either a clock entry on the
  machine's clock source (`machine.ClockSource.AddClockEntry`, the mechanism
  Renode's timer peripherals use) with a period of e.g. 1 ms emulated, or the
  time source's sync hook (`machine.LocalTimeSource`, `SyncHook`), which runs
  at every quantum boundary. Renode also has `ExecuteInNearestSyncedState` for
  "run this at the next sync point". These APIs exist in Renode 1.17
  (`Infrastructure.dll`); calling them from IronPython hasn't been tried here.
- In the callback: read up to `budget` bytes from the up-buffer, advance
  `RdOff`, apply pending host frames to the down-buffer. Do **no socket I/O**
  there; push the bytes into a queue that the existing network thread sends.
- Keep the quantum small enough for the poll period
  (`emulation SetGlobalQuantum "0.0001"`); smaller quanta cost speed.
- For CI, write the stream to a file instead of a socket and compare against a
  golden capture, decoded with `tools/sv_ip_probe.py`'s decoder.

### Trade-offs

- More sync points slow emulation somewhat.
- Interactive use with the SystemView GUI is still wall-clock driven (when you
  press F5); virtual time helps reproducibility, not GUI latency.
- A throughput budget is a model; the real J-Link's rate also depends on SWD
  clock, target bus load and host USB.

Recommendation: keep the wall-clock thread as the interactive default and add a
`virtual` mode (`sv_rtt_probe <port> <poll_ms> virtual <bytes_per_ms>`) for CI
and buffer sizing.

## UART transport (`threadx_systemview_uart.resc`)

For comparison with probe-less targets. Build with `-DTXSV_COMM=uart`; the
firmware then runs a pump thread on the PL011 (see `txsv/README.md`).

| SystemView recorder | Renode side | Host side |
| --- | --- | --- |
| IP Recorder (any OS) | `threadx_systemview_uart.resc` (socket :19112) + `tools/sv_renode_bridge.py` (listens on :19111) | IP, `127.0.0.1:19111` |
| UART Recorder (Linux/macOS) | replace the socket with the pty lines commented in the `.resc` | UART, `/tmp/sysview-uart` |

Pitfalls found:

- **The bridge is required** for the real SystemView app. Renode writes UART
  output to the socket a few bytes at a time, and SystemView reads the 32-byte
  hello with one `recv()` ("Failed to receive Hello message"). The bridge
  reassembles the hello and sends it in one write.
- **`CreateServerSocketTerminal $port "sysview" false true`**: `false` turns off
  telnet negotiation (its bytes break the handshake); `true` (flushOnConnect)
  drops bytes the previous client never read, otherwise a reconnect gets old
  trace data instead of the hello.
- **Enable the UART before the CPU runs.** SystemView may connect as soon as the
  socket exists, and Renode's PL011 drops RX bytes while `UARTEN` is 0, so the
  `.resc` writes `LCR_H`/`CR` in the reset macro.
- **Reconnects**: the target treats a host frame length of `'S'` as a new hello,
  stops the old recording and discards its buffer.
- Only one Renode can own port 19112.
