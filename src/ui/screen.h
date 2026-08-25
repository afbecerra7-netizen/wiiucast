// UI de consola sobre OSScreen: dibuja en TV y GamePad, y sobrevive a que el
// sistema nos quite y devuelva el foreground (menú HOME, applets).
#pragma once
#include <wut_types.h>

typedef enum {
   SCREEN_TARGET_BOTH = 0,
   SCREEN_TARGET_TV,
   SCREEN_TARGET_DRC,
} ScreenTarget;

// Registra los callbacks de ProcUI y asigna los buffers. Llamar DESPUÉS de
// WHBProcInit() (necesita ProcUI inicializado).
BOOL screen_init(void);
void screen_shutdown(void);

// ¿Tenemos el foreground? Mientras sea FALSE no se dibuja nada.
BOOL screen_has_foreground(void);

// Un frame: clear -> varias screen_text() -> screen_present()
void screen_begin(uint32_t color);
void screen_text(ScreenTarget target, int col, int row, const char *text);
void screen_textf(ScreenTarget target, int col, int row, const char *fmt, ...);
void screen_present(void);

// Columnas útiles de cada pantalla (para centrar texto)
int screen_cols(ScreenTarget target);
