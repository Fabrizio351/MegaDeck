#include "TouchUI.h"
#include "Input.h"

namespace {
    struct Zone {
        int16_t x, y, w, h;
    };

    Zone zones[TouchUI::MAX_ZONES];
    int zoneCount = 0;

    bool wasTouched = false;
    int16_t startX = 0;
    int16_t startY = 0;

    // Spostamento massimo (al quadrato, per evitare una sqrt ad ogni
    // ciclo) osservato durante la pressione corrente rispetto al punto
    // di partenza - oltre la soglia e' uno scroll/trascinamento, non un tap.
    int32_t maxDeltaSq = 0;

    constexpr int16_t TAP_MOVE_TOLERANCE_PX = 14;
    constexpr int32_t TAP_MOVE_TOLERANCE_SQ =
        static_cast<int32_t>(TAP_MOVE_TOLERANCE_PX) * TAP_MOVE_TOLERANCE_PX;

    bool dragActive = false;
    int16_t lastDragY = 0;
}

namespace TouchUI {

    void clearZones() {
        zoneCount = 0;
    }

    int addZone(int16_t x, int16_t y, int16_t w, int16_t h) {
        if (zoneCount >= MAX_ZONES) {
            return -1;
        }
        zones[zoneCount] = { x, y, w, h };
        return zoneCount++;
    }

    int pollTap() {
        bool touched = Input::isTouched();
        int16_t x = Input::getTouchX();
        int16_t y = Input::getTouchY();
        int result = -1;

        if (touched && !wasTouched) {
            // Inizio pressione: nuovo punto di partenza.
            startX = x;
            startY = y;
            maxDeltaSq = 0;
        } else if (touched && wasTouched) {
            int32_t dx = x - startX;
            int32_t dy = y - startY;
            int32_t deltaSq = dx * dx + dy * dy;
            if (deltaSq > maxDeltaSq) {
                maxDeltaSq = deltaSq;
            }
        } else if (!touched && wasTouched) {
            // Rilascio: tap valido solo se ci si e' spostati poco dal
            // punto di partenza durante tutta la pressione.
            if (maxDeltaSq <= TAP_MOVE_TOLERANCE_SQ) {
                for (int i = 0; i < zoneCount; i++) {
                    const Zone& z = zones[i];
                    if (startX >= z.x && startX < z.x + z.w &&
                        startY >= z.y && startY < z.y + z.h) {
                        result = i;
                        break;
                    }
                }
            }
        }

        wasTouched = touched;
        return result;
    }

    int16_t pollDragDeltaY() {
        bool touched = Input::isTouched();
        int16_t y = Input::getTouchY();
        int16_t delta = 0;

        if (touched) {
            if (dragActive) {
                delta = y - lastDragY;
            }
            dragActive = true;
            lastDragY = y;
        } else {
            dragActive = false;
        }

        return delta;
    }
}
