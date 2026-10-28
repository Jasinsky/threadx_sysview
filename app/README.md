# Demo application

`demo.c` is the workload used for every PoC run. It uses upstream ThreadX's
`ports/cortex_m4/gnu/example_build` startup, vector table and linker script
unmodified, on QEMU `mps2-an386` and on the Renode platform in `../renode/`.

## Workload

| Thread | Priority | Loop |
| --- | --- | --- |
| `ctrl` | 1 | marks phases with user events. QEMU: runs 2 s, dumps buffers via semihosting, exits. Renode: runs forever, one `USR_EVT_PHASE` per second |
| `hi` | 2 | waits on event flags (set by a 3-tick periodic timer), user event, `tx_mutex_get` → busy 200 → `tx_mutex_put`, `tx_semaphore_put` |
| `cons` | 3 | `tx_queue_receive`, `tx_semaphore_get` (5-tick timeout), user event |
| `mid` | 4 | CPU hog: busy 3000, `tx_thread_sleep(1)`; preempted by `hi` |
| `lo` | 6 | `tx_mutex_get` → busy 4000 → `tx_mutex_put`, `tx_queue_send`, `tx_thread_sleep(2)` |

Plus ThreadX's System Timer Thread (priority 0) running the timer callback.

Objects: semaphore `sem`, mutex `mtx` (priority inheritance on), queue `q`
(8 × 1 ULONG), event flags `ev`, timer `tmr` (period 3 ticks), 100 Hz tick.

What it exercises: priorities and preemption, queue blocking (the queue fills
up: `cons` spends time waiting on the semaphore, so `Enqueued = 8` is normal),
event flags from a timer callback, timeouts, user events, and native
trace-buffer wraparound (the 8 KB buffer is small on purpose; wraps are counted
in the buffer-full callback and reported in the final `USR_EVT_PHASE` event).

Mutex contention and priority inheritance are intended (`lo` holds the mutex
for a long busy loop) but timing-dependent. In the Renode captures so far `hi`
always found the mutex free (`tx_mutex_get` OwningThread = 0 in all 740 calls,
no "Mutex susp" blocks); raise `lo`'s busy count or shorten the timer period
to force it.

User event IDs: `USR_EVT_WORK` = `TX_TRACE_USER_EVENT_START + 1` (SystemView
`#1025`), `USR_EVT_PHASE` = `+ 2` (`#1026`).

## Target-specific code in `demo.c`

| Define | Code |
| --- | --- |
| `TXPOC_QEMU` | `txpoc_timestamp()` (SysTick-derived cycles; QEMU has no DWT), semihosting dumps and exit |
| `TXPOC_RENODE` | `board_init()` before `tx_kernel_enter()`: DWT on, UART up; endless run |
| `TXSV_COMM_UART` | starts the UART pump thread (`txsv_comm_start`, 1 KB stack) |
| `TXSV_STATS` | dumps `txsv_stats.bin`, `sysview_ring.bin`, `sysview_wroff.bin` at the end (QEMU) |
| `TXTRACE_SEGGER` | calls `SEGGER_SYSVIEW_Conf()` instead of `txsv_init()` for the fork build |

## SEGGER-fork comparison build

`CMakeLists.segger-fork.txt` and `tx_user_segger.h` build the same `demo.c`
against SEGGER's patched kernel ([SEGGERMicro/threadx](https://github.com/SEGGERMicro/threadx))
with SEGGER's own `Sample/ThreadX` integration, for size and behaviour
comparison (flash: adapter +7.2 KB, fork +7.7 KB, see the assessment).

It is a standalone CMake project, kept here for reference:

- `THREADX_DIR` points at the fork checkout; the startup files come from
  `$HOME/segger-threadx/ports/cortex_m4/gnu/example_build` (hard-coded; the
  fork's own example is stale), `tx_initialize_low_level.S` from `THREADX_DIR`.
- `APP` must be set to `demo.c`.
- QEMU only (`TXPOC_QEMU`).

Copy it to its own directory as `CMakeLists.txt` and adjust the paths before use.
