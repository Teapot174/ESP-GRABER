#ifndef CONFIG_H
#define CONFIG_H

// Параметры экрана (OLED SSD1306 / SH1106)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
#define OLED_ADR OLED_ADDR

#if defined(BOARD_ESP32_C3_SUPERMINI)

// Пины для ESP32-C3 SuperMini
#define OLED_SCL 9
#define OLED_SDA 8

#define BTN_UP 0
#define BTN_DOWN 1
#define BTN_OK 2
#define BTN_BACK 3

#define CC1101_MOSI 7  // Аппаратный SPI MOSI
#define CC1101_MISO 6  // Аппаратный SPI MISO
#define CC1101_SCK 4   // Аппаратный SPI SCK
#define CC1101_CS 5    // Выбор чипа (Chip Select)
#define CC1101_GDO0 10 // Сигнальный пин прерывания

#elif defined(BOARD_ESP_HACK)

// Пины для ESP-HACK (ESP32 38 pin)
#define OLED_SCL 22
#define OLED_SDA 21

#define BTN_UP 27
#define BTN_DOWN 26
#define BTN_OK 33
#define BTN_BACK 32

#define CC1101_MOSI 23
#define CC1101_MISO 19
#define CC1101_SCK 18
#define CC1101_CS 5
#define CC1101_GDO0 4

#else

// Стандартная распиновка для ESP32 Dev Module (30 pin)
#define OLED_SCL 22
#define OLED_SDA 21

#define BTN_UP 27   // K1 (Up)
#define BTN_DOWN 26 // K2 (Down)
#define BTN_OK 33   // K3 (OK)
#define BTN_BACK 32 // K4 (Back)

#define CC1101_MOSI 23
#define CC1101_MISO 19
#define CC1101_SCK 18
#define CC1101_CS 5
#define CC1101_GDO0 2

#endif

#endif
