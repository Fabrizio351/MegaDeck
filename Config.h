#pragma once

#define APP_NAME    "TDeckFW"
#define APP_VERSION "0.18.0"

// Interruttore per disattivare tutti i log seriali di debug (le righe
// "[LORA]", "[BLE]", "[MENU]", ecc. sparse nel progetto): usa
// DEBUG_PRINTLN/DEBUG_PRINTF al posto di Serial.println/printf, cosi'
// con DEBUG_SERIAL_ENABLED=0 diventano no-op a costo zero (niente
// scrittura USB CDC, che ha un piccolo costo in tempo/energia ad ogni
// chiamata). Per la diagnosi "a posteriori" di un crash non serve
// comunque il monitor seriale collegato: vedi CrashLog.h, che funziona
// indipendentemente da questo interruttore.
#ifndef DEBUG_SERIAL_ENABLED
#define DEBUG_SERIAL_ENABLED 1
#endif

#if DEBUG_SERIAL_ENABLED
// Variadic (non un solo parametro fisso): alcune chiamate esistenti
// usano la forma a due argomenti di Serial.println/print (es. valore +
// base numerica come HEX), quindi le macro devono accettarli entrambi.
#define DEBUG_PRINTLN(...)     Serial.println(__VA_ARGS__)
#define DEBUG_PRINT(...)       Serial.print(__VA_ARGS__)
#define DEBUG_PRINTF(...)      Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINTLN(...)
#define DEBUG_PRINT(...)
#define DEBUG_PRINTF(...)
#endif

// ---------------------------------------------------------------------
// Pin ufficiali LilyGO T-Deck / T-Deck Plus
// Fonte: https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/UnitTest/utilities.h
// ---------------------------------------------------------------------

// Alimentazione periferiche (va messo HIGH prima di usare qualsiasi periferica)
#define BOARD_POWERON       10

// I2C (tastiera, IO expander, eventuale touch)
#define BOARD_I2C_SDA       18
#define BOARD_I2C_SCL       8

// Batteria: partitore resistivo 2:1 tipico del T-Deck (la tensione reale
// della cella e' il doppio di quella letta sul pin ADC). Se la percentuale
// mostrata in alto a destra sembra sbagliata, e' il primo posto da
// correggere: la formula esatta del partitore dipende dalla revisione
// hardware e non e' verificabile da codice.
#define BOARD_BAT_ADC        4
#define BATTERY_ADC_DIVIDER  2.0f
#define BATTERY_VOLT_EMPTY   3.3f   // 0%
#define BATTERY_VOLT_FULL    4.2f   // 100%

// Touch (GT911). NB: presente solo sul T-Deck Plus, non sul T-Deck base.
// Se il tuo modulo e' il T-Deck base senza touch fisico, il chip non
// risponde sul bus I2C: Input::begin() lo rileva (log "GT911 non
// risponde") e disattiva semplicemente il sottosistema touch, senza
// impatto sul resto. Questo pin viene usato durante il reset per
// forzare in modo deterministico l'indirizzo I2C del controller
// (vedi resetTouchController() in Input.cpp).
#define BOARD_TOUCH_INT     16

// Interruttore di emergenza per isolare il touch durante il debug: a 0
// Input::begin() salta del tutto rilevamento I2C, task dedicato e
// registrazione dell'indev LVGL. Trackball e tastiera restano invariati.
#ifndef INPUT_TOUCH_ENABLED
#define INPUT_TOUCH_ENABLED 1
#endif

// Interruttore di emergenza per isolare la radio LoRa durante il debug:
// a 0 LoRaRadio::begin() non tocca affatto il bus SPI condiviso col
// display (niente radioSPI.begin(), niente radio.begin()/startReceive()),
// e update() non fa nulla.
//
// Riattivata (antenna montata): NOTA UN RISCHIO NOTO E MAI RISOLTO DAVVERO
// - display (Arduino_GFX/Arduino_ESP32SPI, bus interno proprietario
// _spi_bus_array) e radio (LoRaRadio.cpp, oggetto SPIClass separato)
// condividono lo stesso bus fisico HSPI ma tramite DUE gestori software
// indipendenti che non si sincronizzano fra loro: era il sospetto
// (mai confermato sperimentalmente) dietro al display che smetteva di
// aggiornarsi visivamente pur continuando a ricevere flush da LVGL. Se
// il freeze si ripresenta (schermo fermo proprio in corrispondenza di
// un invio/ricezione LoRa), e' quella la causa piu' probabile e serve
// un lavoro piu' corposo per condividere davvero il bus fra i due.
#ifndef LORA_ENABLED
#define LORA_ENABLED 1
#endif

// Interruttore di emergenza per isolare il GPS durante il debug: a 0
// GPS::begin() non avvia la UART1 e update() non fa nulla. Il GPS non
// condivide bus con display/LoRa/tastiera (solo UART dedicata), quindi
// e' un sospettato debole, ma lo eliminiamo comunque per arrivare a un
// firmware minimo (solo Board+Display+Input+Menu) come ultimo passo di
// bisezione.
#ifndef GPS_ENABLED
#define GPS_ENABLED 1
#endif

// Tastiera fisica (ESP32-C3 come slave I2C)
#define BOARD_KEYBOARD_INT  46
#define BOARD_KEYBOARD_ADDR 0x55

// Altoparlante I2S integrato (verificati contro gli esempi ufficiali
// LilyGo per T-Deck, SimpleTone/I2SPlay) - spostati qui il 28/09 (prima
// vivevano solo dentro Sound.cpp) perche' anche WebRadioUI.cpp deve
// usare lo STESSO fisico I2S (un solo altoparlante, un solo bus): Sound
// e WebRadioUI si passano il possesso a vicenda (Sound::
// pauseForExternalUse()/resumeForOwnUse()), non lo tengono mai aperto
// insieme.
#define BOARD_I2S_BCLK       7
#define BOARD_I2S_WS         5
#define BOARD_I2S_DOUT       6

// Microfono integrato (29/09) - chip codec ES7210 su I2C (stesso bus di
// tastiera/touch) + un bus I2S TUTTO SUO in ricezione, pin diversi da
// quelli dell'altoparlante sopra: nessun conflitto di risorsa fisica,
// puo' girare anche insieme all'altoparlante. Pinout dall'esempio
// ufficiale LilyGo (examples/Microphone/utilities.h).
#define BOARD_ES7210_MCLK    48
#define BOARD_ES7210_LRCK    21
#define BOARD_ES7210_SCK     47
#define BOARD_ES7210_DIN     14

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
// A 9600 arrivavano byte (confermato via log [GPS-DIAG]) ma zero
// sentenze NMEA valide dopo migliaia di byte ricevuti: sintomo di baud
// rate sbagliato (i dati arrivano ma vengono decodificati male, ogni
// checksum fallisce). Provato 38400. Se anche questo non funziona,
// prossimo tentativo: 115200 o 4800 (altri baud comuni per moduli GPS).
#define GPS_BAUD_RATE     38400

// Intervallo minimo tra due letture input
#define INPUT_INTERVAL_MS 25

// Debounce dedicato al trackball: i 4 sensori sono interruttori
// meccanici e "rimbalzano" (piu' impulsi elettrici per un singolo
// scatto fisico), causando salti di 2+ voci di menu per un solo click
// percepito dall'utente. Dopo aver accettato un movimento, ignoriamo
// qualunque altro impulso (su tutti e 4 i pin, non solo quello appena
// letto) per questa finestra di tempo.
#define TRACKBALL_DEBOUNCE_MS 60
