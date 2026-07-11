
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// ============================================================
//  CẤU HÌNH
// ============================================================
#define WIFI_SSID        "FOTA-Gateway"
#define WIFI_PASSWORD    "12345678"   // TODO: đổi mật khẩu trước khi public repo

#define STM8_RESET_PIN   4
#define STM8_TX_PIN      17   // GPIO17 → PA3 STM8 RX
#define STM8_RX_PIN      16   // GPIO16 ← PA2 STM8 TX
#define STM8_BAUDRATE    4800  // Khớp với bit-bang 2MHz

#define FRAME_DATA_SIZE  16
#define MAX_RETRY        3
#define ACK_TIMEOUT_MS   5000
#define STM8_APP_START   0x8400

#define BYTE_HANDSHAKE   0x7F
#define BYTE_NACK        0x1F
#define BYTE_END         0xFF

// Các trường hợp ACK được chấp nhận do lệch bit-bang timing
#define BYTE_ACK         0x79
#define BYTE_ACK2        0xCF
#define BYTE_ACK3        0xFD

#define SIM_MODE         false

// ============================================================
//  BIẾN TOÀN CỤC
// ============================================================
AsyncWebServer server(80);
AsyncEventSource events("/events");
volatile bool ota_in_progress = false;
uint8_t* firmware_buf  = nullptr;
size_t   firmware_size = 0;

// ============================================================
//  CRC16-CCITT
// ============================================================
uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i] << 8);
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

// ============================================================
//  KIỂM TRA ACK
// ============================================================
bool is_ack(uint8_t b) {
  return (b == BYTE_ACK || b == BYTE_ACK2 || b == BYTE_ACK3);
}

// ============================================================
//  GỬI LOG ĐỒNG THỜI QUA WEB EVENT & SERIAL MONITOR
// ============================================================
void send_log(const char* type, String msg, int pct = -1,
              const char* label = "", bool done = false, bool success = false) {
  String json = "{\"type\":\"" + String(type) + "\",\"msg\":\"" + msg + "\"";
  if (pct >= 0) json += ",\"pct\":" + String(pct) + ",\"label\":\"" + label + "\"";
  if (done) json += ",\"done\":true,\"success\":" + String(success ? "true" : "false");
  json += "}";
  events.send(json.c_str(), "message", millis());

  // In ra Serial Monitor định dạng dễ nhìn
  Serial.printf("[LOG_WEB] [%s] %s\n", type, msg.c_str());
}

// ============================================================
//  CHỜ PHẢN HỒI ACK TỪ STM8
// ============================================================
bool wait_ack(int timeout_ms = ACK_TIMEOUT_MS) {
  unsigned long t = millis();
  while (millis() - t < (unsigned long)timeout_ms) {
    if (Serial2.available()) {
      uint8_t b = Serial2.read();
      Serial.printf("[UART] Nhan tu STM8: 0x%02X\n", b);
      if (is_ack(b))       return true;
      if (b == BYTE_NACK)  return false;
    }
    delay(1);
  }
  Serial.println("[UART] Timeout - Khong nhan duoc ACK tu STM8!");
  return false;
}

// ============================================================
//  THỰC HIỆN HANDSHAKE (RESET & BOOTLOADER)
// ============================================================
bool do_handshake() {
  send_log("info", "🔄 Dang Reset cung STM8...");

  if (SIM_MODE) {
    delay(300);
    send_log("ok", "✅ [SIM] Handshake OK");
    return true;
  }

  // Clear bộ đệm RX cũ
  while (Serial2.available()) Serial2.read();

  // Kéo chân Reset xuống Low rồi lên High
  digitalWrite(STM8_RESET_PIN, LOW);
  delay(200);
  digitalWrite(STM8_RESET_PIN, HIGH);
  Serial.println("[RESET] Da kich hoat reset STM8");

  send_log("info", "⏳ Cho Bootloader khoi dong (2 giay)...");
  delay(2000);

  send_log("info", "🤝 Bat dau goi tin hieu Handshake (0x7F)...");

  for (int i = 0; i < 15; i++) {
    while (Serial2.available()) Serial2.read();

    Serial.printf("[HS] Lan gui thu %d: 0x7F\n", i + 1);
    Serial2.write((uint8_t)BYTE_HANDSHAKE);
    Serial2.flush();

    unsigned long t = millis();
    while (millis() - t < 600) {
      if (Serial2.available()) {
        uint8_t b = Serial2.read();
        Serial.printf("[HS] Nhan phan hoi: 0x%02X\n", b);
        if (is_ack(b)) {
          char hex_buf[16];
          snprintf(hex_buf, sizeof(hex_buf), "0x%02X", b);
          send_log("ok", String("✅ Handshake thanh cong! STM8 phan hoi: ") + hex_buf);
          return true;
        }
      }
      delay(1);
    }
    delay(50);
  }

  send_log("err", "❌ Handshake that bai sau 15 lan thu!");
  return false;
}

// ============================================================
//  GỬI KHUNG DỮ LIỆU (FRAME) KHỚP BAUD 4800
// ============================================================
bool send_frame(uint32_t addr, const uint8_t* data, uint8_t len) {
  uint8_t frame[22];
  uint8_t n = 0;
  frame[n++] = (addr >> 16) & 0xFF;
  frame[n++] = (addr >>  8) & 0xFF;
  frame[n++] = (addr      ) & 0xFF;
  frame[n++] = len;
  for (uint8_t i = 0; i < len; i++) frame[n++] = data[i];

  uint16_t crc = crc16_ccitt(frame, n);
  frame[n++] = (crc >> 8) & 0xFF;
  frame[n++] =  crc       & 0xFF;

  Serial.printf("[UART] Dang gui Frame -> Dia chi: 0x%06lX | Do dai: %d | CRC16: 0x%04X\n",
                (unsigned long)addr, len, crc);

  if (SIM_MODE) { delay(20); return true; }

  Serial2.write(frame, n);
  Serial2.flush();
  return wait_ack();
}

// ============================================================
//  TIẾN TRÌNH NẠP FIRMWARE (FreeRTOS Task)
// ============================================================
void flash_task(void* param) {
  char buf[100];

  send_log("info", "🔍 Kiem tra tinh toan ven CRC cua File...", 5, "CRC");
  delay(200);
  uint16_t fw_crc = crc16_ccitt(firmware_buf, firmware_size);
  snprintf(buf, sizeof(buf), "📊 CRC16 File: 0x%04X — Hop le (%u bytes)", fw_crc, firmware_size);
  send_log("ok", buf, 10, "CRC OK");
  delay(200);

  send_log("info", "🔌 Thuc hien dong bo voi STM8...", 12, "Handshake");
  if (!do_handshake()) {
    send_log("err", "❌ Khong the ket noi Bootloader STM8!", -1, "", true, false);
    goto done;
  }
  delay(200);

  {
    size_t total = (firmware_size + FRAME_DATA_SIZE - 1) / FRAME_DATA_SIZE;
    size_t sent  = 0;
    uint32_t addr = STM8_APP_START;

    snprintf(buf, sizeof(buf), "📤 Bat dau truyen: %u frames kieu du lieu...", total);
    send_log("info", buf, 15, "Đang nạp...");

    for (size_t off = 0; off < firmware_size; off += FRAME_DATA_SIZE) {
      uint8_t chunk = (uint8_t)min((size_t)FRAME_DATA_SIZE, firmware_size - off);
      bool ok = false;

      for (int retry = 0; retry < MAX_RETRY; retry++) {
        if (send_frame(addr, firmware_buf + off, chunk)) { ok = true; break; }
        snprintf(buf, sizeof(buf), " Loi Frame %u — Thu lai %d/%d", sent + 1, retry + 1, MAX_RETRY);
        send_log("warn", buf);
        delay(200);
      }

      if (!ok) {
        snprintf(buf, sizeof(buf), "❌ Frame %u gui that bai vinh vien!", sent + 1);
        send_log("err", buf, -1, "", true, false);
        goto done;
      }

      sent++;
      addr += chunk;
      int pct = 15 + (int)((float)sent / total * 75.0f);
      snprintf(buf, sizeof(buf), "✔ Da nap Frame %u/%u tai address: 0x%06lX", sent, total, (unsigned long)(addr - chunk));
      send_log("ok", buf, pct, "Đang nạp...");
      delay(10);
    }
  }

  send_log("info", "🏁 Hoan tat truyen file, gui byte ket thuc...", 92, "Hoàn thiện");
  delay(200);

  {
    Serial2.write((uint8_t)BYTE_END);
    Serial2.flush();
    if (wait_ack(5000)) {
      send_log("ok", " STM8 xac nhan hoan tat — Firmware moi dang chay!", 97, "Done");
      delay(500);
      send_log("ok", " CHÚC MỪNG: Nap firmware vao STM8 thanh cong!", 100, "Xong!", true, true);
    } else {
      send_log("err", " Gửi lệnh END nhưng STM8 khong phan hoi.", -1, "", true, false);
    }
  }

done:
  free(firmware_buf);
  firmware_buf  = nullptr;
  firmware_size = 0;
  ota_in_progress = false;
  vTaskDelete(NULL);
}

// ============================================================
//  CẤU HÌNH WEB SERVER
// ============================================================
void setup_server() {
  // --- Server-Sent Events: đẩy log realtime lên web ---
  events.onConnect([](AsyncEventSourceClient* client) {
    client->send("connected", "message", millis());
  });
  server.addHandler(&events);

  // --- API: trạng thái hệ thống ---
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"heap\":" + String(ESP.getFreeHeap()) +
                  ",\"ota\":" + String(ota_in_progress ? "true" : "false") + "}";
    req->send(200, "application/json", json);
  });

  // --- API: test handshake ---
  server.on("/test_handshake", HTTP_GET, [](AsyncWebServerRequest* req) {
    // Tạm thời kích hoạt cờ ota để loop() không cướp UART trong khi test
    ota_in_progress = true;

    while (Serial2.available()) Serial2.read();
    digitalWrite(STM8_RESET_PIN, LOW);
    delay(200);
    digitalWrite(STM8_RESET_PIN, HIGH);
    delay(2000);

    bool ok = false;
    uint8_t resp_byte = 0;
    String msg = "timeout";

    for (int i = 0; i < 15 && !ok; i++) {
      while (Serial2.available()) Serial2.read();
      Serial2.write((uint8_t)BYTE_HANDSHAKE);
      Serial2.flush();
      unsigned long t = millis();
      while (millis() - t < 600) {
        if (Serial2.available()) {
          uint8_t b = Serial2.read();
          Serial.printf("[TEST_HS] Nhận byte phản hồi: 0x%02X\n", b);
          if (is_ack(b)) {
            ok = true;
            resp_byte = b;
            break;
          }
          msg = "Nhan byte la: 0x" + String(b, HEX);
        }
        vTaskDelay(1);
      }
      delay(50);
    }

    ota_in_progress = false; // Trả lại quyền cho loop()

    String json = "{\"ok\":" + String(ok ? "true" : "false") +
                  ",\"resp\":" + String(resp_byte) +
                  ",\"msg\":\"" + msg + "\"}";
    req->send(200, "application/json", json);
  });

  // --- API: upload firmware .bin ---
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) { req->send(200, "text/plain", "OK"); },
    [](AsyncWebServerRequest* req, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      if (ota_in_progress) { req->send(503, "text/plain", "Busy"); return; }
      if (index == 0) {
        size_t total = req->contentLength();
        if (total > 28672) { req->send(400, "text/plain", "File qua lon"); return; }
        firmware_buf = (uint8_t*)malloc(total);
        if (!firmware_buf) { req->send(500, "text/plain", "Het RAM"); return; }
        firmware_size = 0;
        Serial.printf("[UPLOAD] Dang nhan file: %s (%u bytes)\n", filename.c_str(), total);
      }
      if (firmware_buf) {
        memcpy(firmware_buf + index, data, len);
        firmware_size = index + len;
      }
      if (final && firmware_buf) {
        ota_in_progress = true; // Kích hoạt cờ chặn loop() ngay lập tức
        xTaskCreatePinnedToCore(flash_task, "ota", 8192, NULL, 1, NULL, 0);
      }
    }
  );

  // --- Giao diện web tĩnh từ LittleFS 

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
}

// ============================================================
//  HÀM SETUP BAN ĐẦU
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=============================================");
  Serial.println("         ESP32 FOTA Gateway v3.2             ");
  Serial.println("=============================================");

  // Mount LittleFS để phục vụ giao diện web
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount THAT BAI! (chua nap thu muc data?)");
  } else {
    Serial.println("[FS] LittleFS OK");
  }

  pinMode(STM8_RESET_PIN, OUTPUT);
  digitalWrite(STM8_RESET_PIN, HIGH);

  Serial2.begin(STM8_BAUDRATE, SERIAL_8N1, STM8_RX_PIN, STM8_TX_PIN);
  Serial.printf("[UART2] Khoi dong cấu hình: TX=GPIO%d -> PA3 | RX=GPIO%d <- PA2 | Baudrate: %d\n",
                STM8_TX_PIN, STM8_RX_PIN, STM8_BAUDRATE);

  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Da bat Access Point: '%s' | IP Gateway: %s\n", WIFI_SSID,
                WiFi.softAPIP().toString().c_str());

  setup_server();

  Serial.println("[BOOT] He thong SAN SANG! -> Dia chi truy cap: http://192.168.4.1");
  Serial.println("[BOOT] Danh sach bo loc ACK: [0x79, 0xCF, 0xFD]");
}

// ============================================================
//  HÀM LOOP CHÍNH
// ============================================================
void loop() {
  // CHỈ đọc UART debug ở loop() nếu quá trình OTA KHÔNG diễn ra
  if (!ota_in_progress && Serial2.available()) {
    uint8_t b = Serial2.read();
    Serial.printf("[STM8_DEBUG] Du lieu bat ngo tu STM8: 0x%02X\n", b);
  }

  static unsigned long t = 0;
  if (millis() - t > 15000) {
    t = millis();
    Serial.printf("[STATUS] Free Heap: %u bytes | Trang thai FOTA: %s | Client Connected: %d\n",
                  ESP.getFreeHeap(),
                  ota_in_progress ? "DANG HOAT DONG" : "Ranh roi",
                  WiFi.softAPgetStationNum());
  }
  delay(1);
}
