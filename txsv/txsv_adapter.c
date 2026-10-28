/*
 * txsv_adapter.c - Route upstream ThreadX trace points to SEGGER SystemView
 * without modifying ThreadX sources.
 *
 * Scheduler truth comes from the port's execution-change hooks (PendSV),
 * readiness transitions and API events come from the native trace points.
 */
#include "tx_api.h"
#include "tx_thread.h"
#include "tx_trace.h"
#include "SEGGER_SYSVIEW.h"
#include "txsv_thread_params.h"

/* SystemView OS-event IDs start at 32; native ThreadX IDs 10..199 map 1:1 at +32,
   decoded by txsv/SYSVIEW_ThreadX_native.txt (tools/gen_sysview_desc.py).     */
#define TXSV_OS_ID_BASE      32u
/* Native user events (4096..65535) map into SystemView's user range 1024..2047. */
#define TXSV_USER_ID_BASE    1024u
#define TXSV_USER_ID_SPAN    1024u

#define TXSV_ID(p)  ((U32)(ULONG)(p))

#ifdef TXSV_STATS                 /* PoC/CI instrumentation of the adapter itself */
volatile ULONG txsv_stats[6];     /* start_exec, stop_exec, ready, stop_ready, isr_enter, api */
#define TXSV_COUNT(i) (txsv_stats[i]++)
#else
#define TXSV_COUNT(i)
#endif

static void txsv_send_task_info(TX_THREAD *t)
{
    SEGGER_SYSVIEW_TASKINFO info;
    info.TaskID     = TXSV_ID(t);
    info.sName      = t->tx_thread_name;
    info.Prio       = t->tx_thread_priority;
    info.StackBase  = TXSV_ID(t->tx_thread_stack_start);
    info.StackSize  = (U32) t->tx_thread_stack_size;
    info.StackUsage = 0u;
    SEGGER_SYSVIEW_SendTaskInfo(&info);
}

/* Thread-pointer parameters go out as SystemView task IDs (RAM base removed,
   like OnTaskCreate etc. do), so "%t" in the description file shows the name. */
static U32 txsv_param(unsigned long v, unsigned mask, unsigned bit)
{
    if (((mask >> bit) & 1u) && (v != 0u)) {
        return SEGGER_SYSVIEW_ShrinkId((U32) v);
    }
    return (U32) v;
}

/* ---- Native trace points (TX_TRACE_IN_LINE_INSERT override) ---------------- */
void txsv_on_event(unsigned long id, unsigned long i1, unsigned long i2,
                   unsigned long i3, unsigned long i4, unsigned long filter)
{
    (void) filter;
    if (!SEGGER_SYSVIEW_IsStarted()) {
        return;
    }
    switch (id) {
    case TX_TRACE_THREAD_RESUME:        /* thread became READY (not yet running) */
        TXSV_COUNT(2);
        SEGGER_SYSVIEW_OnTaskStartReady(TXSV_ID(i1));
        return;
    case TX_TRACE_THREAD_SUSPEND:       /* thread left READY; i2 = new state     */
        TXSV_COUNT(3);
        SEGGER_SYSVIEW_OnTaskStopReady(TXSV_ID(i1), (unsigned int) i2);
        return;
    case TX_TRACE_THREAD_CREATE:
        SEGGER_SYSVIEW_OnTaskCreate(TXSV_ID(i1));
        txsv_send_task_info((TX_THREAD *) i1);
        break;                          /* also record as API event below        */
    case TX_TRACE_THREAD_DELETE:
        SEGGER_SYSVIEW_OnTaskTerminate(TXSV_ID(i1));
        break;
    case TX_TRACE_ISR_ENTER:            /* app-called tx_trace_isr_enter_insert() */
        SEGGER_SYSVIEW_RecordEnterISR();
        return;
    case TX_TRACE_ISR_EXIT:
        if (_tx_thread_execute_ptr != _tx_thread_current_ptr) {
            SEGGER_SYSVIEW_RecordExitISRToScheduler();
        } else {
            SEGGER_SYSVIEW_RecordExitISR();
        }
        return;
    case TX_TRACE_RUNNING:
        return;
    default:
        break;
    }
    TXSV_COUNT(5);
    if (id >= TX_TRACE_USER_EVENT_START) {
        unsigned long u = id - TX_TRACE_USER_EVENT_START;
        SEGGER_SYSVIEW_RecordU32x4(TXSV_USER_ID_BASE + (unsigned int)(u % TXSV_USER_ID_SPAN),
                                   (U32) i1, (U32) i2, (U32) i3, (U32) i4);
    } else {
        unsigned m = (id <= TXSV_THREAD_PARAM_MAX_ID) ? txsv_thread_params[id] : 0u;
        SEGGER_SYSVIEW_RecordU32x4(TXSV_OS_ID_BASE + (unsigned int) id,
                                   txsv_param(i1, m, 0), txsv_param(i2, m, 1),
                                   txsv_param(i3, m, 2), txsv_param(i4, m, 3));
    }
}

/* ---- Port execution-change hooks (TX_ENABLE_EXECUTION_CHANGE_NOTIFY) ------- */
VOID _tx_execution_initialize(VOID)
{
}

VOID _tx_execution_thread_enter(VOID)   /* PendSV: about to restore a thread */
{
    TX_THREAD *t;
    TX_THREAD_GET_CURRENT(t)
    if ((t != TX_NULL) && SEGGER_SYSVIEW_IsStarted()) {
        TXSV_COUNT(0);
        SEGGER_SYSVIEW_OnTaskStartExec(TXSV_ID(t));
    }
}

VOID _tx_execution_thread_exit(VOID)    /* PendSV: current thread saved, interrupts masked */
{
    TX_THREAD *t;
    TX_THREAD_GET_CURRENT(t)
    if ((t != TX_NULL) && SEGGER_SYSVIEW_IsStarted()) {
        TXSV_COUNT(1);
        SEGGER_SYSVIEW_OnTaskStopExec();   /* readiness already reported via SUSPEND */
    }
    if (_tx_thread_execute_ptr == TX_NULL) {
        SEGGER_SYSVIEW_OnIdle();
    }
}

VOID _tx_execution_isr_enter(VOID)
{
    TXSV_COUNT(4);
    SEGGER_SYSVIEW_RecordEnterISR();
}

VOID _tx_execution_isr_exit(VOID)
{
    if (_tx_thread_execute_ptr != _tx_thread_current_ptr) {
        SEGGER_SYSVIEW_RecordExitISRToScheduler();
    } else {
        SEGGER_SYSVIEW_RecordExitISR();
    }
}

/* ---- SystemView OS callbacks ---------------------------------------------- */
static U64 txsv_get_time_us(void)
{
    return ((U64) tx_time_get() * 1000000u) / TX_TIMER_TICKS_PER_SECOND;
}

static void txsv_send_task_list(void)
{
    TX_THREAD *t = _tx_thread_created_ptr;
    ULONG n = _tx_thread_created_count;
    while ((t != TX_NULL) && (n-- > 0u)) {
        txsv_send_task_info(t);
        t = t->tx_thread_created_next;
    }
}

const SEGGER_SYSVIEW_OS_API txsv_os_api = { txsv_get_time_us, txsv_send_task_list };

extern unsigned int SystemCoreClock;

static void txsv_send_sys_desc(void)
{
    SEGGER_SYSVIEW_SendSysDesc("N=ThreadX app,O=ThreadX_native,D=Cortex-M");
    SEGGER_SYSVIEW_SendSysDesc("I#15=SysTick");
}

#if !TXTRACE_NATIVE
/* tx_trace_user_event_insert() and tx_trace_isr_enter/exit_insert() return
   TX_NOT_DONE unless a native trace buffer is enabled, even though our macro
   never writes to it. In SystemView-only mode we enable a stub buffer
   (header + 1 registry entry) so those calls reach the adapter. */
static UCHAR txsv_stub_trace_buf[256] __attribute__((aligned(4)));
#endif

void txsv_init(void)
{
    SEGGER_SYSVIEW_Init(SystemCoreClock, SystemCoreClock, &txsv_os_api, txsv_send_sys_desc);
    SEGGER_SYSVIEW_SetRAMBase(0x20000000u);
#if !TXTRACE_NATIVE
    tx_trace_enable(txsv_stub_trace_buf, sizeof txsv_stub_trace_buf, 1);
#endif
#ifdef TXSV_AUTOSTART
    SEGGER_SYSVIEW_Start();          /* record without a host (post-mortem / CI) */
#endif                               /* otherwise the SystemView host sends START */
}
