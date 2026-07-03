#include "uart.h"
#include "mmio/mmio.h"
#include <stdarg.h>

// PL011
void uart_init() {
  // disable uart
  mmio_write32(UART_CR, 0x00000000);

  // clear pending interrupts
  mmio_write32(UART_ICR, 0x7FF);

  // setup baudrate
  // divisor = clk/(16 * baud)
  // clk = 24000000/(16 * 115200) = 13.02083333
  // integer = 13
  // fraction = 0.02083333
  // fraction register = round(0.02083333 * 2^6) = 2
  // FBRD is a 6 bit number
  mmio_write32(UART_IBRD, 13);
  mmio_write32(UART_FBRD, 2);

  // enable FIFO, 8bit data transmission - 1 stop bit, no parity
  mmio_write32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

  // enable UART, RX, TX
  mmio_write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));

  uart_println("UART Initialized !");
}

void uart_putc(const char c) {
  // check if transmit fifo is full - TXFF
  while (mmio_read32(UART_FR) & (1 << 5)) {
  }

  mmio_write32(UART_DR, c);
  return;
}

uint8_t uart_getc() {
  // check if receive fifo is empty - RXFE
  while (mmio_read32(UART_FR) & (1 << 4)) {
  }

  uint8_t value = (uint8_t)mmio_read32(UART_DR);
  return value;
}

void uart_puts(const char *str) {
  while (*str) {
    uart_putc(*str++);
  }
}

void uart_println(const char *str) {
  uart_puts(str);
  uart_putc('\n');
}

void uart_errorln(const char *err) {
  uart_puts("[ERROR!]: ");
  uart_puts(err);
  uart_putc('\n');
}

/* ===========================================================================
 * UART1 (@ 0x09040000) — interrupt-driven RX for the framing layer.
 * PL011 register offsets are the same as UART0; only the base differs.
 * ========================================================================= */
#define UART1_BASE 0x09040000UL
#define UART1_DR   (UART1_BASE + 0x00)
#define UART1_FR   (UART1_BASE + 0x18)
#define UART1_IBRD (UART1_BASE + 0x24)
#define UART1_FBRD (UART1_BASE + 0x28)
#define UART1_LCRH (UART1_BASE + 0x2C)
#define UART1_CR   (UART1_BASE + 0x30)
#define UART1_IMSC (UART1_BASE + 0x38)
#define UART1_ICR  (UART1_BASE + 0x44)

#define UART_FR_RXFE (1U << 4) /* RX FIFO empty */
#define UART_FR_TXFF (1U << 5) /* TX FIFO full  */
#define UART_INT_RX  (1U << 4) /* RXIM / RXIC / RXMIS — receive interrupt */

/* 256-byte RX ring. Single producer (uart1_rx_isr in IRQ context), single
 * consumer (uart1_getc_nonblock in task context, which masks IRQs). */
#define UART1_RING_SZ 256
static volatile uint8_t  u1_ring[UART1_RING_SZ];
static volatile uint32_t u1_head; /* write index (ISR)      */
static volatile uint32_t u1_tail; /* read index  (consumer) */

void uart1_init(void) {
  mmio_write32(UART1_CR, 0x00000000);      /* disable while configuring   */
  mmio_write32(UART1_ICR, 0x7FF);          /* clear all pending interrupts */
  mmio_write32(UART1_IBRD, 13);            /* same 115200 baud as UART0    */
  mmio_write32(UART1_FBRD, 2);
  mmio_write32(UART1_LCRH, (1 << 4) | (1 << 5) | (1 << 6)); /* FIFO, 8N1   */
  mmio_write32(UART1_IMSC, UART_INT_RX);   /* unmask RX interrupt (RXIM)    */
  mmio_write32(UART1_CR, (1 << 0) | (1 << 8) | (1 << 9));   /* UART|TXE|RXE */
  u1_head = 0;
  u1_tail = 0;
  uart_println("[UART1] Framing UART @0x09040000 initialized (RX IRQ on)");
}

void uart1_putc(uint8_t c) {
  while (mmio_read32(UART1_FR) & UART_FR_TXFF) {
  }
  mmio_write32(UART1_DR, c);
}

void uart1_rx_isr(void) {
  /* Drain the RX FIFO into the ring. Runs in IRQ context, so it is atomic
   * with respect to the consumer (which masks IRQs). */
  while (!(mmio_read32(UART1_FR) & UART_FR_RXFE)) {
    uint8_t b = (uint8_t)mmio_read32(UART1_DR);
    uint32_t next = (u1_head + 1) % UART1_RING_SZ;
    if (next != u1_tail) {
      u1_ring[u1_head] = b;
      u1_head = next;
    }
    /* else: ring full — drop the byte (framing CRC will catch the loss). */
  }
  mmio_write32(UART1_ICR, UART_INT_RX); /* clear the RX interrupt */
}

int uart1_getc_nonblock(void) {
  int ret = -1;
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #2"); /* mask IRQ: no ISR races tail */
  if (u1_tail != u1_head) {
    ret = (int)u1_ring[u1_tail];
    u1_tail = (u1_tail + 1) % UART1_RING_SZ;
  }
  __asm__ __volatile__("msr daif, %0" ::"r"(daif));
  return ret;
}

int uart1_rx_available(void) {
  uint64_t daif;
  __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
  __asm__ __volatile__("msr daifset, #2");
  uint32_t h = u1_head, t = u1_tail;
  __asm__ __volatile__("msr daif, %0" ::"r"(daif));
  return (int)((h - t) % UART1_RING_SZ);
}

void uart_puthex(uint64_t value) {
  uart_puts("0x");
  int started = 0;
  for (int i = 60; i >= 0; i -= 4) {
    uint8_t nibble = (value >> i) & 0xF;
    if (nibble || started || i == 0) {
      uart_putc(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
      started = 1;
    }
  }
}

void uart_putdec(uint64_t value) {
  if (value == 0) {
    uart_putc('0');
    return;
  }
  char buf[20];
  int i = 0;
  while (value) {
    buf[i++] = '0' + (value % 10);
    value /= 10;
  }
  while (i--) {
    uart_putc(buf[i]);
  }
}

void uart_putbin(uint64_t value) {
  uart_puts("0b");
  int started = 0;
  for (int i = 63; i >= 0; i--) {
    uint8_t bit = (value >> i) & 1;
    if (bit || started || i == 0) {
      uart_putc('0' + bit);
      started = 1;
    }
  }
}

// Supported format specifiers:
//   %s  - const char *
//   %d  - int64_t
//   %u  - uint64_t
//   %x  - hex with 0x prefix (uint64_t)
//   %p  - pointer (void *), same as %x
//   %b  - binary with 0b prefix (uint64_t)
//   %c  - char
//   %%  - literal '%'
void uart_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  while (*fmt) {
    if (*fmt != '%') {
      uart_putc(*fmt++);
      continue;
    }

    fmt++;

    switch (*fmt) {
    case 's': {
      const char *s = va_arg(args, const char *);
      uart_puts(s ? s : "(null)");
      break;
    }
    case 'd': {
      int64_t val = va_arg(args, int64_t);
      if (val < 0) {
        uart_putc('-');
        uart_putdec((uint64_t)(-val));
      } else {
        uart_putdec((uint64_t)val);
      }
      break;
    }
    case 'u': {
      uint64_t val = va_arg(args, uint64_t);
      uart_putdec(val);
      break;
    }
    case 'x': {
      uint64_t val = va_arg(args, uint64_t);
      uart_puthex(val);
      break;
    }
    case 'p': {
      void *ptr = va_arg(args, void *);
      uart_puthex((uint64_t)ptr);
      break;
    }
    case 'b': {
      uint64_t val = va_arg(args, uint64_t);
      uart_putbin(val);
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      uart_putc(c);
      break;
    }
    case '%': {
      uart_putc('%');
      break;
    }
    case '\0': {
      // format string ended with a lone '%'
      goto done;
    }
    default: {
      // unknown specifier, print as-is
      uart_putc('%');
      uart_putc(*fmt);
      break;
    }
    }

    fmt++;
  }

done:
  va_end(args);
}
