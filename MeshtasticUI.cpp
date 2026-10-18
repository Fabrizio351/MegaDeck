#include "MeshtasticUI.h"
#include "LoRaRadio.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "LoRaToolsMenu.h"
#include "GPS.h"
#include "Sound.h"

#include <Preferences.h>
#include <mbedtls/base64.h>
#include "tdeck_qrcode.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <mbedtls/aes.h>

// Vedi MeshtasticUI.h per il contesto. Livello fisico + header in
// chiaro + decifratura del canale pubblico di default + decodifica
// protobuf a mano dei tipi di messaggio piu' comuni (testo, info nodo,
// posizione) - vedi decodeMeshData() piu' sotto. Non ancora: NodeDB,
// routing/inoltro, canali personalizzati (serve la loro chiave).
namespace {

    bool active = false;       // true SOLO mentre la schermata e' in primo piano (gestisce touch/trackball/disegno)
    bool radioBorrowed = false; // true mentre teniamo la radio (primo piano O secondo piano)

    // Secondo piano (02/10, richiesto: "funzioni di default in primo
    // piano ma che all'occorrenza con un tasto possa passare in secondo
    // piano per poter fare altre operazioni col tdeck") - a differenza
    // di "active" (schermata in primo piano), questo sopravvive quando
    // si esce: finche' e' vero, uscire dalla schermata NON rilascia la
    // radio, che continua ad ascoltare e processare pacchetti via
    // backgroundUpdate() (chiamata sempre da App.cpp, non solo quando
    // Meshtastic e' la schermata attiva - stesso principio di
    // WebRadioUI::backgroundUpdate()). Si disattiva rientrando nella
    // chat e togliendolo esplicitamente, non da solo.
    bool backgroundEnabled = false;

    // true da quando arriva un messaggio mentre la schermata NON e' in
    // primo piano, finche' non la si riapre - guida l'icona LORA in alto
    // (rossa se vero) e il bip di notifica (Sound::playLoRaMessageReceived()).
    bool unread = false;

    // Dichiarata qui (definita molto piu' sotto) - serve a
    // rebuildVisibleMessages()/appendChatMessage(), che devono stare
    // vicino alla struct/array dei messaggi, molto prima nel file.
    uint8_t defaultChannelHash();

    enum class Mode {
        Main,
        Compose,
        NodeList,
        NodeDetail,
        ChannelList,
        ChannelConfig,
        ChannelEditField,
        ChannelQr,
        ChannelImport
    };
    Mode mode = Mode::Main;
    String composeBuffer;
    int composeCancelZone = -1;

    // Canali salvati (02/10) - struct/costante dichiarate qui in alto
    // perche' usate sia dalla lista canali (ChannelList, piu' sotto nel
    // file) sia dalla persistenza vera e propria (ensureChannelConfigLoaded()
    // e dintorni, ancora piu' sotto).
    constexpr int MAX_SAVED_CHANNELS = 5;
    struct SavedChannel {
        String name;
        String pskHex;
    };
    SavedChannel savedChannels[MAX_SAVED_CHANNELS];
    int savedChannelCount = 0;
    int activeChannelIndex = -1;

    // NodeDB minima (01/10, passo "chiudere Meshtastic"): non persistita,
    // solo i nodi visti in questa sessione - nome/posizione aggiornati
    // man mano che arrivano pacchetti decodificabili (canale di
    // default), "visto l'ultima volta" aggiornato per QUALSIASI
    // pacchetto da quel nodo anche se non decifrabile (sappiamo comunque
    // che quel NodeNum e' li').
    struct SeenNode {
        uint32_t nodeNum = 0;
        String longName;
        String shortName;
        bool hasPosition = false;
        double lat = 0.0;
        double lon = 0.0;
        uint32_t lastSeenMs = 0;
        bool used = false;
        // Dettaglio nodo (02/10, richiesto: "vista su tutti i dati che
        // eventualmente quel nodo sta trasmettendo") - ultimo canale/
        // RSSI/SNR visti e quanti pacchetti abbiamo ricevuto da lui.
        String lastChannel;
        float lastRssi = 0;
        float lastSnr = 0;
        uint32_t packetCount = 0;
        // Provenienza del pacchetto (16/10, richiesto esplicitamente per
        // motivi di sicurezza: "ne va della mia sicurezza" - sapere se un
        // nodo e' DAVVERO raggiungibile per radio vicino a noi, o se lo
        // stiamo vedendo solo perche' qualcuno lo rilancia su internet via
        // un bridge MQTT, nel qual caso potrebbe essere ovunque nel mondo
        // e NON nelle vicinanze). lastHopsAway = hop_start - hop_limit
        // residuo nell'ultimo pacchetto (0 = ricevuto diretto via radio,
        // senza nessun relay in mezzo), -1 se non calcolabile (hop_start
        // mancante/zero, pacchetti piu' vecchi).
        int lastHopsAway = -1;
        bool lastViaMqtt = false;

        // Telemetria (03/10, richiesto: "tutto il possibile se e'
        // traducibile senza vedere hash" nel dettaglio nodo) - SOLO in
        // RAM (la NodeDB non viene piu' persistita su flash dal 18/10,
        // vedi clearNodeDb()/open()): sono letture che invecchiano in
        // fretta (temperatura/umidita' di un minuto fa non ha senso
        // "congelarla" fino al prossimo avvio). Un flag di presenza per
        // OGNI campo - sono tutti "optional" nel protobuf reale, un nodo
        // manda solo cio' che il suo sensore misura davvero, zero non
        // distinguerebbe "non misurato" da "letto zero".
        bool hasBattLevel = false;
        uint32_t battLevel = 0;
        bool hasBattVoltage = false;
        float battVoltage = 0;
        bool hasChannelUtil = false;
        float channelUtil = 0;
        bool hasAirUtilTx = false;
        float airUtilTx = 0;
        bool hasUptimeSeconds = false;
        uint32_t uptimeSeconds = 0;
        bool hasTemperature = false;
        float temperature = 0;
        bool hasHumidity = false;
        float humidity = 0;
        bool hasPressure = false;
        float pressure = 0;
        bool hasGasResistance = false;
        float gasResistance = 0;
    };
    constexpr int MAX_NODES = 16;
    SeenNode seenNodes[MAX_NODES];
    int nodeListTopRow = 0;
    int nodeListDragAccumY = 0;

    // Scomparsa automatica dei nodi (16/10, richiesto: "se appare
    // qualcuno scompare o mi sposto ho la lista [aggiornata]") - prima
    // un nodo restava in lista PER SEMPRE una volta visto, con solo
    // "visto X fa" scritto a fianco senza nessuna conseguenza reale.
    // Meshtastic non ha mai avuto una scansione attiva in questo
    // firmware (i nodi si annunciano da soli, la radio ascolta sempre
    // in continuo) - questo e' il pezzo che mancava per far si' che la
    // lista rifletta davvero "chi sento ADESSO", non "chi ho mai
    // sentito dall'ultimo azzeramento": un nodo senza traffico da piu'
    // di NODE_STALE_TIMEOUT_MS si considera non piu' raggiungibile e
    // viene tolto. 30 minuti e' un compromesso: i nodi Meshtastic reali
    // mandano la loro posizione/nodeinfo a intervalli che possono
    // arrivare a 15+ minuti anche da fermi, un timeout piu' corto
    // farebbe scomparire e riapparire nodi fermi ma ancora vicini.
    constexpr uint32_t NODE_STALE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    uint32_t lastStaleCheckMs = 0;

    void expireStaleNodes() {
        uint32_t now = millis();
        for (int i = 0; i < MAX_NODES; i++) {
            if (seenNodes[i].used && (now - seenNodes[i].lastSeenMs) > NODE_STALE_TIMEOUT_MS) {
                seenNodes[i] = SeenNode();
            }
        }
    }

    // Azzera la NodeDB in RAM (16/10, bug segnalato dal vivo: cambiando
    // preset LongFast/MediumFast - spreading factor diverso, reti di
    // fatto diverse che normalmente non si sentono a vicenda - i nodi
    // vecchi restavano per sempre in lista e quelli nuovi del nuovo
    // preset non si vedevano mai, perche' la tabella (16 slot) non era
    // piena e l'LRU in findOrCreateNode() non aveva motivo di sostituire
    // nulla). NON tocca il file persistito su flash: se si torna al
    // preset precedente, al prossimo riavvio quei nodi si ritrovano.
    void clearNodeDb() {
        for (int i = 0; i < MAX_NODES; i++) {
            seenNodes[i] = SeenNode();
        }
        nodeListTopRow = 0;
    }

    // Trova lo slot del nodo, o ne crea uno nuovo (libero, o il meno
    // visto di recente se la tabella e' piena - un LRU semplice, non
    // serve altro per una sessione).
    int findOrCreateNode(uint32_t nodeNum) {
        for (int i = 0; i < MAX_NODES; i++) {
            if (seenNodes[i].used && seenNodes[i].nodeNum == nodeNum) {
                return i;
            }
        }
        int freeSlot = -1;
        int oldestSlot = 0;
        uint32_t oldestTime = 0xFFFFFFFF;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!seenNodes[i].used) {
                freeSlot = i;
                break;
            }
            if (seenNodes[i].lastSeenMs < oldestTime) {
                oldestTime = seenNodes[i].lastSeenMs;
                oldestSlot = i;
            }
        }
        int slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
        seenNodes[slot] = SeenNode();
        seenNodes[slot].nodeNum = nodeNum;
        seenNodes[slot].used = true;
        return slot;
    }

    void recordNodeSeen(uint32_t nodeNum, float rssi, float snr, const String& channel,
                        int hopsAway, bool viaMqtt) {
        int idx = findOrCreateNode(nodeNum);
        seenNodes[idx].lastSeenMs = millis();
        seenNodes[idx].lastRssi = rssi;
        seenNodes[idx].lastSnr = snr;
        seenNodes[idx].lastChannel = channel;
        seenNodes[idx].packetCount++;
        seenNodes[idx].lastHopsAway = hopsAway;
        seenNodes[idx].lastViaMqtt = viaMqtt;
    }

    void recordNodeInfo(uint32_t nodeNum, const String& longName, const String& shortName) {
        int idx = findOrCreateNode(nodeNum);
        if (longName.length() > 0) {
            seenNodes[idx].longName = longName;
        }
        if (shortName.length() > 0) {
            seenNodes[idx].shortName = shortName;
        }
        seenNodes[idx].lastSeenMs = millis();
    }

    void recordNodePosition(uint32_t nodeNum, double lat, double lon) {
        int idx = findOrCreateNode(nodeNum);
        seenNodes[idx].hasPosition = true;
        seenNodes[idx].lat = lat;
        seenNodes[idx].lon = lon;
        seenNodes[idx].lastSeenMs = millis();
    }

    // Quanti nodi/partecipanti abbiamo visto finora (01/10, richiesto per
    // la riga di stato semplificata) - conta le caselle in uso nel NodeDB.
    int nodeDbCount() {
        int count = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (seenNodes[i].used) {
                count++;
            }
        }
        return count;
    }

    // Persistenza NodeDB - COSTRUITA 02/10, RIMOSSA 18/10. L'idea era
    // far sopravvivere la lista nodi a un riavvio (struct POD
    // save/load via Preferences). L'utente ha poi chiarito il vero
    // principio di sicurezza alla base di tutto questo capitolo: dopo
    // QUALUNQUE interruzione dell'ascolto radio (cambio preset, chiusura
    // senza secondo piano, O un vero spegnimento) ci si potrebbe trovare
    // altrove, quindi i nodi di prima non sono mai piu' attendibili "per
    // default" - ripristinarli da flash a un riavvio era l'esatto
    // opposto di questo principio. Vedi clearNodeDb() e open() piu'
    // sotto: ora la lista riparte SEMPRE vuota quando la radio non era
    // gia' nostra ininterrottamente, riavvio compreso.

    // Storico messaggi (30/09 sera, richiesto: "stesso layout/
    // funzionamento della chat OpenClaw") - fumetti rettangolari ad
    // altezza variabile, bordo di colore diverso per inviati/ricevuti,
    // scroll con trascinamento o trackball. Copiato di proposito da
    // HermesUI.cpp (stesso schema, stessi nomi di funzione) invece di
    // reinventarlo - l'unica differenza reale e' che qui il mittente non
    // e' un valore binario "io/OpenClaw" ma il nodo Meshtastic che ha
    // generato l'evento, quindi ChatMessage porta un "sender" esplicito.
    struct MeshChatMessage {
        String sender;
        String text;
        bool outgoing;
        uint8_t channelHash = 0; // quale canale ha generato il messaggio
    };

    constexpr int MAX_MESSAGES = 20;

    // Storico COMBINATO di tutti i canali (02/10, canali multipli -
    // richiesto: "se scrivo e cambio chat i messaggi rimangono" - ogni
    // canale deve avere la sua chat separata, non un'unica lista mescolata)
    // - appendChatMessage() scrive sempre qui, taggando ogni messaggio con
    // l'hash del canale su cui e' davvero transitato. Un po' piu' ampio
    // del singolo canale visibile, cosi' switchare spesso non perde
    // subito la cronologia di un canale meno chiacchierone.
    constexpr int MAX_ALL_MESSAGES = 40;
    MeshChatMessage allMessages[MAX_ALL_MESSAGES];
    int allMessageCount = 0;

    // Vista filtrata sul SOLO canale attivo - tutto il resto del file
    // (scroll, render, conteggio partecipanti) continua a leggere/scrivere
    // qui esattamente come prima, senza saperne nulla dei canali multipli.
    MeshChatMessage messages[MAX_MESSAGES];
    int messageCount = 0;

    void rebuildVisibleMessages() {
        uint8_t activeHash = defaultChannelHash();
        messageCount = 0;
        for (int i = 0; i < allMessageCount && messageCount < MAX_MESSAGES; i++) {
            if (allMessages[i].channelHash == activeHash) {
                messages[messageCount++] = allMessages[i];
            }
        }
    }

    // Mittenti diversi comparsi nello storico chat attuale (01/10,
    // richiesto: NODI e PARTECIPANTI sono due numeri diversi - NODI e'
    // chiunque il NodeDB abbia rilevato via radio, PART e' chi ha
    // davvero scritto un messaggio visibile in questa chat, "Tu" incluso).
    int chatParticipantCount() {
        String seen[MAX_MESSAGES];
        int seenCount = 0;
        for (int i = 0; i < messageCount; i++) {
            const String& s = messages[i].sender;
            bool found = false;
            for (int j = 0; j < seenCount; j++) {
                if (seen[j] == s) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                seen[seenCount++] = s;
            }
        }
        return seenCount;
    }

    int topVisibleMsgIndex = 0;
    // Righe di testo del messaggio in cima gia' "scorse via" (02/10, bug
    // segnalato: "se c'e' un messaggio troppo lungo non posso fare
    // scroll verso il basso" - stesso fix di HermesUI.cpp). 0 = si
    // vede dall'inizio del messaggio.
    int topMsgLineOffset = 0;
    bool followingTail = true;
    int dragAccumY = 0;

    constexpr int16_t BOX_PAD = 6;
    constexpr int16_t BOX_GAP = 4;
    constexpr int16_t SCROLL_STEP_PX = 40;

    void appendChatMessage(const String& sender, const String& text, bool outgoing, uint8_t channelHash) {
        if (allMessageCount >= MAX_ALL_MESSAGES) {
            for (int i = 1; i < MAX_ALL_MESSAGES; i++) {
                allMessages[i - 1] = allMessages[i];
            }
            allMessageCount = MAX_ALL_MESSAGES - 1;
        }
        allMessages[allMessageCount].sender = sender;
        allMessages[allMessageCount].text = text;
        allMessages[allMessageCount].outgoing = outgoing;
        allMessages[allMessageCount].channelHash = channelHash;
        allMessageCount++;
        rebuildVisibleMessages();
    }

    // Parametri PHY dei preset "LongFast"/"MediumFast", regione EU_868
    // (presi dal sorgente reale di meshtastic/firmware, non da
    // documentazione secondaria - vedi MeshtasticUI.h). Frequenza,
    // banda, coding rate, sync word e preambolo sono IDENTICI in tutti i
    // preset "Fast" (Short/Medium/LongFast): cambia solo lo spreading
    // factor, che e' l'unico parametro reso selezionabile (vedi
    // mediumFastPreset/activeSpreadingFactor() piu' sotto).
    constexpr float MESH_FREQ_MHZ = 869.525f;
    constexpr float MESH_BANDWIDTH_KHZ = 250.0f;
    constexpr uint8_t MESH_SF_LONGFAST = 11;
    constexpr uint8_t MESH_SF_MEDIUMFAST = 9;
    constexpr uint8_t MESH_CR = 5;
    constexpr uint8_t MESH_SYNC_WORD = 0x2B;
    constexpr size_t MESH_PREAMBLE = 8;

    // false = LongFast (default storico, invariato per chi non tocca
    // l'impostazione), true = MediumFast - vedi MeshtasticUI.h. Caricato
    // da ensureChannelConfigLoaded()/loadChannelConfig() insieme ai
    // canali salvati (stessa chiave NVS "tdeckmesh"), perche' e' un'altra
    // scelta di identita' radio che va letta una volta sola per sessione.
    bool mediumFastPreset = false;

    uint8_t activeSpreadingFactor() {
        return mediumFastPreset ? MESH_SF_MEDIUMFAST : MESH_SF_LONGFAST;
    }

    // Nome del preset attivo - usato sia per l'hash del canale pubblico
    // (Channels::getName() nel sorgente reale ritorna il nome del preset
    // modem quando il campo nome del canale e' vuoto, vedi
    // activeChannelHashName()/publicChannelHash() piu' sotto) sia per la
    // riga "PRESET LORA" in Impostazioni.
    const char* activePresetName() {
        return mediumFastPreset ? "MediumFast" : "LongFast";
    }

    volatile bool packetFlag = false;

    void IRAM_ATTR onPacket() {
        packetFlag = true;
    }

    int packetCount = 0;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 20;
    // Riga SCRIVI/GPS (01/10, richiesto: "ANNUNCIA NODO" tolto, condividono
    // una riga sola, 2/3 + 1/3), e sotto una riga dedicata al secondo
    // piano (02/10, spostata qui dalla riga di stato - segnalato
    // dall'utente che li' era troppo vicina a "[B] INDIETRO" e rischiava
    // tap sbagliati) - altezza diversa apposta (16 invece di 20) per non
    // sembrare un terzo pulsante pari agli altri due.
    constexpr int16_t BG_BTN_H = 16;
    constexpr int16_t BG_BTN_Y = 216;
    constexpr int16_t SEND_BTN_Y = BG_BTN_Y - 4 - BTN_H;
    constexpr int16_t ROW_GAP = 4;
    constexpr int16_t WRITE_BTN_W = (BTN_W - ROW_GAP) * 2 / 3;
    constexpr int16_t GPS_BTN_X = BTN_X + WRITE_BTN_W + ROW_GAP;
    constexpr int16_t GPS_BTN_W = BTN_W - ROW_GAP - WRITE_BTN_W;
    int sendZone = -1;
    int positionZone = -1;
    int statusZone = -1; // tocca la riga di stato per aprire la lista nodi visti
    int titleZone = -1;  // tocca il titolo per aprire la configurazione del canale
    int bgZone = -1;     // tocca per mettere/togliere il secondo piano (02/10)

    // Focus da trackball per i 5 controlli della schermata Main che
    // prima erano raggiungibili SOLO a tocco (04/10, trovato nell'audit
    // pre-VoIP) - Sinistra/Destra spostano il fuoco, Select attiva
    // quello a fuoco (stesso schema gia' usato in GpsUI.cpp con
    // Focus::Refresh/Meteo/Park). Parte da Compose cosi' il
    // comportamento di sempre di Select (apre SCRIVI) resta identico
    // per chi non usa mai Sinistra/Destra qui.
    enum class MainFocus {
        Compose = 0,
        Gps,
        NodeList,
        ChannelList,
        Background
    };
    constexpr int MAIN_FOCUS_COUNT = 5;
    MainFocus mainFocus = MainFocus::Compose;

    constexpr int16_t TEXT_W = BTN_W - (BOX_PAD * 2) - 2;
    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 22;
    constexpr int16_t STATUS_H = 12;
    constexpr int16_t LIST_TOP = STATUS_Y + 14;
    constexpr int16_t LIST_BOTTOM = SEND_BTN_Y - 4;

    void closeMeshtastic() {
        active = false;
        // Se il secondo piano e' attivo (02/10) la radio NON si rilascia -
        // resta in ascolto, drenata da backgroundUpdate(). Altrimenti e'
        // l'uscita "piena" di sempre.
        if (!backgroundEnabled && radioBorrowed) {
            LoRaRadio::pauseForExternalUse();
            radioBorrowed = false;
        }
        LoRaToolsMenu::showMain();
    }

    // Tasto dedicato (02/10, richiesto esplicitamente: "con un tasto
    // possa passare in secondo piano") - attiva il secondo piano ed esce
    // in un solo gesto, cosi' la radio resta in ascolto mentre si fa
    // altro col T-Deck.
    void goToBackground() {
        backgroundEnabled = true;
        closeMeshtastic();
    }

    // Dichiarate qui, definite piu' sotto (usate anche da drawScreen(),
    // che viene prima nel file) - stesso "nodeIdText()" gia' usato per i
    // pacchetti ricevuti, cosi' il nostro nodo si legge nello stesso
    // formato "!xxxxxxxx".
    uint8_t defaultChannelHash();
    uint32_t ourNodeNum();
    String nodeIdText(uint32_t node);
    String activeChannelDisplayName();
    bool hasCustomChannel();
    String activeCustomChannelName();
    bool addAndActivateChannel(const String& name, const String& pskHex);
    void activateChannel(int index);
    void deleteSavedChannel(int index);
    bool channelKeyValid(const String& pskHex);
    void openChannelListScreen();
    void openChannelQrScreen(const String& name, const String& pskHex);
    void openChannelImportScreen();
    void openChannelCreateScreen();
    String activeChannelHashName();
    const uint8_t* activeChannelKey(size_t& outLen);
    size_t writeVarint(uint8_t* buf, size_t pos, uint32_t value);
    String applyChannelImportText(const String& text);
    bool sendChannelInvite(const String& name, const String& pskHex);
    uint8_t xorHash(const uint8_t* data, size_t len);
    uint8_t publicChannelHash();
    void drawChannelActionButton(int16_t x, int16_t y, int16_t w, int16_t h, const String& label, uint16_t color, bool focused);
    extern String pendingInviteUrl;
    void expireStaleInviteIfNeeded();
    bool sendChannelClaim(uint32_t inviteId);
    size_t parseHexBytes(const String& text, uint8_t* out, size_t outCapacity);
    void ensureChannelConfigLoaded();

    String buildMessageText(int index) {
        const MeshChatMessage& msg = messages[index];
        return msg.sender + ": " + msg.text;
    }

    int16_t messageBoxHeight(int index) {
        return Display::wrappedTextHeight(buildMessageText(index), TEXT_W) + (BOX_PAD * 2);
    }

    // Identica a HermesUI::computeMaxTopIndex() - quanti messaggi
    // (a partire da dove) riempiono la viewport partendo dall'ultimo,
    // per sapere dove fermare lo scroll verso il basso.
    int computeMaxTopIndex() {
        if (messageCount == 0) {
            return 0;
        }
        int16_t viewportH = LIST_BOTTOM - LIST_TOP;
        int i = messageCount - 1;
        int16_t used = 0;
        int firstVisible = i;

        while (i >= 0) {
            int16_t h = messageBoxHeight(i) + BOX_GAP;
            if (used + h > viewportH && firstVisible != i) {
                break;
            }
            used += h;
            firstVisible = i;
            i--;
        }
        return firstVisible;
    }

    // Quante righe di testo stanno nella vista messaggi, e di quante il
    // messaggio in cima la eccede (0 se ci sta per intero) - base dello
    // scroll "dentro" un messaggio (02/10, stesso schema di HermesUI.cpp).
    int visibleTextLines() {
        int16_t viewportH = LIST_BOTTOM - LIST_TOP - (BOX_PAD * 2);
        int lines = viewportH / 11; // 11 = altezza riga a size 1, vedi Display.cpp
        return lines > 0 ? lines : 1;
    }

    int topMsgOverflowLines() {
        if (messageCount == 0) {
            return 0;
        }
        int total = Display::wrappedLineCount(buildMessageText(topVisibleMsgIndex), TEXT_W);
        int overflow = total - visibleTextLines();
        return overflow > 0 ? overflow : 0;
    }

    bool scrollStepDown() {
        if (topMsgLineOffset < topMsgOverflowLines()) {
            topMsgLineOffset++;
            return true;
        }
        int maxTop = computeMaxTopIndex();
        if (topVisibleMsgIndex < maxTop) {
            topVisibleMsgIndex++;
            topMsgLineOffset = 0;
            return true;
        }
        return false;
    }

    bool scrollStepUp() {
        if (topMsgLineOffset > 0) {
            topMsgLineOffset--;
            return true;
        }
        if (topVisibleMsgIndex > 0) {
            topVisibleMsgIndex--;
            topMsgLineOffset = topMsgOverflowLines(); // si arriva "da sotto", parte dalla coda
            return true;
        }
        return false;
    }

    bool isAtBottom() {
        return topVisibleMsgIndex >= computeMaxTopIndex() && topMsgLineOffset >= topMsgOverflowLines();
    }

    void ensureBottomVisible() {
        topVisibleMsgIndex = computeMaxTopIndex();
        topMsgLineOffset = 0;
        followingTail = true;
    }

    void drawMessageBox(int index, int16_t y) {
        Arduino_GFX* gfx = Display::getGfx();
        String text = buildMessageText(index);
        int16_t boxH = messageBoxHeight(index);

        uint16_t borderColor = messages[index].outgoing
            ? Display::DD_ACCENT
            : gfx->color565(0x3D, 0xB8, 0xF5);

        gfx->drawRect(BTN_X, y, BTN_W, boxH, borderColor);
        Display::drawWrappedText(BTN_X + BOX_PAD, y + BOX_PAD, TEXT_W, text, Display::DD_TEXT);
    }

    void drawScreen() {
        expireStaleInviteIfNeeded();
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        // Titolo = nome della chat ATTIVA (01/10, richiesto: capire a colpo
        // d'occhio se sono sulla pubblica o su un canale mio/a cui mi sono
        // unito, non la scritta generica "MESHTASTIC") - tappabile, apre
        // la configurazione canale.
        {
            String chatTitle = hasCustomChannel() ? activeCustomChannelName() : "PUBBLICA";
            if (chatTitle.length() > 14) {
                chatTitle = chatTitle.substring(0, 13) + "~";
            }
            gfx->setTextColor(pendingInviteUrl.length() > 0 ? Display::DD_WARN : Display::DD_ACCENT);
            gfx->setCursor(8, Display::DD_HEADER_H + 8);
            gfx->print(chatTitle + (pendingInviteUrl.length() > 0 ? " > (INVITO!)" : " >"));
        }

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        // Riga di stato ridotta all'essenziale (01/10, richiesto: solo
        // identificativo + partecipanti, "can:"/"pacchetti:" tolti perche'
        // il canale e' gia' nel titolo sopra). Tappabile: apre la lista
        // dei nodi visti. Il secondo piano (02/10) NON sta piu' qui -
        // era troppo vicino a "[B] INDIETRO" (segnalato dall'utente,
        // rischio di tap sbagliati) - vedi la riga dedicata sotto
        // SCRIVI/GPS.
        {
            // Numeri colorati (01/10, richiesto): rosso se a zero, verde
            // se c'e' almeno un nodo/partecipante - colpo d'occhio senza
            // dover leggere la cifra.
            int nodiCount = nodeDbCount();
            int partCount = chatParticipantCount();

            gfx->setCursor(BTN_X, STATUS_Y);
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->print("ID: " + nodeIdText(ourNodeNum()) + " | NODI: ");

            gfx->setTextColor(nodiCount == 0 ? Display::DD_WARN : Display::DD_ACCENT);
            gfx->print(String(nodiCount));

            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->print(" | ATTIVI: ");

            gfx->setTextColor(partCount == 0 ? Display::DD_WARN : Display::DD_ACCENT);
            gfx->print(String(partCount));

            // Preset modem attivo (02/10, richiesto: vederlo a colpo
            // d'occhio sulla stessa riga di NODI/ATTIVI) - vedi
            // activePresetName()/mediumFastPreset sopra.
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->print(" [");
            gfx->print(activePresetName());
            gfx->print("] >");
        }

        // Lista messaggi (30/09 sera, stesso schema di HermesUI.cpp -
        // fumetti rettangolari ad altezza variabile, piu' recenti in
        // fondo, scorrevole).
        TouchUI::clearZones();
        statusZone = TouchUI::addZone(BTN_X, STATUS_Y - 2, BTN_W, STATUS_H);
        titleZone = TouchUI::addZone(0, Display::DD_HEADER_H, 220, Display::DD_HEADER_H);

        if (messageCount == 0) {
            Display::drawWrappedText(
                BTN_X, LIST_TOP + 4, TEXT_W,
                "Nessun messaggio ancora.\nScrivi qualcosa o ascolta la mesh.",
                Display::DD_TEXT_DIM
            );
        } else if (topMsgOverflowLines() > 0) {
            // Messaggio in cima piu' alto della vista (02/10) - occupa
            // tutto lo spazio disponibile mostrando solo la "finestra" di
            // righe scelta da topMsgLineOffset, stesso schema di
            // HermesUI.cpp.
            Arduino_GFX* gfx = Display::getGfx();
            uint16_t borderColor = messages[topVisibleMsgIndex].outgoing
                ? Display::DD_ACCENT
                : gfx->color565(0x3D, 0xB8, 0xF5);
            gfx->drawRect(BTN_X, LIST_TOP, BTN_W, LIST_BOTTOM - LIST_TOP, borderColor);
            Display::drawWrappedTextRange(
                BTN_X + BOX_PAD, LIST_TOP + BOX_PAD, TEXT_W,
                buildMessageText(topVisibleMsgIndex), Display::DD_TEXT,
                topMsgLineOffset, visibleTextLines()
            );
        } else {
            int16_t y = LIST_TOP;
            int i = topVisibleMsgIndex;
            while (i < messageCount) {
                int16_t h = messageBoxHeight(i);
                if (i != topVisibleMsgIndex && y + h > LIST_BOTTOM) {
                    break;
                }
                drawMessageBox(i, y);
                y += h + BOX_GAP;
                i++;
            }
        }

        gfx->drawRect(BTN_X, SEND_BTN_Y, WRITE_BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, SEND_BTN_Y + BTN_H / 2 - 3);
        gfx->print("[ SCRIVI E INVIA MESSAGGIO ]");

        bool gpsFix = GPS::hasFix();
        gfx->drawRect(GPS_BTN_X, SEND_BTN_Y, GPS_BTN_W, BTN_H, gpsFix ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setTextColor(gpsFix ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(GPS_BTN_X + 8, SEND_BTN_Y + BTN_H / 2 - 3);
        gfx->print("[ GPS ]");

        sendZone = TouchUI::addZone(BTN_X, SEND_BTN_Y, WRITE_BTN_W, BTN_H);
        positionZone = TouchUI::addZone(GPS_BTN_X, SEND_BTN_Y, GPS_BTN_W, BTN_H);

        // Secondo piano (02/10) - riga propria sotto SCRIVI/GPS, altezza
        // diversa apposta (BG_BTN_H=16 invece di BTN_H=20) per non sembrare
        // un terzo pulsante identico agli altri due - spostata qui dalla
        // riga di stato (era troppo vicina a "[B] INDIETRO", segnalato
        // dall'utente).
        gfx->drawRect(BTN_X, BG_BTN_Y, BTN_W, BG_BTN_H, backgroundEnabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setTextColor(backgroundEnabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 10, BG_BTN_Y + BG_BTN_H / 2 - 3);
        gfx->print(backgroundEnabled ? "[ ESEGUI MESHTASTIC IN BACKGROUND: ON ]" : "[ ESEGUI MESHTASTIC IN BACKGROUND: OFF ]");
        bgZone = TouchUI::addZone(BTN_X, BG_BTN_Y, BTN_W, BG_BTN_H);

        // Marcatore di fuoco per mainFocus (04/10) - stessa barretta
        // verticale accent usata ovunque in questo progetto per "riga/
        // cella a fuoco" (SettingsUI, liste nodi/canali, griglia stazioni
        // WebRadio), qui applicata ai 5 controlli che prima erano
        // raggiungibili solo a tocco.
        {
            int16_t fx, fy, fh;
            switch (mainFocus) {
                case MainFocus::ChannelList:
                    fx = 2; fy = Display::DD_HEADER_H + 2; fh = Display::DD_HEADER_H - 4;
                    break;
                case MainFocus::NodeList:
                    fx = BTN_X - 6; fy = STATUS_Y - 1; fh = STATUS_H;
                    break;
                case MainFocus::Gps:
                    fx = GPS_BTN_X - 6; fy = SEND_BTN_Y + 2; fh = BTN_H - 4;
                    break;
                case MainFocus::Background:
                    fx = BTN_X - 6; fy = BG_BTN_Y + 2; fh = BG_BTN_H - 4;
                    break;
                case MainFocus::Compose:
                default:
                    fx = BTN_X - 6; fy = SEND_BTN_Y + 2; fh = BTN_H - 4;
                    break;
            }
            gfx->fillRect(fx, fy, 4, fh, Display::DD_ACCENT);
        }
    }

    // Estratta dal tap su bgZone (04/10, cosi' la stessa azione e'
    // richiamabile anche da Select quando mainFocus==Background, niente
    // duplicazione - stessa filosofia di doChannelQrAction() e affini).
    void toggleMainBackground() {
        if (backgroundEnabled) {
            // Disattiva restando qui (02/10) - si ferma davvero alla
            // prossima uscita, non subito (si sta ancora guardando).
            backgroundEnabled = false;
            drawScreen();
        } else {
            goToBackground();
        }
    }

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 40;
    constexpr int16_t EDIT_FIELD_W = 288;
    constexpr int16_t EDIT_FIELD_H = 28;
    constexpr int MAX_MESSAGE_LEN = 180; // margine sotto ai 200 byte gestiti da showPacket()/il buffer di cifratura

    void openComposeScreen() {
        mode = Mode::Compose;
        composeBuffer = "";
        Input::setTextEntryMode(true);

        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("SCRIVI MESSAGGIO");

        TouchUI::clearZones();
        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, composeBuffer, false);
        Display::drawWrappedText(8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Canale pubblico di default, broadcast a tutti. Invio con Invio.", Display::DD_TEXT_DIM);

        gfx->drawRect(BTN_X, 200, BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, 200 + BTN_H / 2 - 3);
        gfx->print("[ ANNULLA ]");
        composeCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showMainScreen() {
        mode = Mode::Main;
        Input::setTextEntryMode(false);
        // Ricostruisce la vista filtrata sul canale ORA attivo (02/10) -
        // serve soprattutto dopo uno switch canale, dove nessun nuovo
        // messaggio arriva ad aggiornarla da solo. Innocuo/a costo zero
        // se il canale non e' cambiato.
        rebuildVisibleMessages();
        ensureBottomVisible();
        drawScreen();
    }

    bool sendDataPacket(uint32_t portnum, const uint8_t* innerPayload, size_t innerLen);

    void confirmCompose() {
        String trimmed = composeBuffer;
        trimmed.trim();
        if (trimmed.length() > 0) {
            bool ok = sendDataPacket(1, reinterpret_cast<const uint8_t*>(trimmed.c_str()), trimmed.length());
            appendChatMessage("Tu", ok ? trimmed : (trimmed + " (invio fallito)"), true, defaultChannelHash());
            if (followingTail) {
                ensureBottomVisible();
            }
        }
        showMainScreen();
    }

    void handleComposeInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == composeCancelZone) {
            showMainScreen();
            return;
        }
        TextInputUI::Result result = TextInputUI::handleKey(composeBuffer, MAX_MESSAGE_LEN);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, composeBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmCompose();
        }
    }

    // --- Lista nodi visti (01/10) - stesso schema di scroll a righe
    // fisse gia' usato in NetGuardUI.cpp (topVisibleRow/dragAccumY),
    // ordinata per "visto piu' di recente" in cima (insertion sort su un
    // indice, MAX_NODES e' piccolo quindi O(n^2) non pesa nulla).
    constexpr int16_t NODE_ROW_H = 24;
    constexpr int16_t NODE_LIST_TOP = Display::DD_HEADER_H + 24;
    constexpr int NODE_VISIBLE_ROWS = (200 - NODE_LIST_TOP) / NODE_ROW_H;

    int nodeOrder[MAX_NODES];
    int nodeOrderCount;
    // Riga evidenziata dal trackball (indice assoluto in nodeOrder[], non
    // relativo alla pagina visibile) e zone di tap per riga (02/10,
    // richiesto: apertura dettaglio nodo sia a touch che a trackball).
    int selectedNodeRow = 0;
    int nodeRowZone[NODE_VISIBLE_ROWS];
    void openNodeDetailScreen(int seenIdx);

    void buildNodeOrder() {
        nodeOrderCount = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (seenNodes[i].used) {
                nodeOrder[nodeOrderCount++] = i;
            }
        }
        for (int i = 1; i < nodeOrderCount; i++) {
            int key = nodeOrder[i];
            int j = i - 1;
            while (j >= 0 && seenNodes[nodeOrder[j]].lastSeenMs < seenNodes[key].lastSeenMs) {
                nodeOrder[j + 1] = nodeOrder[j];
                j--;
            }
            nodeOrder[j + 1] = key;
        }
    }

    String nodeAgeText(uint32_t lastSeenMs) {
        uint32_t ageMs = millis() - lastSeenMs;
        if (ageMs < 60000) {
            return String(ageMs / 1000) + "s fa";
        }
        if (ageMs < 3600000) {
            return String(ageMs / 60000) + "m fa";
        }
        return String(ageMs / 3600000) + "h fa";
    }

    // Uptime del NODO REMOTO (DeviceMetrics::uptime_seconds, 03/10) - NON
    // va confuso con nodeAgeText() sopra, che e' "quanto tempo fa l'abbiamo
    // sentito noi".
    String uptimeText(uint32_t seconds) {
        uint32_t days = seconds / 86400;
        uint32_t hours = (seconds % 86400) / 3600;
        uint32_t minutes = (seconds % 3600) / 60;
        if (days > 0) {
            return String(days) + "g " + String(hours) + "h";
        }
        if (hours > 0) {
            return String(hours) + "h " + String(minutes) + "m";
        }
        return String(minutes) + "m";
    }

    void drawNodeRow(int row) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = NODE_LIST_TOP + row * NODE_ROW_H;
        int absIndex = nodeListTopRow + row;
        bool selected = (absIndex == selectedNodeRow);
        gfx->fillRect(BTN_X, y, BTN_W, NODE_ROW_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, NODE_ROW_H - 2, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);

        const SeenNode& n = seenNodes[nodeOrder[absIndex]];
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 6, y + 3);
        String nameText = n.longName.length() > 0 ? n.longName : "(nome sconosciuto)";
        gfx->print(nodeIdText(n.nodeNum) + " " + nameText);

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 6, y + 13);
        String posText = n.hasPosition ? (String(n.lat, 4) + "," + String(n.lon, 4) + " - ") : "";
        gfx->print(posText + "visto " + nodeAgeText(n.lastSeenMs));

        nodeRowZone[row] = TouchUI::addZone(BTN_X, y, BTN_W, NODE_ROW_H - 2);
    }

    void drawNodeListScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("NODI VISTI (" + String(nodeOrderCount) + ")");
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        TouchUI::clearZones();

        if (nodeOrderCount == 0) {
            Display::drawWrappedText(BTN_X, NODE_LIST_TOP, BTN_W,
                "Nessun nodo visto finora - resta in ascolto nella schermata principale.",
                Display::DD_TEXT_DIM);
            return;
        }

        int shown = nodeOrderCount - nodeListTopRow;
        if (shown > NODE_VISIBLE_ROWS) {
            shown = NODE_VISIBLE_ROWS;
        }
        for (int row = 0; row < shown; row++) {
            drawNodeRow(row);
        }
    }

    void openNodeListScreen() {
        mode = Mode::NodeList;
        buildNodeOrder();
        nodeListTopRow = 0;
        nodeListDragAccumY = 0;
        selectedNodeRow = 0;
        Input::setTextEntryMode(false);
        drawNodeListScreen();
    }

    void handleNodeListInput() {
        int maxTop = nodeOrderCount - NODE_VISIBLE_ROWS;
        if (maxTop < 0) {
            maxTop = 0;
        }

        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && nodeOrderCount > 0) {
            nodeListDragAccumY += dragDelta;
            bool scrolled = false;
            while (nodeListDragAccumY >= NODE_ROW_H && nodeListTopRow > 0) {
                nodeListTopRow--;
                nodeListDragAccumY -= NODE_ROW_H;
                scrolled = true;
            }
            while (nodeListDragAccumY <= -NODE_ROW_H && nodeListTopRow < maxTop) {
                nodeListTopRow++;
                nodeListDragAccumY += NODE_ROW_H;
                scrolled = true;
            }
            if (scrolled) {
                drawNodeListScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped >= 0 && nodeOrderCount > 0) {
            for (int row = 0; row < NODE_VISIBLE_ROWS; row++) {
                if (tapped == nodeRowZone[row]) {
                    openNodeDetailScreen(nodeOrder[nodeListTopRow + row]);
                    return;
                }
            }
        }

        // Su/Giu' spostano la riga evidenziata (02/10, prima scorrevano e
        // basta) - la lista segue per tenerla sempre visibile.
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (selectedNodeRow > 0) {
                    selectedNodeRow--;
                    if (selectedNodeRow < nodeListTopRow) {
                        nodeListTopRow = selectedNodeRow;
                    }
                    drawNodeListScreen();
                }
                break;
            case InputEvent::Down:
                if (selectedNodeRow < nodeOrderCount - 1) {
                    selectedNodeRow++;
                    if (selectedNodeRow >= nodeListTopRow + NODE_VISIBLE_ROWS) {
                        nodeListTopRow = selectedNodeRow - NODE_VISIBLE_ROWS + 1;
                    }
                    drawNodeListScreen();
                }
                break;
            case InputEvent::Select:
                if (nodeOrderCount > 0) {
                    openNodeDetailScreen(nodeOrder[selectedNodeRow]);
                }
                break;
            case InputEvent::Back:
                showMainScreen();
                break;
            default:
                break;
        }
    }

    // --- Dettaglio nodo (02/10, richiesto: "vista su tutti i dati che
    // eventualmente quel nodo sta trasmettendo") - tutto cio' che il
    // NodeDB sa su un singolo nodo: nome, posizione, canale/RSSI/SNR
    // dell'ultimo pacchetto, quanti pacchetti ricevuti, da quanto non si
    // vede piu'. Sola lettura, un solo tasto per tornare alla lista. ---
    int nodeDetailSeenIdx = -1;
    // Scorrimento (03/10, aggiunto insieme alla telemetria - con tutti i
    // campi possibili non ci sta piu' tutto in una schermata sola) - SOLO
    // trackball Su/Giu', il tocco resta "ovunque tocchi, torni indietro"
    // come prima (TouchUI non distingue qui tap da drag, piu' semplice
    // non toccare quella zona esistente).
    int nodeDetailScrollOffset = 0;

    int nodeDetailBackZone = -1;

    void drawNodeDetailScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        // Tutto lo schermo e' tappabile per tornare indietro (02/10) -
        // schermata di sola lettura, non serve altro a touch.
        TouchUI::clearZones();
        nodeDetailBackZone = TouchUI::addZone(0, 0, 320, 240);

        if (nodeDetailSeenIdx < 0 || !seenNodes[nodeDetailSeenIdx].used) {
            gfx->setTextColor(Display::DD_WARN);
            gfx->setCursor(8, Display::DD_HEADER_H + 8);
            gfx->print("Nodo non piu' disponibile");
            return;
        }
        const SeenNode& n = seenNodes[nodeDetailSeenIdx];

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("NODO " + nodeIdText(n.nodeNum));
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        constexpr int16_t LINE_H = 24;
        constexpr int VISIBLE_ROWS = 7;

        // Righe costruite in un array invece che disegnate subito
        // (03/10, aggiunto con la telemetria) - con battery/tensione/uso
        // canale/uso TX/uptime/temperatura/umidita'/pressione/gas
        // possibili in piu' non ci sta tutto in una schermata sola come
        // prima (7 righe fisse arrivavano gia' quasi al fondo): va
        // costruita la lista completa PRIMA per sapere se e quanto
        // scorrere.
        struct DetailRow { const char* label; String value; bool warn; };
        constexpr int MAX_DETAIL_ROWS = 16;
        DetailRow rows[MAX_DETAIL_ROWS];
        int rowCount = 0;
        auto addRow = [&](const char* label, const String& value, bool warn = false) {
            if (rowCount < MAX_DETAIL_ROWS) {
                rows[rowCount++] = DetailRow{ label, value, warn };
            }
        };

        addRow("Nome lungo:", n.longName.length() > 0 ? n.longName : "(sconosciuto)", n.longName.length() == 0);
        addRow("Nome breve:", n.shortName.length() > 0 ? n.shortName : "(sconosciuto)", n.shortName.length() == 0);
        addRow("Posizione:", n.hasPosition ? (String(n.lat, 5) + ", " + String(n.lon, 5)) : "(nessuna ricevuta)", !n.hasPosition);
        addRow("Canale:", n.lastChannel.length() > 0 ? n.lastChannel : "sconosciuto", n.lastChannel.length() == 0);
        addRow("RSSI/SNR:", String(n.lastRssi, 0) + " dB / " + String(n.lastSnr, 1));
        // Provenienza (16/10, richiesto per sicurezza: distinguere un nodo
        // DAVVERO in zona radio da uno visto solo perche' rilanciato via
        // internet/MQTT, che potrebbe trovarsi ovunque) - riga in warning
        // (arancione) quando NON e' radio diretta, cosi' salta all'occhio.
        String viaText;
        bool viaWarn = false;
        if (n.lastViaMqtt) {
            viaText = "INTERNET (MQTT) - non e' per forza vicino!";
            viaWarn = true;
        } else if (n.lastHopsAway < 0) {
            viaText = "radio, salti sconosciuti";
        } else if (n.lastHopsAway == 0) {
            viaText = "radio diretta (0 salti)";
        } else {
            viaText = "radio, " + String(n.lastHopsAway) + " salti (rilanciato)";
            viaWarn = true;
        }
        addRow("Via:", viaText, viaWarn);
        addRow("Pacchetti:", String(n.packetCount));
        addRow("Visto:", nodeAgeText(n.lastSeenMs));
        // Telemetria (03/10, richiesto: "tutto il possibile se e'
        // traducibile senza vedere hash... temperature umidita etc") -
        // una riga solo per i campi che quel nodo ha DAVVERO mandato
        // (vedi i flag has* su SeenNode): un nodo manda quasi sempre o
        // DeviceMetrics o EnvironmentMetrics, raramente tutti i campi
        // insieme, quindi in pratica sono 1-4 righe in piu', non 9.
        if (n.hasBattLevel)     addRow("Batteria:", String(n.battLevel) + " %");
        if (n.hasBattVoltage)   addRow("Tensione:", String(n.battVoltage, 2) + " V");
        if (n.hasChannelUtil)   addRow("Uso canale:", String(n.channelUtil, 1) + " %");
        if (n.hasAirUtilTx)     addRow("Uso TX:", String(n.airUtilTx, 1) + " %");
        if (n.hasUptimeSeconds) addRow("Uptime nodo:", uptimeText(n.uptimeSeconds));
        if (n.hasTemperature)   addRow("Temperatura:", String(n.temperature, 1) + " C");
        if (n.hasHumidity)      addRow("Umidita':", String(n.humidity, 1) + " %");
        if (n.hasPressure)      addRow("Pressione:", String(n.pressure, 1) + " hPa");
        if (n.hasGasResistance) addRow("Gas resist.:", String(n.gasResistance, 2) + " MOhm");

        int maxOffset = (rowCount > VISIBLE_ROWS) ? (rowCount - VISIBLE_ROWS) : 0;
        if (nodeDetailScrollOffset > maxOffset) nodeDetailScrollOffset = maxOffset;
        if (nodeDetailScrollOffset < 0) nodeDetailScrollOffset = 0;

        int16_t y = Display::DD_HEADER_H + 34;
        for (int i = 0; i < VISIBLE_ROWS && (nodeDetailScrollOffset + i) < rowCount; i++) {
            const DetailRow& r = rows[nodeDetailScrollOffset + i];
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(BTN_X, y);
            gfx->print(r.label);
            gfx->setTextColor(r.warn ? Display::DD_WARN : Display::DD_ACCENT);
            gfx->setCursor(BTN_X + 90, y);
            gfx->print(r.value);
            y += LINE_H;
        }

        if (rowCount > VISIBLE_ROWS) {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(BTN_X, y + 2);
            gfx->print("SU/GIU: SCORRI (" + String(nodeDetailScrollOffset + 1) + "-" +
                String(min(nodeDetailScrollOffset + VISIBLE_ROWS, rowCount)) + "/" + String(rowCount) + ")");
        }
    }

    void openNodeDetailScreen(int seenIdx) {
        mode = Mode::NodeDetail;
        nodeDetailSeenIdx = seenIdx;
        nodeDetailScrollOffset = 0;
        Input::setTextEntryMode(false);
        drawNodeDetailScreen();
    }

    void handleNodeDetailInput() {
        int tapped = TouchUI::pollTap();
        InputEvent event = Input::getEvent();
        if (tapped == nodeDetailBackZone || event == InputEvent::Back || event == InputEvent::Select) {
            openNodeListScreen();
            return;
        }
        // Scorrimento SOLO trackball (03/10) - vedi il commento su
        // nodeDetailScrollOffset piu' sopra sul perche' non e' anche a
        // tocco. drawNodeDetailScreen() clampa da solo l'offset.
        if (event == InputEvent::Up) {
            nodeDetailScrollOffset--;
            drawNodeDetailScreen();
        } else if (event == InputEvent::Down) {
            nodeDetailScrollOffset++;
            drawNodeDetailScreen();
        }
    }

    // --- Configurazione canale (01/10) - stesso schema "lista di righe
    // modificabili" gia' usato in LoRaWanUI.cpp per Join EUI/Dev EUI/App
    // Key: due campi (nome canale, chiave esadecimale), tocca una riga
    // per modificarla. AZZERA torna al canale pubblico di default.
    enum class ChannelField { Name, Psk };
    constexpr int CHANNEL_FIELD_COUNT = 2;
    int selectedChannelRow = 0;
    ChannelField editingChannelField = ChannelField::Name;
    String channelEditBuffer;

    // Bozza di un canale NUOVO (02/10) - "+ NUOVO CANALE" parte sempre da
    // zero (non si modifica un canale gia' salvato in-place, se ne crea
    // uno nuovo), svuotata ogni volta che si apre la schermata di
    // creazione, preservata quando si torna qui da una sotto-schermata
    // (ANNULLA di un singolo campo).
    String draftChannelName;
    String draftChannelPskHex;

    void clearDraft() {
        draftChannelName = "";
        draftChannelPskHex = "";
    }
    int channelRowZone[CHANNEL_FIELD_COUNT];
    int channelResetZone = -1;
    int channelValidateZone = -1;
    int channelEditCancelZone = -1;
    int channelGenerateZone = -1;
    int channelQrZone = -1;
    QRCode qrChannelCode;
    uint8_t* qrChannelModules = nullptr;

    int channelImportZone = -1;
    int channelInviteZone = -1;
    int channelImportCancelZone = -1;
    String channelImportBuffer;

    // Invito canale ricevuto in chat (01/10) - "" se nessuno, altrimenti
    // l'URL dell'ultimo invito riconosciuto (vedi showPacket()) - un
    // pulsante sulla schermata canale appare solo quando questo non e'
    // vuoto. "Usa e getta" (01/10, richiesto esplicitamente):
    // pendingInviteId e' l'identificativo casuale di QUEL preciso
    // invito, usato per 2 cose - (1) farlo scadere da solo dopo
    // INVITE_TTL_MS anche se nessuno lo tocca, (2) riconoscere un
    // messaggio "CLAIM:<id>" di un altro dispositivo che l'ha gia'
    // importato e scartare la nostra copia (best-effort "primo arrivato,
    // primo servito" - non una garanzia assoluta su un mezzo broadcast).
    String pendingInviteUrl;
    uint32_t pendingInviteId = 0;
    uint32_t pendingInviteReceivedMs = 0;
    constexpr uint32_t INVITE_TTL_MS = 10UL * 60UL * 1000UL; // 10 minuti

    void expireStaleInviteIfNeeded() {
        if (pendingInviteUrl.length() > 0 && millis() - pendingInviteReceivedMs > INVITE_TTL_MS) {
            pendingInviteUrl = "";
            pendingInviteId = 0;
        }
    }

    constexpr int16_t CH_ROW_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t CH_ROW_H = 36;

    const char* channelFieldLabel(ChannelField f) {
        switch (f) {
            case ChannelField::Name: return "NOME CANALE";
            case ChannelField::Psk:  return "CHIAVE (hex, 32 o 64 cifre)";
        }
        return "?";
    }

    String channelFieldValue(ChannelField f) {
        switch (f) {
            case ChannelField::Name: return draftChannelName;
            case ChannelField::Psk:  return draftChannelPskHex;
        }
        return "";
    }

    void drawChannelConfigRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        ChannelField f = static_cast<ChannelField>(index);
        int16_t y = CH_ROW_TOP + index * CH_ROW_H;
        bool selected = (index == selectedChannelRow);

        gfx->fillRect(BTN_X, y, BTN_W, CH_ROW_H - 4, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, CH_ROW_H - 4, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        if (selected) {
            gfx->fillRect(BTN_X + 3, y + 4, 4, CH_ROW_H - 12, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 12, y + 4);
        gfx->print(channelFieldLabel(f));

        String value = channelFieldValue(f);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + 18);
        gfx->print(value.length() > 0 ? value : "(vuoto - usa il default pubblico)");
    }

    // "+ NUOVO CANALE" (02/10, canali multipli) - sempre una bozza vuota,
    // CREA aggiunge una riga nuova all'elenco e la attiva, ANNULLA scarta
    // e torna alla lista senza salvare nulla.
    void drawChannelConfigScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("NUOVO CANALE");
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        TouchUI::clearZones();
        for (int i = 0; i < CHANNEL_FIELD_COUNT; i++) {
            drawChannelConfigRow(i);
            int16_t y = CH_ROW_TOP + i * CH_ROW_H;
            channelRowZone[i] = TouchUI::addZone(BTN_X, y, BTN_W, CH_ROW_H - 4);
        }

        constexpr int16_t CH_ACTION_H = 24;
        constexpr int16_t CH_HALF_GAP = 4;
        constexpr int16_t CH_HALF_W = (BTN_W - CH_HALF_GAP) / 2;
        int16_t y = CH_ROW_TOP + CHANNEL_FIELD_COUNT * CH_ROW_H + 4;

        // CREA/ANNULLA ora raggiungibili anche a trackball (02/10) - Su/Giu'
        // prosegue oltre i campi fino a questi due, Select li attiva.
        drawChannelActionButton(BTN_X, y, CH_HALF_W, CH_ACTION_H, "[ CREA ]", Display::DD_ACCENT, selectedChannelRow == CHANNEL_FIELD_COUNT + 0);
        channelValidateZone = TouchUI::addZone(BTN_X, y, CH_HALF_W, CH_ACTION_H);

        int16_t cancelX = BTN_X + CH_HALF_W + CH_HALF_GAP;
        drawChannelActionButton(cancelX, y, CH_HALF_W, CH_ACTION_H, "[ ANNULLA ]", Display::DD_WARN, selectedChannelRow == CHANNEL_FIELD_COUNT + 1);
        channelResetZone = TouchUI::addZone(cancelX, y, CH_HALF_W, CH_ACTION_H);
    }

    // Ritorno "interno" (da ANNULLA del singolo campo, o da confirmChannelEdit()
    // dopo aver scritto nella bozza) - NON tocca la bozza.
    void showChannelConfigScreen() {
        mode = Mode::ChannelConfig;
        selectedChannelRow = 0;
        Input::setTextEntryMode(false);
        drawChannelConfigScreen();
    }

    // Ingresso "fresco" da "+ NUOVO CANALE" nella lista - parte sempre da
    // una bozza vuota.
    void openChannelCreateScreen() {
        clearDraft();
        showChannelConfigScreen();
    }

    void drawChannelEditScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print(channelFieldLabel(editingChannelField));

        TouchUI::clearZones();
        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, channelEditBuffer, false);
        Display::drawWrappedText(8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            editingChannelField == ChannelField::Psk
                ? "16 byte (32 cifre, AES-128) o 32 byte (64 cifre, AES-256) - dal pannello del tuo network/canale Meshtastic."
                : "Nome esatto del canale, come configurato sugli altri nodi.",
            Display::DD_TEXT_DIM);

        channelGenerateZone = -1;
        if (editingChannelField == ChannelField::Psk) {
            // Per CREARE un canale nuovo (non solo unirsi a uno esistente)
            // serve poter generare una chiave vera, non inventarsela come
            // una password - richiesto 01/10 dopo che l'utente ha
            // provato a digitarne una a mano.
            gfx->drawRect(BTN_X, 174, BTN_W, BTN_H, Display::DD_ACCENT);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(BTN_X + 12, 174 + BTN_H / 2 - 3);
            gfx->print("[ GENERA CHIAVE CASUALE (AES-128) ]");
            channelGenerateZone = TouchUI::addZone(BTN_X, 174, BTN_W, BTN_H);
        }

        gfx->drawRect(BTN_X, 200, BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, 200 + BTN_H / 2 - 3);
        gfx->print("[ ANNULLA ]");
        channelEditCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showChannelEditScreen(ChannelField field) {
        mode = Mode::ChannelEditField;
        editingChannelField = field;
        channelEditBuffer = channelFieldValue(field);
        Input::setTextEntryMode(true);
        drawChannelEditScreen();
    }

    void confirmChannelEdit() {
        String trimmed = channelEditBuffer;
        trimmed.trim();
        if (editingChannelField == ChannelField::Psk) {
            trimmed.toUpperCase();
        }

        // Scrive solo nella BOZZA del canale che si sta creando (02/10) -
        // niente di attivo finche' non si preme CREA nella schermata
        // "NUOVO CANALE", cosi' modificare un campo non crea a meta' un
        // canale incompleto. La chiave viene comunque controllata subito
        // (formato/lunghezza) cosi' l'errore si vede appena digitata.
        bool pskValid = true;
        if (editingChannelField == ChannelField::Name) {
            draftChannelName = trimmed;
        } else {
            draftChannelPskHex = trimmed;
            if (trimmed.length() > 0) {
                uint8_t tmp[32];
                size_t parsedLen = parseHexBytes(trimmed, tmp, sizeof(tmp));
                pskValid = (parsedLen == 16 || parsedLen == 32);
            }
        }

        showChannelConfigScreen();

        if (!pskValid) {
            // drawChannelConfigScreen() nel callback, non showChannelConfigScreen()
            // (04/10): quella resetterebbe di nuovo selectedChannelRow a 0 e
            // text-entry-mode, inutile - lo stato giusto e' GIA' impostato
            // dalla chiamata subito sopra, qui serve solo un ridisegno dopo
            // la chiusura del popup.
            Display::showMessage(
                "CANALE",
                "Chiave non valida - deve essere lunga esattamente 32 cifre (AES-128) o 64 cifre (AES-256), solo 0-9 e A-F.",
                []() { drawChannelConfigScreen(); }
            );
        }
    }

    // --- QR condivisibile (01/10) - formato REALE di Meshtastic
    // (meshtastic.org/e/#<base64url(ChannelSet)>), scansionabile
    // dall'app ufficiale: niente di inventato, letto dagli .proto reali
    // (meshtastic/protobufs, apponly.proto + channel.proto + config.proto):
    //   ChannelSet:      settings=1 (ChannelSettings, embedded),
    //                    lora_config=2 (Config.LoRaConfig, embedded)
    //   ChannelSettings: psk=2 (bytes), name=3 (string)
    //   LoRaConfig:      use_preset=1 (bool), region=7 (enum, EU_868=3)
    // modem_preset (campo 2) omesso: il default LONG_FAST e' lo 0, e in
    // protobuf un campo col valore di default puo' restare non scritto.

    // Nome+chiave passati espliciti (02/10, canali multipli: il QR/invito
    // deve poter condividere QUALSIASI canale salvato, selezionato nella
    // lista, non solo quello attivo in quel momento).
    size_t buildChannelSettingsProto(uint8_t* out, const String& name, const uint8_t* key, size_t keyLen) {
        size_t pos = 0;
        out[pos++] = 0x12; // tag campo 2 (psk), wire type 2
        pos = writeVarint(out, pos, keyLen);
        memcpy(out + pos, key, keyLen);
        pos += keyLen;
        out[pos++] = 0x1A; // tag campo 3 (name), wire type 2
        pos = writeVarint(out, pos, name.length());
        memcpy(out + pos, name.c_str(), name.length());
        pos += name.length();
        return pos;
    }

    size_t buildLoRaConfigProto(uint8_t* out) {
        size_t pos = 0;
        out[pos++] = 0x08; // tag campo 1 (use_preset), wire type 0 (varint)
        out[pos++] = 0x01; // true
        out[pos++] = 0x38; // tag campo 7 (region), wire type 0 (varint) - (7<<3)|0
        out[pos++] = 0x03; // EU_868
        return pos;
    }

    // Ritorna la lunghezza scritta, 0 se non basta lo spazio (nomi/
    // chiavi molto lunghi - capita solo con AES-256 e un nome al limite).
    size_t buildChannelSetProto(uint8_t* out, size_t capacity, const String& name, const uint8_t* key, size_t keyLen) {
        uint8_t settingsBuf[80];
        size_t settingsLen = buildChannelSettingsProto(settingsBuf, name, key, keyLen);
        uint8_t loraBuf[8];
        size_t loraLen = buildLoRaConfigProto(loraBuf);

        size_t pos = 0;
        if (pos + 2 + settingsLen > capacity) return 0;
        out[pos++] = 0x0A; // tag campo 1 (settings), wire type 2
        pos = writeVarint(out, pos, settingsLen);
        memcpy(out + pos, settingsBuf, settingsLen);
        pos += settingsLen;

        if (pos + 2 + loraLen > capacity) return 0;
        out[pos++] = 0x12; // tag campo 2 (lora_config), wire type 2
        pos = writeVarint(out, pos, loraLen);
        memcpy(out + pos, loraBuf, loraLen);
        pos += loraLen;

        return pos;
    }

    // Base64 URL-safe (RFC 4648 sez.5): '+'->'-', '/'->'_', niente '='
    // finale - mbedtls fa solo il base64 "standard", la conversione e'
    // un post-processo di due sostituzioni + un taglio.
    String base64UrlEncode(const uint8_t* data, size_t len) {
        size_t outLen = 0;
        mbedtls_base64_encode(nullptr, 0, &outLen, data, len); // dlen=0 -> solo calcola la lunghezza
        uint8_t* buf = static_cast<uint8_t*>(malloc(outLen + 1));
        if (buf == nullptr) {
            return "";
        }
        size_t written = 0;
        mbedtls_base64_encode(buf, outLen, &written, data, len);
        buf[written] = 0;

        String result;
        for (size_t i = 0; i < written; i++) {
            char c = static_cast<char>(buf[i]);
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
            else if (c == '=') continue; // niente padding nell'URL-safe
            result += c;
        }
        free(buf);
        return result;
    }

    String buildChannelShareUrl(const String& name, const uint8_t* key, size_t keyLen) {
        uint8_t channelSetBuf[100];
        size_t len = buildChannelSetProto(channelSetBuf, sizeof(channelSetBuf), name, key, keyLen);
        if (len == 0) {
            return "";
        }
        return "https://meshtastic.org/e/#" + base64UrlEncode(channelSetBuf, len);
    }

    // Libreria QRCode di ricmoo/Nayuki, vendorizzata in libraries/QRCode
    // (file rinominati tdeck_qrcode.*: il core arduino-esp32 ha un
    // "qrcode.h" fantasma - componente ESP-IDF "espressif__qrcode" il
    // cui .h e' presente ma il cui codice compilato NON e' linkato in
    // questa distribuzione, verificato con nm - usare quel nome avrebbe
    // fatto risolvere l'include a quello sbagliato).
    constexpr uint8_t QR_VERSION = 8; // 49x49 moduli, capacita' ampiamente sufficiente per il nostro URL
    constexpr uint8_t QR_ECC = ECC_LOW;

    void drawChannelQrScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("QR CANALE");
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        TouchUI::clearZones();

        if (qrChannelModules == nullptr) {
            Display::drawWrappedText(BTN_X, Display::DD_HEADER_H + 40, BTN_W,
                "Impossibile generare il QR (URL troppo lungo per questa versione) - accorcia nome/chiave del canale.",
                Display::DD_WARN);
            return;
        }

        int size = qrChannelCode.size;
        int16_t areaW = 300;
        int16_t areaH = 190;
        int16_t scale = min(areaW / size, areaH / size);
        if (scale < 1) {
            scale = 1;
        }
        int16_t qrPx = size * scale;
        int16_t originX = (320 - qrPx) / 2;
        int16_t originY = Display::DD_HEADER_H + 24;

        gfx->fillRect(originX - 4, originY - 4, qrPx + 8, qrPx + 8, Display::DD_TEXT); // quiet zone bianca
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                if (qrcode_getModule(&qrChannelCode, x, y)) {
                    gfx->fillRect(originX + x * scale, originY + y * scale, scale, scale, Display::DD_BG);
                }
            }
        }
    }

    void openChannelQrScreen(const String& name, const String& pskHex) {
        mode = Mode::ChannelQr;
        Input::setTextEntryMode(false);

        if (qrChannelModules != nullptr) {
            free(qrChannelModules);
            qrChannelModules = nullptr;
        }

        uint8_t key[32];
        size_t keyLen = parseHexBytes(pskHex, key, sizeof(key));
        String url = (keyLen == 16 || keyLen == 32) ? buildChannelShareUrl(name, key, keyLen) : String();
        if (url.length() > 0) {
            uint16_t bufSize = qrcode_getBufferSize(QR_VERSION);
            qrChannelModules = static_cast<uint8_t*>(malloc(bufSize));
            if (qrChannelModules != nullptr) {
                int8_t result = qrcode_initText(&qrChannelCode, qrChannelModules, QR_VERSION, QR_ECC, url.c_str());
                if (result != 0) {
                    free(qrChannelModules);
                    qrChannelModules = nullptr;
                }
            }
        }

        drawChannelQrScreen();
    }

    void closeChannelQrScreen() {
        if (qrChannelModules != nullptr) {
            free(qrChannelModules);
            qrChannelModules = nullptr;
        }
        openChannelListScreen();
    }

    void handleChannelQrInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::Back || event == InputEvent::Select) {
            closeChannelQrScreen();
        }
    }

    // --- Import canale: incolla a mano, o un invito ricevuto gia'
    // pronto in pendingInviteUrl (01/10) - stesso campo di testo per
    // entrambi i casi, precompilato quando c'e' un invito in attesa. ---

    void drawChannelImportScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("IMPORTA CANALE");

        TouchUI::clearZones();
        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, channelImportBuffer, false);
        Display::drawWrappedText(8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Incolla il link meshtastic.org/e/#... condiviso, poi Invio.",
            Display::DD_TEXT_DIM);

        gfx->drawRect(BTN_X, 200, BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, 200 + BTN_H / 2 - 3);
        gfx->print("[ ANNULLA ]");
        channelImportCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void openChannelImportScreen() {
        expireStaleInviteIfNeeded();
        mode = Mode::ChannelImport;
        channelImportBuffer = pendingInviteUrl; // vuoto se non c'e' nessun invito in attesa
        Input::setTextEntryMode(true);
        drawChannelImportScreen();
    }

    void confirmChannelImport() {
        // "Usa e getta" (01/10): solo se il testo importato e' ESATTAMENTE
        // l'invito ricevuto (non un link incollato a mano) mandiamo il
        // claim - altrimenti un copia-incolla manuale scatenerebbe un
        // claim per un invito che nessuno ha mai ricevuto via radio.
        bool wasPendingInvite = (pendingInviteUrl.length() > 0 && channelImportBuffer == pendingInviteUrl);
        uint32_t inviteIdToClaim = pendingInviteId;

        String error = applyChannelImportText(channelImportBuffer);
        pendingInviteUrl = ""; // consumato, che sia andata bene o male
        pendingInviteId = 0;

        if (wasPendingInvite && error.length() == 0) {
            sendChannelClaim(inviteIdToClaim);
        }

        // Torna direttamente alla chat (01/10) - stesso motivo di
        // confirmChannelEdit(): si deve vedere subito il titolo della chat
        // attiva dopo l'import.
        showMainScreen();
        if (error.length() > 0) {
            Display::showMessage("CANALE", error.c_str(), []() { drawScreen(); });
        }
    }

    void handleChannelImportInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == channelImportCancelZone) {
            openChannelListScreen();
            return;
        }
        TextInputUI::Result result = TextInputUI::handleKey(channelImportBuffer, 150);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, channelImportBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmChannelImport();
        }
    }

    void doChannelCreateAction() {
        // CREA (02/10): nome+chiave devono essere entrambi presenti e
        // validi - un canale non puo' esistere "a meta'".
        if (draftChannelName.length() == 0 || draftChannelPskHex.length() == 0) {
            Display::showMessage("CANALE", "Serve sia il nome che la chiave per creare un canale.", []() { drawChannelConfigScreen(); });
            return;
        }
        if (!channelKeyValid(draftChannelPskHex)) {
            Display::showMessage("CANALE", "Chiave non valida - deve essere lunga esattamente 32 cifre (AES-128) o 64 cifre (AES-256), solo 0-9 e A-F.", []() { drawChannelConfigScreen(); });
            return;
        }
        if (!addAndActivateChannel(draftChannelName, draftChannelPskHex)) {
            String msg = "Elenco canali pieno (max " + String(MAX_SAVED_CHANNELS) + ") - elimina un canale salvato prima di crearne uno nuovo.";
            Display::showMessage("CANALE", msg.c_str(), []() { drawChannelConfigScreen(); });
            return;
        }
        showMainScreen();
    }

    void handleChannelConfigInput() {
        int tapped = TouchUI::pollTap();
        for (int i = 0; i < CHANNEL_FIELD_COUNT; i++) {
            if (channelRowZone[i] == tapped) {
                showChannelEditScreen(static_cast<ChannelField>(i));
                return;
            }
        }
        if (tapped == channelValidateZone) {
            doChannelCreateAction();
            return;
        }
        if (tapped == channelResetZone) {
            // ANNULLA (02/10): scarta la bozza, torna alla lista - non
            // tocca il canale attivo.
            openChannelListScreen();
            return;
        }

        // Su/Giu' proseguono oltre i 2 campi fino a CREA/ANNULLA (02/10,
        // prima raggiungibili solo a touch) - Select attiva il campo o
        // il pulsante a seconda di dove si trova il fuoco.
        constexpr int totalSlots = CHANNEL_FIELD_COUNT + 2;
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (selectedChannelRow > 0) {
                    selectedChannelRow--;
                    drawChannelConfigScreen();
                }
                break;
            case InputEvent::Down:
                if (selectedChannelRow < totalSlots - 1) {
                    selectedChannelRow++;
                    drawChannelConfigScreen();
                }
                break;
            case InputEvent::Select:
                if (selectedChannelRow < CHANNEL_FIELD_COUNT) {
                    showChannelEditScreen(static_cast<ChannelField>(selectedChannelRow));
                } else if (selectedChannelRow == CHANNEL_FIELD_COUNT) {
                    doChannelCreateAction();
                } else {
                    openChannelListScreen();
                }
                break;
            case InputEvent::Back:
                openChannelListScreen();
                break;
            default:
                break;
        }
    }

    // --- Lista canali (02/10, canali multipli - richiesto esplicitamente:
    // "vedere le chat a cui sono iscritto, inclusa la pubblica, e passare
    // dall'una all'altra senza perderle") - Pubblica sempre in cima (riga
    // 0, indice canale -1), poi i canali salvati. Tap su una riga = passa
    // a quel canale e torna alla chat. Le azioni sotto (NUOVO/IMPORTA/QR/
    // INVITA/ELIMINA) agiscono sulla riga EVIDENZIATA (selectedChannelListRow),
    // non per forza quella attiva, cosi' si puo' es. vedere il QR di un
    // canale senza doverci prima passare.
    //
    // Navigabile interamente a trackball (02/10, verificato esplicitamente
    // su richiesta - "verificare che tutto sia navigabile da touch e
    // trackball"): channelFocusSlot scorre righe canale E pulsanti azione
    // in un'unica sequenza (Su/Giu'), Select attiva la riga o l'azione a
    // seconda di dove si trova il fuoco - prima le azioni erano
    // raggiungibili solo a touch. ---
    int selectedChannelListRow = 0;
    constexpr int CH_ACTION_COUNT = 5; // NUOVO, IMPORTA, QR, INVITA, ELIMINA
    int channelFocusSlot = 0;
    int channelListRowZone[MAX_SAVED_CHANNELS + 1];
    int channelNewZone = -1;
    int channelDeleteZone = -1;

    void drawChannelActionButton(int16_t x, int16_t y, int16_t w, int16_t h, const String& label, uint16_t color, bool focused) {
        Arduino_GFX* gfx = Display::getGfx();
        if (focused) {
            gfx->fillRect(x, y, w, h, color);
            gfx->setTextColor(Display::DD_BG);
        } else {
            gfx->drawRect(x, y, w, h, color);
            gfx->setTextColor(color);
        }
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setCursor(x + 8, y + h / 2 - 3);
        gfx->print(label);
    }

    constexpr int16_t CHLIST_ROW_TOP = Display::DD_HEADER_H + 26;
    constexpr int16_t CHLIST_ROW_H = 20;

    String channelListRowName(int row) {
        return row == 0 ? "Pubblica" : savedChannels[row - 1].name;
    }

    void drawChannelListRow(int row) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = CHLIST_ROW_TOP + row * CHLIST_ROW_H;
        bool selected = (row == selectedChannelListRow);
        bool isActive = ((row - 1) == activeChannelIndex);

        gfx->fillRect(BTN_X, y, BTN_W, CHLIST_ROW_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, CHLIST_ROW_H - 2, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(isActive ? Display::DD_ACCENT : Display::DD_TEXT);
        gfx->setCursor(BTN_X + 8, y + 4);
        gfx->print(String(isActive ? "> " : "  ") + channelListRowName(row));

        channelListRowZone[row] = TouchUI::addZone(BTN_X, y, BTN_W, CHLIST_ROW_H - 2);
    }

    void drawChannelListScreen() {
        expireStaleInviteIfNeeded();
        ensureChannelConfigLoaded();
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("CANALI");
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        TouchUI::clearZones();

        int rowCount = savedChannelCount + 1;
        for (int row = 0; row < rowCount; row++) {
            drawChannelListRow(row);
        }

        constexpr int16_t CH_ACTION_H = 22;
        constexpr int16_t CH_ACTION_GAP = 2;
        constexpr int16_t CH_HALF_GAP = 4;
        constexpr int16_t CH_HALF_W = (BTN_W - CH_HALF_GAP) / 2;
        int16_t y = CHLIST_ROW_TOP + rowCount * CHLIST_ROW_H + 4;

        drawChannelActionButton(BTN_X, y, CH_HALF_W, CH_ACTION_H, "[ + NUOVO ]", Display::DD_ACCENT, channelFocusSlot == rowCount + 0);
        channelNewZone = TouchUI::addZone(BTN_X, y, CH_HALF_W, CH_ACTION_H);

        bool hasPendingInvite = pendingInviteUrl.length() > 0;
        int16_t importX = BTN_X + CH_HALF_W + CH_HALF_GAP;
        drawChannelActionButton(importX, y, CH_HALF_W, CH_ACTION_H, hasPendingInvite ? "[ INVITO! ]" : "[ IMPORTA ]",
            hasPendingInvite ? Display::DD_WARN : Display::DD_ACCENT, channelFocusSlot == rowCount + 1);
        channelImportZone = TouchUI::addZone(importX, y, CH_HALF_W, CH_ACTION_H);
        y += CH_ACTION_H + CH_ACTION_GAP;

        drawChannelActionButton(BTN_X, y, CH_HALF_W, CH_ACTION_H, "[ QR ]", Display::DD_ACCENT, channelFocusSlot == rowCount + 2);
        channelQrZone = TouchUI::addZone(BTN_X, y, CH_HALF_W, CH_ACTION_H);

        int16_t inviteX = BTN_X + CH_HALF_W + CH_HALF_GAP;
        drawChannelActionButton(inviteX, y, CH_HALF_W, CH_ACTION_H, "[ INVITA ]", Display::DD_ACCENT, channelFocusSlot == rowCount + 3);
        channelInviteZone = TouchUI::addZone(inviteX, y, CH_HALF_W, CH_ACTION_H);
        y += CH_ACTION_H + CH_ACTION_GAP;

        drawChannelActionButton(BTN_X, y, BTN_W, CH_ACTION_H, "[ ELIMINA CANALE SELEZIONATO ]", Display::DD_WARN, channelFocusSlot == rowCount + 4);
        channelDeleteZone = TouchUI::addZone(BTN_X, y, BTN_W, CH_ACTION_H);
    }

    void openChannelListScreen() {
        mode = Mode::ChannelList;
        ensureChannelConfigLoaded();
        selectedChannelListRow = (activeChannelIndex >= 0) ? (activeChannelIndex + 1) : 0;
        channelFocusSlot = selectedChannelListRow;
        Input::setTextEntryMode(false);
        drawChannelListScreen();
    }

    // Le 5 azioni, estratte cosi' touch (tap diretto) e trackball (Select
    // su un pulsante evidenziato) chiamano la STESSA logica, nessuna
    // duplicazione (02/10, verifica navigabilita' touch+trackball).
    void doChannelQrAction() {
        if (selectedChannelListRow == 0) {
            // Callback vero, non nullptr (04/10, bug trovato nell'audit
            // pre-VoIP: senza, lo schermo resta congelato sul popup dopo
            // la chiusura - stessa classe di bug gia' corretta in
            // LoRaToolsMenu.cpp il 02/10, qui sfuggita su tutta la
            // gestione canali).
            Display::showMessage("CANALE", "Seleziona un canale tuo dalla lista (non il pubblico) per condividerlo.", []() { drawChannelListScreen(); });
            return;
        }
        const SavedChannel& ch = savedChannels[selectedChannelListRow - 1];
        openChannelQrScreen(ch.name, ch.pskHex);
    }

    void doChannelInviteAction() {
        if (selectedChannelListRow == 0) {
            Display::showMessage("CANALE", "Seleziona un canale tuo dalla lista (non il pubblico) per invitare qualcuno.", []() { drawChannelListScreen(); });
            return;
        }
        SavedChannel ch = savedChannels[selectedChannelListRow - 1];
        bool ok = sendChannelInvite(ch.name, ch.pskHex);
        if (ok) {
            // Lascia un segno nello storico (01/10) e torna subito alla
            // chat per vedere le reazioni/risposte sul pubblico in diretta.
            appendChatMessage("Tu", "[invito canale \"" + ch.name + "\" condiviso sul pubblico]", true, publicChannelHash());
            if (followingTail) {
                ensureBottomVisible();
            }
            showMainScreen();
        } else {
            // Niente piu' un drawChannelListScreen() qui PRIMA del popup
            // (04/10): veniva comunque ridipinto sopra dal popup stesso,
            // lavoro sprecato - il ridisegno vero va nel callback, dopo
            // la chiusura.
            Display::showMessage("CANALE", "Invio fallito.", []() { drawChannelListScreen(); });
        }
    }

    void doChannelDeleteAction() {
        if (selectedChannelListRow == 0) {
            Display::showMessage("CANALE", "Non puoi eliminare il canale pubblico.", []() { drawChannelListScreen(); });
            return;
        }
        deleteSavedChannel(selectedChannelListRow - 1);
        openChannelListScreen();
    }

    void handleChannelListInput() {
        int tapped = TouchUI::pollTap();
        int rowCount = savedChannelCount + 1;

        for (int row = 0; row < rowCount; row++) {
            if (tapped == channelListRowZone[row]) {
                activateChannel(row - 1);
                showMainScreen();
                return;
            }
        }
        if (tapped == channelNewZone) {
            openChannelCreateScreen();
            return;
        }
        if (tapped == channelImportZone) {
            openChannelImportScreen();
            return;
        }
        if (tapped == channelQrZone) {
            doChannelQrAction();
            return;
        }
        if (tapped == channelInviteZone) {
            doChannelInviteAction();
            return;
        }
        if (tapped == channelDeleteZone) {
            doChannelDeleteAction();
            return;
        }

        // Su/Giu' scorrono righe canale E pulsanti azione in sequenza
        // unica (02/10) - prima le azioni erano raggiungibili solo a
        // touch. Select attiva la riga, o l'azione, a seconda di dove si
        // trova il fuoco.
        int totalSlots = rowCount + CH_ACTION_COUNT;
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (channelFocusSlot > 0) {
                    channelFocusSlot--;
                    if (channelFocusSlot < rowCount) {
                        selectedChannelListRow = channelFocusSlot;
                    }
                    drawChannelListScreen();
                }
                break;
            case InputEvent::Down:
                if (channelFocusSlot < totalSlots - 1) {
                    channelFocusSlot++;
                    if (channelFocusSlot < rowCount) {
                        selectedChannelListRow = channelFocusSlot;
                    }
                    drawChannelListScreen();
                }
                break;
            case InputEvent::Select:
                if (channelFocusSlot < rowCount) {
                    activateChannel(channelFocusSlot - 1);
                    showMainScreen();
                } else {
                    switch (channelFocusSlot - rowCount) {
                        case 0: openChannelCreateScreen(); break;
                        case 1: openChannelImportScreen(); break;
                        case 2: doChannelQrAction(); break;
                        case 3: doChannelInviteAction(); break;
                        case 4: doChannelDeleteAction(); break;
                        default: break;
                    }
                }
                break;
            case InputEvent::Back:
                showMainScreen();
                break;
            default:
                break;
        }
    }

    void handleChannelEditInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == channelEditCancelZone) {
            showChannelConfigScreen();
            return;
        }
        if (tapped == channelGenerateZone && channelGenerateZone != -1) {
            uint8_t randomKey[16];
            for (int i = 0; i < 16; i++) {
                randomKey[i] = static_cast<uint8_t>(esp_random());
            }
            char hex[33];
            for (int i = 0; i < 16; i++) {
                snprintf(hex + i * 2, 3, "%02X", randomKey[i]);
            }
            channelEditBuffer = String(hex);
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, channelEditBuffer, false);
            return;
        }
        TextInputUI::Result result = TextInputUI::handleKey(channelEditBuffer,
            editingChannelField == ChannelField::Psk ? 64 : 24);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, channelEditBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmChannelEdit();
        }
    }

    // Header a 16 byte IN CHIARO che precede ogni pacchetto Meshtastic
    // sull'aria (struct PacketHeader, src/mesh/RadioInterface.h nel
    // sorgente reale) - non e' protobuf, e' un formato fisso a byte
    // allineati, quindi si puo' leggere subito senza nessuna libreria
    // protobuf. SOLO questo header e' in chiaro: il payload che segue e'
    // cifrato AES-128-CTR - ma se e' sul canale pubblico di default
    // "LongFast" (il caso comune per chi non ha personalizzato nulla),
    // la chiave e' NOTA E PUBBLICA (src/mesh/Channels.h, e' proprio
    // pensata per essere "pubblica di default") - vedi decryptDefault()
    // sotto. Non serve ancora nessuna libreria protobuf/crypto esterna:
    // AES-CTR e' gia' disponibile via mbedtls, vendorizzato dentro il
    // core arduino-esp32 stesso.
    struct MeshHeader {
        uint32_t to;
        uint32_t from;
        uint32_t id;
        uint8_t flags;
        uint8_t channelHash;
        uint8_t nextHop;
        uint8_t relayNode;
    };

    constexpr uint32_t NODENUM_BROADCAST = 0xFFFFFFFF;

    // Inoltro pacchetti (02/10, "chiudere il capitolo Meshtastic" -
    // finora il firmware riceveva/inviava ma non si comportava da vero
    // nodo mesh, un pacchetto non per noi veniva solo letto e ignorato).
    // Dedup via ID gia' inoltrati - un ID a 32 bit casuale (esp_random())
    // rende trascurabile una collisione accidentale, non serve altro per
    // un buffer piccolo come questo.
    constexpr int RELAY_SEEN_CAP = 16;
    uint32_t relaySeenIds[RELAY_SEEN_CAP];
    int relaySeenCount = 0;
    int relaySeenNext = 0;

    bool wasRelayed(uint32_t id) {
        for (int i = 0; i < relaySeenCount; i++) {
            if (relaySeenIds[i] == id) {
                return true;
            }
        }
        return false;
    }

    void markRelayed(uint32_t id) {
        relaySeenIds[relaySeenNext] = id;
        relaySeenNext = (relaySeenNext + 1) % RELAY_SEEN_CAP;
        if (relaySeenCount < RELAY_SEEN_CAP) {
            relaySeenCount++;
        }
    }

    uint32_t readU32LE(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    MeshHeader parseHeader(const uint8_t* data) {
        MeshHeader h;
        h.to = readU32LE(data);
        h.from = readU32LE(data + 4);
        h.id = readU32LE(data + 8);
        h.flags = data[12];
        h.channelHash = data[13];
        h.nextHop = data[14];
        h.relayNode = data[15];
        return h;
    }

    String nodeIdText(uint32_t node) {
        if (node == NODENUM_BROADCAST) {
            return "BROADCAST";
        }
        char buf[10];
        snprintf(buf, sizeof(buf), "!%08x", static_cast<unsigned>(node));
        return String(buf);
    }

    constexpr size_t MESH_HEADER_LEN = 16;

    // "Managed flood" minimale (02/10): ritrasmette un broadcast non
    // nostro con hop_limit residuo, bytes invariati (l'header viaggia in
    // chiaro, non serve decifrare per inoltrare - inoltriamo anche
    // pacchetti su canali a cui non siamo iscritti, come fa un vero nodo
    // mesh). Omesso apposta il ritardo casuale che il vero Meshtastic usa
    // per ridurre le collisioni tra piu' nodi che inoltrano insieme -
    // semplificazione accettabile per un firmware minimale, incide sulle
    // prestazioni in reti affollate, non sulla correttezza.
    void maybeRelayPacket(const uint8_t* data, int length, const MeshHeader& h, int hopLimit) {
        if (!radioBorrowed) {
            return;
        }
        if (h.to != NODENUM_BROADCAST) {
            return; // solo broadcast - questo firmware non fa instradamento diretto
        }
        if (h.from == ourNodeNum()) {
            return; // non rimbalziamo i nostri stessi pacchetti
        }
        if (hopLimit <= 0) {
            return; // arrivato a fine corsa
        }
        if (wasRelayed(h.id)) {
            return; // gia' inoltrato (sentito su piu' percorsi/rimbalzi)
        }
        if (length > 255) {
            return; // non dovrebbe mai succedere - guardia di sicurezza
        }

        markRelayed(h.id);

        uint8_t onAir[255];
        memcpy(onAir, data, length);
        uint8_t newHopLimit = static_cast<uint8_t>(hopLimit - 1);
        onAir[12] = (onAir[12] & 0xF8) | newHopLimit; // bit 0-2, hop_start (bit 5-7) invariato

        SX1262& radio = LoRaRadio::getRadioForExternalUse();
        radio.transmit(onAir, length);
        radio.startReceive(); // il transmit lascia la radio ferma in TX

        DEBUG_PRINTF("[MESHTASTIC] inoltrato id=%08X da !%08x, hop %d->%d\n",
            static_cast<unsigned>(h.id), static_cast<unsigned>(h.from), hopLimit, newHopLimit);
    }

    // Chiave AES-128 del canale pubblico di default "LongFast" - NON e'
    // un segreto di questo progetto, e' pubblicata cosi' com'e' nel
    // sorgente ufficiale (src/mesh/Channels.h, "16 bytes of random PSK
    // for our _public_ default channel that all devices power up on").
    // Chiunque usi Meshtastic senza personalizzare i canali la usa.
    constexpr uint8_t DEFAULT_CHANNEL_PSK[16] = {
        0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
    };

    // Canali salvati (02/10) - elenco di canali a cui si e' iscritti,
    // switch libero senza perderli (richiesto esplicitamente: "vedere le
    // chat a cui sono iscritto, inclusa la pubblica, e passare dall'una
    // all'altra senza perderle") - sostituisce il vecchio slot singolo
    // che si sovrascriveva a ogni nuovo canale creato/importato.
    // activeChannelIndex == -1 e' il canale pubblico di default, sempre
    // disponibile e MAI salvato nell'elenco. Chiave AES-128 o AES-256 (16
    // o 32 byte) a seconda della lunghezza della stringa hex, Meshtastic
    // supporta entrambe.
    Preferences channelPrefs;
    constexpr const char* CHANNEL_PREFS_NS = "tdeckmesh";
    bool channelConfigLoaded = false;

    // Stesso parser esadecimale gia' usato in BleGattUI.cpp/LoRaWanUI.cpp
    // - spazi ignorati, coppie di cifre -> byte. Ritorna il numero di
    // byte validi scritti (0 se la stringa e' vuota o malformata).
    size_t parseHexBytes(const String& text, uint8_t* out, size_t outCapacity) {
        String clean;
        for (size_t i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c != ' ') {
                clean += c;
            }
        }
        if (clean.length() % 2 != 0) {
            return 0;
        }
        size_t byteCount = clean.length() / 2;
        if (byteCount > outCapacity) {
            return 0;
        }
        for (size_t i = 0; i < byteCount; i++) {
            String pair = clean.substring(i * 2, i * 2 + 2);
            if (!isxdigit(pair.charAt(0)) || !isxdigit(pair.charAt(1))) {
                return 0;
            }
            out[i] = static_cast<uint8_t>(strtoul(pair.c_str(), nullptr, 16));
        }
        return byteCount;
    }

    void persistChannelList() {
        channelPrefs.begin(CHANNEL_PREFS_NS, false);
        channelPrefs.putInt("cnt", savedChannelCount);
        channelPrefs.putInt("sel", activeChannelIndex);
        for (int i = 0; i < savedChannelCount; i++) {
            channelPrefs.putString(("n" + String(i)).c_str(), savedChannels[i].name);
            channelPrefs.putString(("k" + String(i)).c_str(), savedChannels[i].pskHex);
        }
        channelPrefs.end();
    }

    void loadChannelConfig() {
        channelPrefs.begin(CHANNEL_PREFS_NS, true);
        savedChannelCount = channelPrefs.getInt("cnt", 0);
        if (savedChannelCount < 0) {
            savedChannelCount = 0;
        }
        if (savedChannelCount > MAX_SAVED_CHANNELS) {
            savedChannelCount = MAX_SAVED_CHANNELS;
        }
        for (int i = 0; i < savedChannelCount; i++) {
            savedChannels[i].name = channelPrefs.getString(("n" + String(i)).c_str(), "");
            savedChannels[i].pskHex = channelPrefs.getString(("k" + String(i)).c_str(), "");
        }
        activeChannelIndex = channelPrefs.getInt("sel", -1);
        mediumFastPreset = channelPrefs.getBool("presetMF", false);
        // Migrazione dal vecchio slot singolo (01/10, prima di avere una
        // lista) - se non c'e' ancora nessun canale salvato ma esistono le
        // vecchie chiavi "name"/"psk" di una versione precedente di questo
        // firmware, le importiamo una volta sola come primo canale.
        if (savedChannelCount == 0) {
            String oldName = channelPrefs.getString("name", "");
            String oldPsk = channelPrefs.getString("psk", "");
            if (oldName.length() > 0 && oldPsk.length() > 0) {
                savedChannels[0].name = oldName;
                savedChannels[0].pskHex = oldPsk;
                savedChannelCount = 1;
                activeChannelIndex = 0;
            }
        }
        channelPrefs.end();
        if (activeChannelIndex < -1 || activeChannelIndex >= savedChannelCount) {
            activeChannelIndex = -1;
        }
    }

    void ensureChannelConfigLoaded() {
        if (!channelConfigLoaded) {
            channelConfigLoaded = true;
            loadChannelConfig();
        }
    }

    bool hasCustomChannel() {
        ensureChannelConfigLoaded();
        return activeChannelIndex >= 0 && activeChannelIndex < savedChannelCount;
    }

    String activeChannelDisplayName() {
        ensureChannelConfigLoaded();
        return hasCustomChannel() ? savedChannels[activeChannelIndex].name : "Pubblica (default)";
    }

    // Solo il nome nudo del canale attivo, senza "(default)" - per il
    // titolo compatto della chat (02/10). Valido solo se hasCustomChannel().
    String activeCustomChannelName() {
        ensureChannelConfigLoaded();
        return hasCustomChannel() ? savedChannels[activeChannelIndex].name : "";
    }

    // Nome usato per l'hash del canale - il nome del preset modem attivo
    // ("LongFast" o "MediumFast") e' il nome reale che Meshtastic usa
    // internamente per il canale di default (vedi Channels::getName(),
    // non e' realmente senza nome - ritorna il nome del preset quando il
    // campo nome e' vuoto, e deve combaciare col preset scelto dai nodi
    // vicini per calcolare lo stesso hash di canale).
    String activeChannelHashName() {
        ensureChannelConfigLoaded();
        return hasCustomChannel() ? savedChannels[activeChannelIndex].name : String(activePresetName());
    }

    const uint8_t* activeChannelKey(size_t& outLen) {
        ensureChannelConfigLoaded();
        static uint8_t buf[32];
        if (hasCustomChannel()) {
            size_t len = parseHexBytes(savedChannels[activeChannelIndex].pskHex, buf, sizeof(buf));
            if (len == 16 || len == 32) {
                outLen = len;
                return buf;
            }
        }
        outLen = sizeof(DEFAULT_CHANNEL_PSK);
        return DEFAULT_CHANNEL_PSK;
    }

    bool channelKeyValid(const String& pskHex) {
        uint8_t tmp[32];
        size_t len = parseHexBytes(pskHex, tmp, sizeof(tmp));
        return len == 16 || len == 32;
    }

    // -1 = pubblico, altrimenti un indice gia' presente in savedChannels[].
    void activateChannel(int index) {
        ensureChannelConfigLoaded();
        if (index < -1 || index >= savedChannelCount) {
            return;
        }
        activeChannelIndex = index;
        persistChannelList();
    }

    // Crea un canale nuovo e lo attiva, o riattiva quello gia' salvato se
    // nome+chiave combaciano esattamente (dedup - es. importare due volte
    // lo stesso invito non deve creare righe duplicate). False se la
    // chiave non e' valida o l'elenco e' pieno.
    bool addAndActivateChannel(const String& name, const String& pskHex) {
        ensureChannelConfigLoaded();
        if (name.length() == 0 || !channelKeyValid(pskHex)) {
            return false;
        }
        for (int i = 0; i < savedChannelCount; i++) {
            if (savedChannels[i].name == name && savedChannels[i].pskHex == pskHex) {
                activateChannel(i);
                return true;
            }
        }
        if (savedChannelCount >= MAX_SAVED_CHANNELS) {
            return false;
        }
        savedChannels[savedChannelCount].name = name;
        savedChannels[savedChannelCount].pskHex = pskHex;
        activeChannelIndex = savedChannelCount;
        savedChannelCount++;
        persistChannelList();
        return true;
    }

    void deleteSavedChannel(int index) {
        ensureChannelConfigLoaded();
        if (index < 0 || index >= savedChannelCount) {
            return;
        }
        for (int i = index; i < savedChannelCount - 1; i++) {
            savedChannels[i] = savedChannels[i + 1];
        }
        savedChannelCount--;
        if (activeChannelIndex == index) {
            activeChannelIndex = -1;
        } else if (activeChannelIndex > index) {
            activeChannelIndex--;
        }
        persistChannelList();
    }

    // AES-128-CTR: stesso algoritmo sia per cifrare che decifrare (CTR e'
    // simmetrico). Nonce a 16 byte costruito come fa CryptoEngine::
    // initNonce() nel sorgente reale: packetId a 64 bit little-endian
    // (il nostro "id" sull'aria e' di soli 32 bit, la meta' alta resta
    // zero) seguito dal NodeNum del mittente a 32 bit, poi 4 byte a zero
    // (extraNonce, usato solo per i DM cifrati con PKI - non qui).
    // Tentativo "a occhi chiusi": se il pacchetto non era davvero sul
    // canale di default, il risultato e' semplicemente rumore leggibile
    // come tale (niente di rotto, solo non decifrabile senza la chiave
    // giusta).
    void channelCrypt(const uint8_t* encrypted, size_t len, uint32_t fromNode, uint32_t packetId,
                       const uint8_t* key, size_t keyLen, uint8_t* out) {
        uint8_t nonce[16] = {0};
        nonce[0] = static_cast<uint8_t>(packetId);
        nonce[1] = static_cast<uint8_t>(packetId >> 8);
        nonce[2] = static_cast<uint8_t>(packetId >> 16);
        nonce[3] = static_cast<uint8_t>(packetId >> 24);
        // byte 4..7 = meta' alta del packetId a 64 bit, sempre zero qui
        nonce[8] = static_cast<uint8_t>(fromNode);
        nonce[9] = static_cast<uint8_t>(fromNode >> 8);
        nonce[10] = static_cast<uint8_t>(fromNode >> 16);
        nonce[11] = static_cast<uint8_t>(fromNode >> 24);
        // byte 12..15 = extraNonce, zero per i normali pacchetti mesh

        mbedtls_aes_context ctx;
        mbedtls_aes_init(&ctx);
        mbedtls_aes_setkey_enc(&ctx, key, static_cast<unsigned int>(keyLen * 8)); // 128 o 256 bit
        size_t ncOff = 0;
        uint8_t streamBlock[16];
        mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, streamBlock, encrypted, out);
        mbedtls_aes_free(&ctx);
    }

    // Hash del canale PUBBLICO di default, SEMPRE quello (non quello
    // attivo) - serve per "INVIA INVITO", che deve restare leggibile da
    // chiunque sia ancora sul pubblico indipendentemente dal canale che
    // abbiamo attivo noi in quel momento (magari gia' quello privato a
    // cui li stiamo invitando). Il nome e' quello del preset modem
    // ATTIVO (non sempre "LongFast": vedi activePresetName()) perche' e'
    // quello che calcolano anche i nodi vicini sul pubblico se usano lo
    // stesso preset.
    uint8_t publicChannelHash() {
        const char* name = activePresetName();
        uint8_t h = xorHash(reinterpret_cast<const uint8_t*>(name), strlen(name));
        h ^= xorHash(DEFAULT_CHANNEL_PSK, sizeof(DEFAULT_CHANNEL_PSK));
        return h;
    }

    // AES-128/256-CTR sul canale ATTIVO (stesso algoritmo sia per
    // cifrare che decifrare, CTR e' simmetrico) - usata sia in ricezione
    // (showPacket()) sia in invio (sendDataPacket()). Tentativo "a occhi
    // chiusi": se il pacchetto non era davvero sul canale attivo, il
    // risultato e' semplicemente rumore leggibile come tale.
    void decryptDefaultChannel(const uint8_t* encrypted, size_t len, uint32_t fromNode, uint32_t packetId, uint8_t* out) {
        size_t keyLen;
        const uint8_t* key = activeChannelKey(keyLen);
        channelCrypt(encrypted, len, fromNode, packetId, key, keyLen, out);
    }

    // Parser protobuf minimale, scritto a mano (30/09 sera, passo 4) -
    // niente nanopb, nessun file generato dagli .proto: il wire format
    // di protobuf e' un formato a byte semplice (tag = numero_campo<<3
    // | wire_type, poi il valore secondo il wire_type), e servono solo
    // pochi campi specifici (letti dagli .proto REALI di
    // meshtastic/protobufs, non indovinati):
    //   Data:     portnum=1 (varint), payload=2 (length-delimited)
    //   User:     id=1, long_name=2, short_name=3 (tutte string)
    //   Position: latitude_i=1, longitude_i=2 (sfixed32, wire type 5)
    // Portnum che sappiamo interpretare: TEXT_MESSAGE_APP=1 (il payload
    // e' testo UTF8 grezzo, NON un altro protobuf), NODEINFO_APP=4
    // (payload = User), POSITION_APP=3 (payload = Position). Tutto il
    // resto (telemetria, routing, admin...) resta non decodificato per
    // ora - portnum comunque mostrato a schermo cosi' si sa cos'era.
    struct ProtoField {
        uint32_t fieldNumber = 0;
        uint32_t wireType = 0; // 0=varint, 1=fixed64, 2=length-delimited, 5=fixed32
        uint64_t varintValue = 0;
        const uint8_t* bytes = nullptr;
        size_t bytesLen = 0;
        uint32_t fixed32Value = 0;
    };

    bool readVarint(const uint8_t* data, size_t len, size_t& pos, uint64_t& out) {
        out = 0;
        int shift = 0;
        while (pos < len) {
            uint8_t b = data[pos++];
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return true;
            }
            shift += 7;
            if (shift > 63) {
                return false;
            }
        }
        return false;
    }

    bool nextProtoField(const uint8_t* data, size_t len, size_t& pos, ProtoField& out) {
        if (pos >= len) {
            return false;
        }
        uint64_t tag;
        if (!readVarint(data, len, pos, tag)) {
            return false;
        }
        out.fieldNumber = static_cast<uint32_t>(tag >> 3);
        out.wireType = static_cast<uint32_t>(tag & 0x7);

        switch (out.wireType) {
            case 0:
                return readVarint(data, len, pos, out.varintValue);
            case 1:
                if (pos + 8 > len) return false;
                pos += 8; // fixed64, non ci serve alcun campo cosi' qui: salta
                return true;
            case 2: {
                uint64_t l;
                if (!readVarint(data, len, pos, l)) return false;
                if (pos + l > len) return false;
                out.bytes = data + pos;
                out.bytesLen = static_cast<size_t>(l);
                pos += l;
                return true;
            }
            case 5:
                if (pos + 4 > len) return false;
                out.fixed32Value = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                    (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
                return true;
            default:
                return false; // wire type 3/4 (group, deprecato) non previsto qui
        }
    }

    String bytesToString(const uint8_t* bytes, size_t len) {
        String s;
        for (size_t i = 0; i < len; i++) {
            s += static_cast<char>(bytes[i]);
        }
        return s;
    }

    // --- Import canale da link/invito (01/10) - l'esatto inverso di
    // buildChannelShareUrl()/buildChannelSetProto(): base64url decode
    // poi protobuf ChannelSet -> ChannelSettings.psk/.name. ---

    bool base64UrlDecode(const String& input, uint8_t* out, size_t outCapacity, size_t& outLen) {
        String clean = input;
        int hashPos = clean.indexOf('#');
        if (hashPos >= 0) {
            clean = clean.substring(hashPos + 1);
        }
        clean.trim();
        clean.replace("-", "+");
        clean.replace("_", "/");
        while (clean.length() % 4 != 0) {
            clean += "=";
        }
        size_t written = 0;
        int ret = mbedtls_base64_decode(out, outCapacity, &written,
            reinterpret_cast<const unsigned char*>(clean.c_str()), clean.length());
        if (ret != 0) {
            return false;
        }
        outLen = written;
        return true;
    }

    struct ParsedChannelInvite {
        bool ok = false;
        String name;
        String pskHex;
    };

    ParsedChannelInvite parseChannelSetBytes(const uint8_t* data, size_t len) {
        ParsedChannelInvite result;
        size_t pos = 0;
        ProtoField f;
        while (nextProtoField(data, len, pos, f)) {
            if (f.fieldNumber == 1 && f.wireType == 2) { // ChannelSettings (embedded)
                size_t p2 = 0;
                ProtoField f2;
                while (nextProtoField(f.bytes, f.bytesLen, p2, f2)) {
                    if (f2.fieldNumber == 2 && f2.wireType == 2) { // psk
                        String hex;
                        for (size_t i = 0; i < f2.bytesLen; i++) {
                            char buf[3];
                            snprintf(buf, sizeof(buf), "%02X", f2.bytes[i]);
                            hex += buf;
                        }
                        result.pskHex = hex;
                    } else if (f2.fieldNumber == 3 && f2.wireType == 2) { // name
                        result.name = bytesToString(f2.bytes, f2.bytesLen);
                    }
                }
            }
            // lora_config (campo 2) ignorato in importazione: usiamo
            // sempre i nostri parametri fissi EU_868/LongFast.
        }
        result.ok = (result.pskHex.length() == 32 || result.pskHex.length() == 64) && result.name.length() > 0;
        return result;
    }

    // "" se importato con successo, altrimenti un messaggio d'errore da
    // mostrare - usata sia dall'incolla manuale sia dall'importazione di
    // un invito ricevuto in chat.
    String applyChannelImportText(const String& text) {
        uint8_t raw[150];
        size_t rawLen = 0;
        if (!base64UrlDecode(text, raw, sizeof(raw), rawLen)) {
            return "Testo non valido - non sembra un link/codice di canale Meshtastic.";
        }
        ParsedChannelInvite parsed = parseChannelSetBytes(raw, rawLen);
        if (!parsed.ok) {
            return "Link decodificato ma incompleto (manca nome o chiave valida).";
        }
        if (!addAndActivateChannel(parsed.name, parsed.pskHex)) {
            return "Elenco canali pieno (max " + String(MAX_SAVED_CHANNELS) + ") - elimina un canale salvato prima di importarne un altro.";
        }
        return "";
    }

    struct DecodedData {
        bool parsed = false;
        uint32_t portnum = 0;
        String textMessage;               // TEXT_MESSAGE_APP
        String userId, longName, shortName; // NODEINFO_APP
        bool hasPosition = false;
        double latitude = 0.0, longitude = 0.0; // POSITION_APP

        // TELEMETRY_APP (03/10) - solo i due varianti piu' comuni per un
        // uso da escursione/campo (DeviceMetrics, EnvironmentMetrics);
        // gli altri (air_quality/power/local_stats/health/host/traffic/
        // soil_water) restano fuori scope, sono rari su un nodo "da
        // zaino". Campo numero esatto preso dal vero
        // meshtastic/protobufs (telemetry.proto), non indovinato.
        bool hasBattLevel = false;
        uint32_t battLevel = 0;
        bool hasBattVoltage = false;
        float battVoltage = 0;
        bool hasChannelUtil = false;
        float channelUtil = 0;
        bool hasAirUtilTx = false;
        float airUtilTx = 0;
        bool hasUptimeSeconds = false;
        uint32_t uptimeSeconds = 0;
        bool hasTemperature = false;
        float temperature = 0;
        bool hasHumidity = false;
        float humidity = 0;
        bool hasPressure = false;
        float pressure = 0;
        bool hasGasResistance = false;
        float gasResistance = 0;
    };

    // Le due sezioni di EnvironmentMetrics/DeviceMetrics sono "float"
    // nel vero protobuf (wire type 5, fixed32) - NON un intero scalato
    // come latitude_i/longitude_i di Position: i 4 byte vanno
    // reinterpretati bit-per-bit come IEEE-754, non divisi per 1e7.
    float fixed32ToFloat(uint32_t bits) {
        float v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    }

    DecodedData decodeMeshData(const uint8_t* plain, size_t len) {
        DecodedData out;
        const uint8_t* payload = nullptr;
        size_t payloadLen = 0;

        size_t pos = 0;
        ProtoField f;
        while (nextProtoField(plain, len, pos, f)) {
            if (f.fieldNumber == 1 && f.wireType == 0) {
                out.portnum = static_cast<uint32_t>(f.varintValue);
            } else if (f.fieldNumber == 2 && f.wireType == 2) {
                payload = f.bytes;
                payloadLen = f.bytesLen;
            }
        }
        out.parsed = true;
        if (payload == nullptr) {
            return out;
        }

        if (out.portnum == 1) { // TEXT_MESSAGE_APP: testo grezzo, non protobuf
            out.textMessage = bytesToString(payload, payloadLen);
        } else if (out.portnum == 4) { // NODEINFO_APP -> User
            size_t p2 = 0;
            ProtoField f2;
            while (nextProtoField(payload, payloadLen, p2, f2)) {
                if (f2.wireType != 2) continue;
                if (f2.fieldNumber == 1) out.userId = bytesToString(f2.bytes, f2.bytesLen);
                else if (f2.fieldNumber == 2) out.longName = bytesToString(f2.bytes, f2.bytesLen);
                else if (f2.fieldNumber == 3) out.shortName = bytesToString(f2.bytes, f2.bytesLen);
            }
        } else if (out.portnum == 3) { // POSITION_APP -> Position
            size_t p2 = 0;
            ProtoField f2;
            while (nextProtoField(payload, payloadLen, p2, f2)) {
                if (f2.fieldNumber == 1 && f2.wireType == 5) {
                    out.latitude = static_cast<double>(static_cast<int32_t>(f2.fixed32Value)) * 1e-7;
                    out.hasPosition = true;
                } else if (f2.fieldNumber == 2 && f2.wireType == 5) {
                    out.longitude = static_cast<double>(static_cast<int32_t>(f2.fixed32Value)) * 1e-7;
                }
            }
        } else if (out.portnum == 67) { // TELEMETRY_APP -> Telemetry (oneof variant)
            size_t p2 = 0;
            ProtoField f2;
            while (nextProtoField(payload, payloadLen, p2, f2)) {
                if (f2.fieldNumber == 2 && f2.wireType == 2) { // device_metrics
                    size_t p3 = 0;
                    ProtoField f3;
                    while (nextProtoField(f2.bytes, f2.bytesLen, p3, f3)) {
                        if (f3.fieldNumber == 1 && f3.wireType == 0) {
                            out.hasBattLevel = true;
                            out.battLevel = static_cast<uint32_t>(f3.varintValue);
                        } else if (f3.fieldNumber == 2 && f3.wireType == 5) {
                            out.hasBattVoltage = true;
                            out.battVoltage = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 3 && f3.wireType == 5) {
                            out.hasChannelUtil = true;
                            out.channelUtil = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 4 && f3.wireType == 5) {
                            out.hasAirUtilTx = true;
                            out.airUtilTx = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 5 && f3.wireType == 0) {
                            out.hasUptimeSeconds = true;
                            out.uptimeSeconds = static_cast<uint32_t>(f3.varintValue);
                        }
                    }
                } else if (f2.fieldNumber == 3 && f2.wireType == 2) { // environment_metrics
                    size_t p3 = 0;
                    ProtoField f3;
                    while (nextProtoField(f2.bytes, f2.bytesLen, p3, f3)) {
                        if (f3.fieldNumber == 1 && f3.wireType == 5) {
                            out.hasTemperature = true;
                            out.temperature = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 2 && f3.wireType == 5) {
                            out.hasHumidity = true;
                            out.humidity = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 3 && f3.wireType == 5) {
                            out.hasPressure = true;
                            out.pressure = fixed32ToFloat(f3.fixed32Value);
                        } else if (f3.fieldNumber == 4 && f3.wireType == 5) {
                            out.hasGasResistance = true;
                            out.gasResistance = fixed32ToFloat(f3.fixed32Value);
                        }
                    }
                }
                // Altri varianti del oneof (air_quality/power/local_stats/
                // health/host/traffic/soil_water, campi 4/5/6/7/8/9/11) non
                // decodificati - fuori scope, rari su un nodo da escursione.
            }
        }

        return out;
    }

    void recordNodeTelemetry(uint32_t nodeNum, const DecodedData& decoded) {
        int idx = findOrCreateNode(nodeNum);
        SeenNode& n = seenNodes[idx];
        if (decoded.hasBattLevel)     { n.hasBattLevel = true;     n.battLevel = decoded.battLevel; }
        if (decoded.hasBattVoltage)   { n.hasBattVoltage = true;   n.battVoltage = decoded.battVoltage; }
        if (decoded.hasChannelUtil)   { n.hasChannelUtil = true;   n.channelUtil = decoded.channelUtil; }
        if (decoded.hasAirUtilTx)     { n.hasAirUtilTx = true;     n.airUtilTx = decoded.airUtilTx; }
        if (decoded.hasUptimeSeconds) { n.hasUptimeSeconds = true; n.uptimeSeconds = decoded.uptimeSeconds; }
        if (decoded.hasTemperature)   { n.hasTemperature = true;   n.temperature = decoded.temperature; }
        if (decoded.hasHumidity)      { n.hasHumidity = true;      n.humidity = decoded.humidity; }
        if (decoded.hasPressure)      { n.hasPressure = true;      n.pressure = decoded.pressure; }
        if (decoded.hasGasResistance) { n.hasGasResistance = true; n.gasResistance = decoded.gasResistance; }
        n.lastSeenMs = millis();
    }

    const char* portnumLabel(uint32_t portnum) {
        switch (portnum) {
            case 1: return "MESSAGGIO TESTO";
            case 3: return "POSIZIONE";
            case 4: return "INFO NODO";
            case 67: return "TELEMETRIA";
            default: return nullptr;
        }
    }

    // --- Trasmissione (30/09 sera, passo 5): un messaggio di testo di
    // prova sul canale pubblico di default - il minimo per poter
    // verificare sul campo domani che un'app/nodo Meshtastic reale lo
    // riceva davvero, non solo che noi sappiamo ricevere. ---

    // Stesso algoritmo di Channels::xorHash() nel sorgente reale: un
    // semplice xor byte a byte, usato per l'hash del canale.
    uint8_t xorHash(const uint8_t* data, size_t len) {
        uint8_t code = 0;
        for (size_t i = 0; i < len; i++) {
            code ^= data[i];
        }
        return code;
    }

    // Hash del canale ATTIVO (01/10: default pubblico, o personalizzato
    // se configurato) - xorHash(nome) XOR xorHash(chiave), identico a
    // Channels::generateHash() nel sorgente reale. Per il canale di
    // default senza nome personalizzato, Channels::getName() ritorna il
    // nome del preset modem - "LongFast" per EU_868/LONG_FAST, stringa
    // esatta presa da DisplayFormatters::getModemPresetDisplayName().
    // NON va piu' messo in cache (una volta in cache, cambiare canale a
    // runtime non si rifletterebbe mai piu' qui) - il calcolo e'
    // comunque banale, due xor su stringhe/chiavi corte.
    uint8_t channelHashFor(const String& name, const uint8_t* key, size_t keyLen) {
        uint8_t h = xorHash(reinterpret_cast<const uint8_t*>(name.c_str()), name.length());
        h ^= xorHash(key, keyLen);
        return h;
    }

    uint8_t defaultChannelHash() {
        String name = activeChannelHashName();
        size_t keyLen;
        const uint8_t* key = activeChannelKey(keyLen);
        return channelHashFor(name, key, keyLen);
    }

    // Cerca tra TUTTI i canali salvati (non solo quello attivo in questo
    // momento) uno il cui hash combaci con quello ricevuto (02/10,
    // richiesto esplicitamente: "tutti i canali creati o partecipati
    // rimangono attivi per la ricezione?") - cosi' un canale continua a
    // ricevere anche mentre se ne sta guardando/scrivendo un altro, non
    // solo quello attivo+il pubblico come prima. Il messaggio arriva
    // comunque taggato con il SUO hash (vedi h.channelHash in
    // showPacket()), quindi si trova nello storico di quel canale non
    // appena ci si passa sopra.
    bool findMatchingSavedChannel(uint8_t wantHash, const uint8_t** outKey, size_t* outKeyLen) {
        ensureChannelConfigLoaded();
        static uint8_t buf[32];
        for (int i = 0; i < savedChannelCount; i++) {
            uint8_t tmp[32];
            size_t len = parseHexBytes(savedChannels[i].pskHex, tmp, sizeof(tmp));
            if (len != 16 && len != 32) {
                continue;
            }
            if (channelHashFor(savedChannels[i].name, tmp, len) == wantHash) {
                memcpy(buf, tmp, len);
                *outKey = buf;
                *outKeyLen = len;
                return true;
            }
        }
        return false;
    }

    // Numero nodo per questo T-Deck: non replica l'esatta derivazione
    // di Meshtastic (usa gli ultimi byte del proprio NodeNum interno),
    // ci basta un numero a 32 bit plausibile, stabile per la sessione e
    // diverso da 0/broadcast - preso dal MAC efuse del chip, che e'
    // comunque univoco per dispositivo.
    uint32_t ourNodeNum() {
        static uint32_t cached = 0;
        if (cached == 0) {
            uint64_t mac = ESP.getEfuseMac();
            cached = static_cast<uint32_t>(mac & 0xFFFFFFFF);
            if (cached == 0 || cached == NODENUM_BROADCAST) {
                cached = 0x7DEC0001; // fallback improbabile, mai 0/broadcast
            }
        }
        return cached;
    }

    void writeU32LE(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    // Varint protobuf in scrittura (l'inverso di readVarint() sopra) -
    // serve per il tag/lunghezza di ogni campo che costruiamo a mano.
    size_t writeVarint(uint8_t* buf, size_t pos, uint32_t value) {
        while (value >= 0x80) {
            buf[pos++] = static_cast<uint8_t>(value) | 0x80;
            value >>= 7;
        }
        buf[pos++] = static_cast<uint8_t>(value);
        return pos;
    }

    // Costruisce il messaggio Data { portnum=1 (varint), payload=2
    // (length-delimited) } attorno a un payload gia' pronto (testo
    // grezzo per i messaggi, o un altro protobuf incapsulato per
    // NodeInfo/Position/...).
    size_t buildDataMessage(uint8_t* out, uint32_t portnum, const uint8_t* innerPayload, size_t innerLen) {
        size_t pos = 0;
        out[pos++] = 0x08; // tag campo 1, wire type 0 (varint)
        pos = writeVarint(out, pos, portnum);
        out[pos++] = 0x12; // tag campo 2, wire type 2 (length-delimited)
        pos = writeVarint(out, pos, static_cast<uint32_t>(innerLen));
        memcpy(out + pos, innerPayload, innerLen);
        pos += innerLen;
        return pos;
    }

    // Punto unico di invio (30/09 sera) - costruisce l'header, cifra e
    // trasmette un qualunque messaggio Data gia' incapsulato. Usato sia
    // dal messaggio di testo di prova sia dall'annuncio NodeInfo.
    bool sendDataPacket(uint32_t portnum, const uint8_t* innerPayload, size_t innerLen) {
        if (!radioBorrowed) {
            return false;
        }
        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        uint8_t plainPayload[160];
        size_t plainLen = buildDataMessage(plainPayload, portnum, innerPayload, innerLen);

        uint32_t fromNode = ourNodeNum();
        uint32_t packetId = esp_random();
        if (packetId == 0) {
            packetId = 1; // nonce zero e' l'unico valore da evitare (azzererebbe il nonce insieme al resto)
        }

        uint8_t encryptedPayload[sizeof(plainPayload)];
        decryptDefaultChannel(plainPayload, plainLen, fromNode, packetId, encryptedPayload); // AES-CTR: cifra e decifra sono la stessa operazione

        uint8_t onAir[MESH_HEADER_LEN + sizeof(plainPayload)];
        writeU32LE(onAir, NODENUM_BROADCAST);      // to
        writeU32LE(onAir + 4, fromNode);           // from
        writeU32LE(onAir + 8, packetId);           // id
        onAir[12] = 0x03 | (0x03 << 5);            // flags: hop_limit=3, hop_start=3
        onAir[13] = defaultChannelHash();          // channel
        onAir[14] = 0;                             // next_hop
        onAir[15] = 0;                             // relay_node
        memcpy(onAir + MESH_HEADER_LEN, encryptedPayload, plainLen);

        size_t totalLen = MESH_HEADER_LEN + plainLen;

        int state = radio.transmit(onAir, totalLen);
        radio.startReceive(); // il transmit lascia la radio ferma in TX, va sempre riavviato l'ascolto dopo

        return state == RADIOLIB_ERR_NONE;
    }

    // Invia un messaggio di testo grezzo SEMPRE sul canale PUBBLICO di
    // default (chiave/hash fissi, non quelli del canale attivo) - usata
    // sia per l'invito vero e proprio sia per il claim "usa e getta"
    // (01/10), cosi' arriva a chiunque sia ancora sul pubblico anche se
    // noi nel frattempo siamo gia' passati a un canale privato.
    bool sendPublicChannelText(const String& text) {
        if (!radioBorrowed) {
            return false;
        }
        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        uint8_t plainPayload[220];
        size_t plainLen = buildDataMessage(plainPayload, 1, reinterpret_cast<const uint8_t*>(text.c_str()), text.length());

        uint32_t fromNode = ourNodeNum();
        uint32_t packetId = esp_random();
        if (packetId == 0) {
            packetId = 1;
        }

        uint8_t encryptedPayload[sizeof(plainPayload)];
        channelCrypt(plainPayload, plainLen, fromNode, packetId, DEFAULT_CHANNEL_PSK, sizeof(DEFAULT_CHANNEL_PSK), encryptedPayload);

        uint8_t onAir[MESH_HEADER_LEN + sizeof(plainPayload)];
        writeU32LE(onAir, NODENUM_BROADCAST);
        writeU32LE(onAir + 4, fromNode);
        writeU32LE(onAir + 8, packetId);
        onAir[12] = 0x03 | (0x03 << 5);
        onAir[13] = publicChannelHash(); // SEMPRE il pubblico, non activeChannelKey()
        onAir[14] = 0;
        onAir[15] = 0;
        memcpy(onAir + MESH_HEADER_LEN, encryptedPayload, plainLen);

        int state = radio.transmit(onAir, MESH_HEADER_LEN + plainLen);
        radio.startReceive();

        return state == RADIOLIB_ERR_NONE;
    }

    // "Invito canale" (01/10, richiesto: poter invitare qualcuno via
    // mesh invece di dover digitare/condividere il link a mano) - manda
    // il link di condivisione del canale scelto (02/10: non per forza
    // quello attivo - uno qualsiasi tra quelli salvati, selezionato dalla
    // lista), preceduto da un ID casuale di 8 cifre hex ("INV:<id>:<url>").
    // L'ID serve per il claim "usa e getta": chi lo importa trasmette
    // "CLAIM:<id>", e chiunque altro abbia ancora lo stesso invito in
    // sospeso lo scarta (vedi showPacket() e sendChannelClaim()) -
    // best-effort, non garanzia assoluta su un mezzo broadcast, ma copre
    // il caso normale di un invito mostrato/mandato a una persona sola.
    bool sendChannelInvite(const String& name, const String& pskHex) {
        uint8_t key[32];
        size_t keyLen = parseHexBytes(pskHex, key, sizeof(key));
        if (keyLen != 16 && keyLen != 32) {
            return false;
        }
        String url = buildChannelShareUrl(name, key, keyLen);
        if (url.length() == 0) {
            return false;
        }
        uint32_t inviteId = esp_random();
        char idHex[9];
        snprintf(idHex, sizeof(idHex), "%08X", static_cast<unsigned>(inviteId));
        return sendPublicChannelText("INV:" + String(idHex) + ":" + url);
    }

    bool sendChannelClaim(uint32_t inviteId) {
        char idHex[9];
        snprintf(idHex, sizeof(idHex), "%08X", static_cast<unsigned>(inviteId));
        return sendPublicChannelText("CLAIM:" + String(idHex));
    }

    void writeFixed32LE(uint8_t* p, int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        p[0] = static_cast<uint8_t>(u);
        p[1] = static_cast<uint8_t>(u >> 8);
        p[2] = static_cast<uint8_t>(u >> 16);
        p[3] = static_cast<uint8_t>(u >> 24);
    }

    // Posizione (01/10): stesso Position che decodeMeshData() sa gia'
    // leggere (portnum POSITION_APP=3) - latitude_i/longitude_i sono
    // sfixed32 (wire type 5, 4 byte little-endian grezzi, NON varint -
    // l'errore piu' facile da fare copiando lo schema a memoria), gradi
    // moltiplicati per 1e7 e arrotondati a intero per spec.
    void sendPosition() {
        if (!GPS::hasFix()) {
            appendChatMessage("Tu", "invio posizione annullato: nessun fix GPS", true, defaultChannelHash());
            if (followingTail) {
                ensureBottomVisible();
            }
            drawScreen();
            return;
        }

        int32_t latI = static_cast<int32_t>(GPS::getLatitude() * 1e7);
        int32_t lonI = static_cast<int32_t>(GPS::getLongitude() * 1e7);

        uint8_t posMsg[10];
        size_t pos = 0;
        posMsg[pos++] = 0x0D; // tag campo 1 (latitude_i), wire type 5 (fixed32)
        writeFixed32LE(posMsg + pos, latI);
        pos += 4;
        posMsg[pos++] = 0x15; // tag campo 2 (longitude_i), wire type 5 (fixed32)
        writeFixed32LE(posMsg + pos, lonI);
        pos += 4;

        bool ok = sendDataPacket(3, posMsg, pos);
        appendChatMessage("Tu", ok
            ? ("posizione inviata: " + String(GPS::getLatitude(), 6) + ", " + String(GPS::getLongitude(), 6))
            : "invio posizione FALLITO", true, defaultChannelHash());
        if (followingTail) {
            ensureBottomVisible();
        }
        drawScreen();
    }

    void showPacket(int length, float rssi, float snr, const uint8_t* data) {
        packetCount++;

        if (length < static_cast<int>(MESH_HEADER_LEN)) {
            // Troppo corto per avere anche solo l'header - non e' un
            // pacchetto Meshtastic valido (rumore su un sync word
            // coincidente, o un frame di un altro protocollo). Solo log
            // seriale, non finisce in chat - sarebbe solo rumore li'.
            DEBUG_PRINTF("[MESHTASTIC] pacchetto troppo corto (%d byte), scartato\n", length);
            return;
        }

        MeshHeader h = parseHeader(data);
        int hopLimit = h.flags & 0x07;
        bool wantAck = (h.flags & 0x08) != 0;
        bool viaMqtt = (h.flags & 0x10) != 0;
        // hop_start (bit 5-7) - invariato durante il relay, vedi
        // maybeRelayPacket() - ci dice con quanti salti residui e'
        // PARTITO il pacchetto, cosi' la differenza con hopLimit (quanti
        // ne restano adesso) e' il numero di relay che ha davvero
        // attraversato finora. hop_start=0 e' ambiguo (mittenti troppo
        // vecchi/non conformi) quindi lo trattiamo come "non calcolabile".
        int hopStart = (h.flags >> 5) & 0x07;
        int hopsAway = (hopStart > 0 && hopStart >= hopLimit) ? (hopStart - hopLimit) : -1;

        // Inoltro (02/10) - indipendente da canale/decifratura, un vero
        // nodo mesh fa da relay anche per traffico che non puo' leggere.
        maybeRelayPacket(data, length, h, hopLimit);

        // Bug segnalato dall'utente coi test reali (03/10): "quando mando
        // io un messaggio successivamente lo ricevo io stesso piu' volte"
        // - non e' un vero rimbalzo locale, sono i vicini che rilanciano
        // (relay) il NOSTRO stesso pacchetto broadcast, uno ciascuno: lo
        // riceviamo di nuovo via radio con h.from ancora uguale al nostro
        // NodeNum (il relay non lo cambia). Il nostro messaggio e' gia'
        // mostrato otticamente in chat da noi stessi al momento
        // dell'invio - mostrarlo di nuovo qui duplicherebbe il fumetto una
        // volta per ogni vicino che lo rilancia.
        if (h.from == ourNodeNum()) {
            return;
        }

        int payloadLen = length - static_cast<int>(MESH_HEADER_LEN);
        // 200 byte: sotto il tetto reale (255 - 16 header = 239), copre
        // comodamente i messaggi di testo e i campi nome/posizione.
        constexpr int MAX_TRY = 200;
        int tryLen = payloadLen < MAX_TRY ? payloadLen : MAX_TRY;

        // Prova sia il canale ATTIVO (privato se configurato, altrimenti
        // il pubblico) sia SEMPRE il canale pubblico di default (01/10,
        // bug segnalato: con un canale privato attivo non si vedeva piu'
        // nulla sul pubblico, incluse le risposte all'invito appena
        // mandato - i due hash coincidono quando non c'e' un canale
        // privato configurato, quindi in quel caso e' lo stesso identico
        // comportamento di prima).
        uint8_t activeHash = defaultChannelHash();
        uint8_t pubHash = publicChannelHash();
        bool matchesActive = (h.channelHash == activeHash);
        bool matchesPublic = (h.channelHash == pubHash);

        const uint8_t* keyToUse = nullptr;
        size_t keyLenToUse = 0;
        String channelLabel;
        if (matchesActive) {
            keyToUse = activeChannelKey(keyLenToUse);
            channelLabel = hasCustomChannel() ? "privato" : "pubblico";
        } else if (matchesPublic) {
            keyToUse = DEFAULT_CHANNEL_PSK;
            keyLenToUse = sizeof(DEFAULT_CHANNEL_PSK);
            channelLabel = "pubblico";
        } else if (findMatchingSavedChannel(h.channelHash, &keyToUse, &keyLenToUse)) {
            // 02/10, richiesto esplicitamente: un canale salvato ma non
            // attivo in questo momento continua comunque a ricevere - il
            // messaggio arriva taggato con il SUO hash (h.channelHash,
            // vedi appendChatMessage() piu' sotto), quindi si trova nello
            // storico di quel canale appena ci si passa sopra dalla lista.
            channelLabel = "privato";
        }

        // NodeDB (01/10): un nodo con quel NodeNum e' comunque li', anche
        // se non riusciamo a leggere il contenuto del pacchetto. Canale
        // "sconosciuto" (02/10) se l'hash non combacia con nessuno dei due
        // che proviamo - il nodo c'e', ma non sappiamo su quale canale.
        recordNodeSeen(h.from, rssi, snr, channelLabel.length() > 0 ? channelLabel : "sconosciuto",
                      hopsAway, viaMqtt);

        String contentLine;
        String typeText;
        // Filtro chat (03/10, bug segnalato dai test reali: "si vedono
        // anche i messaggi di telemetria o altri, bisogna filtrare e
        // vedere solo i messaggi di testo inviati dagli utenti, la
        // telemetria ed il resto sara' visibile nella sezione della
        // lista nodi") - vero SOLO per un vero messaggio di testo
        // (portnum 1, incluso l'invito canale qui sotto); nodeinfo/
        // posizione/telemetria/canale non decifrabile aggiornano
        // comunque NodeDB (recordNodeSeen/Info/Position, sopra/sotto)
        // ma non finiscono come fumetto in chat.
        bool showInChat = false;

        if (keyToUse == nullptr) {
            typeText = "canale sconosciuto";
            contentLine = "canale 0x" + String(h.channelHash, HEX) + " non riconosciuto - non decifrabile qui";
        } else {
            uint8_t decrypted[MAX_TRY];
            channelCrypt(data + MESH_HEADER_LEN, tryLen, h.from, h.id, keyToUse, keyLenToUse, decrypted);

            DecodedData decoded = decodeMeshData(decrypted, tryLen);

            bool isPublicChannelMsg = (channelLabel == "pubblico");

            if (decoded.portnum == 1 && decoded.textMessage.length() > 0 && isPublicChannelMsg && decoded.textMessage.startsWith("CLAIM:")) {
                // "Usa e getta" (01/10): un altro dispositivo ha gia'
                // importato questo invito - se e' lo stesso che abbiamo
                // ancora in sospeso, lo scartiamo subito. E' un messaggio
                // di servizio, non finisce in chat.
                uint32_t claimedId = strtoul(decoded.textMessage.substring(6).c_str(), nullptr, 16);
                if (pendingInviteUrl.length() > 0 && claimedId == pendingInviteId) {
                    pendingInviteUrl = "";
                    pendingInviteId = 0;
                    // "active", non "mode == Mode::Main" (03/10, bug
                    // segnalato: "passa una schermata in primo piano che
                    // copre la schermata che sto utilizzando" mentre
                    // Meshtastic e' in secondo piano) - "mode" resta
                    // Main anche da sfondo (closeMeshtastic() non lo
                    // tocca, serve a backgroundUpdate() per continuare a
                    // decodificare), quindi ridisegnare su quella
                    // condizione scriveva sullo schermo vero sopra
                    // qualunque altra schermata l'utente stesse usando.
                    if (active) {
                        drawScreen();
                    }
                }
                return;
            } else if (decoded.portnum == 1 && decoded.textMessage.length() > 0 && isPublicChannelMsg && decoded.textMessage.startsWith("INV:")) {
                // Invito canale (01/10): formato "INV:<id hex 8 cifre>:<url>"
                // - lo teniamo da parte, un pulsante sulla schermata CANALE
                // lo propone per l'import con un tap. Riconosciuto SOLO sul
                // pubblico, dove gli inviti viaggiano sempre.
                String rest = decoded.textMessage.substring(4);
                int colon = rest.indexOf(':');
                String idHex = colon == 8 ? rest.substring(0, colon) : String();
                String url = colon == 8 ? rest.substring(colon + 1) : String();
                if (idHex.length() == 8 && url.startsWith("https://meshtastic.org/e/#")) {
                    pendingInviteUrl = url;
                    pendingInviteId = strtoul(idHex.c_str(), nullptr, 16);
                    pendingInviteReceivedMs = millis();
                    contentLine = "[invito canale]\n(vedi MESHTASTIC > per importarlo)";
                } else {
                    contentLine = "\"" + decoded.textMessage + "\"";
                }
                showInChat = true;
            } else if (decoded.portnum == 1 && decoded.textMessage.length() > 0) {
                contentLine = "\"" + decoded.textMessage + "\"";
                showInChat = true;
            } else if (decoded.portnum == 4 && (decoded.longName.length() > 0 || decoded.shortName.length() > 0)) {
                contentLine = decoded.longName;
                if (decoded.shortName.length() > 0) {
                    contentLine += " (" + decoded.shortName + ")";
                }
                recordNodeInfo(h.from, decoded.longName, decoded.shortName);
            } else if (decoded.portnum == 3 && decoded.hasPosition) {
                contentLine = String(decoded.latitude, 6) + ", " + String(decoded.longitude, 6);
                recordNodePosition(h.from, decoded.latitude, decoded.longitude);
            } else if (decoded.portnum == 67) {
                // TELEMETRIA (03/10) - mai in chat (showInChat resta
                // false), finisce SOLO nel dettaglio nodo - vedi
                // recordNodeTelemetry()/drawNodeDetailScreen().
                recordNodeTelemetry(h.from, decoded);
            } else {
                // Portnum non ancora interpretato: stessi byte
                // stampabili di prima, come riscontro grezzo.
                String readable;
                for (int i = 0; i < tryLen; i++) {
                    char c = static_cast<char>(decrypted[i]);
                    readable += (c >= 0x20 && c <= 0x7E) ? c : '.';
                }
                contentLine = readable;
            }

            const char* typeLabel = portnumLabel(decoded.portnum);
            typeText = typeLabel != nullptr ? String(typeLabel) : ("portnum " + String(decoded.portnum));
        }

        if (showInChat) {
            String meta = "[" + typeText + " - " + (channelLabel.length() > 0 ? channelLabel : "?") + "] h" + String(hopLimit) +
                (wantAck ? " ACK" : "") + (viaMqtt ? " MQTT" : "") + " " + String(rssi, 0) + "dB";
            appendChatMessage(nodeIdText(h.from), contentLine + "\n" + meta, false, h.channelHash);
            // Notifica (02/10, richiesto: "non li perdo"; estesa il
            // 18/10 - segnalato dall'utente: stando su un'ALTRA
            // sotto-schermata di Meshtastic, es. lista nodi, non solo
            // fuori da Meshtastic del tutto, vale la stessa logica:
            // "active" da solo significa solo "Meshtastic ha il fuoco",
            // NON "sto guardando la chat" - serve anche "mode ==
            // Mode::Main" per quello.
            if (!active || mode != Mode::Main) {
                unread = true;
                Sound::playLoRaMessageReceived();
            }
            if (followingTail) {
                ensureBottomVisible();
            }
        }
        // Ridisegnata SOLO se la schermata CHAT e' davvero quella
        // mostrata in questo momento (03/10, bug segnalato dall'utente:
        // in secondo piano un messaggio in arrivo faceva comparire la
        // chat Meshtastic SOPRA qualunque altra schermata si stesse
        // usando, e quella restava "congelata" dopo perche' non sapeva
        // di dover ridisegnarsi - "facciamo che quando e' in background
        // se arriva il messaggio fa solo il suono... non viene mostrata
        // alcuna maschera"). Esteso il 18/10: lo stesso identico
        // problema succedeva ANCHE stando dentro Meshtastic ma su
        // un'altra sotto-schermata (lista nodi, dettaglio, ecc.) -
        // "active" da solo non bastava, serviva anche "mode ==
        // Mode::Main" perche' drawScreen() e' la funzione della SOLA
        // chat principale: chiamarla con un "mode" diverso sovrascriveva
        // lo schermo con la chat senza pero' cambiare "mode", lasciando
        // poi l'input dispatch (vedi update()) a gestire eventi per una
        // sotto-schermata che non era piu' quella davvero disegnata -
        // da qui la sensazione di "si blocca tutto".
        if (active && mode == Mode::Main) {
            drawScreen();
        }
    }

    // Estratta da update() (02/10, richiesto: poter ascoltare anche in
    // secondo piano) - chiamata sia dal primo piano (update(), stesso
    // comportamento di prima) sia da backgroundUpdate() quando la
    // schermata non e' in vista, cosi' i pacchetti continuano ad arrivare
    // in NodeDB/storico e la notifica a scattare anche fuori dalla chat.
    void drainIncomingPacket() {
        if (!packetFlag) {
            return;
        }
        packetFlag = false;
        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        size_t length = radio.getPacketLength();
        if (length > 0 && length <= 255) {
            uint8_t buffer[255];
            int state = radio.readData(buffer, length);
            if (state == RADIOLIB_ERR_NONE) {
                showPacket(static_cast<int>(length), radio.getRSSI(), radio.getSNR(), buffer);
            } else {
                // Interrupt scattato ma la lettura e' fallita (es. CRC non
                // valido) - diverso da "non e' arrivato niente": conferma
                // che la radio sta davvero rilevando trasmissioni sul
                // canale, solo non decodificabili.
                DEBUG_PRINTF("[MESHTASTIC] interrupt ricevuto, readData fallita: lunghezza=%u stato=%d\n",
                    static_cast<unsigned>(length), state);
            }
        } else {
            DEBUG_PRINTF("[MESHTASTIC] interrupt ricevuto ma lunghezza pacchetto non valida: %u\n",
                static_cast<unsigned>(length));
        }
        radio.startReceive();
    }

} // namespace

namespace MeshtasticUI {

    void open() {
        active = true;
        mode = Mode::Main;
        unread = false; // si sta guardando la chat adesso, i messaggi arrivati si "leggono"

        // Sicurezza (16/10, segnalato dall'utente: "potenzialmente
        // potrei essere da tutt'altra parte del mondo"; chiarito
        // ulteriormente il 18/10 - vale anche per un vero spegnimento/
        // riavvio, non solo per la chiusura senza secondo piano) -
        // radioBorrowed falso qui vuol dire che l'ascolto radio NON e'
        // ininterrotto da qualche momento nel passato (chiusura senza
        // secondo piano, O il primissimo avvio dopo un riavvio/
        // spegnimento): in ENTRAMBI i casi i nodi di una sessione
        // precedente non sono piu' attendibili, si riparte sempre da
        // una lista vuota. Stesso trattamento del cambio preset: vedi
        // clearNodeDb().
        if (!radioBorrowed) {
            clearNodeDb();
        }

        Display::setDirectDrawMode(true);

        // Se la radio e' gia' nostra (02/10: si rientra con il secondo
        // piano ancora attivo) e' gia' configurata e in ascolto - non la
        // si tocca, altrimenti si perderebbe un pacchetto a meta' lettura
        // e si riazzererebbe inutilmente packetCount.
        if (!radioBorrowed) {
            packetCount = 0;
            LoRaRadio::pauseForExternalUse();
            radioBorrowed = true;
            SX1262& radio = LoRaRadio::getRadioForExternalUse();

            // Log diagnostico (30/09 sera, richiesto: nessun pacchetto
            // ricevuto - prima di fidarsi del silenzio radio, verificare
            // che la configurazione sia stata DAVVERO accettata, non solo
            // "chiamata senza controllare l'esito") - ogni setter RadioLib
            // ritorna RADIOLIB_ERR_NONE (0) solo se il parametro e' valido.
            int errFreq = radio.setFrequency(MESH_FREQ_MHZ);
            int errBw = radio.setBandwidth(MESH_BANDWIDTH_KHZ);
            int errSf = radio.setSpreadingFactor(activeSpreadingFactor());
            int errCr = radio.setCodingRate(MESH_CR);
            int errSync = radio.setSyncWord(MESH_SYNC_WORD);
            int errPreamble = radio.setPreambleLength(MESH_PREAMBLE);

            DEBUG_PRINTF(
                "[MESHTASTIC] setFrequency=%d setBandwidth=%d setSpreadingFactor=%d setCodingRate=%d setSyncWord=%d setPreambleLength=%d (0=OK)\n",
                errFreq, errBw, errSf, errCr, errSync, errPreamble
            );

            radio.setDio1Action(onPacket);
            packetFlag = false;
            int errReceive = radio.startReceive();
            DEBUG_PRINTF("[MESHTASTIC] startReceive=%d (0=OK), freq impostata=%.3f MHz\n", errReceive, MESH_FREQ_MHZ);
        }

        ensureBottomVisible();
        drawScreen();
    }

    bool isActive() {
        return active;
    }

    // Per l'icona LORA in alto (02/10) - true se la radio sta davvero
    // ascoltando per Meshtastic, in primo piano O in secondo piano.
    bool isListening() {
        return radioBorrowed;
    }

    bool hasUnread() {
        return unread;
    }

    // Vedi MeshtasticUI.h - richiamata da SettingsUI.cpp (riga "PRESET
    // LORA").
    bool isMediumFastPreset() {
        ensureChannelConfigLoaded();
        return mediumFastPreset;
    }

    void setMediumFastPreset(bool mediumFast) {
        ensureChannelConfigLoaded();
        if (mediumFastPreset == mediumFast) {
            return;
        }
        mediumFastPreset = mediumFast;
        channelPrefs.begin(CHANNEL_PREFS_NS, false);
        channelPrefs.putBool("presetMF", mediumFastPreset);
        channelPrefs.end();

        // Bug segnalato dal vivo il 16/10: cambiare preset e' cambiare
        // rete (spreading factor diverso, normalmente non ci si sente
        // tra reti diverse) - i nodi visti col preset precedente vanno
        // svuotati, altrimenti restano in lista per sempre mescolati ai
        // nuovi (o al posto loro, dato che l'LRU non li sostituisce se
        // la tabella non e' piena). Vedi clearNodeDb().
        clearNodeDb();

        // Se la radio e' gia' nostra (primo piano o secondo piano gia'
        // attivi), va riconfigurata SUBITO con il nuovo spreading factor -
        // altrimenti si resterebbe in ascolto col preset vecchio finche'
        // non si richiude e riapre la schermata.
        if (radioBorrowed) {
            SX1262& radio = LoRaRadio::getRadioForExternalUse();
            radio.setSpreadingFactor(activeSpreadingFactor());
            packetFlag = false;
            radio.startReceive();
        }
    }

    // Chiamata SEMPRE da App.cpp (come WebRadioUI::backgroundUpdate()),
    // non solo quando questa schermata e' in primo piano - drena i
    // pacchetti radio mentre si e' in secondo piano. Non fa nulla se non
    // stiamo tenendo la radio (ne' primo ne' secondo piano attivi).
    void backgroundUpdate() {
        if (!radioBorrowed) {
            return;
        }
        drainIncomingPacket();

        // Controllo scadenza nodi (16/10) - ogni minuto basta e avanza
        // dato il timeout di 30 minuti, non serve farlo ad ogni ciclo.
        uint32_t now = millis();
        if (now - lastStaleCheckMs > 60000) {
            lastStaleCheckMs = now;
            expireStaleNodes();
        }
    }

    void update() {
        if (!active) {
            return;
        }

        // Il drenaggio pacchetti (02/10) e' passato a backgroundUpdate(),
        // chiamata SEMPRE da App.cpp indipendentemente da quale schermata
        // e' in primo piano - qui restano solo le cose che hanno senso
        // SOLO quando questa schermata e' davvero quella visibile
        // (log di liveness, touch/trackball).

        // Log periodico di "sono vivo" (ogni 10s) con l'RSSI istantaneo
        // del canale - prova che la radio sta davvero campionando in
        // continuo, non solo che non e' arrivato nulla di valido finora.
        static uint32_t lastLivenessLog = 0;
        if (millis() - lastLivenessLog > 10000) {
            lastLivenessLog = millis();
            SX1262& radio = LoRaRadio::getRadioForExternalUse();
            DEBUG_PRINTF("[MESHTASTIC] in ascolto, RSSI canale=%.1fdBm, pacchetti finora=%d\n",
                radio.getRSSI(false), packetCount);
        }

        if (mode == Mode::Compose) {
            handleComposeInput();
            return;
        }
        if (mode == Mode::NodeList) {
            handleNodeListInput();
            return;
        }
        if (mode == Mode::NodeDetail) {
            handleNodeDetailInput();
            return;
        }
        if (mode == Mode::ChannelList) {
            handleChannelListInput();
            return;
        }
        if (mode == Mode::ChannelConfig) {
            handleChannelConfigInput();
            return;
        }
        if (mode == Mode::ChannelEditField) {
            handleChannelEditInput();
            return;
        }
        if (mode == Mode::ChannelQr) {
            handleChannelQrInput();
            return;
        }
        if (mode == Mode::ChannelImport) {
            handleChannelImportInput();
            return;
        }

        // Scroll della lista messaggi (trascinamento) - stesso schema di
        // HermesUI::handleChatInput(), incluso lo scroll "dentro" un
        // messaggio piu' alto della vista (02/10).
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && messageCount > 0) {
            dragAccumY += dragDelta;

            bool scrolled = false;
            while (dragAccumY >= SCROLL_STEP_PX) {
                if (!scrollStepUp()) {
                    dragAccumY = SCROLL_STEP_PX;
                    break;
                }
                dragAccumY -= SCROLL_STEP_PX;
                scrolled = true;
            }
            while (dragAccumY <= -SCROLL_STEP_PX) {
                if (!scrollStepDown()) {
                    dragAccumY = -SCROLL_STEP_PX;
                    break;
                }
                dragAccumY += SCROLL_STEP_PX;
                scrolled = true;
            }

            followingTail = isAtBottom();
            if (scrolled) {
                drawScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped == sendZone) {
            openComposeScreen();
            return;
        }
        if (tapped == positionZone) {
            sendPosition();
            return;
        }
        if (tapped == statusZone) {
            openNodeListScreen();
            return;
        }
        if (tapped == titleZone) {
            openChannelListScreen();
            return;
        }
        if (tapped == bgZone) {
            toggleMainBackground();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeMeshtastic();
                break;

            case InputEvent::Select:
                // Attiva il controllo a fuoco (04/10, vedi MainFocus
                // sopra) - Compose e' il default, stesso comportamento
                // di sempre per chi non tocca mai Sinistra/Destra qui.
                switch (mainFocus) {
                    case MainFocus::Compose:     openComposeScreen();   break;
                    case MainFocus::Gps:         sendPosition();        break;
                    case MainFocus::NodeList:    openNodeListScreen();  break;
                    case MainFocus::ChannelList: openChannelListScreen(); break;
                    case MainFocus::Background:  toggleMainBackground(); break;
                }
                break;

            case InputEvent::Left:
                if (mainFocus != MainFocus::Compose) {
                    mainFocus = static_cast<MainFocus>(static_cast<int>(mainFocus) - 1);
                    drawScreen();
                }
                break;

            case InputEvent::Right:
                if (mainFocus != MainFocus::Background) {
                    mainFocus = static_cast<MainFocus>(static_cast<int>(mainFocus) + 1);
                    drawScreen();
                }
                break;

            case InputEvent::Up:
                if (scrollStepUp()) {
                    followingTail = isAtBottom();
                    drawScreen();
                }
                break;

            case InputEvent::Down:
                if (scrollStepDown()) {
                    followingTail = isAtBottom();
                    drawScreen();
                }
                break;

            default:
                break;
        }
    }
}
