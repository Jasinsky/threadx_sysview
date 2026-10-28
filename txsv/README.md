# txsv — the adapter

This directory is the part you would take into a product: it turns upstream
ThreadX's trace points and scheduler hooks into SEGGER SystemView events,
without touching kernel sources.

| File | Role |
| --- | --- |
| `tx_user.h` | `TX_USER_FILE` for ThreadX: picks the backend, redirects `TX_TRACE_IN_LINE_INSERT` |
| `txsv_adapter.c` | hook implementations, SystemView OS callbacks, `txsv_init()` |
| `txsv_native_insert.c` | dual mode only: out-of-line copy of upstream's native trace insert |
| `txsv_thread_params.h` | **generated** — which trace parameters are thread pointers |
| `SYSVIEW_ThreadX_native.txt` | **generated** — SystemView description file for the events below |
| `txsv_comm_uart.c` | optional transport for targets without a debug probe |
| `svconf/SEGGER_SYSVIEW_Conf.h` | SystemView configuration (only adds the QEMU timestamp source) |

## Hooks used

All three are upstream features; nothing is patched.

### 1. `TX_USER_FILE` → `tx_user.h`

Upstream's CMake copies the file named by `TX_USER_FILE` into its build tree
as `tx_user.h` (`configure_file`, re-copied when it changes) and defines
`TX_INCLUDE_USER_DEFINE_FILE`, so every kernel source includes it. It derives:

```c
TXTRACE_NATIVE   = TXTRACE_BACKEND & 1     /* TraceX buffer          */
TXTRACE_SYSVIEW  = TXTRACE_BACKEND & 2     /* SystemView             */
```

and defines `TX_ENABLE_EVENT_TRACE` for any non-zero backend.

> The Cortex-M port's `.S` files include `tx_user.h` too. Every C
> declaration in it is inside `#ifndef __ASSEMBLER__`.

### 2. `TX_TRACE_IN_LINE_INSERT` override

`tx_trace.h` provides its default macro only `#ifndef TX_TRACE_IN_LINE_INSERT`.
With SystemView enabled, `tx_user.h` defines it as a call to:

```c
void txsv_on_event(unsigned long id, unsigned long i1, unsigned long i2,
                   unsigned long i3, unsigned long i4, unsigned long filter);
```

In dual mode (`TXTRACE_BACKEND=3`) the macro first calls
`txsv_native_insert()` — a copy of upstream's macro body — so the TraceX
buffer still fills, then `txsv_on_event()`.

Most call sites run with interrupts disabled; SystemView takes its own RTT
lock anyway.

### 3. `TX_ENABLE_EXECUTION_CHANGE_NOTIFY`

The Cortex-M ports call these from PendSV and ISR save/restore:

| Port hook | When | Adapter action |
| --- | --- | --- |
| `_tx_execution_thread_enter` | PendSV, about to restore a thread | `SEGGER_SYSVIEW_OnTaskStartExec(thread)` |
| `_tx_execution_thread_exit` | PendSV, current thread saved | `SEGGER_SYSVIEW_OnTaskStopExec()`; `OnIdle()` if nothing is ready |
| `_tx_execution_isr_enter` | ISR context save | `SEGGER_SYSVIEW_RecordEnterISR()` |
| `_tx_execution_isr_exit` | ISR context restore | `RecordExitISRToScheduler()` if a switch is pending, else `RecordExitISR()` |

These give the exact instant a thread starts or stops running. The native trace
points only say a thread became ready or blocked; in a native `.trx` the switch
time has to be inferred (see `tools/trx_decode.py`).

The upstream hook respects `TX_PORT_USE_BASEPRI`; the SEGGER fork instead
inserts an unconditional `CPSID i` in PendSV.

## Event mapping

`txsv_on_event()` is a switch on the native trace ID (`tx_trace.h`):

| Native trace point | SystemView call | Also sent as API event? |
| --- | --- | --- |
| `TX_TRACE_THREAD_RESUME` (1) — thread became READY | `OnTaskStartReady(thread)` | no |
| `TX_TRACE_THREAD_SUSPEND` (2) — thread left READY | `OnTaskStopReady(thread, new_state)` | no |
| `TX_TRACE_ISR_ENTER` / `EXIT` (3/4) — from `tx_trace_isr_*_insert()` | `RecordEnterISR` / `RecordExitISR[ToScheduler]` | no |
| `TX_TRACE_RUNNING` (6) | dropped | no |
| `TX_TRACE_THREAD_CREATE` | `OnTaskCreate(thread)` + task info (name, priority, stack) | yes |
| `TX_TRACE_THREAD_DELETE` | `OnTaskTerminate(thread)` | yes |
| any other ID < 4096 | `RecordU32x4(ID + 32, I1..I4)` | — |
| user events (ID ≥ 4096) | `RecordU32x4(1024 + (ID − 4096) % 1024, I1..I4)` | — |

The `new_state` passed to `OnTaskStopReady` is ThreadX's state (`TX_SLEEP`,
`TX_QUEUE_SUSP`, `TX_MUTEX_SUSP` = 13, ...). The description file's
`TaskState` line names them, so SystemView shows "Task Block: Mutex susp".

### Event IDs

- SystemView reserves IDs below 32. Native IDs 1..199 become **33..231**, inside
  the 32..511 range a description file can name. Keeping the native number
  (instead of SEGGER's own table) means the mapping needs no table and follows
  upstream automatically.
- User events land in SystemView's user range 1024..2047. They are recorded but
  not named (see Known gaps in the top-level README).

### Thread-pointer parameters

SystemView identifies a task by its ID minus the RAM base
(`SEGGER_SYSVIEW_SetRAMBase(0x20000000)`), shifted by
`SEGGER_SYSVIEW_ID_SHIFT`. `OnTaskCreate` and friends do this internally, but
`RecordU32x4` sends values as they are. A raw `TX_THREAD *` therefore doesn't
match any task, and SystemView shows `Task 0x2000007C` instead of the name.

`txsv_thread_params.h` (generated from the same rule that writes `%t` into the
description file) holds a bitmask per native ID. For marked parameters the
adapter sends `SEGGER_SYSVIEW_ShrinkId(ptr)`; NULL stays 0. 19 trace points
are affected: thread resume/suspend/time-slice, mutex owner, and the
`tx_thread_*` APIs.

Because the description file and the header come from one generator run, they
cannot disagree. Regenerate both on every ThreadX update.

## SystemView OS callbacks and initialisation

`txsv_init()` (call from `tx_application_define()`):

1. `SEGGER_SYSVIEW_Init(SystemCoreClock, SystemCoreClock, &txsv_os_api, txsv_send_sys_desc)`
2. `SEGGER_SYSVIEW_SetRAMBase(0x20000000)` — adjust for your part
3. SystemView-only mode: enables a 256-byte stub native trace buffer (see below)
4. `TXSV_AUTOSTART`: starts recording immediately (post-mortem / CI)

| Callback | Implementation |
| --- | --- |
| `pfGetTime` | `tx_time_get()` ticks → µs. Calling it records one `tx_time_get` event at START. |
| `pfSendTaskList` | walks `_tx_thread_created_ptr` and sends name, priority, stack for each thread. SystemView calls it on START, so threads created before the host connected still get names. |
| system description | `N=ThreadX app,O=ThreadX_native,D=Cortex-M`, `I#15=SysTick` |

`O=ThreadX_native` makes SystemView load `SYSVIEW_ThreadX_native.txt`.

### Host commands without a transport thread

With RTT, SystemView's host commands (START, STOP, task-list request) land in
the RTT down-buffer. SystemView checks it inside `SEGGER_SYSVIEW_IsStarted()`.
The adapter calls `IsStarted()` at the top of every trace point and scheduler
hook, so commands are processed at the next context switch or API call. No
thread or timer is needed, which is what makes the plain-RTT build work both
on J-Link and with `renode/sv_rtt_probe.py`.

### The stub trace buffer

`tx_trace_user_event_insert()` and `tx_trace_isr_enter/exit_insert()` return
`TX_NOT_DONE` unless a native trace buffer has been enabled, even though the
redirected macro never writes to it. In SystemView-only mode `txsv_init()`
therefore calls `tx_trace_enable()` on a 256-byte static buffer (header plus one
registry entry). It is never filled.

## Timestamps

| Target | Source |
| --- | --- |
| Renode / hardware | SystemView default for Cortex-M: DWT `CYCCNT`. `board_init()` enables it (`DEMCR.TRCENA`, `DWT_CTRL.CYCCNTENA`). |
| QEMU | QEMU has no DWT. `svconf/SEGGER_SYSVIEW_Conf.h` and `tx_user.h` use `txpoc_timestamp()` (SysTick count + tick counter, in `app/demo.c`) for both SystemView and the native buffer. |

## Configuration macros

| Macro | Where | Effect |
| --- | --- | --- |
| `TXTRACE_BACKEND` | CMake | 0/1/2/3, see top-level README |
| `TX_TIMER_THREAD_STACK_SIZE` | `tx_user.h` (default 2048 with SystemView) | recording runs on the caller's stack; the default 1 KB System Timer Thread stack overflowed at `-O0` |
| `TXSV_AUTOSTART` | CMake via `TXSV_STATS` | start recording in `txsv_init()` |
| `TXSV_STATS` | CMake | counts hook calls in `txsv_stats[6]`: start_exec, stop_exec, ready, stop_ready, isr_enter, api |
| `SEGGER_SYSVIEW_RTT_BUFFER_SIZE` | CMake | 8192 on Renode, 32768 with `TXSV_STATS` |
| `TXSV_COMM_UART` | CMake via `TXSV_COMM=uart` | starts the UART pump thread from `app/demo.c` |

## Optional UART transport (`txsv_comm_uart.c`)

For targets without a debug probe. A lowest-priority ThreadX thread
("SysView comm") moves bytes between the RTT buffers and a UART and speaks both
SystemView host protocols, chosen from the host's first two bytes:

- `"SE"` → **IP Recorder**: 32-byte hellos, host data framed as `[len][bytes]`.
- `"SV"` → **UART Recorder**: delegated to SEGGER's `Sample/COMM/SEGGER_SYSVIEW_REC.c`.

On a new IP hello it stops the old recording and drops its buffer, so
reconnects work. Board code supplies `txsv_uart_rx_ready()`, `txsv_uart_getc()`,
`txsv_uart_putc()`.

The thread shows up in the trace and wakes every tick. Draining the buffer
creates new events. SEGGER's own UART sample is interrupt-driven
(`_cbOnRx`/`_cbOnTx` + `SEGGER_SYSVIEW_X_OnEventRecorded`), which would avoid
both. ThreadX has no idle hook (the idle loop runs inside PendSV), so an idle
task isn't an option. With J-Link none of this is needed.

## Dual mode maintenance

`txsv_native_insert.c` is a copy of the body of upstream's default
`TX_TRACE_IN_LINE_INSERT`. It must stay identical to `deps/threadx/common/inc/tx_trace.h`;
add a CI check that diffs the two on every ThreadX bump.
