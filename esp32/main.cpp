/* ============================================================================
 *  ESP32 FOTA Gateway  —  v5.0  "In-page compile"
 *  Matches bootloader.c on STM8L152C6T6 (hardware USART1 on PA2/PA3).
 *
 *  What's new in v5:
 *      The web page served at http://192.168.4.1 now contains a full C code
 *      editor with template picker.  When the user clicks "Compile & Flash":
 *          browser  --POST source-->  http://localhost:8080/compile-only
 *                              (Python helper on user's PC runs SDCC)
 *                    <-- .bin bytes --
 *          browser  --POST /upload multipart-->  http://192.168.4.1  (this)
 *          browser  <-- SSE /events --  progress
 *      The user still needs the Python helper running on their PC because the
 *      ESP32 cannot compile C.  But they only ever open ONE URL.
 *
 *  Fallback:  the page still accepts an already-compiled .bin file directly.
 *
 *  Wiring / hardware (unchanged from v4):
 *      ESP32 GPIO17 (TX2)  ─→  PA3   (P1 pin 6)   STM8 USART1_RX
 *      ESP32 GPIO16 (RX2)  ←─  PA2   (P1 pin 5)   STM8 USART1_TX
 *      ESP32 GPIO4         ─→  NRST  (P1 pin 4)
 *      ESP32 GND           ──  GND   (P1 pin 3)
 *      JP1 removed, LCD may remain plugged in.
 *
 *  Protocol expected by bootloader.c (unchanged):
 *      ESP32 →  0x7F  → STM8 →  0x79
 *      ESP32 →  frames [ADDR_HI][ADDR_MD][ADDR_LO][LEN][DATA*LEN][CRC_HI][CRC_LO]
 *      ESP32 →  0xFF  → STM8 →  0x79   → jump to 0x8400
 * ============================================================================ */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>

/* ---------------- configuration ---------------- */
#define WIFI_SSID        "FOTA-Gateway"
#define WIFI_PASSWORD    "12345678"

#define STM8_RESET_PIN   4
#define STM8_TX_PIN      17    // ESP32 TX2 -> STM8 RX (PA3)
#define STM8_RX_PIN      16    // ESP32 RX2 <- STM8 TX (PA2)
#define STM8_BAUDRATE    9600

#define FRAME_DATA_SIZE  16
#define MAX_RETRY        3
#define ACK_TIMEOUT_MS   1500
#define READY_TIMEOUT_MS 4000

#define STM8_APP_START   0x8400UL
#define APP_MAX_SIZE     (16 * 1024)  /* STM8L152C6T6 has 32 KB flash; ample */

#define BYTE_TRIG        0x7F
#define BYTE_ACK         0x79
#define BYTE_NACK        0x1F
#define BYTE_EOF         0xFF

/* ---------------- state ---------------- */
AsyncWebServer    server(80);
AsyncEventSource  events("/events");
volatile bool     ota_in_progress = false;
uint8_t*          firmware_buf  = nullptr;
size_t            firmware_size = 0;

/* ============================================================================
 *  Web UI  (served from SPIFFS)
 *  Files: /index.html, /style.css, /script.js
 * ============================================================================ */

/* ============================================================================
 *  CRC-16 CCITT — must match STM8 bootloader exactly
 * ============================================================================ */

/* ============================================================================
 *  CRC-16 CCITT — must match STM8 bootloader exactly
 * ============================================================================ */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ---------------- SSE log helper ---------------- */
static void send_log(const char *type, const String &msg,
                     int pct = -1, const char *label = "",
                     bool done = false, bool success = false)
{
    String j = "{\"type\":\"" + String(type) + "\",\"msg\":\"" + msg + "\"";
    if (pct >= 0) j += ",\"pct\":" + String(pct) + ",\"label\":\"" + label + "\"";
    if (done)    j += ",\"done\":true,\"success\":" + String(success ? "true" : "false");
    j += "}";
    events.send(j.c_str(), "message", millis());
    Serial.println("[LOG] " + msg);
}

/* ---------------- low-level UART ---------------- */
static void uart_flush_input(void)
{
    while (Serial2.available()) Serial2.read();
}

static int uart_read_timeout(uint32_t ms)
{
    uint32_t t = millis();
    while (millis() - t < ms) {
        if (Serial2.available()) return Serial2.read();
        vTaskDelay(1);
    }
    return -1;
}

/* ---------------- pulse NRST low ---------------- */
static void pulse_reset(void)
{
    digitalWrite(STM8_RESET_PIN, LOW);
    delay(80);
    uart_flush_input();
    digitalWrite(STM8_RESET_PIN, HIGH);
}

/* ============================================================================
 *  Handshake: reset STM8, then send 0x7F, expect 0x79 within READY_TIMEOUT_MS.
 *  Returns the byte received on success, or -1 on failure.
 * ============================================================================ */
static int do_handshake(bool log_to_web)
{
    pulse_reset();

    /* Bootloader does ~1.2 s of 5-blink before listening; wait for it. */
    if (log_to_web) send_log("info", "Chờ bootloader khởi động (5 nhịp LED)...");
    delay(1400);

    uart_flush_input();

    for (int attempt = 1; attempt <= 8; attempt++) {
        Serial2.write((uint8_t)BYTE_TRIG);
        Serial2.flush();
        int b = uart_read_timeout(400);
        Serial.printf("[HS] try %d  -> 0x%02X\n", attempt, b);
        if (b == BYTE_ACK) {
            if (log_to_web) send_log("ok",
                String("Bootloader sẵn sàng (nhận 0x79 sau ") + attempt + " lần)");
            return b;
        }
    }
    if (log_to_web) send_log("err", "STM8 không trả 0x79");
    return -1;
}

/* ============================================================================
 *  Wait for ACK/NACK from STM8 after sending a frame.
 *  Returns: +1 ACK, 0 NACK, -1 timeout/other
 * ============================================================================ */
static int wait_frame_response(uint32_t ms)
{
    int b = uart_read_timeout(ms);
    if (b == BYTE_ACK)  return  1;
    if (b == BYTE_NACK) return  0;
    if (b < 0)          return -1;
    /* anything else — treat as protocol error */
    return -1;
}

/* ============================================================================
 *  Send one frame and wait for response.
 * ============================================================================ */
static bool send_frame(uint32_t flash_addr,
                       const uint8_t *data, uint8_t data_len)
{
    uint8_t frame[4 + FRAME_DATA_SIZE + 2];
    uint8_t n = 0;

    frame[n++] = (uint8_t)((flash_addr >> 16) & 0xFF);
    frame[n++] = (uint8_t)((flash_addr >>  8) & 0xFF);
    frame[n++] = (uint8_t)( flash_addr        & 0xFF);
    frame[n++] = data_len;
    for (uint8_t i = 0; i < data_len; i++) frame[n++] = data[i];

    uint16_t crc = crc16_ccitt(frame, n);
    frame[n++] = (uint8_t)(crc >> 8);
    frame[n++] = (uint8_t)(crc & 0xFF);

    uart_flush_input();
    Serial2.write(frame, n);
    Serial2.flush();

    int r = wait_frame_response(ACK_TIMEOUT_MS);
    return (r == 1);
}

/* ============================================================================
 *  OTA worker task — runs on Core 0 so the web server (Core 1) stays responsive.
 * ============================================================================ */
static void ota_task(void *param)
{
    char buf[96];

    send_log("info", "Đang tính CRC firmware...", 3, "Kiểm tra");
    uint16_t fw_crc = crc16_ccitt(firmware_buf, firmware_size);
    snprintf(buf, sizeof(buf), "Firmware CRC = 0x%04X (%u bytes)", fw_crc, (unsigned)firmware_size);
    send_log("ok", buf, 6, "CRC OK");

    send_log("info", "Bắt đầu handshake với STM8...", 8, "Handshake");
    if (do_handshake(true) < 0) {
        send_log("err", "Không kết nối được STM8. Đã hủy.", -1, "", true, false);
        goto cleanup;
    }

    {
        const size_t total_frames =
            (firmware_size + FRAME_DATA_SIZE - 1) / FRAME_DATA_SIZE;
        size_t sent = 0;
        uint32_t addr = STM8_APP_START;

        snprintf(buf, sizeof(buf),
                 "Bắt đầu truyền %u frames × %u bytes...",
                 (unsigned)total_frames, FRAME_DATA_SIZE);
        send_log("info", buf, 14, "Truyền dữ liệu");

        for (size_t off = 0; off < firmware_size; off += FRAME_DATA_SIZE) {
            uint8_t chunk =
                (uint8_t)min((size_t)FRAME_DATA_SIZE, firmware_size - off);

            bool ok = false;
            for (int retry = 0; retry < MAX_RETRY; retry++) {
                if (send_frame(addr, firmware_buf + off, chunk)) {
                    ok = true;
                    break;
                }
                snprintf(buf, sizeof(buf),
                         "NACK/timeout frame %u — retry %d/%d",
                         (unsigned)(sent + 1), retry + 1, MAX_RETRY);
                send_log("warn", buf);
                delay(80);
            }
            if (!ok) {
                snprintf(buf, sizeof(buf),
                         "Frame %u thất bại sau %d lần thử — hủy.",
                         (unsigned)(sent + 1), MAX_RETRY);
                send_log("err", buf, -1, "", true, false);
                goto cleanup;
            }

            sent++;
            addr += chunk;

            int pct = 14 + (int)((float)sent / total_frames * 78.0f);
            snprintf(buf, sizeof(buf),
                     "✔ Frame %u/%u @ 0x%06lX (%u B)",
                     (unsigned)sent, (unsigned)total_frames,
                     (unsigned long)(addr - chunk), chunk);
            send_log("ok", buf, pct, "Đang nạp");
        }
    }

    /* End-of-firmware marker */
    send_log("info", "Gửi EOF (0xFF)...", 94, "Hoàn thiện");
    uart_flush_input();
    Serial2.write((uint8_t)BYTE_EOF);
    Serial2.flush();

    {
        int r = uart_read_timeout(2000);
        if (r != BYTE_ACK) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp),
                     "Bootloader không xác nhận EOF (nhận 0x%02X)", r);
            send_log("warn", tmp);
            /* Continue anyway — chip will reboot on watchdog if stuck */
        } else {
            send_log("ok", "Bootloader xác nhận EOF", 97, "Reset & boot app");
        }
    }

    /* The bootloader now JPFs to 0x8400 — give it a moment, then declare success */
    delay(300);
    send_log("ok", "STM8 đang chạy firmware mới", 100, "Xong", true, true);

cleanup:
    if (firmware_buf) {
        free(firmware_buf);
        firmware_buf = nullptr;
    }
    firmware_size   = 0;
    ota_in_progress = false;
    vTaskDelete(NULL);
}

/* ============================================================================
 *  HTTP handlers
 * ============================================================================ */
static void handle_handshake(AsyncWebServerRequest *req)
{
    /* Runs in HTTP context — must NOT block long. We use a short version. */
    pulse_reset();
    delay(1400);
    uart_flush_input();

    int last = -1;
    for (int i = 0; i < 6; i++) {
        Serial2.write((uint8_t)BYTE_TRIG);
        Serial2.flush();
        int b = uart_read_timeout(400);
        if (b >= 0) last = b;
        if (b == BYTE_ACK) {
            String j = "{\"ok\":true,\"byte\":" + String(b) + "}";
            req->send(200, "application/json", j);
            return;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    String j = "{\"ok\":false,\"byte\":" + String(last) + "}";
    req->send(200, "application/json", j);
}

static void handle_upload(AsyncWebServerRequest *req,
                          const String &filename,
                          size_t index, uint8_t *data, size_t len, bool final)
{
    if (ota_in_progress) {
        req->send(503, "text/plain", "Busy");
        return;
    }
    if (index == 0) {
        size_t total = req->contentLength();
        Serial.printf("[UP] %s  %u B\n", filename.c_str(), (unsigned)total);
        if (total == 0 || total > APP_MAX_SIZE) {
            req->send(400, "text/plain", "Size invalid");
            return;
        }
        firmware_buf = (uint8_t *)malloc(total);
        if (!firmware_buf) {
            req->send(500, "text/plain", "OOM");
            return;
        }
        firmware_size = 0;
    }
    if (firmware_buf) {
        memcpy(firmware_buf + index, data, len);
        firmware_size = index + len;
    }
    if (final && firmware_buf) {
        Serial.printf("[UP] done %u B\n", (unsigned)firmware_size);
        ota_in_progress = true;
        xTaskCreatePinnedToCore(ota_task, "ota", 8192, NULL, 1, NULL, 0);
    }
}

/* ============================================================================
 *  setup() / loop()
 * ============================================================================ */
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n========== ESP32 FOTA Gateway v5.0 ==========");
    Serial.println("Target: STM8L152C6T6  USART1 remap PA2/PA3");
    Serial.printf("UART2  TX=GPIO%d  RX=GPIO%d  @ %d baud\n",
                  STM8_TX_PIN, STM8_RX_PIN, STM8_BAUDRATE);

    pinMode(STM8_RESET_PIN, OUTPUT);
    digitalWrite(STM8_RESET_PIN, HIGH);

    Serial2.begin(STM8_BAUDRATE, SERIAL_8N1, STM8_RX_PIN, STM8_TX_PIN);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS init failed!");
    } else {
        Serial.println("SPIFFS initialized");
    }

    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("WiFi AP '%s' IP=%s\n",
                  WIFI_SSID, WiFi.softAPIP().toString().c_str());

    // Serve web files from SPIFFS
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (SPIFFS.exists("/index.html")) {
            r->send(SPIFFS, "/index.html", "text/html");
        } else {
            r->send(404, "text/plain", "index.html not found in SPIFFS");
        }
    });
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (SPIFFS.exists("/style.css")) {
            r->send(SPIFFS, "/style.css", "text/css");
        } else {
            r->send(404, "text/plain", "style.css not found");
        }
    });
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (SPIFFS.exists("/script.js")) {
            r->send(SPIFFS, "/script.js", "application/javascript");
        } else {
            r->send(404, "text/plain", "script.js not found");
        }
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
        String status = "SPIFFS files:\n";
        status += "index.html: " + String(SPIFFS.exists("/index.html") ? "OK" : "MISSING") + "\n";
        status += "style.css: " + String(SPIFFS.exists("/style.css") ? "OK" : "MISSING") + "\n";
        status += "script.js: " + String(SPIFFS.exists("/script.js") ? "OK" : "MISSING") + "\n";
        r->send(200, "text/plain", status);
    });

    events.onConnect([](AsyncEventSourceClient *c) {
        c->send("connected", "message", millis());
    });
    server.addHandler(&events);
    server.on("/handshake", HTTP_GET, handle_handshake);
    server.on("/upload", HTTP_POST,
              [](AsyncWebServerRequest *r) { r->send(200, "text/plain", "OK"); },
              handle_upload);
    server.begin();

    Serial.println("Ready. Open http://192.168.4.1");
}

void loop()
{
    /* Passive logging — useful for debugging */
    if (!ota_in_progress && Serial2.available()) {
        uint8_t b = Serial2.read();
        Serial.printf("[STM8->ESP32] 0x%02X\n", b);
    }

    static uint32_t t = 0;
    if (millis() - t > 20000) {
        t = millis();
        Serial.printf("[STATUS] heap=%u  ota=%s  clients=%d\n",
                      ESP.getFreeHeap(),
                      ota_in_progress ? "yes" : "no",
                      WiFi.softAPgetStationNum());
    }
    delay(2);
}
