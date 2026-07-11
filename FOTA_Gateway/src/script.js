// ============================================================
//  MONACO EDITOR SETUP
// ============================================================
let editor;
let builtBin = null;

require.config({ paths: { vs: 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.44.0/min/vs' } });

require(['vs/editor/editor.main'], function() {
  // Define STM8 C color theme
  monaco.editor.defineTheme('stm8-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'comment', foreground: '6e7681' },
      { token: 'keyword', foreground: 'ff7b72' },
      { token: 'number', foreground: '79c0ff' },
      { token: 'string', foreground: 'a5d6ff' },
    ],
    colors: {
      'editor.background': '#0d1117',
      'editor.foreground': '#c9d1d9',
      'editor.lineHighlightBackground': '#161b22',
      'editorLineNumber.foreground': '#484f58',
      'editorLineNumber.activeForeground': '#8b949e',
      'editor.selectionBackground': '#264f78',
      'editorCursor.foreground': '#58a6ff',
    }
  });

  editor = monaco.editor.create(document.getElementById('editor'), {
    value: getDefaultCode(),
    language: 'c',
    theme: 'stm8-dark',
    fontSize: 13,
    fontFamily: "'Cascadia Code', 'Consolas', 'Courier New', monospace",
    fontLigatures: true,
    minimap: { enabled: false },
    scrollBeyondLastLine: false,
    lineNumbers: 'on',
    renderLineHighlight: 'line',
    automaticLayout: true,
    tabSize: 4,
    wordWrap: 'off',
    bracketPairColorization: { enabled: true },
    suggest: { showKeywords: true },
  });

  // Auto-resize
  window.addEventListener('resize', () => editor.layout());
});

// ============================================================
//  DEFAULT CODE
// ============================================================
function getDefaultCode() {
  return `/*
 * STM8L152C6T6 - Application Example
 * Nháy LED xanh (PE7) và xanh dương (PC7)
 * Build & Flash qua FOTA IDE
 */

typedef unsigned char  u8;
typedef unsigned int   u16;
#define REG(addr) (*(volatile u8*)(addr))

/* GPIO C - LD4 xanh duong = PC7 */
#define PC_ODR REG(0x500A)
#define PC_DDR REG(0x500C)
#define PC_CR1 REG(0x500D)

/* GPIO E - LD3 xanh la = PE7 */
#define PE_ODR REG(0x5014)
#define PE_DDR REG(0x5016)
#define PE_CR1 REG(0x5017)

/* IWDG - feed watchdog */
#define IWDG_KR REG(0x50E0)

static void delay(void) {
    volatile u16 i;
    for (i = 0; i < 30000; i++);
}

void main(void) {
    /* Tat nguon watchdog */
    IWDG_KR = 0xAA;

    /* PC7 = LD4 xanh duong output */
    PC_DDR |= (u8)(1 << 7);
    PC_CR1 |= (u8)(1 << 7);
    PC_ODR &= (u8)~(1 << 7);

    /* PE7 = LD3 xanh la output */
    PE_DDR |= (u8)(1 << 7);
    PE_CR1 |= (u8)(1 << 7);
    PE_ODR &= (u8)~(1 << 7);

    /* Nhay 3 lan nhanh: bao firmware moi dang chay */
    {
        u8 k;
        for (k = 0; k < 3; k++) {
            PC_ODR |= (u8)(1 << 7);
            PE_ODR |= (u8)(1 << 7);
            delay();
            PC_ODR &= (u8)~(1 << 7);
            PE_ODR &= (u8)~(1 << 7);
            delay();
        }
    }

    /* Nhay lien tuc xen ke */
    while (1) {
        IWDG_KR = 0xAA;          /* Feed watchdog */

        PC_ODR |=  (u8)(1 << 7); /* LD4 ON  */
        PE_ODR &= (u8)~(1 << 7); /* LD3 OFF */
        delay();

        IWDG_KR = 0xAA;

        PC_ODR &= (u8)~(1 << 7); /* LD4 OFF */
        PE_ODR |=  (u8)(1 << 7); /* LD3 ON  */
        delay();
    }
}
`;
}

// ============================================================
//  BUILD
// ============================================================
async function buildCode() {
  const code = editor.getValue();
  setBuildInfo('Đang build...', '#e3b341');
  setBtn('btn-build', true);
  clearBuildOutput();
  addBuildLog('info', '🔨 Đang biên dịch...');

  try {
    const res = await fetch('/api/build', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code })
    });
    const data = await res.json();

    if (data.ok) {
      builtBin = data.bin_base64;
      addBuildLog('ok', `✅ Build thành công! Size: ${data.size} bytes`);
      setBuildInfo(`✅ ${data.size} bytes`, '#3fb950');
      document.getElementById('btn-flash').disabled = false;
    } else {
      builtBin = null;
      addBuildLog('err', '❌ Build thất bại:');
      data.error.split('\n').forEach(line => {
        if (line.trim()) addBuildLog('err', '  ' + line);
      });
      setBuildInfo('❌ Build lỗi', '#f85149');
      document.getElementById('btn-flash').disabled = true;
    }
  } catch(e) {
    addBuildLog('err', '❌ Lỗi kết nối server: ' + e.message);
    setBuildInfo('❌ Lỗi', '#f85149');
  }

  setBtn('btn-build', false);
}

// ============================================================
//  FLASH
// ============================================================
async function flashOnly() {
  if (!builtBin) { addFlashLog('err', '❌ Chưa có firmware. Bấm Build trước!'); return; }
  await doFlash(builtBin);
}

async function buildAndFlash() {
  await buildCode();
  if (!builtBin) return;
  await doFlash(builtBin);
}

async function doFlash(binBase64) {
  const filename = document.getElementById('filename').value || 'app_fota.bin';
  setBtn('btn-flash', true);
  setBtn('btn-both', true);
  clearFlashLog();
  setProgress(0, 'Đang kết nối ESP32...');
  addFlashLog('info', `⚡ Gửi ${filename} lên ESP32...`);

  // Ket noi SSE de nhan log
  const evtSource = new EventSource('/api/esp32_log');
  evtSource.onmessage = (e) => {
    try {
      const d = JSON.parse(e.data);
      if (d.msg) {
        const type = d.type || 'info';
        addFlashLog(type, d.msg);
      }
      if (d.pct !== undefined) setProgress(d.pct, d.label || '');
      if (d.done) {
        evtSource.close();
        setBtn('btn-flash', false);
        setBtn('btn-both', false);
        if (d.success) {
          addFlashLog('ok', '🎉 Nạp firmware thành công!');
          setProgress(100, 'Hoàn thành!');
        } else {
          addFlashLog('err', '❌ Nạp firmware thất bại');
          setProgress(0, 'Thất bại');
        }
      }
    } catch(e2) {}
  };
  evtSource.onerror = () => evtSource.close();

  try {
    const res = await fetch('/api/flash', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ bin_base64: binBase64, filename })
    });
    const data = await res.json();
    if (!data.ok) {
      evtSource.close();
      addFlashLog('err', '❌ ' + data.error);
      setBtn('btn-flash', false);
      setBtn('btn-both', false);
      setProgress(0, 'Lỗi');
    }
  } catch(e) {
    evtSource.close();
    addFlashLog('err', '❌ Lỗi: ' + e.message);
    setBtn('btn-flash', false);
    setBtn('btn-both', false);
  }
}

// ============================================================
//  HELPERS
// ============================================================
function addBuildLog(type, msg) {
  const el = document.getElementById('build-output');
  const div = document.createElement('div');
  div.className = 'log-' + type;
  div.textContent = msg;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

function clearBuildOutput() {
  document.getElementById('build-output').innerHTML = '';
}

function addFlashLog(type, msg) {
  const el = document.getElementById('flash-log');
  const div = document.createElement('div');
  div.className = 'log-' + type;
  const t = new Date().toLocaleTimeString('vi-VN', { hour12: false });
  div.textContent = `[${t}] ${msg}`;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

function clearFlashLog() {
  document.getElementById('flash-log').innerHTML = '';
}

function clearAll() {
  clearBuildOutput();
  clearFlashLog();
  builtBin = null;
  setProgress(0, 'Sẵn sàng');
  setBuildInfo('Chưa build', '#8b949e');
  document.getElementById('btn-flash').disabled = true;
}

function setProgress(pct, label) {
  document.getElementById('prog-fill').style.width = pct + '%';
  document.getElementById('prog-pct').textContent = Math.round(pct) + '%';
  document.getElementById('prog-label').textContent = label || '';
}

function setBuildInfo(text, color) {
  const el = document.getElementById('build-info');
  el.innerHTML = `<span style="color:${color}">${text}</span>`;
}

function setBtn(id, disabled) {
  document.getElementById(id).disabled = disabled;
}

// ============================================================
//  RESIZE HANDLE
// ============================================================
const handle = document.getElementById('resize-handle');
const rightPanel = document.getElementById('right-panel');
let isResizing = false;

handle.addEventListener('mousedown', (e) => {
  isResizing = true;
  document.body.style.cursor = 'col-resize';
  document.body.style.userSelect = 'none';
});

document.addEventListener('mousemove', (e) => {
  if (!isResizing) return;
  const container = document.querySelector('.main');
  const rect = container.getBoundingClientRect();
  const newWidth = rect.right - e.clientX;
  if (newWidth > 250 && newWidth < 700) {
    rightPanel.style.width = newWidth + 'px';
    editor && editor.layout();
  }
});

document.addEventListener('mouseup', () => {
  isResizing = false;
  document.body.style.cursor = '';
  document.body.style.userSelect = '';
});

// ============================================================
//  CHECK ESP32 CONNECTION
// ============================================================
async function checkESP32() {
  try {
    const res = await fetch('/api/esp32_check', { signal: AbortSignal.timeout(2000) });
    document.getElementById('esp32-dot').className = 'status-dot ok';
    document.getElementById('esp32-status').textContent = 'ESP32 đã kết nối';
  } catch {
    document.getElementById('esp32-dot').className = 'status-dot';
    document.getElementById('esp32-status').textContent = 'ESP32 chưa kết nối';
  }
}

setInterval(checkESP32, 5000);
checkESP32();
