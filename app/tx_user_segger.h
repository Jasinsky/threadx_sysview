#ifndef TX_USER_H
#define TX_USER_H
#define ENABLE_TRACE_API 1
#ifdef TXPOC_QEMU
#ifndef __ASSEMBLER__
unsigned long txpoc_timestamp(void);
#endif
#endif
#endif
