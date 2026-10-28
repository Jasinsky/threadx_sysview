# txsv — SEGGER SystemView for unmodified Eclipse ThreadX

`txsv` feeds [SEGGER SystemView](https://www.segger.com/products/development-tools/systemview/)
from **upstream, unmodified** Eclipse ThreadX. It only uses extension points
upstream already ships, so there is no kernel patch to carry and no fork to
rebase. SEGGER's own ThreadX support is a patched fork
([SEGGERMicro/threadx](https://github.com/SEGGERMicro/threadx), based on
6.4.2); this project is the zero-kernel-diff alternative.

It is a proof of concept on Cortex-M4. It runs on QEMU (post-mortem, native
TraceX buffer + Perfetto) and on Renode (live SystemView, with the target
firmware identical to a J-Link build).

| Area | Status |
| --- | --- |
| Adapter, all 4 trace backends build | verified (QEMU mps2-an386, ThreadX v6.5.2) |
| Live SystemView on Renode via emulated J-Link RTT | verified with SystemView V4.10b GUI and `tools/sv_ip_probe.py` |
| Event names, parameters, thread names (`SYSVIEW_ThreadX_native.txt`) | verified in SystemView V4.10b export |
| Live SystemView on Renode via UART (no probe) | verified, GUI through `tools/sv_renode_bridge.py` |
| Native `.trx` → Perfetto JSON | verified (decoder), Perfetto rendering not checked |
| Real silicon + J-Link | not done |

## Contents

- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Build options](#build-options)
- [Quick start: live SystemView on Renode](#quick-start-live-systemview-on-renode)
- [Quick start: QEMU post-mortem and Perfetto](#quick-start-qemu-post-mortem-and-perfetto)
- [SystemView description file](#systemview-description-file)
- [Moving to real hardware (J-Link)](#moving-to-real-hardware-j-link)
- [Known gaps](#known-gaps)
- [Creating the git repository](#creating-the-git-repository)
- [Licensing](#licensing)

Detailed documentation:

- [`txsv/README.md`](txsv/README.md) — the adapter: hooks, event mapping, IDs, configuration
- [`renode/README.md`](renode/README.md) — Renode platform, the RTT probe, UART transport, virtual time
- [`tools/README.md`](tools/README.md) — host tools (probe client, decoders, generators)
- [`app/README.md`](app/README.md) — the demo workload and the SEGGER-fork comparison build
- [`threadx-systemview-assessment.md`](threadx-systemview-assessment.md) — the original assessment and decision record

## How it works

```
 upstream ThreadX (unmodified)                    txsv                               SystemView
 ──────────────────────────────    ─────────────────────────────────────    ────────────────────────
 TX_TRACE_IN_LINE_INSERT(...)  ──▶ txsv_on_event()                          
   (~90 API/kernel trace points,     • READY/BLOCK  → OnTaskStart/StopReady  
    macro guarded by #ifndef)        • thread create/delete → task info      
                                     • API events   → RecordU32x4(id+32)  ─┐
 PendSV / ISR save-restore     ──▶ _tx_execution_thread_enter/exit          │   SEGGER_SYSVIEW.c
   (TX_ENABLE_EXECUTION_CHANGE_       → OnTaskStartExec / StopExec / Idle   ├─▶ RTT up-buffer ──▶ J-Link (HW)
    NOTIFY, Cortex-M port)          _tx_execution_isr_enter/exit           │   in target RAM  ──▶ sv_rtt_probe.py (Renode)
                                      → RecordEnterISR / ExitISR[ToSched.] ─┘                 ──▶ UART pump (optional)
 TX_USER_FILE (upstream CMake) ──▶ txsv/tx_user.h selects the backend
```

Three upstream extension points do all the work:

1. **`TX_USER_FILE`** (upstream CMake option) injects `txsv/tx_user.h` into every
   kernel translation unit, including the port's assembly files.
2. **`TX_ENABLE_EVENT_TRACE`** compiles in ThreadX's trace points. `tx_trace.h`
   defines `TX_TRACE_IN_LINE_INSERT` only `#ifndef`, so `tx_user.h` redirects
   every trace point to `txsv_on_event()`.
3. **`TX_ENABLE_EXECUTION_CHANGE_NOTIFY`** makes the Cortex-M port call
   `_tx_execution_thread_enter/exit` and `_tx_execution_isr_enter/exit` from
   PendSV and ISR context save/restore. These give exact context-switch
   instants; the native trace points only describe readiness changes.

Upstream event IDs are sent unchanged (offset by 32) and decoded on the host by
a description file generated from upstream `tx_trace.h`. See
[`txsv/README.md`](txsv/README.md) for the full mapping.

## Repository layout

```
CMakeLists.txt                 build: backend, target, transport selection
app/
  demo.c                       workload: 5 threads, mutex/semaphore/queue/flags/timer, user events
  tx_user_segger.h             TX_USER_FILE for the SEGGER-fork comparison build
  CMakeLists.segger-fork.txt   comparison build against SEGGERMicro/threadx
txsv/                          the adapter (what you'd take to a product)
  tx_user.h                    backend selection, TX_TRACE_IN_LINE_INSERT override
  txsv_adapter.c               trace-point and scheduler hooks → SystemView API
  txsv_native_insert.c         dual mode: out-of-line copy of upstream's native insert
  txsv_thread_params.h         generated: which trace parameters are thread pointers
  SYSVIEW_ThreadX_native.txt   generated: SystemView description file
  txsv_comm_uart.c             optional UART transport (no debug probe)
  svconf/SEGGER_SYSVIEW_Conf.h SystemView configuration override
renode/
  threadx_m4.repl              Renode platform: Cortex-M4, NVIC, DWT, PL011
  threadx_systemview.resc      live SystemView through the emulated J-Link (default)
  threadx_systemview_uart.resc live SystemView through the UART transport
  sv_rtt_probe.py              Renode-side J-Link stand-in (reads RTT from target RAM)
  board_pl011.c                DWT enable + polled PL011 driver
tools/
  gen_sysview_desc.py          generates the description file + thread-param table
  sv_ip_probe.py               headless SystemView IP-Recorder client (capture + summary)
  sv_renode_bridge.py          hello-reassembling relay for the UART transport
  trx_decode.py                native .trx → event list, stats, Perfetto JSON
deps/                          upstream sources (git submodules, see below)
```

## Prerequisites

| Tool | Tested version | Needed for |
| --- | --- | --- |
| Arm GNU Toolchain (`arm-none-eabi-gcc`) | 14.2.Rel1 | all builds |
| CMake ≥ 3.20, Ninja | CMake 4.2.3 | all builds |
| Python 3 | 3.14 | host tools |
| Renode | 1.17.0 (portable) | live SystemView; the UART flow was also run on 1.16.0 |
| SEGGER SystemView (host app) | V4.10b (Linux) | viewing; commercial licence needed for commercial use |
| QEMU (`qemu-system-arm`) | as of 16 Oct 2026 | post-mortem flow |

Dependencies (pinned in `deps/`, all unmodified):

| Path | Upstream | Commit | Tag |
| --- | --- | --- | --- |
| `deps/threadx` | https://github.com/eclipse-threadx/threadx | `93387b0a60382a29579bbad5045c68328e3751df` | v6.5.2.202603_rel |
| `deps/SystemView` | https://github.com/SEGGERMicro/SystemView | `92ca7a810c5765ba64911919acd511c61b6b083f` | V4.12.0 (target code V3.32) |
| `deps/RTT` | https://github.com/SEGGERMicro/RTT | `4d8feab3150f86f37a9d323ddc88d6cdf5673072` | V8.58 |

After cloning this repository: `git submodule update --init`.

## Build options

| CMake option | Values | Default | Effect |
| --- | --- | --- | --- |
| `TXTRACE_BACKEND` | `0` none, `1` native (TraceX buffer), `2` SystemView, `3` both | `2` | which trace backend(s) the kernel feeds |
| `TXPOC_TARGET` | `qemu`, `renode` | `qemu` | `qemu`: semihosting dumps, SysTick-derived timestamps. `renode`: DWT timestamps, live streaming |
| `TXSV_COMM` | `rtt`, `uart` | `rtt` | Renode only. `rtt`: firmware only writes RTT, same as a J-Link build. `uart`: adds a pump thread on the PL011 |
| `TXSV_STATS` | `ON`/`OFF` | `OFF` | counts adapter hook calls, records without a host (`TXSV_AUTOSTART`), post-mortem ring |
| `THREADX_DIR`, `SYSVIEW_DIR`, `RTT_DIR` | paths | — | dependency checkouts (`deps/...`) |

Every build uses `-DCMAKE_TOOLCHAIN_FILE=deps/threadx/cmake/cortex_m4.cmake`.

## Quick start: live SystemView on Renode

```sh
cmake -S . -B build-renode -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/threadx/cmake/cortex_m4.cmake \
  -DTHREADX_DIR=$PWD/deps/threadx -DSYSVIEW_DIR=$PWD/deps/SystemView -DRTT_DIR=$PWD/deps/RTT \
  -DTXTRACE_BACKEND=2 -DTXPOC_TARGET=renode -DCMAKE_BUILD_TYPE=MinSizeRel
ninja -C build-renode

# once: install the description file (see below)
sudo cp txsv/SYSVIEW_ThreadX_native.txt /opt/SEGGER/SystemView_V410b/Description/

renode renode/threadx_systemview.resc        # run from the repository root
```

In SystemView: *Target → Recorder Configuration → IP*, address `127.0.0.1`,
port `19111`, then *Start Recording* (F5). Renode logs
`sv_rtt_probe: host connected`.

Without the GUI:

```sh
python3 tools/sv_ip_probe.py --port 19111 --seconds 10 --out capture.bin
```

It performs the same handshake, sends START, saves the raw stream (loadable in
SystemView via *File → Load Data*) and prints a summary: system description,
task list, event counts.

The firmware has no transport code: SystemView writes its RTT ring in RAM, and
`renode/sv_rtt_probe.py` reads it over the system bus like a J-Link does over
SWD. Details, the UART alternative and the virtual-time discussion are in
[`renode/README.md`](renode/README.md).

## Quick start: QEMU post-mortem and Perfetto

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/threadx/cmake/cortex_m4.cmake \
  -DTHREADX_DIR=$PWD/deps/threadx -DSYSVIEW_DIR=$PWD/deps/SystemView -DRTT_DIR=$PWD/deps/RTT \
  -DTXTRACE_BACKEND=3 -DTXSV_STATS=ON -DCMAKE_BUILD_TYPE=MinSizeRel
ninja -C build

cd build
qemu-system-arm -M mps2-an386 -nographic -monitor none -serial none \
  -semihosting-config enable=on,target=native -icount shift=3 -kernel demo
python3 -I ../tools/trx_decode.py native_trace.trx --stats --json native.json
```

The demo runs 2 s of emulated time, then dumps through semihosting:

| File | Content |
| --- | --- |
| `native_trace.trx` | native ThreadX trace buffer (TraceX format) |
| `txsv_stats.bin` | adapter hook counters (6 × u32) |
| `sysview_ring.bin`, `sysview_wroff.bin` | SystemView post-mortem RTT ring and its write offset |

Open `native.json` in https://ui.perfetto.dev. With `-icount` the run is
bit-identical every time. (Last run 16 Oct 2026; QEMU was not installed for
the 17 Oct changes, but the QEMU build was rebuilt cleanly.)

## SystemView description file

The adapter sends native trace point `TX_TRACE_<X>` as SystemView event
`<X> + 32` with the trace point's four arguments as parameters, and names the
OS `ThreadX_native` in the system description. SystemView looks for
`SYSVIEW_<OS name>.txt`, so it loads `SYSVIEW_ThreadX_native.txt`:

```sh
sudo cp txsv/SYSVIEW_ThreadX_native.txt /opt/SEGGER/SystemView_V410b/Description/
```

or put it in a SystemView project's `.sysview/` directory. Restart SystemView
after changing it.

**Do not use the stock `SYSVIEW_ThreadX.txt`.** It describes the SEGGER fork's
own event IDs and parameters. With it every event appears under a wrong name
(`tx_mutex_get` shows as `_tx_semaphore_prioritize`, `tx_thread_sleep` as
`_txe_queue_prioritize`), and its task states differ from upstream (mutex
suspension is `13` upstream, `9` in that file). This is also why the OS name
is not plain `ThreadX`: a missing file then shows generic event numbers rather
than wrong names.

Both generated files come from upstream headers; regenerate them on every
ThreadX update:

```sh
python3 tools/gen_sysview_desc.py deps/threadx/common/inc/tx_trace.h deps/threadx/common/inc/tx_api.h \
  txsv/SYSVIEW_ThreadX_native.txt txsv/txsv_thread_params.h
```

`txsv_thread_params.h` tells the adapter which parameters are thread pointers;
it sends those as SystemView task IDs, so `%t` shows the task name. Syntax
pitfalls hit while writing the file (SystemView only says "Failed to parse
description file") are listed in [`tools/README.md`](tools/README.md).

## Moving to real hardware (J-Link)

The default Renode build (`TXSV_COMM=rtt`) is the J-Link firmware. To port:

1. Add `txsv/` sources, `SEGGER_SYSVIEW.c`, `SEGGER_RTT.c`,
   `SEGGER_RTT_ASM_ARMv7M.S` to the build; set `TX_USER_FILE` to `txsv/tx_user.h`
   and `TXTRACE_BACKEND=2`.
2. Call `txsv_init()` in `tx_application_define()` after the clock is set up
   (`SystemCoreClock` is the timestamp frequency).
3. Enable the DWT cycle counter (`board_init()` in `renode/board_pl011.c` shows how).
4. Build startup/ISR `.S` files with `-DTX_ENABLE_EXECUTION_CHANGE_NOTIFY` —
   they don't include `tx_user.h` — or wrap ISRs with
   `tx_trace_isr_enter_insert()` / `tx_trace_isr_exit_insert()`.
5. Adjust `SEGGER_SYSVIEW_SetRAMBase(0x20000000)` in `txsv_init()` to your RAM.
6. Leave out `txsv_comm_uart.c` and `renode/board_pl011.c`'s UART functions.
7. Install the description file; in SystemView choose the J-Link recorder.

Recording starts when SystemView sends START. Define `TXSV_AUTOSTART` to record
from boot without a host. Without a J-Link, port the three `txsv_uart_*`
functions and use `TXSV_COMM=uart` with SystemView's UART Recorder.

## Known gaps

- **No API return values or durations.** Upstream has no API-exit hook; the
  description file has no `| Returns` part.
- **No automatic timer-callback span.** Wrap callbacks, or needs an upstream hook.
- **User events** (`tx_trace_user_event_insert`) show as `Function #1025` etc.;
  they map into SystemView's user range, which an OS description file can't name.
- **NULL thread parameters** show as `Task 0x00000000` (for example
  `tx_mutex_get` OwningThread when the mutex is free).
- **Object pointers** (mutex, queue, semaphore) show as addresses; object names
  are not sent to SystemView yet.
- **Dual mode** (`TXTRACE_BACKEND=3`) copies upstream's macro body into
  `txsv/txsv_native_insert.c`; CI should diff it against `tx_trace.h` on every
  ThreadX update.
- **Renode RTT drain rate** follows host wall-clock polling, not SWD speed, and
  is not deterministic; see the virtual-time section in
  [`renode/README.md`](renode/README.md#using-renode-virtual-time-for-the-probe).
- **Overhead on silicon** (cycles per event, stack use) not measured. The System
  Timer Thread stack is raised to 2 KB because recording runs on the caller's stack.

## Creating the git repository

`.gitignore` excludes build directories, SystemView installers and exports,
captures and Python caches. The three dependencies are best tracked as
submodules at the pinned commits:

```sh
git init
git submodule add https://github.com/eclipse-threadx/threadx.git    deps/threadx
git submodule add https://github.com/SEGGERMicro/SystemView.git     deps/SystemView
git submodule add https://github.com/SEGGERMicro/RTT.git            deps/RTT
git -C deps/threadx    checkout 93387b0a60382a29579bbad5045c68328e3751df
git -C deps/SystemView checkout 92ca7a810c5765ba64911919acd511c61b6b083f
git -C deps/RTT        checkout 4d8feab3150f86f37a9d323ddc88d6cdf5673072
git add .
git commit -m "ThreadX to SystemView adapter PoC"
```

Because `deps/*` are already clones, `git submodule add` reuses them in place.
The generated files (`txsv/SYSVIEW_ThreadX_native.txt`,
`txsv/txsv_thread_params.h`) are committed on purpose, so a build doesn't need
Python. Regenerate them when ThreadX changes.

## Licensing

- Eclipse ThreadX: MIT. `txsv/txsv_native_insert.c` contains a copy of upstream's
  macro body and credits its origin; keep the MIT notice with it.
- SEGGER SystemView target sources and RTT: SEGGER's BSD-style licence (see `deps/`).
- SEGGER SystemView host application: free for non-commercial use; commercial
  use needs a licence.
- This repository's own files: no licence chosen yet. Pick one before publishing.
