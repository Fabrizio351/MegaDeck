#pragma once

// Scansione RSSI grezza della banda 868MHz (26/09, vedi LoRaScanUI.cpp
// per i dettagli): misura solo l'energia radio presente ad ogni
// frequenza, un passo alla volta, senza decodificare nulla - primo passo
// per capire DOVE c'e' attivita' (es. un telecomando) prima di provare a
// costruire una cattura/decodifica vera e propria (fase successiva, non
// ancora iniziata: un eventuale codice "rolling" non si potrebbe comunque
// clonare con un semplice registra-e-ripeti).
namespace LoRaScanUI {
    void open();
    void update();
    bool isActive();
}
