/**
 * lv_conf.h - configurazione LVGL per TDeckFW (ESP32-S3 T-Deck, LVGL 8.3.x)
 *
 * DOVE METTERLO:
 * Dopo aver installato la libreria "lvgl" da Library Manager, questo file
 * va copiato in: Documenti/Arduino/libraries/lv_conf.h
 * (cioe' FUORI dalla cartella lvgl/, un livello sopra, accanto ad essa).
 * Se non lo trova li', mettilo anche nella cartella dello sketch (TDeckFW/).
 *
 * NB: e' volutamente minimale. Tutte le macro NON definite qui vengono
 * risolte automaticamente da lv_conf_internal.h con i valori di default
 * di LVGL: non serve compilare l'intero template da migliaia di righe.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*==== Colore ====*/
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   0   // se i colori escono con R/B invertiti, metti a 1

/*==== Memoria ====*/
// Con PSRAM attiva i draw buffer NON passano da qui (li allochiamo noi
// in Display.cpp con heap_caps_malloc su SPIRAM). Questo pool serve solo
// per gli oggetti/widget LVGL stessi.
// Riportato a 64KB (valore originale): un aumento a 96KB/160KB era
// servito a evitare un crash quando NetScan crea molte righe in una
// volta (lv_obj_create() torna silenziosamente nullptr a pool pieno),
// ma ha lasciato troppo poca RAM interna libera per gli handshake TLS
// (GeoLookup/WeatherLookup/ParkLookup, HTTPS) - falliti SEMPRE con 96KB,
// dove prima (64KB) funzionavano. Il crash di NetScan resta comunque
// evitato dai controlli nullptr aggiunti in NetScanUI.cpp (la lista si
// tronca invece di andare in crash se il pool finisce davvero), quindi
// tornare a 64KB e' sicuro.
#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (64U * 1024U)

// Con pool fisso da 64KB (vedi sopra) una malloc() interna di LVGL PUO'
// fallire sotto pressione (tante righe/widget in una schermata, molti
// cambi schermo di fila). Di fabbrica LVGL, se questo succede, ignora
// l'errore e continua a scrivere nel puntatore NULL restituito: crash
// indiagnosticabile, indirizzo diverso ogni volta a seconda del campo
// struct scritto (confermato con addr2line: EXCVADDR=0x8, dentro
// lv_draw_add_task() in lv_draw.c, offset del campo "area" in
// lv_draw_task_t quando new_task e' NULL). Con questi due flag, una
// malloc fallita diventa invece un riavvio controllato e tracciato
// (vedi crashlog_record_lvgl_oom_and_restart() in CrashLog.cpp): stesso
// problema di fondo (pool troppo piccolo per il carico di widget), ma
// un evento diagnosticabile invece di un crash casuale.
#define LV_USE_ASSERT_MALLOC        1
#define LV_ASSERT_USE_CUSTOM_INCLUDE 1
#define LV_ASSERT_CUSTOM_INCLUDE    "LvAssertHandler.h"

/*==== Tick ====*/
#define LV_TICK_CUSTOM     1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/*==== Refresh ====*/
#define LV_DISP_DEF_REFR_PERIOD  30   // ms

/*==== Font ====*/
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/*==== Widget usati ====*/
#define LV_USE_LABEL    1
#define LV_USE_BTN      1
#define LV_USE_BTNMATRIX 0
#define LV_USE_IMG      0
#define LV_USE_FLEX     1
#define LV_USE_GRID     1

/*==== Logging (disattivo, risparmia flash) ====*/
#define LV_USE_LOG      0

/*==== Varie ====*/
#define LV_USE_PERF_MONITOR   0
#define LV_USE_MEM_MONITOR    0

#define LV_USE_FONT_MONTSERRAT_14 1
#define LV_USE_FONT_MONTSERRAT_20 1

#endif // LV_CONF_H
