// Shared, theme-aware drawing for TypeThru's native Win32 controls.
#include "ui.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <new>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace tt::ui {

static void drawToggleAnimated(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font,
                               UINT dpi, float checkedProgress, float hoverProgress,
                               bool disabled, bool focused, bool surface);
static void drawComboAnimated(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font,
                              UINT dpi, bool disabled, float hoverProgress,
                              bool focused, bool open, bool surface);

namespace {

using namespace Gdiplus;

constexpr UINT_PTR kUiSubclassId = 0x54545549;
constexpr COLORREF kBrandCyan = RGB(92, 224, 255);
constexpr COLORREF kBrandViolet = RGB(140, 97, 255);
constexpr COLORREF kBrandBlue = RGB(96, 154, 255);

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusFailed = false;
Palette g_palette = {
    RGB(246, 247, 250), RGB(255, 255, 255), RGB(31, 33, 39), RGB(104, 108, 118),
    RGB(224, 227, 232), kBrandBlue, RGB(12, 28, 58), RGB(46, 185, 126),
    RGB(194, 129, 31), false, false
};

struct ControlState {
    ControlKind kind;
    bool surface;
    bool hot;
    bool tracking;
    struct Tween {
        float from = 0.0f;
        float target = 0.0f;
        ULONGLONG started = 0;
        bool active = false;
    } hover;
    Tween checked;
};

constexpr UINT_PTR kAnimationTimerId = 0x5454414E;
constexpr ULONGLONG kAnimationDurationMs = 120;

bool ensureGdiplus() {
    if (g_gdiplusToken) return true;
    if (g_gdiplusFailed) return false;
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Ok) {
        g_gdiplusFailed = true;
        return false;
    }
    return true;
}

int scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

REAL scaleReal(UINT dpi, REAL value) {
    return value * static_cast<REAL>(dpi ? dpi : 96) / 96.0f;
}

void fillSolid(HDC dc, const RECT& bounds, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
}

COLORREF mix(COLORREF first, COLORREF second, int amount) {
    const int inverse = 100 - amount;
    return RGB((GetRValue(first) * inverse + GetRValue(second) * amount) / 100,
               (GetGValue(first) * inverse + GetGValue(second) * amount) / 100,
               (GetBValue(first) * inverse + GetBValue(second) * amount) / 100);
}

Color gdipColor(COLORREF color, BYTE alpha = 255) {
    return Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

float tweenValue(const ControlState::Tween& tween, ULONGLONG now) {
    if (!tween.active || now - tween.started >= kAnimationDurationMs) return tween.target;
    const float linear = static_cast<float>(now - tween.started)
        / static_cast<float>(kAnimationDurationMs);
    const float inverse = 1.0f - linear;
    const float eased = 1.0f - inverse * inverse * inverse;
    return tween.from + (tween.target - tween.from) * eased;
}

bool animationsAllowed() {
    if (g_palette.highContrast) return false;
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)
        && enabled != FALSE;
}

void setTween(ControlState::Tween& tween, float target, bool animated) {
    const ULONGLONG now = GetTickCount64();
    const float current = tweenValue(tween, now);
    tween.from = current;
    tween.target = target;
    tween.started = now;
    tween.active = animated && std::fabs(current - target) > 0.001f;
    if (!tween.active) tween.from = target;
}

bool animationActive(const ControlState& state, ULONGLONG now) {
    return (state.hover.active && now - state.hover.started < kAnimationDurationMs)
        || (state.checked.active && now - state.checked.started < kAnimationDurationMs);
}

void updateAnimationTimer(HWND window, const ControlState& state) {
    if (animationActive(state, GetTickCount64())) {
        SetTimer(window, kAnimationTimerId, 16, nullptr);
    } else {
        KillTimer(window, kAnimationTimerId);
    }
}

void targetChecked(HWND window, ControlState& state, bool animated) {
    if (state.kind != ControlKind::Toggle) return;
    const float target = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0f : 0.0f;
    setTween(state.checked, target, animated);
    updateAnimationTimer(window, state);
}

RectF rectF(const RECT& bounds) {
    return RectF(static_cast<REAL>(bounds.left), static_cast<REAL>(bounds.top),
                 static_cast<REAL>(std::max(0L, bounds.right - bounds.left)),
                 static_cast<REAL>(std::max(0L, bounds.bottom - bounds.top)));
}

void prepare(Graphics& graphics) {
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
}

void fillSolid(Graphics& graphics, const RectF& bounds, COLORREF color) {
    SolidBrush brush(gdipColor(color));
    graphics.FillRectangle(&brush, bounds);
}

void roundedPath(GraphicsPath& path, const RectF& bounds, REAL radius) {
    const REAL width = bounds.Width;
    const REAL height = bounds.Height;
    if (width <= 0.0f || height <= 0.0f) return;
    const REAL r = std::max(0.0f, std::min(radius, std::min(width, height) / 2.0f));
    if (r < 0.75f) {
        path.AddRectangle(bounds);
        return;
    }
    const REAL diameter = r * 2.0f;
    path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(bounds.X + width - diameter, bounds.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(bounds.X + width - diameter, bounds.Y + height - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(bounds.X, bounds.Y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void fillRounded(Graphics& graphics, const RectF& bounds, REAL radius, COLORREF fill,
                 COLORREF edge = CLR_INVALID, BYTE alpha = 255, REAL edgeWidth = 1.0f) {
    GraphicsPath path;
    roundedPath(path, bounds, radius);
    SolidBrush brush(gdipColor(fill, alpha));
    graphics.FillPath(&brush, &path);
    if (edge != CLR_INVALID && edgeWidth > 0.0f) {
        Pen pen(gdipColor(edge, alpha), edgeWidth);
        pen.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&pen, &path);
    }
}

void fillRoundedGradient(Graphics& graphics, const RectF& bounds, REAL radius,
                         COLORREF start, COLORREF finish, COLORREF edge,
                         LinearGradientMode mode = LinearGradientModeHorizontal) {
    GraphicsPath path;
    roundedPath(path, bounds, radius);
    LinearGradientBrush brush(bounds, gdipColor(start), gdipColor(finish), mode);
    graphics.FillPath(&brush, &path);
    if (edge != CLR_INVALID) {
        Pen pen(gdipColor(edge), 1.0f);
        pen.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&pen, &path);
    }
}

void drawFocusRing(Graphics& graphics, const RectF& bounds, REAL radius, UINT dpi) {
    if (g_palette.highContrast) return;
    RectF ring(bounds.X + 0.75f, bounds.Y + 0.75f,
               std::max(0.0f, bounds.Width - 1.5f), std::max(0.0f, bounds.Height - 1.5f));
    GraphicsPath path;
    roundedPath(path, ring, radius);
    Pen glow(gdipColor(g_palette.accent, 65), static_cast<REAL>(scale(dpi, 4)));
    glow.SetLineJoin(LineJoinRound);
    Pen core(gdipColor(g_palette.accent, 235), static_cast<REAL>(std::max(1, scale(dpi, 1))));
    core.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&glow, &path);
    graphics.DrawPath(&core, &path);
}

void drawHighContrastFocus(HDC dc, RECT bounds, UINT dpi) {
    InflateRect(&bounds, -std::max(1, scale(dpi, 4)), -std::max(1, scale(dpi, 3)));
    DrawFocusRect(dc, &bounds);
}

void drawText(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font,
              COLORREF color, UINT format) {
    if (!text || !*text) return;
    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT textBounds = bounds;
    DrawTextW(dc, text, -1, &textBounds, format);
    if (oldFont) SelectObject(dc, oldFont);
    RestoreDC(dc, saved);
}

void fillEllipse(Graphics& graphics, const RectF& bounds, COLORREF fill,
                 COLORREF edge = CLR_INVALID, BYTE alpha = 255) {
    SolidBrush brush(gdipColor(fill, alpha));
    graphics.FillEllipse(&brush, bounds);
    if (edge != CLR_INVALID) {
        Pen pen(gdipColor(edge, alpha), 1.0f);
        graphics.DrawEllipse(&pen, bounds);
    }
}

void addBrandStroke(Graphics& graphics, REAL x1, REAL y1, REAL x2, REAL y2,
                    COLORREF color, REAL width, BYTE alpha = 255) {
    GraphicsPath path;
    path.AddLine(x1, y1, x2, y2);
    Pen pen(gdipColor(color, alpha), width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

void drawKeyboardGlyph(Graphics& graphics, const RECT& bounds, COLORREF color, UINT dpi) {
    const RectF frame = rectF(bounds);
    GraphicsPath outer;
    roundedPath(outer, frame, scaleReal(dpi, 3.5f));
    Pen outline(gdipColor(color), std::max(1.0f, scaleReal(dpi, 1.35f)));
    outline.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&outline, &outer);

    const REAL keyWidth = scaleReal(dpi, 2.4f);
    const REAL keyHeight = scaleReal(dpi, 1.35f);
    const REAL xGap = scaleReal(dpi, 4.0f);
    const REAL yGap = scaleReal(dpi, 4.1f);
    const REAL innerLeft = frame.X + scaleReal(dpi, 4.2f);
    const REAL innerTop = frame.Y + scaleReal(dpi, 4.3f);
    Pen keyPen(gdipColor(color), std::max(1.0f, scaleReal(dpi, 1.0f)));
    keyPen.SetStartCap(LineCapRound);
    keyPen.SetEndCap(LineCapRound);
    for (int row = 0; row < 2; row++) {
        for (int column = 0; column < 4; column++) {
            const REAL x = innerLeft + static_cast<REAL>(column) * xGap;
            const REAL y = innerTop + static_cast<REAL>(row) * yGap;
            graphics.DrawLine(&keyPen, x, y, x + keyWidth, y);
            if (row == 0) graphics.DrawLine(&keyPen, x, y + keyHeight, x + keyWidth, y + keyHeight);
        }
    }
    const REAL spaceY = frame.Y + frame.Height - scaleReal(dpi, 3.8f);
    graphics.DrawLine(&keyPen, frame.X + scaleReal(dpi, 7.0f), spaceY,
                      frame.X + frame.Width - scaleReal(dpi, 7.0f), spaceY);
}

void drawButtonLabel(Graphics& graphics, HDC dc, const RECT& bounds, const wchar_t* text,
                     HFONT font, COLORREF ink, UINT dpi, bool primary) {
    std::wstring label = text ? text : L"";
    const size_t split = primary ? label.find(L"     ") : std::wstring::npos;
    if (split == std::wstring::npos || split == 0 || split + 5 >= label.size()) {
        graphics.Flush(FlushIntentionSync);
        drawText(dc, bounds, label.c_str(), font, ink,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }

    const std::wstring title = label.substr(0, split);
    const std::wstring shortcut = label.substr(split + 5);
    const int iconLeft = bounds.left + scale(dpi, 18);
    const int centerY = bounds.top + (bounds.bottom - bounds.top) / 2;
    const int iconWidth = scale(dpi, 22);
    const int iconHeight = scale(dpi, 15);
    RECT iconBounds{iconLeft, centerY - iconHeight / 2,
                    iconLeft + iconWidth, centerY + iconHeight / 2};
    drawKeyboardGlyph(graphics, iconBounds, ink, dpi);

    const int shortcutInset = scale(dpi, 18);
    const int shortcutWidth = scale(dpi, 70);
    const int titleLeft = iconLeft + scale(dpi, 34);
    RECT titleBounds{titleLeft, bounds.top,
                     std::max<LONG>(titleLeft, bounds.right - shortcutWidth - scale(dpi, 8)),
                     bounds.bottom};
    RECT shortcutBounds{std::max(titleBounds.right + scale(dpi, 6),
                                 bounds.right - shortcutWidth - scale(dpi, 8)),
                        bounds.top, bounds.right - shortcutInset, bounds.bottom};
    graphics.Flush(FlushIntentionSync);
    drawText(dc, titleBounds, title.c_str(), font, ink,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const COLORREF shortcutInk = ink;
    drawText(dc, shortcutBounds, shortcut.c_str(), font, shortcutInk,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

std::wstring controlText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    const int boundedLength = std::min(length, 32767);
    std::wstring text(static_cast<size_t>(boundedLength) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), boundedLength + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

std::wstring comboText(HWND window) {
    const LRESULT selection = SendMessageW(window, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return controlText(window);
    const LRESULT length = SendMessageW(window, CB_GETLBTEXTLEN,
                                        static_cast<WPARAM>(selection), 0);
    if (length == CB_ERR || length < 0 || length > 32767) return {};
    const int boundedLength = static_cast<int>(length);
    std::wstring text(static_cast<size_t>(boundedLength) + 1, L'\0');
    const LRESULT copied = SendMessageW(window, CB_GETLBTEXT,
                                        static_cast<WPARAM>(selection),
                                        reinterpret_cast<LPARAM>(text.data()));
    if (copied == CB_ERR || copied < 0) return {};
    text.resize(std::min(static_cast<size_t>(copied), static_cast<size_t>(boundedLength)));
    return text;
}

void renderControl(HWND window, HDC dc, const ControlState& state) {
    RECT bounds{};
    GetClientRect(window, &bounds);
    const UINT dpi = GetDpiForWindow(window);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    const bool disabled = !IsWindowEnabled(window);
    const bool focused = GetFocus() == window;
    const ULONGLONG now = GetTickCount64();
    const bool animate = animationsAllowed();
    const float hoverProgress = animate && state.hover.active
        ? tweenValue(state.hover, now) : (state.hot ? 1.0f : 0.0f);

    if (state.kind == ControlKind::Toggle) {
        const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const float checkedProgress = animate && state.checked.active
            ? tweenValue(state.checked, now) : (checked ? 1.0f : 0.0f);
        const std::wstring label = controlText(window);
        drawToggleAnimated(dc, bounds, label.c_str(), font, dpi, checkedProgress,
                           hoverProgress, disabled, focused, state.surface);
    } else if (state.kind == ControlKind::Combo) {
        const bool open = SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != 0;
        const std::wstring label = comboText(window);
        drawComboAnimated(dc, bounds, label.c_str(), font, dpi, disabled,
                          hoverProgress, focused, open, state.surface);
    }
}

void paintControl(HWND window, const ControlState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    if (!target) return;
    RECT bounds{};
    GetClientRect(window, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        EndPaint(window, &paint);
        return;
    }

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    HGDIOBJ oldBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
    const bool bufferReady = bitmap && oldBitmap && oldBitmap != HGDI_ERROR;
    HDC drawing = bufferReady ? buffer : target;
    RECT fillBounds{0, 0, width, height};
    fillSolid(drawing, fillBounds, g_palette.background);
    renderControl(window, drawing, state);
    if (drawing != target) BitBlt(target, 0, 0, width, height, drawing, 0, 0, SRCCOPY);
    if (bufferReady) SelectObject(buffer, oldBitmap);
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
    EndPaint(window, &paint);
}

LRESULT CALLBACK controlSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<ControlState*>(referenceData);
    if (!state) return DefSubclassProc(window, message, wParam, lParam);

    if (message == WM_NCDESTROY) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RemoveWindowSubclass(window, controlSubclassProc, subclassId);
        delete state;
        return result;
    }

    if (message == WM_MOUSEMOVE) {
        if (!state->hot) {
            state->hot = true;
            setTween(state->hover, 1.0f, animationsAllowed());
            if (!state->tracking) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                state->tracking = TrackMouseEvent(&tracking) != FALSE;
            }
            updateAnimationTimer(window, *state);
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE) {
        state->hot = false;
        state->tracking = false;
        setTween(state->hover, 0.0f, animationsAllowed());
        updateAnimationTimer(window, *state);
        InvalidateRect(window, nullptr, FALSE);
    }

    if (message == WM_TIMER && wParam == kAnimationTimerId) {
        const ULONGLONG now = GetTickCount64();
        if (state->hover.active && now - state->hover.started >= kAnimationDurationMs) {
            state->hover.active = false;
            state->hover.from = state->hover.target;
        }
        if (state->checked.active && now - state->checked.started >= kAnimationDurationMs) {
            state->checked.active = false;
            state->checked.from = state->checked.target;
        }
        updateAnimationTimer(window, *state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }

    const bool customPaint = state->kind == ControlKind::Toggle
        || state->kind == ControlKind::Combo;
    if (customPaint && message == WM_PAINT) {
        paintControl(window, *state);
        return 0;
    }
    if (customPaint && message == WM_PRINTCLIENT) {
        renderControl(window, reinterpret_cast<HDC>(wParam), *state);
        return 0;
    }
    if (customPaint && message == WM_ERASEBKGND) return 1;

    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    DWORD_PTR currentData = 0;
    if (!GetWindowSubclass(window, controlSubclassProc, subclassId, &currentData)
        || currentData != referenceData) return result;
    state = reinterpret_cast<ControlState*>(currentData);
    if (state->kind == ControlKind::Toggle
        && (message == BM_SETCHECK || message == WM_LBUTTONUP || message == WM_KEYUP)) {
        targetChecked(window, *state, animationsAllowed());
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE) {
        if (!animationsAllowed()) {
            state->hover.from = state->hover.target;
            state->hover.active = false;
            state->checked.from = state->checked.target;
            state->checked.active = false;
            updateAnimationTimer(window, *state);
        }
    }
    if (customPaint && (message == WM_ENABLE || message == WM_SETTEXT
        || message == WM_SETFONT || message == WM_STYLECHANGED
        || message == BM_SETCHECK || message == CB_SETCURSEL
        || message == CB_SHOWDROPDOWN || message == WM_SETFOCUS
        || message == WM_KILLFOCUS || message == WM_LBUTTONUP
        || message == WM_KEYUP || message == WM_KEYDOWN
        || message == WM_MOUSEWHEEL)) {
        InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}

} // namespace

Palette paletteFor(bool dark, bool contrast) {
    if (contrast) {
        return {
            GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_BTNFACE),
            GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_GRAYTEXT),
            GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHT), dark, true
        };
    }
    if (dark) {
        return {
            RGB(22, 23, 26), RGB(37, 38, 42), RGB(246, 246, 248),
            RGB(166, 167, 173), RGB(66, 67, 73), kBrandBlue,
            RGB(12, 28, 58), RGB(74, 202, 147), RGB(246, 190, 104), true, false
        };
    }
    return {
        RGB(246, 247, 250), RGB(255, 255, 255), RGB(31, 33, 39),
        RGB(104, 108, 118), RGB(224, 227, 232), kBrandBlue,
        RGB(12, 28, 58), RGB(46, 185, 126), RGB(194, 129, 31), false, false
    };
}

bool initialize() {
    return ensureGdiplus();
}

void shutdown() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
        g_gdiplusFailed = false;
    }
}

void setPalette(const Palette& palette) {
    g_palette = palette;
}

const Palette& currentPalette() {
    return g_palette;
}

void rounded(HDC dc, const RECT& bounds, float radius, COLORREF fill, COLORREF edge) {
    if (!dc) return;
    if (ensureGdiplus()) {
        Graphics graphics(dc);
        prepare(graphics);
        fillRounded(graphics, rectF(bounds), static_cast<REAL>(std::max(0.0f, radius)), fill, edge);
        return;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = edge == CLR_INVALID ? reinterpret_cast<HPEN>(GetStockObject(NULL_PEN))
                                   : CreatePen(PS_SOLID, 1, edge);
    if (!brush || !pen) {
        if (brush) DeleteObject(brush);
        if (edge != CLR_INVALID && pen) DeleteObject(pen);
        return;
    }
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int diameter = std::max(0, static_cast<int>(radius * 2.0f));
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, diameter, diameter);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (edge != CLR_INVALID) DeleteObject(pen);
    DeleteObject(brush);
}

static void drawButtonAnimated(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font,
                               UINT dpi, bool primary, bool disabled, float hoverProgress,
                               bool pressed, bool focused, bool quiet, bool surface) {
    if (!dc) return;
    ensureGdiplus();
    hoverProgress = std::max(0.0f, std::min(1.0f, hoverProgress));
    const int hoverPercent = static_cast<int>(hoverProgress * 100.0f + 0.5f);
    const int height = std::max(0L, bounds.bottom - bounds.top);
    const REAL radius = static_cast<REAL>(primary ? height / 2
        : std::min(scale(dpi, 11), height / 2));
    RECT textBounds = bounds;
    if (pressed && !disabled) OffsetRect(&textBounds, 0, scale(dpi, 1));
    const COLORREF backing = surface ? g_palette.surface : g_palette.background;

    if (g_palette.highContrast) {
        fillSolid(dc, bounds, backing);
        COLORREF fill = primary ? g_palette.accent : g_palette.surface;
        COLORREF ink = primary ? g_palette.accentText : g_palette.text;
        if (disabled) {
            fill = backing;
            ink = g_palette.muted;
        }
        rounded(dc, bounds, radius, fill, g_palette.border);
        Graphics graphics(dc);
        prepare(graphics);
        drawButtonLabel(graphics, dc, textBounds, text, font, ink, dpi, primary);
        if (focused) drawHighContrastFocus(dc, bounds, dpi);
        return;
    }

    Graphics graphics(dc);
    prepare(graphics);
    const RectF full = rectF(bounds);
    fillSolid(graphics, full, backing);

    RectF shape(full.X + 0.5f, full.Y + 0.5f,
                std::max(0.0f, full.Width - 1.0f), std::max(0.0f, full.Height - 1.0f));
    if (focused) drawFocusRing(graphics, shape, radius + 1.0f, dpi);

    COLORREF ink = primary ? g_palette.accentText : g_palette.text;
    if (disabled) {
        fillRoundedGradient(graphics, shape, radius,
            mix(g_palette.surface, backing, 16),
            mix(g_palette.surface, backing, 22),
            mix(g_palette.border, backing, 20));
        ink = g_palette.muted;
    } else if (primary) {
        COLORREF start = mix(g_palette.accent, RGB(255, 255, 255), 3 + hoverPercent * 4 / 100);
        COLORREF end = mix(g_palette.accent, kBrandCyan, 19);
        end = mix(end, RGB(255, 255, 255), 2 + hoverPercent * 6 / 100);
        if (pressed) {
            start = mix(start, RGB(0, 0, 0), 10);
            end = mix(end, RGB(0, 0, 0), 10);
        }
        fillRoundedGradient(graphics, shape, radius, start, end,
                            mix(g_palette.accent, RGB(255, 255, 255), 17));
    } else if (quiet) {
        if (hoverPercent > 0 || pressed) {
            const COLORREF tint = pressed ? 13 : 8 * hoverPercent / 100;
            fillRoundedGradient(graphics, shape, radius,
                mix(backing, g_palette.accent, tint),
                mix(backing, g_palette.accent, tint),
                mix(g_palette.border, g_palette.accent, 10 + hoverPercent / 10));
        }
    } else {
        COLORREF fill = g_palette.surface;
        if (hoverPercent > 0) fill = mix(fill, g_palette.accent, pressed ? 12 : 6 * hoverPercent / 100);
        if (pressed) fill = mix(fill, g_palette.text, g_palette.dark ? 9 : 5);
        fillRoundedGradient(graphics, shape, radius,
            mix(fill, RGB(255, 255, 255), g_palette.dark ? 2 : 5), fill,
            mix(g_palette.border, g_palette.text, g_palette.dark ? 5 : 0),
            LinearGradientModeVertical);
    }

    drawButtonLabel(graphics, dc, textBounds, text, font, ink, dpi, primary);
}

void drawButton(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font, UINT dpi,
                bool primary, bool disabled, bool hot, bool pressed, bool focused,
                bool quiet, bool surface) {
    drawButtonAnimated(dc, bounds, text, font, dpi, primary, disabled,
                       hot ? 1.0f : 0.0f, pressed, focused, quiet, surface);
}

void drawButton(HDC dc, const RECT& bounds, const wchar_t* text, HFONT font, UINT dpi,
                bool primary, bool disabled, float hoverProgress, bool pressed, bool focused,
                bool quiet, bool surface) {
    drawButtonAnimated(dc, bounds, text, font, dpi, primary, disabled,
                       hoverProgress, pressed, focused, quiet, surface);
}

static void drawToggleAnimated(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font,
                               UINT dpi, float checkedProgress, float hoverProgress,
                               bool disabled, bool focused, bool surface) {
    if (!dc) return;
    ensureGdiplus();
    checkedProgress = std::max(0.0f, std::min(1.0f, checkedProgress));
    hoverProgress = std::max(0.0f, std::min(1.0f, hoverProgress));
    const bool checked = checkedProgress >= 0.5f;
    const int fullHeight = std::max(0L, bounds.bottom - bounds.top);
    const int fullWidth = std::max(0L, bounds.right - bounds.left);
    const int switchHeight = std::min(scale(dpi, 24), std::max(0, fullHeight - scale(dpi, 4)));
    const int switchWidth = std::min(scale(dpi, 44), std::max(0, fullWidth - scale(dpi, 8)));
    const int rightInset = scale(dpi, 4);
    const int centerY = bounds.top + fullHeight / 2;
    RECT toggle{
        bounds.right - rightInset - switchWidth,
        centerY - switchHeight / 2,
        bounds.right - rightInset,
        centerY + switchHeight / 2
    };

    if (g_palette.highContrast) {
        fillSolid(dc, bounds, surface ? g_palette.surface : g_palette.background);
        RECT labelBounds{bounds.left + scale(dpi, 2), bounds.top,
                         toggle.left - scale(dpi, 12), bounds.bottom};
        drawText(dc, labelBounds, label, font, disabled ? g_palette.muted : g_palette.text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        rounded(dc, toggle, static_cast<float>(switchHeight / 2),
                checked ? g_palette.accent : g_palette.surface, g_palette.text);
        const int knobSize = std::max(0, switchHeight - scale(dpi, 6));
        const int knobX = checked ? toggle.right - scale(dpi, 3) - knobSize
                                  : toggle.left + scale(dpi, 3);
        RECT knob{knobX, centerY - knobSize / 2, knobX + knobSize, centerY + knobSize / 2};
        rounded(dc, knob, static_cast<float>(knobSize / 2), g_palette.accentText, g_palette.text);
        if (focused) drawHighContrastFocus(dc, toggle, dpi);
        return;
    }

    Graphics graphics(dc);
    prepare(graphics);
    const COLORREF backing = surface ? g_palette.surface : g_palette.background;
    fillSolid(graphics, rectF(bounds), backing);
    const RectF switchRect = rectF(toggle);
    const REAL switchRadius = static_cast<REAL>(switchHeight) / 2.0f;
    if (focused) drawFocusRing(graphics,
        RectF(switchRect.X - scaleReal(dpi, 3.0f), switchRect.Y - scaleReal(dpi, 3.0f),
              switchRect.Width + scaleReal(dpi, 6.0f), switchRect.Height + scaleReal(dpi, 6.0f)),
        switchRadius + scaleReal(dpi, 3.0f), dpi);

    COLORREF ink = disabled ? g_palette.muted : g_palette.text;
    RECT labelBounds{bounds.left + scale(dpi, 2), bounds.top,
                     toggle.left - scale(dpi, 12), bounds.bottom};
    graphics.Flush(FlushIntentionSync);
    drawText(dc, labelBounds, label, font, ink,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    graphics.Flush(FlushIntentionSync);

    const COLORREF offTrack = mix(g_palette.surface, g_palette.text, g_palette.dark ? 22 : 13);
    COLORREF track = mix(offTrack, g_palette.accent,
                         static_cast<int>(checkedProgress * 100.0f + 0.5f));
    if (!disabled && hoverProgress > 0.0f) {
        const int hoverTint = static_cast<int>((checked ? 8.0f : 12.0f) * hoverProgress + 0.5f);
        track = mix(track, kBrandCyan, hoverTint);
    }
    if (disabled) track = mix(track, backing, 30);
    if (switchHeight > 0 && switchWidth > 0) {
        fillRounded(graphics, switchRect, switchRadius, track,
                    mix(mix(g_palette.border, g_palette.text, g_palette.dark ? 8 : 0),
                        mix(g_palette.accent, kBrandCyan, 12),
                        static_cast<int>(checkedProgress * 100.0f + 0.5f)));
        const int inset = std::max(2, scale(dpi, 3));
        const int knobSize = std::max(1, switchHeight - inset * 2);
        const int knobLeft = toggle.left + inset;
        const int knobRight = toggle.right - inset - knobSize;
        const REAL knobX = static_cast<REAL>(knobLeft)
            + static_cast<REAL>(knobRight - knobLeft) * checkedProgress;
        RectF knob(knobX, static_cast<REAL>(centerY - knobSize / 2),
                   static_cast<REAL>(knobSize), static_cast<REAL>(knobSize));
        fillEllipse(graphics, RectF(knob.X + scaleReal(dpi, 0.3f), knob.Y + scaleReal(dpi, 1.0f),
                                    knob.Width, knob.Height),
                    RGB(0, 0, 0), CLR_INVALID, g_palette.dark ? 85 : 28);
        knob.Y -= scaleReal(dpi, 0.7f);
        const COLORREF knobFill = disabled
            ? mix(g_palette.surface, backing, 28) : RGB(255, 255, 255);
        fillEllipse(graphics, knob, knobFill, mix(g_palette.border, knobFill, 75));
    }
}

void drawToggle(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
                bool checked, bool disabled, bool hot, bool focused, bool surface) {
    drawToggleAnimated(dc, bounds, label, font, dpi, checked ? 1.0f : 0.0f,
                       hot ? 1.0f : 0.0f, disabled, focused, surface);
}

static void drawComboAnimated(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font,
                              UINT dpi, bool disabled, float hoverProgress,
                              bool focused, bool open, bool surface) {
    if (!dc) return;
    ensureGdiplus();
    hoverProgress = std::max(0.0f, std::min(1.0f, hoverProgress));
    const int height = std::max(0L, bounds.bottom - bounds.top);
    const REAL radius = static_cast<REAL>(std::min(scale(dpi, 9), height / 2));
    const COLORREF backing = surface ? g_palette.surface : g_palette.background;
    if (g_palette.highContrast) {
        fillSolid(dc, bounds, backing);
        rounded(dc, bounds, radius, g_palette.surface,
                focused ? g_palette.accent : g_palette.border);
        RECT textBounds{bounds.left + scale(dpi, 12), bounds.top,
                        bounds.right - scale(dpi, 34), bounds.bottom};
        drawText(dc, textBounds, label, font, disabled ? g_palette.muted : g_palette.text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        const int x = bounds.right - scale(dpi, 18);
        const int y = bounds.top + height / 2;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(dpi, 1)),
                             disabled ? g_palette.muted : g_palette.text);
        if (pen) {
            HGDIOBJ oldPen = SelectObject(dc, pen);
            if (open) {
                MoveToEx(dc, x - scale(dpi, 3), y + scale(dpi, 2), nullptr);
                LineTo(dc, x, y - scale(dpi, 2));
                LineTo(dc, x + scale(dpi, 3), y + scale(dpi, 2));
            } else {
                MoveToEx(dc, x - scale(dpi, 3), y - scale(dpi, 2), nullptr);
                LineTo(dc, x, y + scale(dpi, 2));
                LineTo(dc, x + scale(dpi, 3), y - scale(dpi, 2));
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        if (focused) drawHighContrastFocus(dc, bounds, dpi);
        return;
    }

    Graphics graphics(dc);
    prepare(graphics);
    const RectF full = rectF(bounds);
    fillSolid(graphics, full, backing);
    RectF shape(full.X + 0.5f, full.Y + 0.5f,
                std::max(0.0f, full.Width - 1.0f), std::max(0.0f, full.Height - 1.0f));
    const COLORREF fillTop = mix(g_palette.surface, RGB(255, 255, 255), g_palette.dark ? 2 : 8);
    COLORREF border = g_palette.border;
    if (!disabled && hoverProgress > 0.0f) {
        border = mix(border, g_palette.accent,
                     static_cast<int>(35.0f * hoverProgress + 0.5f));
    }
    if (focused || open) border = g_palette.accent;
    fillRoundedGradient(graphics, shape, radius, fillTop, g_palette.surface, border,
                        LinearGradientModeVertical);
    if (focused && !open) drawFocusRing(graphics, shape, radius + 1.0f, dpi);

    RECT textBounds{bounds.left + scale(dpi, 12), bounds.top,
                    bounds.right - scale(dpi, 36), bounds.bottom};
    graphics.Flush(FlushIntentionSync);
    drawText(dc, textBounds, label, font, disabled ? g_palette.muted : g_palette.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const REAL centerX = static_cast<REAL>(bounds.right - scale(dpi, 17));
    const REAL centerY = static_cast<REAL>(bounds.top + height / 2);
    const REAL halfW = scaleReal(dpi, 3.5f);
    const REAL halfH = scaleReal(dpi, 2.0f);
    GraphicsPath arrow;
    if (open) {
        arrow.AddLine(centerX - halfW, centerY + halfH / 2,
                      centerX, centerY - halfH);
        arrow.AddLine(centerX, centerY - halfH, centerX + halfW, centerY + halfH / 2);
    } else {
        arrow.AddLine(centerX - halfW, centerY - halfH / 2,
                      centerX, centerY + halfH);
        arrow.AddLine(centerX, centerY + halfH, centerX + halfW, centerY - halfH / 2);
    }
    Pen arrowPen(gdipColor(disabled ? g_palette.muted : g_palette.text),
                 std::max(1.0f, scaleReal(dpi, 1.5f)));
    arrowPen.SetStartCap(LineCapRound);
    arrowPen.SetEndCap(LineCapRound);
    arrowPen.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&arrowPen, &arrow);
}

void drawCombo(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
               bool disabled, bool hot, bool focused, bool open, bool surface) {
    drawComboAnimated(dc, bounds, label, font, dpi, disabled,
                      hot ? 1.0f : 0.0f, focused, open, surface);
}

void drawComboItem(HDC dc, const RECT& bounds, const wchar_t* label, HFONT font, UINT dpi,
                   bool selected, bool disabled) {
    if (!dc) return;
    ensureGdiplus();
    const int height = std::max(0L, bounds.bottom - bounds.top);
    if (g_palette.highContrast) {
        const bool highlighted = selected && !disabled;
        fillSolid(dc, bounds, highlighted ? g_palette.accent : g_palette.surface);
        RECT textBounds = bounds;
        InflateRect(&textBounds, -scale(dpi, 10), 0);
        const COLORREF ink = highlighted ? g_palette.accentText
            : disabled ? g_palette.muted : g_palette.text;
        drawText(dc, textBounds, label, font, ink,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }

    Graphics graphics(dc);
    prepare(graphics);
    const RectF full = rectF(bounds);
    fillSolid(graphics, full, g_palette.surface);
    RECT shapeRect = bounds;
    InflateRect(&shapeRect, -scale(dpi, 3), -std::max(1, scale(dpi, 1)));
    if (selected && !disabled) {
        const COLORREF end = mix(g_palette.accent, kBrandCyan, 12);
        fillRoundedGradient(graphics, rectF(shapeRect),
            static_cast<REAL>(std::min(scale(dpi, 7), height / 2)),
            mix(g_palette.accent, RGB(255, 255, 255), 4), end, CLR_INVALID);
    } else if (!disabled) {
        fillRounded(graphics, rectF(shapeRect),
                    static_cast<REAL>(std::min(scale(dpi, 7), height / 2)),
                    mix(g_palette.surface, g_palette.accent, 5));
    }
    RECT textBounds{bounds.left + scale(dpi, 12), bounds.top,
                    bounds.right - scale(dpi, 8), bounds.bottom};
    const COLORREF ink = selected && !disabled ? g_palette.accentText
        : disabled ? g_palette.muted : g_palette.text;
    graphics.Flush(FlushIntentionSync);
    drawText(dc, textBounds, label, font, ink,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void drawBrandTile(HDC dc, const RECT& bounds, float hoverProgress,
                          bool pressed, bool disabled, bool floating, UINT dpi) {
    if (!dc) return;
    ensureGdiplus();
    const int width = std::max(0L, bounds.right - bounds.left);
    const int height = std::max(0L, bounds.bottom - bounds.top);
    const int size = std::min(width, height);
    if (size <= 0) return;
    const REAL inset = pressed ? scaleReal(dpi, 1.0f) : 0.0f;
    const REAL left = static_cast<REAL>(bounds.left + (width - size) / 2) + inset;
    const REAL top = static_cast<REAL>(bounds.top + (height - size) / 2) + inset;
    const REAL tileSize = std::max(1.0f, static_cast<REAL>(size) - inset * 2.0f);
    const RectF tile(left, top, tileSize, tileSize);
    Graphics graphics(dc);
    prepare(graphics);
    const bool contrast = floating && g_palette.highContrast;
    fillSolid(graphics, rectF(bounds), g_palette.background);

    const REAL radius = floating ? scaleReal(dpi, 8.0f) : tileSize * 0.23f;
    GraphicsPath tilePath;
    roundedPath(tilePath, tile, radius);
    const int hover = disabled ? 0 : static_cast<int>(std::clamp(hoverProgress, 0.0f, 1.0f) * 100.0f);
    COLORREF topColor = floating ? mix(RGB(51, 54, 72), RGB(93, 82, 153), hover * 25 / 100)
                                 : RGB(114, 97, 211);
    COLORREF bottomColor = floating ? RGB(25, 28, 40) : RGB(54, 75, 139);
    if (pressed) topColor = mix(topColor, RGB(0, 0, 0), 14);
    if (contrast) topColor = bottomColor = g_palette.background;
    LinearGradientBrush tileBrush(tile, gdipColor(topColor), gdipColor(bottomColor),
                                  LinearGradientModeForwardDiagonal);
    graphics.FillPath(&tileBrush, &tilePath);
    const COLORREF edgeColor = contrast ? g_palette.text
        : floating && hover > 0 ? mix(RGB(166, 166, 190), kBrandCyan, hover / 2)
        : RGB(255, 255, 255);
    Pen tileEdge(gdipColor(edgeColor, contrast ? 255 : static_cast<BYTE>(46 + hover / 2)),
                 std::max(1.0f, tileSize / 48.0f));
    graphics.DrawPath(&tileEdge, &tilePath);

    if (!contrast) {
        GraphicsState clip = graphics.Save();
        graphics.SetClip(&tilePath, CombineModeIntersect);
        RectF sheen(tile.X, tile.Y, tile.Width, tile.Height * 0.60f);
        LinearGradientBrush sheenBrush(sheen, Color(31, 255, 255, 255), Color(0, 255, 255, 255),
                                       LinearGradientModeVertical);
        graphics.FillRectangle(&sheenBrush, sheen);
        graphics.Restore(clip);
    }

    const REAL unit = tileSize * (floating ? 0.72f : 0.84f);
    const REAL originX = tile.X + (tileSize - unit) / 2.0f;
    const REAL originY = tile.Y + (tileSize - unit) / 2.0f;
    const auto px = [originX, unit](REAL value) { return originX + unit * value / 100.0f; };
    const auto py = [originY, unit](REAL value) { return originY + unit * value / 100.0f; };
    const REAL lineWidth = std::max(1.4f, tileSize * 0.046f);
    if (contrast || disabled) {
        COLORREF ink = contrast ? g_palette.text : RGB(136, 143, 163);
        addBrandStroke(graphics, px(4), py(50), px(14), py(50), ink, lineWidth);
        addBrandStroke(graphics, px(25), py(50), px(35), py(50), ink, lineWidth);
        addBrandStroke(graphics, px(50), py(8), px(50), py(34), ink, lineWidth);
        addBrandStroke(graphics, px(50), py(66), px(50), py(92), ink, lineWidth);
        addBrandStroke(graphics, px(37), py(50), px(93), py(50), ink, lineWidth);
        addBrandStroke(graphics, px(76), py(36), px(94), py(50), ink, lineWidth);
        addBrandStroke(graphics, px(94), py(50), px(76), py(64), ink, lineWidth);
        return;
    }

    addBrandStroke(graphics, px(4), py(50), px(14), py(50), kBrandCyan, lineWidth * 1.8f, 85);
    addBrandStroke(graphics, px(25), py(50), px(35), py(50), kBrandCyan, lineWidth * 1.8f, 85);
    addBrandStroke(graphics, px(4), py(50), px(14), py(50), kBrandCyan, lineWidth);
    addBrandStroke(graphics, px(25), py(50), px(35), py(50), kBrandCyan, lineWidth);

    addBrandStroke(graphics, px(50), py(8), px(50), py(34), kBrandViolet, lineWidth * 2.0f, 95);
    addBrandStroke(graphics, px(50), py(66), px(50), py(92), kBrandViolet, lineWidth * 2.0f, 95);
    addBrandStroke(graphics, px(50), py(8), px(50), py(34), RGB(206, 189, 255), lineWidth);
    addBrandStroke(graphics, px(50), py(66), px(50), py(92), RGB(206, 189, 255), lineWidth);

    const REAL arrowWidth = lineWidth * 1.08f;
    addBrandStroke(graphics, px(37), py(50), px(93), py(50), kBrandCyan, arrowWidth * 1.8f, 70);
    addBrandStroke(graphics, px(37), py(50), px(93), py(50), RGB(221, 248, 255), arrowWidth);
    addBrandStroke(graphics, px(76), py(36), px(94), py(50), kBrandCyan, arrowWidth * 1.8f, 70);
    addBrandStroke(graphics, px(94), py(50), px(76), py(64), kBrandCyan, arrowWidth * 1.8f, 70);
    addBrandStroke(graphics, px(76), py(36), px(94), py(50), RGB(221, 248, 255), arrowWidth);
    addBrandStroke(graphics, px(94), py(50), px(76), py(64), RGB(221, 248, 255), arrowWidth);
}

void drawBrand(HDC dc, const RECT& bounds) {
    drawBrandTile(dc, bounds, 0.0f, false, false, false, 96);
}

void drawFloatingButton(HDC dc, const RECT& bounds, UINT dpi, float hoverProgress,
                        bool pressed, bool disabled) {
    drawBrandTile(dc, bounds, hoverProgress, pressed, disabled, true, dpi ? dpi : 96);
}

void drawSurface(HDC dc, const RECT& bounds, UINT dpi, bool card) {
    if (!dc) return;
    ensureGdiplus();
    if (g_palette.highContrast) {
        fillSolid(dc, bounds, card ? g_palette.surface : g_palette.background);
        if (card) {
            rounded(dc, bounds, static_cast<float>(scale(dpi, 12)),
                    g_palette.surface, g_palette.border);
        }
        return;
    }

    Graphics graphics(dc);
    prepare(graphics);
    const RectF full = rectF(bounds);
    if (!card) {
        fillSolid(graphics, full, g_palette.background);
        return;
    }

    fillSolid(graphics, full, g_palette.background);
    RectF cardBounds(full.X + 0.5f, full.Y + 0.5f,
                     std::max(0.0f, full.Width - 1.0f), std::max(0.0f, full.Height - 1.0f));
    GraphicsPath path;
    roundedPath(path, cardBounds, static_cast<REAL>(scale(dpi, 16)));
    SolidBrush brush(gdipColor(g_palette.surface));
    graphics.FillPath(&brush, &path);
    const COLORREF hairline = mix(g_palette.border, g_palette.surface, g_palette.dark ? 18 : 20);
    Pen edge(gdipColor(hairline), 1.0f);
    graphics.DrawPath(&edge, &path);
}

void installControl(HWND window, ControlKind kind, bool surface) {
    if (!window) return;
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, controlSubclassProc, kUiSubclassId, &existing)) {
        auto* state = reinterpret_cast<ControlState*>(existing);
        if (state) {
            state->kind = kind;
            state->surface = surface;
            if (kind == ControlKind::Toggle) {
                const float checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED
                    ? 1.0f : 0.0f;
                state->checked = {checked, checked, GetTickCount64(), false};
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }
    auto* state = new (std::nothrow) ControlState{kind, surface, false, false};
    if (!state) return;
    if (kind == ControlKind::Toggle) {
        const float checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? 1.0f : 0.0f;
        state->checked = {checked, checked, GetTickCount64(), false};
    }
    if (!SetWindowSubclass(window, controlSubclassProc, kUiSubclassId,
                           reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
    }
}

bool isHot(HWND window) {
    if (!window) return false;
    DWORD_PTR referenceData = 0;
    if (!GetWindowSubclass(window, controlSubclassProc, kUiSubclassId, &referenceData)) return false;
    const auto* state = reinterpret_cast<const ControlState*>(referenceData);
    return state && state->hot;
}

float hotProgress(HWND window) {
    if (!window) return 0.0f;
    DWORD_PTR referenceData = 0;
    if (!GetWindowSubclass(window, controlSubclassProc, kUiSubclassId, &referenceData)) return 0.0f;
    const auto* state = reinterpret_cast<const ControlState*>(referenceData);
    if (!state) return 0.0f;
    if (!animationsAllowed()) return state->hot ? 1.0f : 0.0f;
    return std::max(0.0f, std::min(1.0f, tweenValue(state->hover, GetTickCount64())));
}

} // namespace tt::ui
