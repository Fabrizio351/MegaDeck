#pragma once

#define APP_NAME    "TDeckFW"
#define APP_VERSION "0.2.0"

// ---------------------------------------------------------------------
// Pin ufficiali LilyGO T-Deck / T-Deck Plus
// Fonte: https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/UnitTest/utilities.h
// ---------------------------------------------------------------------

// Alimentazione periferiche (va messo HIGH prima di usare qualsiasi periferica)
#define BOARD_POWERON       10

// I2C (tastiera, IO expander, eventuale touch)
#define BOARD_I2C_SDA       18
#define BOARD_I2C_SCL       8

// Batteria
#define BOARD_BAT_ADC       4

// Touch (GT911) - non gestito in questo firmware, T-Deck base non ha touch
#define BOARD_TOUCH_INT     16

// Tastiera fisica (ESP32-C3 come slave I2C)
#define BOARD_KEYBOARD_INT  46
#define BOARD_KEYBOARD_ADDR 0x55

// SD Card (condivide il bus SPI con display e radio)
#define BOARD_SDCARD_CS     39

// Display ST7789
#define BOARD_TFT_CS         12
#define BOARD_TFT_DC         11
#define BOARD_TFT_BACKLIGHT  42
#define DISPLAY_WIDTH         320
#define DISPLAY_HEIGHT        240
#define DISPLAY_ROTATION      1

// Bus SPI condiviso (display + LoRa + SD)
#define BOARD_SPI_MOSI       41
#define BOARD_SPI_MISO       38
#define BOARD_SPI_SCK        40

// Trackball (4 direzioni + pulsante centrale = BOOT pin)
#define BOARD_TBOX_G01       3   // giu
#define BOARD_TBOX_G02       2   // su
#define BOARD_TBOX_G03       15  // destra
#define BOARD_TBOX_G04       1   // sinistra
#define BOARD_TRACKBALL_BTN  0   // = BOARD_BOOT_PIN

// Radio LoRa SX1262
#define RADIO_CS_PIN     9
#define RADIO_BUSY_PIN   13
#define RADIO_RST_PIN    17
#define RADIO_DIO1_PIN   45

#ifndef RADIO_FREQ
#define RADIO_FREQ 868.0     // 868 EU / 915 US / 433 a seconda della versione del modulo che hai comprato
#endif
#define RADIO_BANDWIDTH   125.0
#define RADIO_SF          10
#define RADIO_CR          6
#define RADIO_TX_POWER    22
#define RADIO_SYNC_WORD   0x12  // sync word "privato", usa 0x34 se ti devi interfacciare con LoRaWAN pubblico

// GPS (su Grove/UART1) - modulo MIA-M10Q
#define BOARD_GPS_TX_PIN  43   // collegato a RX del modulo GPS
#define BOARD_GPS_RX_PIN  44   // collegato a TX del modulo GPS
#define GPS_BAUD_RATE     9600 // se non trovi mai fix, prova 38400

// Intervallo minimo tra due letture input
#define INPUT_INTERVAL_MS 25
