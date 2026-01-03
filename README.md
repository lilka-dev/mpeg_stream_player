# Mpeg Video Streaming

Стрімінг відео  на [Лілку](https://lilka.dev) через WiFi за допомогою MJPEG.

Stream your computer screen to Lilka device over WiFi using MJPEG. 

## Як це працює

```
┌─────────────┐    GStreamer     ┌──────────────┐    TCP/MJPEG    ┌─────────────┐
│   Screen    │ ──────────────►  │  stream.sh   │ ──────────────► │   Lilka     │
│  Capture    │   JPEG encode    │  (280x240)   │    WiFi         │  (ESP32-S3) │
└─────────────┘                  └──────────────┘                 └─────────────┘
```

1. **GStreamer** захоплює екран, масштабує до 280x240, кодує як MJPEG
2. **TCP stream** надсилає JPEG-кадри на Лілку через WiFi
3. **TJpgDec** бібліотека на ESP32-S3 декодує JPEG в реальному часі
4. **Arduino_GFX** рендерить кадри на ST7789 дисплей

## Вимоги до обладнання

### Пристрій Lilka
- **Плата**: Lilka v2 (на базі ESP32-S3)
- **Дисплей**: 1.69" ST7789 TFT LCD (280x240 пікселів)
- **Операційна система**: KeiraOS (для налаштування WiFi)
- **Пам'ять**: PSRAM (для буфера JPEG)

### Комп'ютер
- Linux (X11 або PipeWire) або macOS
- GStreamer 1.0 з плагінами

## Встановлення GStreamer

**Linux (Debian/Ubuntu):**
```bash
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-base
```

**Linux (Fedora):**
```bash
sudo dnf install gstreamer1 gstreamer1-plugins-good gstreamer1-plugins-base
```

**macOS:**
```bash
brew install gstreamer gst-plugins-good gst-plugins-base gst-plugins-bad
```

## Інструкція з налаштування

### 1. Налаштуйте WiFi в KeiraOS

WiFi потрібно налаштувати в KeiraOS перед використанням цієї прошивки:

1. Завантажте Лілку в KeiraOS
2. Перейдіть до **Налаштування → WiFi**
3. Виберіть вашу WiFi мережу та введіть пароль
4. Облікові дані зберігаються автоматично

### 2. Зберіть та прошийте

```bash
# Клонуйте репозиторій
git clone https://github.com/lilka-dev/mpeg_stream_player
cd mpeg_stream_player

# Зберіть прошивку
pio run

# Завантажте на Лілку (підключіть через USB)
pio run --target upload
```

**Або завантажте з SD-карти:**
1. Скопіюйте `.pio/build/lilka_v2/firmware.bin` на SD-карту
2. У файловому менеджері KeiraOS відкрийте файл `.bin` для завантаження

### 3. Запам'ятайте IP-адресу

Після завантаження прошивки та підключення до WiFi, на дисплеї з'явиться:
```
MJPEG Receiver
IP Address:
192.168.x.x
Port: 8090
Waiting for stream...
```

### 4. Запустіть стрімінг

```bash
./stream.sh <LILKA_IP>
```

## Використання

```bash
# Стрімінг на Лілку з налаштуваннями за замовчуванням (15 FPS, якість 50)
./stream.sh 192.168.88.239

# Власні налаштування
./stream.sh <IP> [PORT] [FPS] [QUALITY]

# Висока якість, нижчий FPS
./stream.sh 192.168.88.239 8090 10 80

# Швидкий стрімінг, нижча якість
./stream.sh 192.168.88.239 8090 25 30
```

## Продуктивність

Типова продуктивність на ESP32-S3:

| Якість | FPS | Пропускна здатність | Час декодування |
|--------|-----|---------------------|-----------------|
| 30 | 20-25 | ~200 kbps | ~15ms |
| 50 | 15-20 | ~350 kbps | ~20ms |
| 80 | 10-15 | ~600 kbps | ~30ms |

## Ліцензія

MIT License
