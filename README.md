#  FOTA System for STM8 - Cập nhật firmware qua WiFi cho STM8

Hệ thống cập nhật firmware từ xa qua WiFi (FOTA – Firmware Over-The-Air) cho vi điều khiển STM8L152C6T6, kèm giao diện Web IDE để viết, biên dịch và nạp firmware cho STM8 qua Wi-Fi 


##  Tính năng chính

-  Web IDE cho phép viết và chỉnh sửa mã nguồn C trực tiếp trên trình duyệt.
-  Nạp firmware cho STM8 qua Wi-Fi thông qua ESP32 Gateway, không cần kết nối trực tiếp với ST-Link trong quá trình cập nhật.
-  Giao diện web hiển thị trạng thái biên dịch, tiến trình nạp firmware và log thời gian thực.
-  ESP32 đóng vai trò Gateway, nhận firmware từ Web IDE và truyền đến STM8 thông qua giao tiếp UART.
-  Bootloader STM8 hỗ trợ nhận firmware, kiểm tra CRC và ghi vào Flash.
-  Dữ liệu được truyền theo giao thức tùy chỉnh có CRC16, kết hợp cơ chế *ACK/NACK, timeout và retransmission nhằm tăng độ tin cậy trong quá trình cập nhật firmware.

---

##  Kiến trúc hệ thống

```
┌───────────────┐   WiFi / HTTP    ┌───────────────────┐   UART + CRC16     ┌────────────────────┐
│  Trình duyệt  │ ───────────────▶│   ESP32 Gateway    │ ────────────────▶ │  STM8 Bootloader   │
│   (Web UI)    │  Build → .bin    │                    │   khung + ACK     │      @ 0x8000      │
└───────────────┘ ◀─────────────── └───────────────────┘ ◀──────────────── └────────────────────┘
                                                            ACK / NACK               │
                                                                                     ▼
                                                                        Ghi Flash Application tại địa chỉ 0x8400
```

**Luồng hoạt động:** Người dùng truy cập giao diện Web IDE, viết hoặc chỉnh sửa mã nguồn C trực tiếp trên trình duyệt -> gửi yêu cầu biên dịch (Build) -> backend sử dụng trình biên dịch SDCC để tạo firmware `.bin` -> firmware được gửi đến ESP32 Gateway -> ESP32 reset STM8 và thực hiện bắt tay (handshake) với bootloader -> firmware được truyền theo từng khung dữ liệu có kiểm tra CRC16 -> bootloader STM8 ghi dữ liệu vào Flash -> sau khi nạp hoàn tất, bootloader thực thi sang chương trình  tại địa chỉ 0x8400.

---

##  Cấu trúc thư mục

```
.
├── FOTA_Gateway/
│   └── src/
│       ├── main.cpp        # Firmware ESP32 Gateway (WiFi AP + web server + nạp UART)
│       ├── index.html      # Giao diện web
│       ├── style.css       # CSS giao diện
│       └── script.js       # JS giao diện (upload .bin, thanh tiến trình, log)
├── BootloaderSTM8.c        # Bootloader STM8 (bare-metal, bản chính)
├── .gitignore
├── LICENSE                 # MIT
└── README.md
```

---

##  Phần cứng & đấu nối

- **MCU:** STM8L152C6T6 (board **STM8L-DISCOVERY**)
- **Gateway:** ESP32 DevKit

| ESP32           | STM8 (STM8L-DISCOVERY) | Ghi chú                   |
|-----------------|------------------------|---------------------------|
| GPIO17 (TX)     | PA3 (P1-6, STM8 RX)    | ESP32 gửi -> STM8 nhận     |
| GPIO16 (RX)     | PA2 (P1-5, STM8 TX)    | STM8 gửi -> ESP32 nhận     |
| GPIO4           | PA1 / RST (P1-4)       | ESP32 điều khiển reset    |
| GND             | GND (P1-3)             | Nối chung mass            |

>  Trên STM8L-DISCOVERY nhớ tháo jumper JP1 để tránh xung đột nguồn/tín hiệu.

---

## Giao thức nạp firmware

**Các byte điều khiển:**

| Byte        | Giá trị            | Ý nghĩa                                            |
|-------------|--------------------|----------------------------------------------------|
| Handshake   | `0x7F`             | ESP32 gửi để đánh thức bootloader                  |
| ACK         | `0x79`             | STM8 xác nhận thành công (chấp nhận thêm 0xCF, 0xFD do lệch timing bit-bang) |
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

## Trình tự thực hiện

### 1) Build bootloader STM8 (SDCC)

Nạp bootloader **một lần duy nhất** vào STM8 bằng ST-Link ). Sau bước này, mọi lần cập nhật ứng dụng về sau đều làm **qua WiFi**, không cần ST-Link nữa.

### 2) Nạp firmware ESP32 Gateway

- Mở `FOTA_Gateway/src/main.cpp` bằng **Arduino IDE** hoặc **PlatformIO** (board: ESP32 Dev Module).
- Thư viện cần: `ESPAsyncWebServer`, `AsyncTCP`.
- Nạp sketch lên ESP32.

### 3) Sử dụng

1. Kết nối WiFi **`FOTA-Gateway`** (mật khẩu mặc định `[ ]`).
2. Mở trình duyệt tại **`http://192.168.4.1`**.
3. Viết hoặc chỉnh sửa mã nguồn C trên **Web IDE**, sau đó nhấn **Build** để biên dịch thành firmware `.bin`.
4. Sau khi biên dịch thành công, nhấn **Flash** để gửi firmware đến ESP32 và nạp xuống STM8.
5. Theo dõi tiến trình và log trên giao diện; sau khi hoàn tất, bootloader sẽ chuyển sang thực thi chương trình mới trên STM8.

---



