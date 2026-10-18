# MegaDeck

Firmware custom per il **LilyGO T-Deck Plus** (ESP32-S3), scritto da zero
in Arduino/C++ senza LVGL (disegno diretto su display). Nessuna
dipendenza da librerie SIP/VoIP/Meshtastic pronte all'uso: i protocolli
di rete sono stati reimplementati a mano dove non esisteva una libreria
Arduino completa e mantenuta.

## Funzionalita' principali

- **Rete**: scansione WiFi, scanner di rete (ArpSweep/NetScan), NetGuard
  (monitoraggio dispositivi noti/sconosciuti sulla rete), Telnet, VPN
  WireGuard (configurazione da SD card).
- **LoRa**: scanner RSSI, LoRaWAN, client Meshtastic completo (chat
  multi-canale, NodeDB con telemetria, inviti via QR, relay, modalita'
  secondo piano).
- **VoIP**: client SIP/RTP scritto da zero (REGISTER, chiamate in
  uscita, audio full-duplex G.711, digest auth, gestione NAT) -
  funziona con qualsiasi provider SIP standard, nessun vendor lock-in.
- **BLE**: scanner e client GATT generico.
- **Audio**: web radio (stream MP3/AAC), microfono (ES7210), bip di
  sistema.
- **Chat AI**: client per un server Hermes self-hosted (agente AI via
  API HTTP).
- **Altro**: GPS con info localita'/meteo, blocco schermo con combo,
  impostazioni centralizzate, diagnostica crash on-device.

## Build

Compilato e caricato con [arduino-cli](https://arduino.github.io/arduino-cli/),
mai con l'IDE grafico. Le librerie necessarie sono vendorizzate nella
cartella `libraries/` di questo repository.

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app" \
  --libraries ./libraries .

arduino-cli upload -p /dev/ttyACM0 \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app" .
```

**Importante**: il flag `PSRAM=opi` e' obbligatorio - senza, il
display resta nero al boot senza nessun errore di compilazione.
`PartitionScheme=huge_app` serve per lo spazio flash che questo
firmware occupa.

## Configurazione personale (nessun segreto nel codice)

Questo repository non contiene credenziali di nessun tipo - ogni
account (VoIP/SIP, server Hermes AI) va impostato dalla schermata
Impostazioni del dispositivo dopo il primo avvio, non nel codice
sorgente. I valori restano salvati solo in NVS sul tuo T-Deck.

## Licenza

MIT - vedi [LICENSE](LICENSE).

## Supporto

Se questo progetto ti e' utile: [paypal.me/fabrizioca](https://paypal.me/fabrizioca)

---

Sviluppato da fabrizio351 con [Claude Code](https://claude.com/claude-code).
