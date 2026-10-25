#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tt::ui {

struct Palette {
    COLORREF background;
    COLORREF surface;
    COLORREF text;
    COLORREF muted;
    COLORREF border;
    COLORREF accent;
    COLORREF accentText;
    COLORREF success;
    COLORREF warning;
    bool dark;
    bool highContrast;
};

enum class ControlKind { Button, Toggle, Combo };

Palette paletteFor(bool dark, bool contrast);
bool initialize();
void shutdown();
void setPalette(const Palette& palette);
const Palette& currentPalette();

void rounded(HDC dc, const RECT& bounds, float radius, COLORREF fill,
             COLORREF edge = CLR_INVALID);
void drawButton(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font, UINT dpi,
                bool primary, bool disabled, bool hot, bool pressed, bool focused,
                bool quiet = false, bool surface = false);
void drawButton(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font, UINT dpi,
                bool primary, bool disabled, float hoverProgress, bool pressed, bool focused,
                bool quiet = false, bool surface = false);
void drawToggle(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
                bool checked, bool disabled, bool hot, bool focused, bool surface = true);
void drawCombo(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
               bool disabled, bool hot, bool focused, bool open, bool surface = false);
void drawComboItem(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
                   bool selected, bool disabled);
void drawBrand(HDC dc, const RECT& bounds);
void drawFloatingButton(HDC dc, const RECT& bounds, UINT dpi, float hoverProgress,
                        bool pressed, bool disabled);
void drawSurface(HDC dc, const RECT& bounds, UINT dpi, bool card = false);

void installControl(HWND window, ControlKind kind, bool surface = false);
bool isHot(HWND window);
float hotProgress(HWND window);

} // namespace tt::ui
