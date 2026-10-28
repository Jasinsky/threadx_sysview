/*
 * txsv_native_insert.c - out-of-line copy of upstream ThreadX's default
 * TX_TRACE_IN_LINE_INSERT body (MIT, Eclipse ThreadX common/inc/tx_trace.h),
 * used only in dual-backend mode so the TraceX-format buffer keeps working.
 *
 * MAINTENANCE: CI must diff this body against tx_trace.h on each ThreadX bump.
 */
#define TX_SOURCE_CODE
#include "tx_api.h"
#include "tx_thread.h"
#include "tx_trace.h"

#if TXTRACE_NATIVE

void txsv_native_insert(unsigned long i, unsigned long a, unsigned long b,
                        unsigned long c, unsigned long d, unsigned long e)
{
    TX_TRACE_BUFFER_ENTRY *trace_event_ptr;
    ULONG                  trace_system_state;
    ULONG                  trace_priority;
    TX_THREAD             *trace_thread_ptr;

    trace_event_ptr = _tx_trace_buffer_current_ptr;
    if ((trace_event_ptr) && (_tx_trace_event_enable_bits & ((ULONG) e))) {
        TX_TRACE_PORT_EXTENSION
        trace_system_state = (ULONG) TX_THREAD_GET_SYSTEM_STATE();
        TX_THREAD_GET_CURRENT(trace_thread_ptr)
        if (trace_system_state == 0) {
            trace_priority = trace_thread_ptr->tx_thread_priority;
            trace_priority = trace_priority | 0x80000000UL | (trace_thread_ptr->tx_thread_preempt_threshold << 16);
        } else if (trace_system_state < 0xF0F0F0F0UL) {
            trace_priority   = (ULONG) trace_thread_ptr;
            trace_thread_ptr = (TX_THREAD *) 0xFFFFFFFFUL;
        } else {
            trace_thread_ptr = (TX_THREAD *) 0xF0F0F0F0UL;
            trace_priority   = 0;
        }
        trace_event_ptr->tx_trace_buffer_entry_thread_pointer  = (ULONG) trace_thread_ptr;
        trace_event_ptr->tx_trace_buffer_entry_thread_priority = (ULONG) trace_priority;
        trace_event_ptr->tx_trace_buffer_entry_event_id        = (ULONG) i;
        trace_event_ptr->tx_trace_buffer_entry_time_stamp      = (ULONG) TX_TRACE_TIME_SOURCE;
        TX_TRACE_INFO_FIELD_ASSIGNMENT(a, b, c, d)
        trace_event_ptr++;
        if (trace_event_ptr >= _tx_trace_buffer_end_ptr) {
            trace_event_ptr              = _tx_trace_buffer_start_ptr;
            _tx_trace_buffer_current_ptr = trace_event_ptr;
            _tx_trace_header_ptr->tx_trace_header_buffer_current_pointer = (ULONG) trace_event_ptr;
            if (_tx_trace_full_notify_function) {
                (_tx_trace_full_notify_function)((VOID *) _tx_trace_header_ptr);
            }
        } else {
            _tx_trace_buffer_current_ptr = trace_event_ptr;
            _tx_trace_header_ptr->tx_trace_header_buffer_current_pointer = (ULONG) trace_event_ptr;
        }
    }
}

#endif
