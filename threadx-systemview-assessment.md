# ThreadX × SystemView tracing — assessment summary (16 Oct 2026, Renode update 17 Oct 2026)

Full report: https://claude.ai/code/artifact/d6249cb9-d9e3-43c5-8601-da299de1caca

Decision: build a zero-kernel-diff adapter (Option E) instead of adopting SEGGER's patched ThreadX; keep native tracing selectable (dual mode); add an open path native .trx → Perfetto for CI.

Key verified facts
- SEGGER's official ThreadX support is a fork (SEGGERMicro/threadx), +2,630 lines / 258 files, based on 6.4.2, last commit Feb 2025. Upstream is v6.5.2 (Mar 2026), 298 commits ahead.
- Upstream already has the hooks: `#ifndef TX_TRACE_IN_LINE_INSERT` in tx_trace.h; `TX_ENABLE_EXECUTION_CHANGE_NOTIFY` → `_tx_execution_thread_enter/exit`, `_tx_execution_isr_enter/exit` in Cortex-M PendSV/context save; CMake `TX_USER_FILE`.
- Fork inserts unconditional `CPSID i` in PendSV; upstream hook honours `TX_PORT_USE_BASEPRI`.
- PoC on QEMU mps2-an386 (Cortex-M4), unmodified v6.5.2: all 4 backends build; flash Δ native +5.2 KB, SystemView +7.2 KB, dual +8.8 KB, fork +7.7 KB; runs bit-identical under `-icount`.
- Pitfalls: tx_user.h is included by port .S files (guard C with `#ifndef __ASSEMBLER__`); example startup .S needs `-DTX_ENABLE_EXECUTION_CHANGE_NOTIFY`; System Timer Thread 1 KB stack overflowed at -O0 with recording on → set `TX_TIMER_THREAD_STACK_SIZE` 2048.
- `tx_trace_user_event_insert` / `tx_trace_isr_*_insert` return TX_NOT_DONE unless a native trace buffer is enabled, even with the macro redirected → SystemView-only mode enables a 256-byte stub buffer.
- "Perfecto" assumed to mean Perfetto (Apache-2.0). SystemView host needs commercial licence for commercial use; target sources permissive.

Renode + SystemView (verified 17 Oct 2026, Renode 1.16.0)
- No RTT/J-Link bridge in Renode → stream over an emulated PL011 UART. `txsv_comm_uart.c` speaks both host protocols: IP Recorder (32-byte "SEGGER SystemView V3.xx" hellos, host→target framed [len][bytes]) and UART Recorder (4-byte "SV" hello, via SEGGER's Sample/COMM/SEGGER_SYSVIEW_REC.c).
- IP Recorder: `emulation CreateServerSocketTerminal 19111 "sysview" false` + SystemView IP → 127.0.0.1:19111. UART Recorder: `CreateUartPtyTerminal` (Linux/macOS).
- Renode models DWT CYCCNT (`Miscellaneous.DWT`) → real timestamps, no QEMU workaround.
- Verified with a scripted host (tools/sv_ip_probe.py): handshake, START, task names, balanced exec/ready events, ISRs, user events, 0 overflow packets, both protocols.

Open: SystemView GUI rendering of adapter stream; overhead/latency on silicon; Perfetto rendering.

Update 17 Oct 2026 (Renode 1.17.0)
- Default Renode transport is now emulated J-Link RTT: firmware has no transport code (same as a J-Link build); `renode/sv_rtt_probe.py` reads/writes `_SEGGER_RTT` over the system bus from a host thread and serves SystemView's IP Recorder on :19111 (hello in one write, so no bridge). UART transport kept as `-DTXSV_COMM=uart`.
- Renode's bundled `segger-rtt.py` (function-call hooks, terminal only) does not fit SystemView: misses `WriteSkipNoLock`, its `ReadNoLock` hook ignores the channel.
- Stock `SYSVIEW_ThreadX.txt` describes the fork's IDs → wrong names for the adapter's native IDs. Generated `txsv/SYSVIEW_ThreadX_native.txt` (+ `txsv_thread_params.h` for `%t` task names) from upstream `tx_trace.h`; OS name `ThreadX_native`. Verified in SystemView V4.10b export: names, parameters, thread names, counts match the workload.
- Next: virtual-time probe mode for deterministic CI captures and RTT buffer sizing (see renode/README.md).
