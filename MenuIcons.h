#pragma once

#include <lvgl.h>

static lv_color_t menuIconColor(int index)
{
    switch (index) {
        case 0:
            return lv_color_hex(0x3498DB); // VoIP

        case 1:
            return lv_color_hex(0x2ECC71); // WiFi

        case 2:
            return lv_color_hex(0x2980B9); // BT

        case 3:
            return lv_color_hex(0x9B59B6); // LoRa

        case 4:
            return lv_color_hex(0xE67E22); // GPS

        case 5:
            return lv_color_hex(0xF1C40F); // Net Tools

        case 6:
            return lv_color_hex(0x1ABC9C); // VPN

        case 7:
            return lv_color_hex(0xE74C3C); // OpenClaw

        case 8:
            return lv_color_hex(0x27AE60); // Home Assistant

        case 9:
            return lv_color_hex(0xD35400); // Frigate

        case 10:
            return lv_color_hex(0x8E44AD); // Drone

        case 11:
            return lv_color_hex(0x95A5A6); // Impostazioni

        default:
            return lv_color_hex(0x7F8C8D);
    }
}

// Simboli gia' inclusi nei font bitmap montserrat forniti da LVGL (icone
// stile FontAwesome nel range Unicode privato 0xF000-0xF8FF): niente
// immagini/font aggiuntivi da caricare, funzionano ovunque sia abilitato
// LV_FONT_MONTSERRAT_14.
static const char* menuIconSymbol(int index)
{
    switch (index) {
        case 0:
            return LV_SYMBOL_CALL;       // VoIP

        case 1:
            return LV_SYMBOL_WIFI;       // WiFi

        case 2:
            return LV_SYMBOL_BLUETOOTH;  // BT

        case 3:
            return LV_SYMBOL_LOOP;       // LoRa

        case 4:
            return LV_SYMBOL_GPS;        // GPS

        case 5:
            return LV_SYMBOL_SHUFFLE;    // Net Tools (SETTINGS e' riservato a Impostazioni, vedi case 12)

        case 6:
            return LV_SYMBOL_EYE_CLOSE;  // VPN

        case 7:
            return LV_SYMBOL_PLAY;       // OpenClaw

        case 8:
            return LV_SYMBOL_HOME;       // Home Assistant

        case 9:
            return LV_SYMBOL_VIDEO;      // Frigate

        case 10:
            return LV_SYMBOL_UP;         // Drone

        case 11:
            return LV_SYMBOL_SETTINGS;   // Impostazioni

        default:
            return LV_SYMBOL_LIST;
    }
}

// Icona generica (simbolo + colore a scelta): usata dai sottomenu a
// griglia diversi dal menu principale (es. Net Tools), che hanno il
// proprio set di voci e non devono condividere gli indici della griglia
// principale sopra.
static lv_obj_t* createMenuIconCustom(
    lv_obj_t* parent,
    const char* symbol,
    lv_color_t color
)
{
    lv_obj_t* icon = lv_obj_create(parent);

    lv_obj_remove_style_all(icon);

    lv_obj_set_size(
        icon,
        28,
        28
    );

    lv_obj_set_style_radius(
        icon,
        14,
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_color(
        icon,
        color,
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        icon,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        icon,
        0,
        LV_PART_MAIN
    );

    lv_obj_set_style_pad_all(
        icon,
        0,
        LV_PART_MAIN
    );

    lv_obj_t* label = lv_label_create(icon);

    lv_label_set_text(
        label,
        symbol
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_white(),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_font(
        label,
        &lv_font_montserrat_14,
        LV_PART_MAIN
    );

    lv_obj_center(label);

    return icon;
}

static lv_obj_t* createMenuIcon(
    lv_obj_t* parent,
    int index
)
{
    return createMenuIconCustom(parent, menuIconSymbol(index), menuIconColor(index));
}