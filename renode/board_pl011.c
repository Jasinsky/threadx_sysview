/*
 * board_pl011.c - minimal polled ARM PL011 driver + DWT enable for the
 * Renode "threadx_m4" platform (renode/threadx_m4.repl).
 */
#include <stdint.h>

#define UART_BASE   0x40004000u
#define UART_DR     (*(volatile uint32_t *)(UART_BASE + 0x000))
#define UART_FR     (*(volatile uint32_t *)(UART_BASE + 0x018))
#define UART_LCR_H  (*(volatile uint32_t *)(UART_BASE + 0x02C))
#define UART_CR     (*(volatile uint32_t *)(UART_BASE + 0x030))
#define FR_RXFE     (1u << 4)
#define FR_TXFF     (1u << 5)

#define DEMCR       (*(volatile uint32_t *) 0xE000EDFCu)
#define DWT_CTRL    (*(volatile uint32_t *) 0xE0001000u)
#define DWT_CYCCNT  (*(volatile uint32_t *) 0xE0001004u)

void board_init(void)
{
    if ((UART_CR & 1u) == 0) {                     /* skip if already up (Renode .resc) */
        UART_LCR_H = (3u << 5) | (1u << 4);        /* WLEN = 8 bit, FEN, 8N1 */
        UART_CR = (1u << 0) | (1u << 8) | (1u << 9);   /* UARTEN | TXE | RXE */
    }
    DEMCR  |= (1u << 24);                          /* TRCENA            */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;                                /* CYCCNTENA         */
}

int  txsv_uart_rx_ready(void) { return (UART_FR & FR_RXFE) == 0; }
int  txsv_uart_getc(void)     { return (int)(UART_DR & 0xFFu); }
void txsv_uart_putc(int c)
{
    while (UART_FR & FR_TXFF) { }
    UART_DR = (uint32_t)(c & 0xFF);
}
