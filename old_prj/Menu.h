#pragma once

#include "Input.h"

enum class MenuAction {
    None,
    FirstItem,
    SecondItem,
    ThirdItem,
    FourthItem,
    Settings,
    About
};

namespace Menu {

    using ActionCallback = void (*)(MenuAction action);

    void begin(ActionCallback callback);
    void update();

    void showMain();
    void showCurrentScreen();
    void back();

    bool isMainMenu();
}
