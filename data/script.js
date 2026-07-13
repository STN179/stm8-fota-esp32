/* ============================================================
 *  FOTA STM8L — Compile & Flash client
 *  Loaded from ESP32 SPIFFS at http://192.168.4.1
 *
 *  Flow:
 *    1) POST source to  http://localhost:5555/compile-only
 *       (Python server.py runs SDCC, auto-links stm8_lcd.c if needed)
 *    2) POST .bin to    /upload
 *    3) Read SSE from   /events
 * ============================================================ */

(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const src      = $('src');
  const srcInfo  = $('src-info');
  const btnFlash = $('btn-flash');
  const btnClear = $('btn-clear');
  const tpl      = $('tpl');
  const fileInp  = $('file');
  const dot      = $('dot');
  const statusTx = $('status-text');
  const lastStat = $('last-status');
  const logBox   = $('log');
  const result   = $('result');
  const pfill    = $('pfill');
  const ppct     = $('ppct');

  const HELPER = 'http://localhost:5555';

  /* ============================================================
   *  Templates
   *  - LED / UART: standalone
   *  - LCD: use #include "stm8_lcd.h" (thư viện có sẵn ở lib/)
   * ============================================================ */
  const TEMPLATES = {
    blink: `/* Blink LD3 (PE7) va LD4 (PC7) moi 500 ms */
typedef unsigned char u8;
typedef unsigned int  u16;
#define REG8(a) (*(volatile u8*)(a))
#define CLK_CKDIVR REG8(0x50C0)
#define PE_ODR REG8(0x5014)
#define PE_DDR REG8(0x5016)
#define PE_CR1 REG8(0x5017)
#define PC_ODR REG8(0x500A)
#define PC_DDR REG8(0x500C)
#define PC_CR1 REG8(0x500D)

static void d(u16 ms) { u16 i, j; for (i=0; i<ms; i++) for (j=0; j<1600; j++); }

void main(void) {
    CLK_CKDIVR = 0x00;
    PE_DDR |= (1<<7); PE_CR1 |= (1<<7);
    PC_DDR |= (1<<7); PC_CR1 |= (1<<7);
    while (1) { PE_ODR ^= (1<<7); PC_ODR ^= (1<<7); d(500); }
}
`,

    blink_fast: `/* LD3 (PE7) blink nhanh 120 ms */
typedef unsigned char u8;
typedef unsigned int  u16;
#define REG8(a) (*(volatile u8*)(a))
#define CLK_CKDIVR REG8(0x50C0)
#define PE_ODR REG8(0x5014)
#define PE_DDR REG8(0x5016)
#define PE_CR1 REG8(0x5017)

static void d(u16 ms) { u16 i, j; for (i=0; i<ms; i++) for (j=0; j<1600; j++); }

void main(void) {
    CLK_CKDIVR = 0x00;
    PE_DDR |= (1<<7); PE_CR1 |= (1<<7);
    while (1) { PE_ODR ^= (1<<7); d(120); }
}
`,

    button_toggle: `/* Bam B2 (PC1) de toggle LD4 (PC7) */
typedef unsigned char u8;
#define REG8(a) (*(volatile u8*)(a))
#define CLK_CKDIVR REG8(0x50C0)
#define PC_IDR REG8(0x500B)
#define PC_ODR REG8(0x500A)
#define PC_DDR REG8(0x500C)
#define PC_CR1 REG8(0x500D)

void main(void) {
    u8 last = 1;
    CLK_CKDIVR = 0x00;
    PC_DDR &= ~(1<<1); PC_CR1 |= (1<<1);
    PC_DDR |= (1<<7);  PC_CR1 |= (1<<7);
    while (1) {
        u8 cur = (PC_IDR >> 1) & 1;
        if (last == 1 && cur == 0) PC_ODR ^= (1<<7);
        last = cur;
    }
}
`,

    uart_echo: `/* UART1 echo tren PA2 (TX) / PA3 (RX), 9600 baud */
typedef unsigned char u8;
typedef unsigned int  u16;
#define REG8(a) (*(volatile u8*)(a))
#define CLK_CKDIVR    REG8(0x50C0)
#define CLK_PCKENR1   REG8(0x50C3)
#define SYSCFG_RMPCR1 REG8(0x509D)
#define PA_DDR REG8(0x5002)
#define PA_CR1 REG8(0x5003)
#define PA_CR2 REG8(0x5004)
#define USART1_SR   REG8(0x5230)
#define USART1_DR   REG8(0x5231)
#define USART1_BRR1 REG8(0x5232)
#define USART1_BRR2 REG8(0x5233)
#define USART1_CR2  REG8(0x5235)

void main(void) {
    CLK_CKDIVR = 0x00;
    CLK_PCKENR1 |= (1<<5);
    SYSCFG_RMPCR1 = (SYSCFG_RMPCR1 & 0xCF) | 0x10;
    PA_DDR |= (1<<2); PA_CR1 |= (1<<2); PA_CR2 |= (1<<2);
    PA_DDR &= ~(1<<3); PA_CR1 |= (1<<3);
    USART1_BRR2 = 0x02; USART1_BRR1 = 0x68;
    USART1_CR2 = 0x0C;
    while (1) {
        if (USART1_SR & 0x20) {
            u8 b = USART1_DR;
            while (!(USART1_SR & 0x80));
            USART1_DR = b;
        }
    }
}
`,

    lcd_hello: `/* Hien thi HELLO tren LCD glass STM8L-Discovery.
 * Server tu dong biet stm8_lcd.h -> compile kem lib/stm8_lcd.c */
#include "stm8_lcd.h"

void main(void) {
    lcd_init();
    lcd_puts("HELLO");
    while (1) { }
}
`,

    lcd_counter: `/* Dem 0 -> 999 tren LCD, moi 500 ms */
#include "stm8_lcd.h"

typedef unsigned char u8;
typedef unsigned int  u16;

static void delay_ms(u16 ms) {
    u16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 1600; j++);
}

static void utoa3(u16 n, char *b) {
    b[2] = '0' + (n % 10); n /= 10;
    b[1] = n ? '0' + (n % 10) : ' '; n /= 10;
    b[0] = n ? '0' + (n % 10) : ' ';
    b[3] = 0;
}

void main(void) {
    u16 n = 0;
    char buf[4];
    lcd_init();
    while (1) {
        utoa3(n, buf);
        lcd_puts(buf);
        delay_ms(500);
        n = (n + 1) % 1000;
    }
}
`,

    lcd_scroll: `/* Chay chu ngang tren LCD */
#include "stm8_lcd.h"

typedef unsigned char u8;
typedef unsigned int  u16;

static const char MSG[] = "      STM8L FOTA OK      ";

static void delay_ms(u16 ms) {
    u16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 1600; j++);
}

void main(void) {
    u16 i;
    char win[7];
    lcd_init();
    while (1) {
        for (i = 0; i + 6 <= (u16)(sizeof(MSG) - 1); i++) {
            u8 k;
            for (k = 0; k < 6; k++) win[k] = MSG[i + k];
            win[6] = 0;
            lcd_puts(win);
            delay_ms(250);
        }
    }
}
`,

    lcd_diag: `/* Diagnostic: bat toan bo segment LCD */
#include "stm8_lcd.h"

typedef unsigned char u8;
#define LCD_RAM(n) (*(volatile u8*)(0x540C + (n)))

void main(void) {
    u8 i;
    lcd_init();
    for (i = 0; i < 22; i++) LCD_RAM(i) = 0xFF;
    while (1) { }
}
`,
  };

  /* ============================================================
   *  Utilities
   * ============================================================ */
  const now = () => new Date().toLocaleTimeString('vi-VN', { hour12: false });

  function log(kind, text) {
    const div = document.createElement('div');
    div.className = kind || 'info';
    const t = document.createElement('time');
    t.textContent = '[' + now() + ']';
    div.appendChild(t);
    div.appendChild(document.createTextNode(' ' + text));
    logBox.appendChild(div);
    logBox.scrollTop = logBox.scrollHeight;
  }

  function setStatus(kind, msg) {
    dot.className = 'dot ' + kind;
    statusTx.textContent = msg;
  }

  function setProgress(pct, label) {
    pfill.style.width = Math.max(0, Math.min(100, pct)) + '%';
    ppct.textContent  = Math.round(pct) + '%';
    if (label && lastStat) lastStat.textContent = label;
  }

  function updateSrcInfo() {
    const t = src.value;
    const lines = t ? t.split('\n').length : 0;
    srcInfo.textContent = lines + ' dòng · ' + t.length + ' ký tự';
  }

  /* ============================================================
   *  Poll helper server
   * ============================================================ */
  let serverReady = false;
  function pollHelper() {
    fetch(HELPER + '/status')
      .then(r => r.json())
      .then(s => {
        if (s.sdcc_found) {
          serverReady = true;
          setStatus('ok', 'Server compile sẵn sàng');
          btnFlash.disabled = false;
        } else {
          serverReady = false;
          setStatus('warn', 'Server compile chạy nhưng thiếu SDCC');
        }
      })
      .catch(() => {
        serverReady = false;
        setStatus('err', 'Không thấy server compile — chạy python server.py');
      });
  }
  pollHelper();
  setInterval(pollHelper, 5000);

  /* ============================================================
   *  Editor wiring
   * ============================================================ */
  src.addEventListener('input', updateSrcInfo);
  updateSrcInfo();

  tpl.addEventListener('change', () => {
    const k = tpl.value;
    if (k && TEMPLATES[k]) {
      src.value = TEMPLATES[k];
      updateSrcInfo();
      log('info', 'Đã nạp mẫu: ' + k);
    }
    tpl.value = '';
  });

  fileInp.addEventListener('change', e => {
    const f = e.target.files[0];
    if (!f) return;
    const r = new FileReader();
    r.onload = () => {
      src.value = r.result;
      updateSrcInfo();
      log('info', 'Đã đọc: ' + f.name + ' (' + f.size + ' B)');
    };
    r.readAsText(f);
  });

  btnClear.addEventListener('click', () => {
    if (!src.value || confirm('Xoá toàn bộ code?')) {
      src.value = '';
      updateSrcInfo();
      logBox.innerHTML = '';
      result.className = 'result';
    }
  });

  /* ============================================================
   *  SSE from ESP32
   * ============================================================ */
  let evtSource = null;
  function openSSE() {
    if (evtSource) evtSource.close();
    evtSource = new EventSource('/events');
    evtSource.onmessage = e => {
      let d;
      try { d = JSON.parse(e.data); } catch { return; }
      if (d.msg) log(d.type || 'info', d.msg);
      if (typeof d.pct === 'number') setProgress(d.pct, d.label || '');
      if (d.done) {
        btnFlash.disabled = !serverReady;
        result.className = 'result ' + (d.success ? 'success' : 'error');
        result.textContent = d.success ? 'Nạp thành công' : 'Nạp thất bại';
      }
    };
    evtSource.onerror = () => { /* silent retry */ };
  }

  /* ============================================================
   *  Compile & Flash
   * ============================================================ */
  btnFlash.addEventListener('click', async () => {
    const code = src.value;
    if (!code.trim()) { log('err', 'Chưa có code C.'); return; }
    if (!serverReady) { log('err', 'Server compile chưa sẵn sàng.'); return; }

    btnFlash.disabled = true;
    result.className = 'result';
    result.textContent = '';
    logBox.innerHTML = '';
    setProgress(0, 'Bắt đầu');

    log('info', 'Gửi source (' + code.length + ' B) sang server biên dịch...');
    setProgress(15, 'SDCC compile');

    let bin;
    try {
      const r = await fetch(HELPER + '/compile-only', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain; charset=utf-8' },
        body: code
      });
      if (!r.ok) {
        let m = 'HTTP ' + r.status;
        try {
          const j = await r.json();
          m = j.error || m;
          if (j.stderr) log('err', j.stderr);
          if (j.stdout) log('dim', j.stdout);
        } catch { }
        log('err', 'Biên dịch thất bại: ' + m);
        btnFlash.disabled = false;
        return;
      }
      const w = r.headers.get('X-Sdcc-Warnings');
      if (w) log('warn', 'SDCC: ' + w);
      const buf = await r.arrayBuffer();
      bin = new Uint8Array(buf);
      log('ok', 'Biên dịch OK (' + bin.length + ' B)');
      setProgress(35, 'Đã có .bin');
    } catch (e) {
      log('err', 'Không kết nối được server compile: ' + e.message);
      btnFlash.disabled = false;
      return;
    }

    openSSE();
    log('info', 'Gửi .bin sang ESP32...');
    setProgress(45, 'Gửi ESP32');

    try {
      const fd = new FormData();
      fd.append('firmware', new Blob([bin]), 'app.bin');
      const r = await fetch('/upload', { method: 'POST', body: fd });
      if (!r.ok) {
        log('err', 'ESP32 từ chối: ' + r.status);
        btnFlash.disabled = false;
        return;
      }
      log('ok', 'ESP32 nhận file — theo dõi tiến trình bên dưới');
    } catch (e) {
      log('err', 'Lỗi upload: ' + e.message);
      btnFlash.disabled = false;
    }
  });

  /* Default template on first load */
  if (!src.value) {
    src.value = TEMPLATES.lcd_hello;
    updateSrcInfo();
  }
})();
