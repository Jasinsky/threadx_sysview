/*
 * demo.c - ThreadX trace PoC workload (Cortex-M4, runs on QEMU mps2-an386).
 * Exercises: priorities/preemption, semaphore + mutex contention (priority
 * inheritance), queue blocking, event flags, periodic timer, user events,
 * and trace-buffer wraparound. At the end it dumps the native trace buffer
 * to the host via ARM semihosting and exits.
 */
#include "tx_api.h"
#include <stdint.h>
#include <string.h>
#if TXTRACE_SYSVIEW
#include "SEGGER_RTT.h"
#endif

unsigned int SystemCoreClock = 6000000u;   /* matches upstream example SYSTEM_CLOCK */

/* ---- minimal ARM semihosting (no newlib startup required) ---------------- */
static int semihost(int op, void *arg)
{
    register int r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile ("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void host_dump(const char *path, const void *buf, unsigned len)
{
    uint32_t open_args[3] = { (uint32_t) path, 6u /* "wb" */, (uint32_t) strlen(path) };
    int fd = semihost(0x01, open_args);
    if (fd < 0) return;
    uint32_t wr_args[3] = { (uint32_t) fd, (uint32_t) buf, len };
    semihost(0x05, wr_args);
    semihost(0x02, &fd);
}
static void host_exit(void)
{
    uint32_t args[2] = { 0x20026u /* ADP_Stopped_ApplicationExit */, 0u };
    semihost(0x20, args);           /* SYS_EXIT_EXTENDED */
    for (;;) { }
}

/* ---- trace timestamp: SysTick-derived, monotonic, deterministic under -icount */
#define SYST_CVR (*(volatile uint32_t *) 0xE000E018u)
#define SYST_RVR (*(volatile uint32_t *) 0xE000E014u)
extern volatile ULONG _tx_timer_system_clock;
unsigned long txpoc_timestamp(void)
{
    ULONG t1, v;
    do {
        t1 = _tx_timer_system_clock;
        v  = SYST_CVR;
    } while (t1 != _tx_timer_system_clock);
    return (t1 * (SYST_RVR + 1u)) + (SYST_RVR - v);   /* CPU cycles */
}

/* ---- objects -------------------------------------------------------------- */
#define STACK 1024
static TX_THREAD th_ctrl, th_hi, th_mid, th_lo, th_cons;
static UCHAR stk[5][STACK];
static TX_SEMAPHORE sem;
static TX_MUTEX mtx;
static TX_QUEUE q;
static ULONG q_storage[8];
static TX_EVENT_FLAGS_GROUP ev;
static TX_TIMER tmr;

#define TRACE_BUF_SIZE (8u * 1024u)        /* small on purpose: forces wraparound */
static UCHAR trace_buf[TRACE_BUF_SIZE] __attribute__((aligned(4)));

#define USR_EVT_WORK  (TX_TRACE_USER_EVENT_START + 1)
#define USR_EVT_PHASE (TX_TRACE_USER_EVENT_START + 2)

static volatile ULONG wraps;
static VOID trace_full(VOID *hdr) { (void) hdr; wraps++; }

static void busy(ULONG n) { for (volatile ULONG i = 0; i < n; i++) { } }

static VOID timer_cb(ULONG id) { tx_event_flags_set(&ev, 0x1u, TX_OR); (void) id; }

static VOID hi_entry(ULONG a)       /* prio 2: waits on event flags, contends mutex */
{
    ULONG got;
    (void) a;
    for (;;) {
        tx_event_flags_get(&ev, 0x1u, TX_OR_CLEAR, &got, TX_WAIT_FOREVER);
        tx_trace_user_event_insert(USR_EVT_WORK, 2, got, 0, 0);
        tx_mutex_get(&mtx, TX_WAIT_FOREVER);
        busy(200);
        tx_mutex_put(&mtx);
        tx_semaphore_put(&sem);
    }
}

static VOID mid_entry(ULONG a)      /* prio 4: CPU hog, preempted by hi */
{
    (void) a;
    for (;;) {
        busy(3000);
        tx_thread_sleep(1);
    }
}

static VOID lo_entry(ULONG a)       /* prio 6: holds mutex long -> priority inheritance */
{
    (void) a;
    for (;;) {
        tx_mutex_get(&mtx, TX_WAIT_FOREVER);
        busy(4000);
        tx_mutex_put(&mtx);
        tx_queue_send(&q, &(ULONG){ 0xC0DEu }, TX_WAIT_FOREVER);
        tx_thread_sleep(2);
    }
}

static VOID cons_entry(ULONG a)     /* prio 3: blocks on queue and semaphore */
{
    ULONG msg;
    (void) a;
    for (;;) {
        tx_queue_receive(&q, &msg, TX_WAIT_FOREVER);
        tx_semaphore_get(&sem, 5);
        tx_trace_user_event_insert(USR_EVT_WORK, 3, msg, 0, 0);
    }
}

static VOID ctrl_entry(ULONG a)     /* prio 1: orchestrates the run, then dumps */
{
    (void) a;
    tx_trace_user_event_insert(USR_EVT_PHASE, 1, 0, 0, 0);
#ifdef TXPOC_RENODE                 /* live streaming: run forever, mark each second */
    for (ULONG sec = 1;; sec++) {
        tx_thread_sleep(100);
        tx_trace_user_event_insert(USR_EVT_PHASE, 3, sec, 0, 0);
    }
#endif
    tx_thread_sleep(200);           /* 2 s of workload at 100 Hz tick */
    tx_trace_user_event_insert(USR_EVT_PHASE, 2, wraps, 0, 0);
    tx_trace_disable();
    host_dump("native_trace.trx", trace_buf, TRACE_BUF_SIZE);
#if TXTRACE_SYSVIEW && defined(TXSV_STATS)
    {
        extern volatile ULONG txsv_stats[6];
        extern SEGGER_RTT_CB _SEGGER_RTT;
        host_dump("txsv_stats.bin", (const void *) txsv_stats, sizeof(ULONG) * 6);
        /* post-mortem SystemView ring (RTT up-channel 1) + its write offset */
        host_dump("sysview_ring.bin", _SEGGER_RTT.aUp[1].pBuffer, _SEGGER_RTT.aUp[1].SizeOfBuffer);
        host_dump("sysview_wroff.bin", (const void *) &_SEGGER_RTT.aUp[1].WrOff, 4);
    }
#endif
    host_exit();
}

#if TXTRACE_SYSVIEW
void txsv_init(void);
#endif
#ifdef TXPOC_RENODE
void board_init(void);
#endif
#ifdef TXSV_COMM_UART
void txsv_comm_start(VOID *stack, ULONG stack_size);
static UCHAR comm_stack[1024];
#endif
#ifdef TXTRACE_SEGGER          /* comparison build against SEGGERMicro/threadx */
void SEGGER_SYSVIEW_Conf(void);
#endif

VOID tx_application_define(VOID *first_unused_memory)
{
    (void) first_unused_memory;
#if TXTRACE_NATIVE
    tx_trace_enable(trace_buf, TRACE_BUF_SIZE, 16);
    tx_trace_buffer_full_notify(trace_full);
#endif
#if TXTRACE_SYSVIEW
    txsv_init();
#endif
#ifdef TXSV_COMM_UART
    txsv_comm_start(comm_stack, sizeof comm_stack);
#endif
#ifdef TXTRACE_SEGGER
    SEGGER_SYSVIEW_Conf();
#endif
    tx_semaphore_create(&sem, "sem", 0);
    tx_mutex_create(&mtx, "mtx", TX_INHERIT);
    tx_queue_create(&q, "q", TX_1_ULONG, q_storage, sizeof q_storage);
    tx_event_flags_create(&ev, "ev");
    tx_timer_create(&tmr, "tmr", timer_cb, 0, 3, 3, TX_AUTO_ACTIVATE);
    tx_thread_create(&th_ctrl, "ctrl", ctrl_entry, 0, stk[0], STACK, 1, 1, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&th_hi,   "hi",   hi_entry,   0, stk[1], STACK, 2, 2, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&th_cons, "cons", cons_entry, 0, stk[2], STACK, 3, 3, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&th_mid,  "mid",  mid_entry,  0, stk[3], STACK, 4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);
    tx_thread_create(&th_lo,   "lo",   lo_entry,   0, stk[4], STACK, 6, 6, TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(void)
{
#ifdef TXPOC_RENODE
    board_init();                   /* DWT on; UART up before a host hello can arrive */
#endif
    tx_kernel_enter();
    return 0;
}
