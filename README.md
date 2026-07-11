# 🔌 FOTA System for STM8 — Cập nhật firmware qua WiFi (ESP32 Gateway)

Hệ thống **cập nhật firmware từ xa qua WiFi (FOTA – Firmware Over-The-Air)** cho vi điều khiển **STM8L152C6T6**, dùng **ESP32 làm cổng trung gian (gateway)** kèm **giao diện web kéo-thả** để nạp firmware — **không cần mạch nạp ST-Link hay tháo thiết bị**.

![Platform](https://img.shields.io/badge/MCU-STM8L152C6T6-blue)
![Gateway](https://img.shields.io/badge/Gateway-ESP32-informational)
![Language](https://img.shields.io/badge/Code-C%20%2F%20C%2B%2B-orange)
![License](https://img.shields.io/badge/License-MIT-green)

> 🎓 Đề tài nghiên cứu khoa học · Nhóm 3 người · **Vai trò của tôi: lập trình bootloader trên STM8 (bare-metal).**

---

## 📸 Demo

> _Thêm ảnh/GIF của bạn vào đây để repo ấn tượng hơn (rất khuyến khích với dự án phần cứng):_
> - Ảnh giao diện web nạp firmware
> - GIF/video quá trình nạp thành công (LED STM8 nháy theo firmware mới)
> - Ảnh đấu nối thực tế ESP32 ↔ STM8-DISCOVERY

```
![Web UI](docs/web_ui.png)
![Demo](docs/demo.gif)
```

---

## ✨ Tính năng chính

- 📡 Nạp firmware cho STM8 **qua WiFi**, không cần ST-Link.
- 🖥️ **Giao diện web** kéo-thả file `.bin`, hiển thị **tiến trình** và **log realtime** (Server-Sent Events).
- ⚙️ **Bootloader tự viết** cho STM8 theo kiểu **bare-metal** (thao tác trực tiếp thanh ghi, không HAL, không OS).
- 🛡️ Truyền dữ liệu theo **khung có kiểm tra CRC16-CCITT**, kèm **retry** và **timeout** đảm bảo toàn vẹn.
- 🔒 Ghi Flash bằng **IAP có kiểm tra vùng địa chỉ hợp lệ**; có **watchdog** và cơ chế **tự chạy lại ứng dụng** khi lỗi/hết thời gian chờ.

---

## 🧩 Kiến trúc hệ thống

```
┌───────────────┐   WiFi / HTTP    ┌───────────────────┐   UART + CRC16    ┌────────────────────┐
│  Trình duyệt  │ ───────────────▶ │   ESP32 Gateway   │ ────────────────▶ │  STM8 Bootloader   │
│   (Web UI)    │   kéo-thả .bin   │  AP: 192.168.4.1  │   khung + ACK     │      @ 0x8000      │
└───────────────┘ ◀─────────────── └───────────────────┘ ◀──────────────── └────────────────────┘
                    log realtime                            ACK / NACK               │
                    (SSE)                                                             ▼
                                                                        Ghi Flash Application @ 0x8400
```

**Luồng hoạt động:** người dùng mở web trên ESP32 → chọn file `.bin` → ESP32 reset STM8, bắt tay (handshake) → truyền firmware theo từng khung có CRC16 → bootloader STM8 ghi vào Flash → nạp xong thì nhảy sang chạy ứng dụng mới tại `0x8400`.

---

## 📁 Cấu trúc thư mục

```
.
├── FOTA_Gateway/
│   └── src/
│       ├── main.cpp        # Firmware ESP32 Gateway (WiFi AP + web server + nạp UART)
│       ├── bootloader.c    # Bootloader STM8 (bản đi kèm gateway)
│       ├── index.html      # Giao diện web
│       ├── style.css       # CSS giao diện
│       └── script.js       # JS giao diện (upload .bin, thanh tiến trình, log)
├── BootloaderSTM8.c        # Bootloader STM8 (bare-metal, bản chính)
├── .gitignore
├── LICENSE                 # MIT
└── README.md
```

---

## 🔧 Phần cứng & đấu nối

- **MCU:** STM8L152C6T6 (board **STM8L-DISCOVERY**)
- **Gateway:** ESP32 DevKit

| ESP32           | STM8 (STM8L-DISCOVERY) | Ghi chú                   |
|-----------------|------------------------|---------------------------|
| GPIO17 (TX)     | PA3 (P1-6, STM8 RX)    | ESP32 gửi → STM8 nhận     |
| GPIO16 (RX)     | PA2 (P1-5, STM8 TX)    | STM8 gửi → ESP32 nhận     |
| GPIO4           | PA1 / RST (P1-4)       | ESP32 điều khiển reset    |
| GND             | GND (P1-3)             | Nối chung mass            |

> ⚠️ Trên STM8L-DISCOVERY nhớ **tháo jumper JP1** để tránh xung đột nguồn/tín hiệu.

---

## 📦 Giao thức nạp firmware

**Các byte điều khiển:**

| Byte        | Giá trị            | Ý nghĩa                                            |
|-------------|--------------------|----------------------------------------------------|
| Handshake   | `0x7F`             | ESP32 gửi để đánh thức bootloader                  |
| ACK         | `0x79`             | STM8 xác nhận thành công (chấp nhận thêm `0xCF`, `0xFD` do lệch timing bit-bang) |
| NACK        | `0x1F`             | Lỗi CRC / địa chỉ không hợp lệ → yêu cầu gửi lại   |
| EOF         | `0xFF`             | Báo kết thúc truyền firmware                       |

**Định dạng khung dữ liệu (mỗi khung tối đa 16 byte data):**

```
[ADDR_HI][ADDR_MD][ADDR_LO][LEN][DATA x LEN][CRC_HI][CRC_LO]
```

- **CRC16-CCITT** (poly `0x1021`, init `0xFFFF`) tính trên `ADDR + LEN + DATA`.
- Bootloader kiểm tra CRC và **vùng địa chỉ hợp lệ** trước khi ghi; sai → trả `NACK`, ESP32 **retry tối đa 3 lần**.

**Bản đồ bộ nhớ Flash STM8:**

| Vùng        | Địa chỉ   | Nội dung                    |
|-------------|-----------|-----------------------------|
| Bootloader  | `0x8000`  | Cố định, chạy đầu tiên sau reset |
| Application | `0x8400`  | Firmware do người dùng nạp  |

---

## 🛠️ Hướng dẫn Build & Nạp

### 1) Build bootloader STM8 (SDCC)

```bash
sdcc -mstm8 --out-fmt-ihx --code-loc 0x8000 --code-size 0x400 \
     --iram-size 2048 --stack-loc 0x07FF \
     --nogcse --noinvariant --noinduction \
     BootloaderSTM8.c -o bootloader.ihx
```

Nạp bootloader **một lần duy nhất** vào STM8 bằng ST-Link (ví dụ dùng `stm8flash`). Sau bước này, mọi lần cập nhật ứng dụng về sau đều làm **qua WiFi**, không cần ST-Link nữa.

### 2) Nạp firmware ESP32 Gateway

- Mở `FOTA_Gateway/src/main.cpp` bằng **Arduino IDE** hoặc **PlatformIO** (board: ESP32 Dev Module).
- Thư viện cần: `ESPAsyncWebServer`, `AsyncTCP`.
- Nếu giao diện web được phục vụ từ **LittleFS**: đặt `index.html`, `style.css`, `script.js` vào thư mục `data/` rồi nạp bằng công cụ *ESP32 Sketch Data Upload* / *arduino-littlefs-upload*.
- Nạp sketch lên ESP32.

### 3) Sử dụng

1. Kết nối WiFi **`FOTA-Gateway`** (mật khẩu mặc định `12345678`).
2. Mở trình duyệt tới **`http://192.168.4.1`**.
3. Kéo-thả file firmware `.bin` (≤ 28 KB) → bấm **Nạp**.
4. Theo dõi tiến trình & log; nạp xong STM8 tự chạy firmware mới.

---

## 🧪 Công nghệ sử dụng

- **STM8:** C bare-metal, SDCC, UART/USART1, Flash IAP, IWDG (watchdog), CRC16-CCITT.
- **ESP32:** C/C++ (Arduino), WiFi SoftAP, ESPAsyncWebServer, Server-Sent Events, FreeRTOS task.
- **Web:** HTML/CSS/JavaScript (kéo-thả file, progress bar, log realtime).

---

## 👤 Tác giả & Đóng góp

- **Nguyễn Thúy Hiền** — phụ trách **bootloader STM8** (bare-metal, giao thức UART + CRC16, ghi Flash IAP, watchdog).
- Đề tài thực hiện theo nhóm (3 thành viên); phần cổng ESP32 và giao diện web là công sức chung của nhóm.

---

## 📄 License

Dự án phát hành theo giấy phép **MIT** — xem file [LICENSE](LICENSE).
