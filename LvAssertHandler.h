#pragma once

// Incluso da LVGL dentro file .c puri (lv_assert.h, via
// LV_ASSERT_CUSTOM_INCLUDE in lv_conf.h) - deve quindi essere valido
// anche come header C semplice, niente C++/namespace qui dentro. La
// funzione vera e propria e' in CrashLog.cpp (C++, esposta con
// collegamento C tramite extern "C").
#ifdef __cplusplus
extern "C" {
#endif

void crashlog_record_lvgl_oom_and_restart(void);

#ifdef __cplusplus
}
#endif

#define LV_ASSERT_HANDLER crashlog_record_lvgl_oom_and_restart();
