/*
 * STM8L152C6T6 - FOTA Bootloader FINAL
 * Dia chi register kiem tra tu datasheet STM8L151XX Doc ID 15962
 *
 * CLK_ICKR    = 0x50C2  (khong phai 0x50C0!)
 * CLK_PCKENR1 = 0x50C3  (khong phai 0x50C7!)
 * CLK_CKDIVR  = 0x50C0  (day moi la DIVR)
 * SYSCFG_RMPCR1 = 0x509E
 * USART1_BRR1 = 0x5232, BRR2 = 0x5233
 * USART1_CR2  = 0x5235
 *
 * LED: LD3=PE7 (xanh la), LD4=PC7 (xanh duong)
 * UART: USART1 remap PA2(TX)/PA3(RX) @ 9600 baud 16MHz
 *
 * Build:
 *   sdcc -mstm8 --out-fmt-ihx --code-loc 0x8000 --code-size 0x400
 *        --iram-size 2048 --stack-loc 0x07FF
 *        --nogcse --noinvariant --noinduction
 *        stm8l_bootloader.c -o stm8l_bootloader.ihx
 */

typedef unsigned char  u8;
typedef unsigned int   u16;
typedef unsigned long  u32;
#define REG8(a) (*(volatile u8*)(a))

/* ============================================================
   CLOCK - dia chi DUNG theo datasheet trang 35
   0x50C0 = CLK_DIVR   (master divider)
   0x50C1 = CLK_CRTCR  (RTC clock)
   0x50C2 = CLK_ICKR   (internal clock control) <-- HSION/HSIRDY o day
   0x50C3 = CLK_PCKENR1
   0x50C4 = CLK_PCKENR2
   ============================================================ */
#define CLK_DIVR    REG8(0x50C0)   /* [3:0] HSIDIV, [5:4] CPUDIV */
#define CLK_ICKR    REG8(0x50C2)   /* bit0=HSION, bit1=HSIRDY    */
#define CLK_PCKENR1 REG8(0x50C3)   /* bit5=USART1                */
#define CLK_PCKENR2 REG8(0x50C4)

/* ============================================================
   SYSCFG - dia chi DUNG
   0x509E = SYSCFG_RMPCR1
   0x509F = SYSCFG_RMPCR2
   ============================================================ */
#define SYSCFG_RMPCR1 REG8(0x509E)
#define SYSCFG_RMPCR2 REG8(0x509F)

/* ============================================================
   USART1 - dia chi DUNG (0x5230-0x5239)
   BRR1 = 0x5232, BRR2 = 0x5233 (ghi BRR1 truoc BRR2)
   ============================================================ */
#define USART1_SR   REG8(0x5230)
#define USART1_DR   REG8(0x5231)
#define USART1_BRR1 REG8(0x5232)   /* ghi BRR1 TRUOC */
#define USART1_BRR2 REG8(0x5233)   /* ghi BRR2 SAU   */
#define USART1_CR1  REG8(0x5234)
#define USART1_CR2  REG8(0x5235)
#define USART1_CR3  REG8(0x5236)

#define SR_TXE  0x80
#define SR_RXNE 0x20

/* ============================================================
   GPIO
   ============================================================ */
#define PA_ODR REG8(0x5000)
#define PA_IDR REG8(0x5001)
#define PA_DDR REG8(0x5002)
#define PA_CR1 REG8(0x5003)
#define PA_CR2 REG8(0x5004)

#define PC_ODR REG8(0x500A)
#define PC_DDR REG8(0x500C)
#define PC_CR1 REG8(0x500D)

#define PE_ODR REG8(0x5014)
#define PE_DDR REG8(0x5016)
#define PE_CR1 REG8(0x5017)

/* ============================================================
   FLASH
   ============================================================ */
#define FLASH_CR2   REG8(0x5051)
#define FLASH_PUKR  REG8(0x5052)
#define FLASH_IAPSR REG8(0x5054)
#define IAPSR_PUL   0x02
#define IAPSR_EOP   0x04
#define IAPSR_FAIL  0x01

/* ============================================================
   IWDG
   ============================================================ */
#define IWDG_KR REG8(0x50E0)

/* ============================================================
   PROTOCOL
   ============================================================ */
#define APP_ADDR  ((u32)0x8400)
#define BYTE_TRIG 0x7F
#define BYTE_ACK  0x79
#define BYTE_NACK 0x1F
#define BYTE_EOF  0xFF
#define MAX_DATA  16

/* ============================================================
   LED helpers - PE7=LD3 xanh la, PC7=LD4 xanh duong
   ============================================================ */
static void led_init(void)
{
    PE_DDR |= (u8)(1<<7); PE_CR1 |= (u8)(1<<7); PE_ODR &= (u8)~(1<<7);
    PC_DDR |= (u8)(1<<7); PC_CR1 |= (u8)(1<<7); PC_ODR &= (u8)~(1<<7);
}

static void delay_loop(u16 n)
{
    volatile u16 i;
    for (i = 0; i < n; i++) { IWDG_KR = 0xAA; }
}

static void led_both(u8 on)
{
    if (on) { PE_ODR |= (u8)(1<<7); PC_ODR |= (u8)(1<<7); }
    else    { PE_ODR &= (u8)~(1<<7); PC_ODR &= (u8)~(1<<7); }
}

static void led_ld3_toggle(void) { PE_ODR ^= (u8)(1<<7); }
static void led_ld4_on(void)     { PC_ODR |=  (u8)(1<<7); }
static void led_ld4_off(void)    { PC_ODR &= (u8)~(1<<7); }

static void blink_n(u8 n)
{
    u8 i;
    for (i = 0; i < n; i++) {
        led_both(1); delay_loop(30000); delay_loop(30000);
        led_both(0); delay_loop(30000); delay_loop(30000);
    }
}

/* ============================================================
   CLOCK - HSI 16MHz, no divider
   Sau reset: CLK_DIVR = 0x03 (HSI/8 = 2MHz)
   Can xoa ve 0x00 de chay 16MHz
   ============================================================ */
static void clock_init(void)
{
    /* Bat HSION, cho HSIRDY */
    CLK_ICKR |= 0x01;
    while (!(CLK_ICKR & 0x02)) { IWDG_KR = 0xAA; }

    /* Xoa divider: CPU=HSI, HSIDIV=1 → 16MHz */
    CLK_DIVR = 0x00;

    /* Bat clock USART1 (bit 5 cua PCKENR1) */
    CLK_PCKENR1 |= (u8)(1<<5);
}

/* ============================================================
   USART1 init - remap PA2(TX)/PA3(RX)
   SYSCFG_RMPCR1 bit[5:4]:
     00 = PC3/PC2 (default, bi LCD chiem)
     01 = PA2/PA3  <-- dung cai nay
     10 = PC5/PC6
   ============================================================ */
static void usart_init(void)
{
    /* Remap USART1 TX/RX sang PA2/PA3: set bit4, clear bit5 */
    SYSCFG_RMPCR1 = (u8)((SYSCFG_RMPCR1 & (u8)~0x30) | 0x10);

    /* PA2 = TX: output push-pull, AF */
    PA_DDR |=  (u8)(1<<2);
    PA_CR1 |=  (u8)(1<<2);
    PA_CR2 |=  (u8)(1<<2);

    /* PA3 = RX: input pull-up */
    PA_DDR &= (u8)~(1<<3);
    PA_CR1 |=  (u8)(1<<3);
    PA_CR2 &= (u8)~(1<<3);

    /* 9600 baud @ 16MHz: BRR = 1667
       BRR[15:12]=0, BRR[11:4]=0x68, BRR[3:0]=3
       BRR1=0x68 (bits 11:4), BRR2=0x03 (bits 15:12 << 4 | bits 3:0)
       QUAN TRONG: ghi BRR1 TRUOC, BRR2 SAU */
    USART1_CR1  = 0x00;
    USART1_CR3  = 0x00;
    USART1_BRR1 = 0x68;   /* ghi BRR1 truoc */
    USART1_BRR2 = 0x03;   /* ghi BRR2 sau  */
    USART1_CR2  = 0x0C;   /* TEN=1, REN=1  */
}

static void uart_tx(u8 b)
{
    while (!(USART1_SR & SR_TXE)) { IWDG_KR = 0xAA; }
    USART1_DR = b;
}

static u8 uart_rx(void)
{
    while (!(USART1_SR & SR_RXNE)) { IWDG_KR = 0xAA; }
    return USART1_DR;
}

/* timeout ~5s, tra 0xFFFF neu het gio */
static u16 uart_rx_timeout(u16 outer)
{
    u16 i, j;
    for (i = 0; i < outer; i++) {
        for (j = 0; j < 10000; j++) {
            if (USART1_SR & SR_RXNE) { IWDG_KR = 0xAA; return (u16)USART1_DR; }
        }
        IWDG_KR = 0xAA;
    }
    return 0xFFFF;
}

/* ============================================================
   CRC16-CCITT poly=0x1021 init=0xFFFF
   ============================================================ */
static u16 crc16(u8 *buf, u8 len)
{
    u16 crc = 0xFFFF;
    u8 i, j;
    for (i = 0; i < len; i++) {
        crc ^= ((u16)buf[i] << 8);
        for (j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (u16)((crc<<1)^0x1021) : (u16)(crc<<1);
    }
    return crc;
}

/* ============================================================
   FLASH IAP
   ============================================================ */
static void flash_unlock(void)
{
    FLASH_PUKR = 0x56; FLASH_PUKR = 0xAE;
    while (!(FLASH_IAPSR & IAPSR_PUL)) { IWDG_KR = 0xAA; }
}

static void flash_lock(void)
{
    FLASH_IAPSR &= (u8)~IAPSR_PUL;
}

static void flash_write_byte(u32 addr, u8 data)
{
    FLASH_CR2 = 0x00;
    *((volatile u8*)addr) = data;
    while (!(FLASH_IAPSR & (IAPSR_EOP | IAPSR_FAIL))) { IWDG_KR = 0xAA; }
}

/* ============================================================
   JUMP TO APP
   ============================================================ */
static void jump_to_app(void)
{
    USART1_CR2   = 0x00;
    CLK_PCKENR1 &= (u8)~(1<<5);
    led_both(0);
    ((void(*)(void))APP_ADDR)();
    while (1) { IWDG_KR = 0xAA; }
}

/* ============================================================
   RECEIVE FRAME + WRITE FLASH
   [ADDR_HI][ADDR_MD][ADDR_LO][LEN][DATA x LEN][CRC_HI][CRC_LO]
   ============================================================ */
static void recv_and_write(u8 first)
{
    u8  buf[4 + MAX_DATA];
    u8  i, len;
    u16 crc_rx, crc_calc;
    u32 addr;

    buf[0] = first;
    for (i = 1; i < 4; i++) buf[i] = uart_rx();

    len = buf[3];
    if (len == 0 || len > MAX_DATA) { uart_tx(BYTE_NACK); return; }

    for (i = 0; i < len; i++) buf[4+i] = uart_rx();

    crc_rx  = (u16)uart_rx() << 8;
    crc_rx |= (u16)uart_rx();
    crc_calc = crc16(buf, (u8)(4+len));
    if (crc_calc != crc_rx) { uart_tx(BYTE_NACK); return; }

    addr = ((u32)buf[0]<<16) | ((u32)buf[1]<<8) | (u32)buf[2];
    if (addr < APP_ADDR || (addr+len) > 0xA000UL) { uart_tx(BYTE_NACK); return; }

    for (i = 0; i < len; i++) flash_write_byte(addr+i, buf[4+i]);

    led_ld3_toggle();
    uart_tx(BYTE_ACK);
}

/* ============================================================
   MAIN
   ============================================================ */
void main(void)
{
    u8  first;
    u16 r;

    IWDG_KR = 0xAA;
    clock_init();
    led_init();

    blink_n(5);        /* 5 nhay = bootloader dang chay */

    usart_init();
    led_ld4_on();      /* LD4 sang = cho handshake */

    /* Cho 0x7F tu ESP32, timeout 5s thi tu nhay app */
    r = uart_rx_timeout(200);
    for (;;) {
        if (r == 0xFFFF) { led_ld4_off(); jump_to_app(); }
        first = (u8)(r & 0xFF);
        if (first == BYTE_TRIG) { uart_tx(BYTE_ACK); break; }
        r = uart_rx_timeout(200);
    }

    /* Handshake OK - nhan firmware */
    flash_unlock();

    for (;;) {
        first = uart_rx();
        if (first == BYTE_EOF) {
            uart_tx(BYTE_ACK);
            flash_lock();
            led_both(1); delay_loop(50000); led_both(0);
            delay_loop(20000);
            jump_to_app();
        }
        recv_and_write(first);
    }
}
