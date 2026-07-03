#ifndef LIB_UART_H
#define LIB_UART_H

#include <stdint.h>

#define UART_BASE 0x09000000UL
#define UART_DR (UART_BASE + 0x00)
#define UART_FR (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR (UART_BASE + 0x30)
#define UART_ICR (UART_BASE + 0x44)

/* GIC INTIDs for the two QEMU virt PL011 UARTs (confirmed from the virt DTB):
 *   pl011@9000000 -> interrupts <0 1 4> -> SPI 1  -> INTID 33  (console, UART0)
 *   pl011@9040000 -> interrupts <0 8 4> -> SPI 8  -> INTID 40  (framing, UART1)
 */
#define UART0_INTID 33
#define UART1_INTID 40

void uart_init(void);
void uart_putc(const char c);
uint8_t uart_getc(void);
void uart_puts(const char *str);
void uart_println(const char *str);
void uart_errorln(const char *err);

void uart_puthex(uint64_t value);
void uart_putdec(uint64_t value);
void uart_putbin(uint64_t value);

// format specifiers: %s %d %u %x %p %b %c %%
void uart_printf(const char *fmt, ...);

/* ---- Secondary UART (UART1 @ 0x09040000) — dedicated to the framing layer.
 * Console I/O stays on UART0 (polling); UART1 is interrupt-driven so incoming
 * framed bytes are buffered in a ring while a task is busy. Kept physically
 * separate from /dev/console so the framing byte stream never mixes with
 * kernel/console output. */
void uart1_init(void);
void uart1_putc(uint8_t c);
/* Pop one received byte from the RX ring; returns -1 if the ring is empty.
 * Interrupt-safe (masks IRQs around the tail update). */
int uart1_getc_nonblock(void);
/* Number of bytes currently queued in the RX ring. */
int uart1_rx_available(void);
/* RX interrupt service routine — called from the IRQ dispatch on INTID 40.
 * Drains the PL011 RX FIFO into the ring and clears the interrupt. */
void uart1_rx_isr(void);

#endif