#include "CrashLog.h"
#include "Config.h"

#include <Preferences.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include <string.h>

namespace {
    constexpr int RING_SIZE = 12;
    constexpr int ENTRY_LEN = 24;
    constexpr uint32_t RING_MAGIC_VALUE = 0xC0FFEE42;

    // RAM RTC: non azzerata dal C++ runtime ad ogni avvio (a differenza
    // della RAM normale), il suo contenuto sopravvive a reset software/
    // watchdog/panico. Su un vero power-on il contenuto e' indefinito,
    // per questo il controllo di ringMagic sotto serve a distinguere
    // "dati validi di una sessione precedente" da "spazzatura mai
    // inizializzata".
    RTC_NOINIT_ATTR char ring[RING_SIZE][ENTRY_LEN];
    RTC_NOINIT_ATTR uint8_t ringIndex;
    RTC_NOINIT_ATTR uint32_t ringMagic;

    Preferences crashPrefs;
    constexpr const char* CRASH_PREFS_NAMESPACE = "tdeckcrash";
    constexpr const char* CRASH_PREFS_KEY_LOG = "log";
    constexpr const char* CRASH_PREFS_KEY_REASON = "reason";
    constexpr const char* CRASH_PREFS_KEY_COREDUMP = "coredump";

    // Cronologia rotante degli ultimi CRASH_HISTORY_SIZE crash (non solo
    // l'ultimo): HIST_IDX_KEY punta al prossimo slot da scrivere (quindi
    // e' anche il piu' vecchio, che verra' sovrascritto per primo).
    constexpr int CRASH_HISTORY_SIZE = 5;
    constexpr const char* CRASH_PREFS_KEY_HIST_IDX = "hidx";
    constexpr const char* CRASH_PREFS_KEY_HIST_CNT = "hcnt";

    bool hadCrashFlag = false;
    String lastCrashReason;
    String lastCoreDumpSummary;
    int lastRawResetReason = -1;

    void historyKey(char* out, size_t outSize, char suffix, int slot) {
        snprintf(out, outSize, "h%d%c", slot, suffix);
    }

    // Legge il riepilogo del coredump ESP-IDF (se presente e valido),
    // lo formatta in una stringa leggibile, lo stampa su seriale e poi
    // svuota il coredump grezzo in flash (letto una volta sola, come le
    // breadcrumb sopra): il testo formattato resta comunque salvato in
    // NVS da chi chiama.
    String readAndClearCoreDumpSummary() {
        if (esp_core_dump_image_check() != ESP_OK) {
            return "";
        }

        esp_core_dump_summary_t summary;
        esp_err_t err = esp_core_dump_get_summary(&summary);

        String text;
        if (err == ESP_OK) {
            char reasonBuf[160] = { 0 };
            esp_core_dump_get_panic_reason(reasonBuf, sizeof(reasonBuf));

            if (reasonBuf[0] != '\0') {
                text += String(reasonBuf) + "\n";
            }

            text += "Task: " + String(summary.exc_task);
            text += "  PC: 0x" + String(summary.exc_pc, HEX) + "\n";
            text += "Causa eccezione: " + String(summary.ex_info.exc_cause);
            text += "  Indirizzo: 0x" + String(summary.ex_info.exc_vaddr, HEX) + "\n";

            text += "Backtrace:";
            uint32_t depth = summary.exc_bt_info.depth;
            if (depth > 16) {
                depth = 16;
            }
            for (uint32_t i = 0; i < depth; i++) {
                text += " 0x" + String(summary.exc_bt_info.bt[i], HEX);
            }

            DEBUG_PRINTLN("[CRASHDUMP] ---- riepilogo coredump ----");
            DEBUG_PRINTLN(text);
            DEBUG_PRINTLN("[CRASHDUMP] ----------------------------");
        }

        // Consumato: il prossimo avvio (anomalo o no) non lo ritrova piu'
        // qui, ma il testo sopra resta salvato in NVS da chi chiama.
        esp_core_dump_image_erase();

        return text;
    }

    const char* resetReasonText(esp_reset_reason_t reason) {
        switch (reason) {
            case ESP_RST_POWERON:    return "Accensione normale";
            case ESP_RST_EXT:        return "Reset esterno";
            case ESP_RST_SW:         return "Riavvio software";
            case ESP_RST_PANIC:      return "Panico/eccezione";
            case ESP_RST_INT_WDT:    return "Watchdog interrupt";
            case ESP_RST_TASK_WDT:   return "Watchdog task";
            case ESP_RST_WDT:        return "Watchdog";
            case ESP_RST_DEEPSLEEP:  return "Uscita da deep sleep";
            case ESP_RST_BROWNOUT:   return "Brownout (alimentazione instabile)";
            case ESP_RST_SDIO:       return "Reset da SDIO";
            case ESP_RST_USB:        return "Reset da USB";
            case ESP_RST_JTAG:       return "Reset da JTAG";
            default:                 return "Sconosciuto";
        }
    }

    // Scrive un evento sia come "ultimo crash" sia nello slot rotante
    // della cronologia. Condivisa fra begin() (dopo un reset anomalo
    // vero) e recordControlledFault() (chiamata PRIMA di un riavvio
    // volontario, per registrare un problema intercettato "in diretta"
    // invece di lasciarlo degenerare in un crash indiagnosticabile).
    void writeHistoryEntry(const String& reasonText, const String& log, const String& coreDumpText) {
        crashPrefs.begin(CRASH_PREFS_NAMESPACE, false);
        crashPrefs.putString(CRASH_PREFS_KEY_LOG, log);
        crashPrefs.putString(CRASH_PREFS_KEY_REASON, reasonText);
        crashPrefs.putString(CRASH_PREFS_KEY_COREDUMP, coreDumpText);

        uint8_t histIdx = crashPrefs.getUChar(CRASH_PREFS_KEY_HIST_IDX, 0);
        uint8_t histCnt = crashPrefs.getUChar(CRASH_PREFS_KEY_HIST_CNT, 0);

        char key[8];
        historyKey(key, sizeof(key), 'r', histIdx);
        crashPrefs.putString(key, reasonText);
        historyKey(key, sizeof(key), 'l', histIdx);
        crashPrefs.putString(key, log);
        historyKey(key, sizeof(key), 'c', histIdx);
        crashPrefs.putString(key, coreDumpText);

        histIdx = (histIdx + 1) % CRASH_HISTORY_SIZE;
        if (histCnt < CRASH_HISTORY_SIZE) {
            histCnt++;
        }
        crashPrefs.putUChar(CRASH_PREFS_KEY_HIST_IDX, histIdx);
        crashPrefs.putUChar(CRASH_PREFS_KEY_HIST_CNT, histCnt);

        crashPrefs.end();
    }

    String currentBreadcrumbLog() {
        String log;
        for (int i = 0; i < RING_SIZE; i++) {
            int idx = (ringIndex + i) % RING_SIZE;
            if (ring[idx][0] != '\0') {
                log += ring[idx];
                log += "\n";
            }
        }
        return log;
    }
}

namespace CrashLog {

    void begin() {
        esp_reset_reason_t reason = esp_reset_reason();

        lastRawResetReason = static_cast<int>(reason);
        DEBUG_PRINTF("[CRASHLOG] esp_reset_reason() grezzo = %d\n", lastRawResetReason);

        // CONFERMATO con una misura reale (2026-09-23), non supposto: il
        // reset di fine caricamento su questa scheda (USB nativo, non un
        // chip USB-seriale separato con RTS->EN) arriva come
        // ESP_RST_USB, non ESP_RST_EXT come ci si aspetterebbe da un
        // reset "esterno" classico. Senza escluderlo, OGNI singolo
        // caricamento veniva trattato come un crash vero, sovrascrivendo
        // i dati reali del crash precedente prima ancora di poterli
        // leggere - bug reale, scoperto mentre si cercava di leggere un
        // crash confermato dall'utente e non trovando nulla di salvato.
        // ESP_RST_EXT resta comunque escluso a scopo cautelativo (un
        // riavvio manuale dal pulsante fisico non e' un crash nemmeno
        // lui), anche se qui non e' mai stato osservato accadere.
        bool abnormal = (
            reason != ESP_RST_POWERON &&
            reason != ESP_RST_SW &&
            reason != ESP_RST_EXT &&
            reason != ESP_RST_USB
        );
        bool ringValid = (ringMagic == RING_MAGIC_VALUE);

        // Indipendente da ringValid/abnormal: un coredump in flash e'
        // gestito dall'hardware/ESP-IDF per conto suo, verificato e
        // consumato ad ogni avvio comunque (l'operazione e' economica
        // quando non c'e' nulla da leggere).
        String coreDumpText = readAndClearCoreDumpSummary();

        if (abnormal && ringValid) {
            writeHistoryEntry(resetReasonText(reason), currentBreadcrumbLog(), coreDumpText);
        }

        // Azzera la traccia per la nuova sessione che sta per iniziare.
        memset(ring, 0, sizeof(ring));
        ringIndex = 0;
        ringMagic = RING_MAGIC_VALUE;

        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        lastCrashReason = crashPrefs.getString(CRASH_PREFS_KEY_REASON, "");
        lastCoreDumpSummary = crashPrefs.getString(CRASH_PREFS_KEY_COREDUMP, "");
        crashPrefs.end();

        hadCrashFlag = abnormal;

        // Il [CRASHDUMP] grezzo (dentro readAndClearCoreDumpSummary())
        // si stampa una sola volta, sul riavvio subito dopo il crash: se
        // ci si collega alla seriale un momento dopo, quella finestra e'
        // gia' persa. Questo invece si ristampa ad OGNI avvio finche' non
        // arriva un nuovo crash a sovrascriverlo (letto da NVS sopra),
        // apposta per poterlo leggere "alle volte successive" senza
        // dover per forza catturare il crash o il boot immediatamente
        // dopo dal vivo.
        if (lastCrashReason.length() > 0) {
            DEBUG_PRINTLN("[CRASHLOG] ---- ultimo crash registrato (persiste finche' non ne arriva uno nuovo) ----");
            DEBUG_PRINTLN("[CRASHLOG] Motivo: " + lastCrashReason);
            if (lastCoreDumpSummary.length() > 0) {
                DEBUG_PRINTLN(lastCoreDumpSummary);
            }
            DEBUG_PRINTLN("[CRASHLOG] --------------------------------------------------------------------");
        }
    }

    void breadcrumb(const char* tag) {
        strncpy(ring[ringIndex], tag, ENTRY_LEN - 1);
        ring[ringIndex][ENTRY_LEN - 1] = '\0';
        ringIndex = (ringIndex + 1) % RING_SIZE;
    }

    bool hadCrash() {
        return hadCrashFlag;
    }

    String getLastCrashReason() {
        return lastCrashReason;
    }

    String getLastCrashLog() {
        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        String log = crashPrefs.getString(CRASH_PREFS_KEY_LOG, "(nessun crash registrato)");
        crashPrefs.end();
        return log;
    }

    String getLastCoreDumpSummary() {
        return lastCoreDumpSummary;
    }

    int getLastRawResetReason() {
        return lastRawResetReason;
    }

    int getCrashHistoryCount() {
        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        uint8_t cnt = crashPrefs.getUChar(CRASH_PREFS_KEY_HIST_CNT, 0);
        crashPrefs.end();
        return cnt;
    }

    // indexFromRecent: 0 = piu' recente, count-1 = piu' vecchio ancora
    // in memoria. Va convertito nello slot fisico di scrittura: histIdx
    // punta al PROSSIMO slot da scrivere, quindi l'ultimo scritto e'
    // (histIdx - 1), il penultimo (histIdx - 2), ecc.
    int historySlotFor(int indexFromRecent) {
        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        uint8_t histIdx = crashPrefs.getUChar(CRASH_PREFS_KEY_HIST_IDX, 0);
        crashPrefs.end();

        int slot = (static_cast<int>(histIdx) - 1 - indexFromRecent) % CRASH_HISTORY_SIZE;
        if (slot < 0) {
            slot += CRASH_HISTORY_SIZE;
        }
        return slot;
    }

    String getCrashHistoryReason(int indexFromRecent) {
        int slot = historySlotFor(indexFromRecent);
        char key[8];
        historyKey(key, sizeof(key), 'r', slot);

        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        String text = crashPrefs.getString(key, "");
        crashPrefs.end();
        return text;
    }

    String getCrashHistoryLog(int indexFromRecent) {
        int slot = historySlotFor(indexFromRecent);
        char key[8];
        historyKey(key, sizeof(key), 'l', slot);

        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        String text = crashPrefs.getString(key, "");
        crashPrefs.end();
        return text;
    }

    String getCrashHistorySummary(int indexFromRecent) {
        int slot = historySlotFor(indexFromRecent);
        char key[8];
        historyKey(key, sizeof(key), 'c', slot);

        crashPrefs.begin(CRASH_PREFS_NAMESPACE, true);
        String text = crashPrefs.getString(key, "");
        crashPrefs.end();
        return text;
    }

    void recordControlledFault(const char* reason) {
        breadcrumb(reason);
        DEBUG_PRINTF("[CRASHLOG] guasto intercettato, riavvio controllato: %s\n", reason);
        writeHistoryEntry(String("Intercettato: ") + reason, currentBreadcrumbLog(), "");
    }
}

// Wrapper a collegamento C: richiamata dal macro LV_ASSERT_HANDLER (vedi
// LvAssertHandler.h), che LVGL espande dentro file .c puri - non puo'
// quindi chiamare direttamente funzioni C++ con namespace. Usata al
// momento SOLO per LV_ASSERT_MALLOC (vedi lv_conf.h): prima di questo
// fix, una malloc() fallita nel pool interno di LVGL (fisso, 64KB)
// veniva ignorata silenziosamente e il codice scriveva comunque nel
// puntatore NULL restituito, con un crash indiagnosticabile e casuale
// nell'indirizzo esatto (dipende dal campo struct scritto). Con questo
// handler diventa invece un riavvio pulito e tracciato, con la stessa
// cronologia crash gia' in uso per i panic veri.
extern "C" void crashlog_record_lvgl_oom_and_restart() {
    CrashLog::recordControlledFault("LVGL: pool memoria esaurito (malloc fallita)");
    esp_restart();
}
