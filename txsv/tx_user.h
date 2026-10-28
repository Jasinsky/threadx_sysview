/*
 * tx_user.h - ThreadX trace backend selection (no kernel source modifications).
 *
 * Select with -DTXTRACE_BACKEND=<n>:
 *   0 = none, 1 = native ThreadX/TraceX buffer, 2 = SystemView, 3 = both
 *
 * Mechanism (all upstream-provided extension points):
 *   - TX_ENABLE_EVENT_TRACE compiles the ~90 in-line TX_TRACE_IN_LINE_INSERT sites.
 *   - tx_trace.h guards its default macro with #ifndef TX_TRACE_IN_LINE_INSERT,
 *     so we can supply our own expansion here.
 *   - TX_ENABLE_EXECUTION_CHANGE_NOTIFY makes the Cortex-M ports call
 *     _tx_execution_thread_enter/exit and _tx_execution_isr_enter/exit from
 *     PendSV and context save/restore. We implement those in txsv_adapter.c.
 */
#ifndef TX_USER_H
#define TX_USER_H

#ifndef TXTRACE_BACKEND
#define TXTRACE_BACKEND 2
#endif

#define TXTRACE_NATIVE     ((TXTRACE_BACKEND) & 1)
#define TXTRACE_SYSVIEW    ((TXTRACE_BACKEND) & 2)

#if TXTRACE_BACKEND != 0
#define TX_ENABLE_EVENT_TRACE
#endif

/* QEMU has no DWT cycle counter: derive cycles from SysTick (PoC only). */
#ifdef TXPOC_QEMU
#ifndef __ASSEMBLER__
unsigned long txpoc_timestamp(void);
#endif
#define TX_TRACE_TIME_SOURCE    txpoc_timestamp()
#define TX_TRACE_TIME_MASK      0xFFFFFFFFUL
#endif

#if TXTRACE_SYSVIEW
#define TX_ENABLE_EXECUTION_CHANGE_NOTIFY

/* SystemView recording runs on the caller's stack inside every traced API
   call, including in the System Timer Thread. The PoC overflowed the default
   1 KB timer stack at -O0 (precise BusFault in SystemView's encoder). */
#ifndef TX_TIMER_THREAD_STACK_SIZE
#define TX_TIMER_THREAD_STACK_SIZE  2048
#endif

/* NOTE: the Cortex-M port .S files include tx_user.h too, so every C
   declaration here must be hidden from the assembler. */
#ifndef __ASSEMBLER__
/* Adapter entry point. Called with interrupts disabled at most sites. */
void txsv_on_event(unsigned long id, unsigned long i1, unsigned long i2,
                   unsigned long i3, unsigned long i4, unsigned long filter);

#if TXTRACE_NATIVE
/* Dual mode: native buffer insert is a copy of the upstream macro body,
   compiled out-of-line in txsv_native_insert.c. */
void txsv_native_insert(unsigned long id, unsigned long i1, unsigned long i2,
                        unsigned long i3, unsigned long i4, unsigned long filter);
#define TX_TRACE_IN_LINE_INSERT(i,a,b,c,d,e) \
    { txsv_native_insert((unsigned long)(i),(unsigned long)(a),(unsigned long)(b), \
                         (unsigned long)(c),(unsigned long)(d),(unsigned long)(e)); \
      txsv_on_event((unsigned long)(i),(unsigned long)(a),(unsigned long)(b), \
                    (unsigned long)(c),(unsigned long)(d),(unsigned long)(e)); }
#else
#define TX_TRACE_IN_LINE_INSERT(i,a,b,c,d,e) \
    { txsv_on_event((unsigned long)(i),(unsigned long)(a),(unsigned long)(b), \
                    (unsigned long)(c),(unsigned long)(d),(unsigned long)(e)); }
#endif
#endif /* __ASSEMBLER__ */
#endif /* TXTRACE_SYSVIEW */

#endif /* TX_USER_H */
