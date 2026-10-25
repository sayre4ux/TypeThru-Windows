// Offline visual QA: draws the production UI components to memory bitmaps.
// No windows, clipboard reads, hooks, network, or keyboard input are created.
#include "../src/ui.h"
#include "../src/ui_layout.h"
#include "../src/version.h"
#include "../src/shared.h"
#include <gdiplus.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <iterator>

namespace {

const CLSID pngEncoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};

struct Canvas {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* pixels = nullptr;
    int width, height;
    Canvas(int w, int h) : width(w), height(h) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (bitmap) previous = SelectObject(dc, bitmap);
    }
    ~Canvas() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool save(const std::filesystem::path& path, bool icon = false) {
        if (!pixels) return false;
        GdiFlush();
        const double radius = width * 0.23;
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
            auto& pixel = pixels[static_cast<size_t>(y) * width + x];
            pixel |= 0xFF000000;
            if (icon) {
                double cx = x + 0.5 < radius ? radius : width - radius;
                double cy = y + 0.5 < radius ? radius : height - radius;
                bool corner = (x + 0.5 < radius || x + 0.5 > width - radius)
                    && (y + 0.5 < radius || y + 0.5 > height - radius);
                if (corner && (x + 0.5 - cx) * (x + 0.5 - cx)
                    + (y + 0.5 - cy) * (y + 0.5 - cy) > radius * radius) pixel = 0;
            }
        }
        Gdiplus::Bitmap image(width, height, width * 4, PixelFormat32bppARGB,
                              reinterpret_cast<BYTE*>(pixels));
        return image.Save(path.c_str(), &pngEncoder) == Gdiplus::Ok;
    }
};

struct Fonts {
    HFONT body, bold, caption, title, brand, primary;
    static HFONT make(int points, int weight, UINT dpi) {
        return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    explicit Fonts(UINT dpi) : body(make(tt::ui_layout::bodyFontPoints, FW_NORMAL, dpi)),
        bold(make(tt::ui_layout::bodyFontPoints, FW_SEMIBOLD, dpi)),
        caption(make(tt::ui_layout::captionFontPoints, FW_NORMAL, dpi)),
        title(make(tt::ui_layout::titleFontPoints, FW_SEMIBOLD, dpi)),
        brand(make(tt::ui_layout::brandTitleFontPoints, FW_SEMIBOLD, dpi)),
        primary(make(tt::ui_layout::primaryFontPoints, FW_SEMIBOLD, dpi)) {}
    ~Fonts() {
        DeleteObject(body); DeleteObject(bold); DeleteObject(caption); DeleteObject(title);
        DeleteObject(brand); DeleteObject(primary);
    }
};

RECT area(int x, int y, int w, int h, int scale = 2) {
    return RECT{x * scale, y * scale, (x + w) * scale, (y + h) * scale};
}

void text(HDC dc, const wchar_t* value, RECT bounds, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &bounds, flags);
    SelectObject(dc, old);
}

bool renderComponents(const std::filesystem::path& path, bool dark) {
    tt::ui::setPalette(tt::ui::paletteFor(dark, false));
    const auto& p = tt::ui::currentPalette();
    Canvas canvas(620 * 2, 420 * 2);
    if (!canvas.bitmap) return false;
    Fonts f(192);
    auto dc = canvas.dc;
    tt::ui::drawSurface(dc, area(0, 0, 620, 420), 192);
    tt::ui::drawBrand(dc, area(26, 26, 42, 42));
    text(dc, L"TypeThru", area(82, 26, 400, 28), f.title, p.text);
    text(dc, L"Production component preview", area(83, 56, 400, 20), f.caption, p.muted);
    tt::ui::drawButton(dc, area(26, 96, 568, 52), L"Type Clipboard     Ctrl+;", f.bold, 192,
                       true, false, false, false, false);
    tt::ui::drawSurface(dc, area(26, 168, 568, 164), 192, true);
    tt::ui::drawToggle(dc, area(44, 180, 532, 40), L"Type line breaks with Shift+Enter", f.body,
                       192, true, false, false, false);
    tt::ui::drawToggle(dc, area(44, 226, 532, 40), L"Open TypeThru when I sign in", f.body,
                       192, false, false, false, false);
    tt::ui::drawCombo(dc, area(44, 278, 532, 38), L"Scan code keys (recommended)", f.body,
                      192, false, false, false, false, true);
    tt::ui::drawButton(dc, area(26, 352, 172, 38), L"Settings", f.body, 192,
                       false, false, false, false, false);
    tt::ui::drawButton(dc, area(212, 352, 172, 38), L"Hovered", f.body, 192,
                       false, false, true, false, false);
    tt::ui::drawButton(dc, area(398, 352, 196, 38), L"Disabled", f.body, 192,
                       false, true, false, false, false);
    return canvas.save(path);
}

bool renderFloating(const std::filesystem::path& path, bool dark) {
    tt::ui::setPalette(tt::ui::paletteFor(dark, false));
    const auto& p = tt::ui::currentPalette();
    Canvas canvas(368 * 2, 100 * 2);
    if (!canvas.bitmap) return false;
    Fonts f(192);
    tt::ui::drawSurface(canvas.dc, area(0, 0, 368, 100), 192);
    const wchar_t* labels[] = {L"Ready", L"Hover", L"Pressed", L"Busy"};
    for (int i = 0; i < 4; i++) {
        int x = 26 + i * 90;
        tt::ui::drawFloatingButton(canvas.dc, area(x, 15, tt::ui_layout::floatingSize,
            tt::ui_layout::floatingSize), 192, i == 1 || i == 2 ? 1.0f : 0.0f, i == 2, i == 3);
        text(canvas.dc, labels[i], area(x - 12, 65, 64, 20), f.caption, p.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return canvas.save(path);
}

const wchar_t* labelFor(int id) {
    using namespace tt::ui_layout;
    auto wide = [](const char* value) {
        int size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), size);
        if (!result.empty()) result.pop_back();
        return result;
    };
    static const std::wstring speedTitle = wide(tt::speeds[0].title);
    static const std::wstring speedDetail = wide(tt::speeds[0].detail);
    if (id == IDC_SPEED) return speedTitle.c_str();
    if (id == IDC_SPEED_DETAIL) return speedDetail.c_str();
    if (id == IDC_TYPE) return L"Type Clipboard     Ctrl+;";
    if (id == IDS_METHOD) return L"Scan code keys (recommended)";
    const wchar_t* shared = tt::ui_layout::label(id);
    return shared ? shared : L"";
}

bool renderWindow(const std::filesystem::path& path, bool dark, bool settings) {
    using namespace tt::ui_layout;
    tt::ui::setPalette(tt::ui::paletteFor(dark, false));
    const auto& p = tt::ui::currentPalette();
    Canvas canvas((settings ? settingsWidth : panelWidth) * 2,
                  (settings ? settingsHeight : panelHeight(false)) * 2);
    if (!canvas.bitmap) return false;
    Fonts f(192);
    auto dc = canvas.dc;
    tt::ui::drawSurface(dc, RECT{0, 0, canvas.width, canvas.height}, 192);
    if (settings) {
        for (const auto& card : settingsSurfaces)
            tt::ui::drawSurface(dc, area(card.x, card.y, card.w, card.h), 192, true);
    }
    auto paint = [&](const Placement& field) {
        int id = field.id;
        if (id == IDC_UPDATE_STATUS || id == IDC_INSTALL || id == IDS_INSTALL) return;
        RECT bounds = area(field.x, field.y, field.w, field.h);
        const wchar_t* label = labelFor(id);
        if (id == IDC_BRAND || id == IDS_SETTINGS_BRAND) { tt::ui::drawBrand(dc, bounds); return; }
        if (id == IDC_SPEED || id == IDS_METHOD) {
            bounds.bottom = bounds.top + comboSelectionHeight * 2;
            tt::ui::drawCombo(dc, bounds, label, f.body, 192, false, false, false, false, settings);
            return;
        }
        if (id == IDC_SHIFTENTER || id == IDS_RUNLOGIN || id == IDS_FLOATBTN
            || id == IDS_REPLACE || id == IDS_SHIFTENTER || id == IDS_CHECKUPDATES) {
            tt::ui::drawToggle(dc, bounds, label, f.body, 192, id != IDS_RUNLOGIN,
                               false, false, false, settings);
            return;
        }
        bool button = id == IDC_TYPE || id == IDC_TEST_SHORT || id == IDC_TEST_CAPITALS
            || id == IDC_TEST_SYMBOLS || id == IDC_TEST_SMART || id == IDC_TEST_LONG
            || id == IDC_SETTINGS_BTN || id == IDC_QUIT || id == IDS_CHANGE || id == IDS_RESET
            || id == IDS_CHECKNOW || id == IDS_DIAGNOSTICS || id == IDS_UNINSTALL || id == IDS_DONE;
        if (button) {
            bool primary = id == IDC_TYPE;
            bool quiet = id == IDC_SETTINGS_BTN || id == IDC_QUIT || id == IDS_DIAGNOSTICS || id == IDS_UNINSTALL;
            tt::ui::drawButton(dc, bounds, label, id == IDC_TYPE ? f.primary : primary ? f.bold : f.body, 192,
                primary, id == IDS_RESET, false, false, false, quiet, buttonUsesSurface(id));
            return;
        }
        if (id == IDC_STATUS) {
            tt::ui::rounded(dc, bounds, 28.0f, p.surface, p.border);
            RECT dot = area(field.x + 12, field.y + 11, 6, 6);
            tt::ui::rounded(dc, dot, 6.0f, p.success);
            bounds.left += 27 * 2;
            text(dc, label, bounds, f.caption, p.text);
            return;
        }
        if (id == IDS_SHORTCUT_VALUE) {
            tt::ui::rounded(dc, bounds, 14.0f, p.background, p.border);
            text(dc, label, bounds, f.bold, p.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        bool heading = id == IDS_GENERAL_HEADING || id == IDS_TYPING_HEADING
            || id == IDS_UPDATES_HEADING || id == IDC_TEST_LABEL;
        bool title = id == IDC_TITLE || id == IDS_SETTINGS_TITLE;
        bool muted = id == IDC_TYPE_HELP || id == IDC_SPEED_DETAIL || id == IDC_TEST_HELP
            || id == IDS_SETTINGS_SUBTITLE || id == IDS_METHOD_HINT || id == IDS_SHIFTENTER_HINT
            || id == IDS_UPDATE_STATUS || id == IDS_NOTE;
        text(dc, label, bounds, id == IDC_TITLE ? f.brand : title ? f.title
            : heading ? f.bold : muted ? f.caption : f.body,
             muted ? p.muted : p.text);
    };
    if (settings) { for (const auto& field : tt::ui_layout::settings()) paint(field); }
    else { for (const auto& field : panel(false)) paint(field); }
    return canvas.save(path);
}

bool makeIcons(const std::filesystem::path& directory, const std::filesystem::path& destination) {
    std::vector<std::vector<char>> images;
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    tt::ui::setPalette(tt::ui::paletteFor(true, false));
    for (int size : sizes) {
        Canvas canvas(size, size);
        if (!canvas.bitmap) return false;
        std::fill(canvas.pixels, canvas.pixels + static_cast<size_t>(size) * size, 0);
        tt::ui::drawBrand(canvas.dc, RECT{0, 0, size, size});
        auto path = directory / (L"brand-" + std::to_wstring(size) + L".png");
        if (!canvas.save(path, true)) return false;
        std::ifstream input(path, std::ios::binary);
        images.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (images.back().empty()) return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    auto u8 = [&output](unsigned value) { output.put(static_cast<char>(value)); };
    auto u16 = [&u8](unsigned value) { u8(value & 0xFF); u8((value >> 8) & 0xFF); };
    auto u32 = [&u16](unsigned value) { u16(value & 0xFFFF); u16(value >> 16); };
    u16(0); u16(1); u16(static_cast<unsigned>(images.size()));
    unsigned offset = 6 + static_cast<unsigned>(images.size()) * 16;
    for (size_t i = 0; i < images.size(); i++) {
        u8(sizes[i] == 256 ? 0 : sizes[i]); u8(sizes[i] == 256 ? 0 : sizes[i]); u8(0); u8(0);
        u16(1); u16(32); u32(static_cast<unsigned>(images[i].size())); u32(offset);
        offset += static_cast<unsigned>(images[i].size());
    }
    for (const auto& bytes : images) output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

}

int wmain(int argc, wchar_t** argv) {
    if (!tt::ui::initialize()) return 1;
    int status = 0;
    try {
        std::filesystem::path directory = argc > 1 ? argv[1] : L".build/previews";
        std::filesystem::create_directories(directory);
        if (!renderComponents(directory / L"components-dark.png", true)
            || !renderComponents(directory / L"components-light.png", false)
            || !renderWindow(directory / L"panel-dark.png", true, false)
            || !renderWindow(directory / L"panel-light.png", false, false)
            || !renderWindow(directory / L"settings-dark.png", true, true)
            || !renderWindow(directory / L"settings-light.png", false, true)
            || !renderFloating(directory / L"floating-dark.png", true)
            || !renderFloating(directory / L"floating-light.png", false)) status = 2;
        if (argc > 2 && !makeIcons(directory, argv[2])) status = 3;
        std::wcout << L"Offline UI render " << (status ? L"failed" : L"complete") << L"\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        status = 4;
    }
    tt::ui::shutdown();
    return status;
}
