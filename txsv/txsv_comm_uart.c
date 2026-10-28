/*
 * txsv_comm_uart.c - stream SystemView data over a plain UART, speaking BOTH
 * SystemView host protocols, chosen by the host's first two bytes:
 *
 *   "SE..." -> IP Recorder   (32-byte "SEGGER SystemView Vx.yy" hellos,
 *                             host->target data framed as [len][bytes]).
 *              Use with Renode: CreateServerSocketTerminal + SystemView
 *              "IP Recorder" pointed at the socket.
 *   "SV.."  -> UART Recorder (4-byte hello, raw host->target bytes);
 *              delegated to SEGGER's own Sample/COMM/SEGGER_SYSVIEW_REC.c.
 *              Use with a real UART/COM port or a Renode pty.
 *
 * The pump runs as the lowest-priority ThreadX thread and sleeps one tick
 * when idle, so it only consumes otherwise-idle CPU. It appears in the
 * trace as a task named "SysView comm".
 *
 * Board code must provide the three txsv_uart_* functions below.
 */
#include "tx_api.h"
#include "SEGGER_SYSVIEW.h"
#include "SEGGER_RTT.h"
#include "SEGGER_SYSVIEW_REC.h"

int  txsv_uart_rx_ready(void);          /* non-zero when a byte is waiting  */
int  txsv_uart_getc(void);              /* returns the waiting byte         */
void txsv_uart_putc(int c);             /* blocking write of one byte       */

#define HELLO_SIZE 32
static const U8 hello_ip[HELLO_SIZE] = {
    'S','E','G','G','E','R',' ','S','y','s','t','e','m','V','i','e','w',' ','V',
    '0' + SEGGER_SYSVIEW_MAJOR, '.', '0' + (SEGGER_SYSVIEW_MINOR / 10), '0' + (SEGGER_SYSVIEW_MINOR % 10),
    '.', '0' + (SEGGER_SYSVIEW_REV / 10), '0' + (SEGGER_SYSVIEW_REV % 10), '\0', 0, 0, 0, 0, 0 };

enum { ST_WAIT_S, ST_WAIT_2ND, ST_IP_HELLO, ST_IP_REC, ST_UART_REC };

static int  state = ST_WAIT_S;
static int  hello_cnt;
static int  frame_left = -1;            /* IP mode: bytes left in host frame */
static unsigned chan;
static U8   up[256];

volatile ULONG txsv_comm_bytes_out;     /* for tests / debugging */

static void send_buf(const U8 *p, unsigned n)
{
    while (n--) {
        txsv_uart_putc(*p++);
    }
}

static void on_rx(U8 c)
{
    switch (state) {
    case ST_WAIT_S:
        if (c == 'S') { state = ST_WAIT_2ND; }
        break;
    case ST_WAIT_2ND:
        if (c == 'E') {                         /* IP Recorder hello */
            state = ST_IP_HELLO;
            hello_cnt = 2;
        } else if (c == 'V') {                  /* UART Recorder hello */
            static const U8 sv[2] = { 'S', 'V' };
            SYSVIEW_REC_ProcessIncoming(sv, 2);
            state = ST_UART_REC;
        } else {
            state = ST_WAIT_S;
        }
        break;
    case ST_IP_HELLO:
        if (++hello_cnt == HELLO_SIZE) {
            SEGGER_SYSVIEW_Stop();              /* host may have left without STOP */
            while (SEGGER_RTT_ReadUpBufferNoLock(chan, up, sizeof up) > 0) { }  /* drop old session's data */
            send_buf(hello_ip, HELLO_SIZE);
            frame_left = -1;
            state = ST_IP_REC;
        }
        break;
    case ST_IP_REC:                             /* [len][len bytes] */
        if (frame_left <= 0 && c == 'S') {
            /* Host commands are a few bytes long, so a length of 'S' (83)
             * means SystemView reconnected and is sending a new hello. */
            state = ST_WAIT_2ND;
        } else if (frame_left <= 0) {
            frame_left = c;
        } else {
            SEGGER_RTT_WriteDownBufferNoLock(chan, &c, 1);
            frame_left--;
        }
        break;
    case ST_UART_REC:
        SYSVIEW_REC_ProcessIncoming(&c, 1);
        break;
    default:
        break;
    }
}

static VOID comm_entry(ULONG arg)
{
    (void) arg;
    chan = SEGGER_SYSVIEW_GetChannelID();
    for (;;) {
        int moved = 0;
        int n;
        while (txsv_uart_rx_ready()) {
            on_rx((U8) txsv_uart_getc());
            moved = 1;
        }
        if (state == ST_IP_REC) {
            n = (int) SEGGER_RTT_ReadUpBufferNoLock(chan, up, sizeof up);
        } else if (state == ST_UART_REC) {
            n = SYSVIEW_REC_GetOutgoing(up, sizeof up);  /* hello reply, then data */
        } else {
            n = 0;
        }
        if (n > 0) {
            send_buf(up, (unsigned) n);
            txsv_comm_bytes_out += (ULONG) n;
            moved = 1;
        }
        if (!moved) {
            tx_thread_sleep(1);
        }
    }
}

static TX_THREAD comm_thread;

void txsv_comm_start(VOID *stack, ULONG stack_size)
{
    tx_thread_create(&comm_thread, "SysView comm", comm_entry, 0, stack, stack_size,
                     TX_MAX_PRIORITIES - 1, TX_MAX_PRIORITIES - 1, TX_NO_TIME_SLICE, TX_AUTO_START);
}
