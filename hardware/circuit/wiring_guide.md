# Wiring Guide — Digital Safe System (A1401)

## ESP32-S3 DevKitC-1 Pin Assignments

### OLED Display (1.5" SH1107 128×128) — I2C
| OLED Pin | ESP32-S3 Pin | Wire Color |
|----------|-------------|------------|
| GND | GND | Black |
| VCC | 3V3 | Red |
| SCL | GPIO 9 | Yellow |
| SDA | GPIO 8 | Blue |

### RFID Reader (MFRC522) — SPI
| RFID Pin | ESP32-S3 Pin | Wire Color |
|----------|-------------|------------|
| 3.3V | 3V3 | Red |
| GND | GND | Black |
| RST | GPIO 14 | White |
| SDA (SS) | GPIO 10 | Green |
| SCK | GPIO 12 | Orange |
| MOSI | GPIO 11 | Purple |
| MISO | GPIO 13 | Cyan |

> ⚠️ RFID module runs on **3.3V only** — connecting to 5V will damage it.

### 4×4 Membrane Keypad — Digital GPIO
| Keypad Pin | ESP32-S3 Pin |
|------------|-------------|
| Row 1 | GPIO 1 |
| Row 2 | GPIO 2 |
| Row 3 | GPIO 42 |
| Row 4 | GPIO 41 |
| Col 1 | GPIO 40 |
| Col 2 | GPIO 39 |
| Col 3 | GPIO 38 |
| Col 4 | GPIO 37 |

### Servo Motor (SG90 Continuous) — PWM
| Servo Wire | ESP32-S3 Pin |
|------------|-------------|
| Signal (Orange) | GPIO 18 |
| VCC (Red) | 5V (VIN) |
| GND (Brown) | GND |

### LEDs — Digital GPIO (each via 220Ω resistor)
| LED | ESP32-S3 Pin |
|-----|-------------|
| Red LED (+) | GPIO 15 → 220Ω → LED |
| Green LED (+) | GPIO 16 → 220Ω → LED |
| Both cathodes (−) | GND |

### Buzzer — Digital GPIO
| Buzzer Pin | ESP32-S3 Pin |
|------------|-------------|
| + (Signal) | GPIO 17 |
| − (Ground) | GND |

### Bluetooth
No external wiring — ESP32-S3 has BLE built in.

## Power Notes

- Total current draw: ~300mA
- USB power from laptop is sufficient
- For maquette demo: USB power bank works
- All components share a common GND rail on the breadboard
- Connect at least 2 GND pins from ESP32-S3 to the breadboard
