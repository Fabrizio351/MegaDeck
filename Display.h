#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace Display {

    // Accesso diretto al driver del pannello: ogni schermata disegna
    // direttamente qui sopra (TouchUI per i tap, TextInputUI per il
    // testo, ognuna col proprio drawScreen()).
    Arduino_GFX* getGfx();

    // Residuo del periodo di coesistenza con LVGL (rimosso il 26/09, vedi
    // project_lvgl_removal_rewrite in memoria): segnava quali schermate
    // erano gia' passate a disegno diretto mentre le altre erano ancora
    // LVGL. Con LVGL sparito non ha piu' alcun effetto - lasciata (con
    // isDirectDrawMode()) solo perche' tolta da ogni singolo chiamante
    // sarebbe stato un cambiamento largo e a rischio per zero beneficio
    // pratico; resta comunque nel log [BACKLIGHT] com'era.
    void setDirectDrawMode(bool active);
    bool isDirectDrawMode();

    // Barra in alto condivisa per le schermate disegnate a mano - stessi
    // dati reali dell'header LVGL (ConnStatus, GPS, batteria) ma
    // disegnata direttamente. Da richiamare ad ogni apertura schermata e
    // ogni volta che si vuole aggiornarla esplicitamente.
    void drawHeaderBar();

    // Richiama drawHeaderBar() da sola quando qualcosa di rilevante e'
    // cambiato (wifi/vpn/ble/lora/netguard/batteria/ora) rispetto
    // all'ultima volta, e SOLO in modalita' disegno diretto. Senza
    // questo, l'header restava "congelato" con lo stato del momento
    // dell'ultimo ridisegno di schermata: stando fermi sul menu, l'icona
    // GUARD non diventava mai verde attivando NetGuard da un'altra
    // schermata, perche' nulla la ridisegnava finche' non arrivava un
    // input (segnalato 26/09). Nessun rischio di riesaurire il pool LVGL
    // (vedi project_lvgl_pool_exhaustion_fix): drawHeaderBar() e' puro
    // Arduino_GFX, non alloca nulla in LVGL. Da richiamare una volta per
    // ciclo da App::update().
    void refreshHeaderIfNeeded();

    // Colori condivisi fra le schermate disegnate a mano - stessa
    // palette validata in Fase 1 (verde matrix). RGB565, non 24 bit.
    extern const uint16_t DD_BG;
    extern const uint16_t DD_ACCENT;
    extern const uint16_t DD_ACCENT_DIM;
    extern const uint16_t DD_TEXT;
    extern const uint16_t DD_TEXT_DIM;

    // Rosso/giallo di allarme, aggiunti il 26/09 per l'elenco live di
    // NetGuard (dispositivo non in whitelist attivo ora / appena
    // scomparso) - stesso stile "matrix" della palette esistente, non
    // colori RGB puri, per restare coerenti visivamente.
    extern const uint16_t DD_DANGER;
    extern const uint16_t DD_WARN;

    // Altezza della barra in alto disegnata a mano, in px: le schermate
    // la usano per sapere dove inizia il loro contenuto.
    constexpr int16_t DD_HEADER_H = 18;

    // Stampa testo multi-riga rispettando i "\n" gia' presenti nel testo
    // E spezzando le righe piu' larghe di w - il font classico e' a
    // larghezza fissa (6px/carattere per unita' di size), quindi basta
    // un calcolo diretto, niente getTextBounds(). Condivisa da tutte le
    // schermate disegnate a mano che mostrano testo libero (stato,
    // risultati...). size: 1 (default, usato quasi ovunque) o 2 (solo
    // dove serve testo piu' leggibile e c'e' spazio, es. GpsUI).
    void drawWrappedText(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color, uint8_t size = 1);

    // Altezza in px che drawWrappedText() occuperebbe per lo stesso testo,
    // larghezza w e size, SENZA disegnare nulla - stessa identica logica
    // di wrap, usata per calcolare il layout prima di disegnare (es. i
    // fumetti dei messaggi in LoRaChatUI, o per posizionare un secondo
    // blocco di testo subito sotto il primo in GpsUI).
    int16_t wrappedTextHeight(const String& text, int16_t w, uint8_t size = 1);

    // Quante righe logiche (dopo il wrap) occuperebbe lo stesso testo -
    // wrappedTextHeight() diviso l'altezza di una riga, usato per lo
    // scroll "dentro" un messaggio troppo alto per lo schermo (02/10,
    // vedi OpenClawUI.cpp/MeshtasticUI.cpp).
    int wrappedLineCount(const String& text, int16_t w, uint8_t size = 1);

    // Come drawWrappedText(), ma disegna SOLO le righe logiche
    // [firstLine, firstLine+maxLines) invece di tutto il testo - righe
    // fuori range vengono contate (per restare coerenti col wrap) ma non
    // disegnate ne' occupano spazio nel disegno. Usata per scorrere
    // pixel per riga dentro un singolo messaggio piu' alto della vista
    // (02/10, bug segnalato: "se c'e' un messaggio troppo lungo non
    // posso fare scroll verso il basso" - prima lo scroll avanzava un
    // messaggio intero alla volta, la coda di un messaggio piu' alto
    // dello schermo restava irraggiungibile).
    void drawWrappedTextRange(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color, int firstLine, int maxLines, uint8_t size = 1);

    void begin();

    // Non ha piu' nulla da fare (LVGL rimosso il 26/09, vedi
    // project_lvgl_removal_rewrite in memoria: ogni schermata e' disegno
    // diretto, niente piu' timer/refresh da guidare qui) - lasciata per
    // i chiamanti che la invocano ancora ogni ciclo, harmless.
    void update();

    // Diagnostica schermo nero (2026-09-24): true se una prova di
    // allocazione in PSRAM (dentro begin()) e' andata a buon fine. Se
    // false, lo schermo resta "retroilluminato ma nero" - il print
    // d'errore e' troppo presto nel boot per catturarlo dal vivo in modo
    // affidabile (vedi project_crash_diagnosis_workflow sulla
    // ri-enumerazione USB), questo getter permette di leggerlo con calma
    // dal log periodico invece.
    bool areBuffersOk();

    // Schermata di boot
    void showBootScreen();

    // Schermata "teschio stile Matrix" mostrata per qualche secondo dopo
    // un riavvio anomalo, al posto del vecchio riepilogo testuale: i
    // dettagli veri (motivo, breadcrumb, coredump) continuano comunque a
    // essere salvati da CrashLog e leggibili in seguito (seriale/NVS),
    // qui e' solo un avviso visivo rapido "qualcosa e' crashato".
    void showCrashSkull();

    // Schermata generica titolo + messaggio, disegnata direttamente -
    // usata da qualunque modulo debba mostrare un avviso "usa e getta"
    // (radio disattivata, whitelist mancante, allarme NetGuard...).
    // onBack (opzionale, chiamato alla chiusura): Indietro/Select da
    // tastiera/trackball E tap sul bottone in basso lo richiamano allo
    // stesso modo (prima, con LVGL, solo il tap rispettava onBack -
    // Indietro fisico tornava sempre e solo al menu principale,
    // inconsistenza eliminata qui). Intercetta l'input PRIMA di
    // qualunque altro modulo finche' non viene chiusa - vedi
    // isMessageActive()/updateMessage(), richiamate da App::update().
    void showMessage(const char* title, const char* message, void (*onBack)() = nullptr);

    // true mentre un messaggio (showMessage) e' a schermo: chi guida il
    // loop principale deve, in questo caso, richiamare updateMessage()
    // al posto del dispatch normale verso le altre schermate.
    bool isMessageActive();

    // Da richiamare una volta per ciclo, SOLO mentre isMessageActive() e'
    // vera: gestisce tap/Indietro/Select e chiude il messaggio (chiamando
    // il suo onBack) quando uno dei tre arriva.
    void updateMessage();

    // true UNA VOLTA SOLA, subito dopo che un messaggio si e' chiuso (si
    // azzera leggendolo, come consumeValuesChanged() in BleGatt.h). Un
    // messaggio (es. l'allarme NetGuard, che puo' scattare da QUALUNQUE
    // schermata) disegna a piena schermata mentre e' a video, e chi lo
    // riceve non viene MAI chiamato durante quella finestra (isMessageActive()
    // ha priorita' assoluta nel dispatch di App::update()) - quindi non puo'
    // accorgersi da solo del fatto che il proprio schermo e' stato preso e
    // rilasciato altrove (es. dall'onBack del messaggio, che spesso naviga
    // altrove prima di tornare). Senza un segnale esplicito come questo, la
    // schermata che riprende il controllo puo' fare solo ridisegni
    // parziali, lasciando residui del messaggio o della schermata
    // intermedia (bug reale, segnalato dall'utente su NetGuardUI: righe del
    // popop d'allarme rimaste a schermo, sovrapposte al contenuto nuovo).
    // Ogni schermata con un proprio ciclo di ridisegno parziale deve
    // controllarlo nel proprio update() e forzare un ridisegno completo se
    // vero.
    bool consumeMessageJustDismissed();
}
