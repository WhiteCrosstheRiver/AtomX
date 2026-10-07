#include "renderer.hpp"
#include "analysis.hpp"
#include "authoring.hpp"
#include "fragment_library.hpp"
#include "fragment_fusion.hpp"
#include "hydrogen_adjust.hpp"
#include "chemical_settings.hpp"
#include "atom_properties.hpp"
#include "atom_constraints.hpp"
#include "structure_clipboard.hpp"
#include "project_archive.hpp"
#include "bond_insertion.hpp"
#include "creation_bonds.hpp"
#include "motion_groups.hpp"
#include "layer_builder.hpp"
#include "creation_display.hpp"
#include "symmetry.hpp"
#include "structure_io.hpp"
#include "desktop.hpp"
#include "imgui.h"
#include "imgui_guard.hpp"
#include "imgui_internal.h" // GetCurrentWindow / ImGuiLayoutType for the title-strip menu row
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <commdlg.h>
#include <shellapi.h>
#include <future>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <memory>
#include <cctype>
#include <numeric>
#include <deque>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
using namespace atomx;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static Renderer *renderer = nullptr;
static bool resized = false;
static std::deque<std::filesystem::path> droppedFiles;
// Viewport tool cursors. ImGui never draws the OS cursor itself; its win32
// backend maps ImGui::GetMouseCursor() onto the stock shapes during
// WM_SETCURSOR, so Pan (hand) and Orbit (4-way) ride that existing path via
// ImGui::SetMouseCursor. Windows has no stock magnifier cursor, so one is
// generated once at startup as a DPI-scaled ARGB icon (thin black ring, 1px
// white edge, diagonal handle, the same magnifier as the zoom button) and
// applied through a frame-local override that wndProc checks BEFORE the ImGui
// backend handler (which would otherwise overwrite it with the last ImGui
// cursor on every WM_SETCURSOR).
static HCURSOR g_cursorOverride = nullptr; // Valid for the current frame only.
static HCURSOR g_magnifierCursor = nullptr;
enum class ViewportCursor { Arrow, Hand, ResizeAll, Magnifier };
// Headless-testable mapping from the timeline viewport tool to the cursor.
static ViewportCursor cursorForViewportTool(int tool) {
    switch (tool) {
    case 0: return ViewportCursor::Magnifier; // zoom
    case 1: return ViewportCursor::Hand;      // pan
    case 2: return ViewportCursor::ResizeAll; // orbit
    default: return ViewportCursor::Arrow;    // FOV / select / no tool
    }
}
static HCURSOR createMagnifierCursor(float scale = 1.f) {
    // 32-bit ARGB color cursor drawn as a thin magnifier, matching the zoom
    // button. A 1px white edge sits outside the black ring and handle so the
    // shape stays readable on the near-black viewport without turning into a
    // thick halo. The lens is biased up-left so the diagonal handle fits; the
    // hotspot is the lens center. The AND mask stays all zero; the color
    // bitmap's alpha channel alone drives transparency.
    constexpr float base = 40.f; // logical pixels at scale 1
    const int cx = std::max(16, int(base * scale + .5f)), cy = cx;
    const float c = base * .38f * scale;         // lens center / hotspot
    const float lensR = base * .22f * scale;     // inner glass radius
    const float ringW = std::max(1.35f, 1.55f * scale);
    const float haloW = std::max(1.f, 1.15f * scale);
    const float handleStart = lensR + ringW * .2f; // tuck the handle into the ring
    const float handleEnd = base * .72f * scale;
    const float handleW = ringW;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = cx;
    header.bV5Height = cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr); // zeroed: alpha rules
    if (!color || !mask || !bits) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return nullptr; // caller falls back to IDC_SIZEALL
    }
    auto circleDistance = [&](double x, double y) {
        return std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
    };
    auto handleDistance = [&](double x, double y) {
        // Distance to the diagonal handle segment; the projection parameter
        // clamps to the segment ends, giving a rounded handle tip.
        const double ax = c + handleStart * 0.7071, ay = ax;
        const double bx = c + handleEnd * 0.7071, by = bx;
        const double dx = bx - ax, dy = by - ay;
        const double t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
        return std::sqrt((x - (ax + t * dx)) * (x - (ax + t * dx)) +
                         (y - (ay + t * dy)) * (y - (ay + t * dy)));
    };
    auto smoothstep = [](double edge0, double edge1, double x) {
        const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
        return t * t * (3 - 2 * t);
    };
    auto *pixels = static_cast<DWORD *>(bits);
    for (int y = 0; y < cy; ++y) {
        // DIB sections are bottom-up: row cy-1-y of the buffer is screen row y.
        DWORD *row = pixels + size_t(cy - 1 - y) * cx;
        for (int x = 0; x < cx; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double d = circleDistance(px, py);
            double alpha = 0, red = 255, green = 255, blue = 255;
            const double ringOuter = lensR + ringW, haloOuter = ringOuter + haloW;
            const double aa = 0.8; // keep a solid core inside the thin stroke
            if (d < lensR) {
                // interior: faint glass, the scene stays visible
                alpha = 40;
            } else if (d < ringOuter) {
                const double ring = 1.0 - smoothstep(ringOuter - aa, ringOuter, d) +
                                    smoothstep(lensR, lensR + aa, d);
                alpha = 255.0 * std::clamp(ring, 0.0, 1.0);
                red = green = blue = 0;
            } else if (d < haloOuter) {
                const double halo = 1.0 - smoothstep(haloOuter - aa, haloOuter, d);
                alpha = 255.0 * std::clamp(halo, 0.0, 1.0);
            }
            const double hd = handleDistance(px, py);
            if (hd < handleW) { // opaque black handle wins over everything inside
                alpha = 255;
                red = green = blue = 0;
            } else if (hd < handleW + haloW) { // white edging so the handle reads on black
                const double edge = 1.0 - smoothstep(handleW + haloW - aa, handleW + haloW, hd);
                const double a = 255.0 * std::clamp(edge, 0.0, 1.0);
                if (a > alpha) {
                    alpha = a;
                    red = green = blue = 255;
                }
            }
            const DWORD a8 = DWORD(alpha + 0.5) & 0xFF;
            row[x] = (a8 << 24) | (DWORD(red + 0.5) << 16) | (DWORD(green + 0.5) << 8) | DWORD(blue + 0.5);
        }
    }
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = DWORD(c + .5f);
    info.yHotspot = DWORD(c + .5f);
    info.hbmMask = mask;
    info.hbmColor = color;
    const HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return cursor;
}
static std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
static LRESULT WINAPI wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Magnifier override for the zoom tool: applied before the ImGui backend
    // handler, which would otherwise answer WM_SETCURSOR with its own cursor.
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g_cursorOverride) {
        SetCursor(g_cursorOverride);
        return TRUE;
    }
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp))
        return 1;
    if (msg == desktop::taskbarCreated && desktop::inTray) {
        if (!Shell_NotifyIconW(NIM_ADD, &desktop::tray)) desktop::restore(h);
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return desktop::hitTest(h, lp);
    case WM_CLOSE:
        desktop::hide(h);
        return 0;
    case desktop::trayMessage:
        if (lp == WM_LBUTTONDBLCLK) desktop::restore(h);
        if (lp == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Restore AtomX");
            AppendMenuW(menu, MF_STRING, 2, L"Exit AtomX");
            POINT p; GetCursorPos(&p); SetForegroundWindow(h);
            int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (action == 1) desktop::restore(h);
            if (action == 2) PostQuitMessage(0);
            PostMessageW(h, WM_NULL, 0, 0);
        }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {1200, 820};
        {
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
            auto *m = (MINMAXINFO *)lp;
            m->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                monitor.rcWork.top - monitor.rcMonitor.top};
            m->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                            monitor.rcWork.bottom - monitor.rcWork.top};
        }
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            resized = true;
        return 0;
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        wchar_t p[32768];
        const UINT count=DragQueryFileW(hDrop,0xFFFFFFFF,nullptr,0);
        for(UINT i=0;i<count;++i){if(DragQueryFileW(hDrop,i,p,32768))droppedFiles.emplace_back(p);}
        DragFinish(hDrop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static std::filesystem::path dialog(HWND window, bool save, const wchar_t *filter,
                                    const wchar_t *ext) {
    wchar_t path[32768]{};
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = window;
    of.lpstrFilter = filter;
    of.lpstrFile = path;
    of.nMaxFile = 32768;
    of.lpstrDefExt = ext;
    of.Flags =
        OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const bool accepted=save ? GetSaveFileNameW(&of)!=FALSE : GetOpenFileNameW(&of)!=FALSE;
    // The native modal receives the shortcut's key-up and the closing click.
    // ImGui must not retain them as held keys/buttons after the dialog returns.
    if (ImGui::GetCurrentContext()) {
        auto &io=ImGui::GetIO();
        io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse();
    }
    return accepted ? std::filesystem::path(path) : std::filesystem::path();
}
static std::string number(uint64_t n) {
    auto s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return s;
}
static float uiScale = 1;
static float U(float value) { return value * uiScale; }
// P11: pure 1/2/5-decade label-step selection for the timeline ruler. Picks
// the smallest step in the {1,2,5}*10^n series that keeps the major-label
// count at 12 or fewer and, when the track is wide enough, a minimum pixel
// pitch of 40px so labels never collide at any frame count or window width.
// 30 frames on a normal track -> 5 (labels 0,5,10,15,20,25); a million
// frames -> at least 100000.
static int64_t timelineLabelStep(int64_t lastFrame, float trackWidthPx) {
    if (lastFrame <= 0) return 1;
    double desired = std::max(double(lastFrame) / 11.0,
                              trackWidthPx > 0 ? double(lastFrame) * 40.0 / double(trackWidthPx) : 0.0);
    desired = std::max(desired, 1.0);
    const double decade = std::pow(10.0, std::floor(std::log10(desired)));
    for (int mantissa : {1, 2, 5, 10}) {
        const int64_t step = int64_t(double(mantissa) * decade);
        if (double(step) >= desired) return step;
    }
    return int64_t(decade * 10.0);
}
static ImFont *headingFont = nullptr;
static ImFont *iconFont = nullptr;
static const char *iconFontName = "text fallback"; // Reported by the smoke run.
static ImVec4 accent{.10f,.34f,.62f,1};
// Segoe MDL2 Assets codepoints used by the compact icon buttons. The range
// table feeds the merged icon font; every button also carries a text fallback
// for the case where the system font is unavailable or a glyph is missing.
static const ImWchar iconGlyphRanges[] = {
    0xE70D,0xE70D, 0xE70E,0xE70F, 0xE713,0xE713,
    0xE71D,0xE71D, 0xE722,0xE722, 0xE72A,0xE72A, 0xE734,0xE734,
    0xE768,0xE768, 0xE769,0xE769,
    0xE76B,0xE76B, 0xE76C,0xE76C, 0xE792,0xE792, 0xE7A6,0xE7A7,
    0xE7AD,0xE7AD, 0xE7B3,0xE7B3, 0xE7B8,0xE7B8, 0xE7C9,0xE7C9,
    0xE80F,0xE80F, 0xE81E,0xE81E,
    0xE823,0xE823, 0xE892,0xE892, 0xE893,0xE893,
    0xE8A0,0xE8A0, 0xE8A4,0xE8A4, 0xE8A7,0xE8A7, 0xE8A9,0xE8AB,
    0xE8B1,0xE8B1, 0xE8B5,0xE8B5, 0xE8B7,0xE8B7,
    0xE8C8,0xE8C8, 0xE8D7,0xE8D7, 0xE8E5,0xE8E5, 0xE8F1,0xE8F1,
    0xE8FA,0xE8FA, 0xE91B,0xE91B,
    0xE72C,0xE72C, 0xE74D,0xE74D, 0xE7F4,0xE7F4, 0xE192,0xE192,
    0xE9D9,0xE9D9,
    0};
// Encodes a Unicode codepoint as UTF-8 (NUL terminated), returns the length.
static int glyphUtf8(unsigned codepoint, char out[8]) {
    if (codepoint < 0x80) {
        out[0] = char(codepoint); out[1] = 0;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = char(0xC0 | (codepoint >> 6));
        out[1] = char(0x80 | (codepoint & 0x3F));
        out[2] = 0;
        return 2;
    }
    out[0] = char(0xE0 | (codepoint >> 12));
    out[1] = char(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = char(0x80 | (codepoint & 0x3F));
    out[3] = 0;
    return 3;
}
static bool glyphAvailable(unsigned codepoint) {
    return iconFont && iconFont->FindGlyphNoFallback(ImWchar(codepoint)) != nullptr;
}
static void theme(int choice = 0) {
    ImGui::GetStyle() = ImGuiStyle{};
    if (choice == 1) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {10,8}; s.FramePadding = {6,3}; s.ItemSpacing = {6,4};
    s.WindowRounding = 0; s.ChildRounding = 3; s.FrameRounding = 3; s.PopupRounding = 5;
    s.ScrollbarSize = 13; s.WindowBorderSize = 0; s.ChildBorderSize = 1;
    s.FrameBorderSize = 1; s.PopupBorderSize = 1; s.GrabRounding = 2;
    s.DisabledAlpha = .72f;
    auto *c = s.Colors;
    if (choice == 0) {
        // P12 "View mode dark": the AtomX redesign reference palette.
        // Window #16181b, panels #1a1d20, borders #22262a, hairlines
        // #2c3137, control wells #111316, popups #1d2024 / border #33383e,
        // text #e6e8eb / #aeb4bb / #9aa1a9 / #6b727a, accent amber #f0a431.
        accent = {.941f,.643f,.192f,1}; // #f0a431
        c[ImGuiCol_Text] = {.902f,.910f,.922f,1};        // #e6e8eb
        c[ImGuiCol_TextDisabled] = {.420f,.447f,.478f,1}; // #6b727a
        c[ImGuiCol_TextSelectedBg] = {.941f,.643f,.192f,.30f};
        c[ImGuiCol_WindowBg] = {.102f,.114f,.125f,1};    // #1a1d20
        c[ImGuiCol_ChildBg] = {.102f,.114f,.125f,1};     // #1a1d20
        c[ImGuiCol_PopupBg] = {.114f,.125f,.141f,1};     // #1d2024
        c[ImGuiCol_Border] = {.133f,.149f,.165f,1};      // #22262a
        c[ImGuiCol_Separator] = {.133f,.149f,.165f,1};   // #22262a
        c[ImGuiCol_FrameBg] = {.067f,.075f,.086f,1};     // #111316
        c[ImGuiCol_FrameBgHovered] = {.086f,.094f,.106f,1};
        c[ImGuiCol_FrameBgActive] = {.114f,.125f,.141f,1};
        c[ImGuiCol_Button] = {.125f,.137f,.157f,1};      // #202328
        c[ImGuiCol_ButtonHovered] = {.149f,.165f,.184f,1}; // #262a2f
        c[ImGuiCol_ButtonActive] = {.173f,.192f,.216f,1}; // #2c3137
        c[ImGuiCol_Header] = {.137f,.153f,.173f,1};      // #23272c
        c[ImGuiCol_HeaderHovered] = {.153f,.169f,.188f,1}; // #272b30
        c[ImGuiCol_HeaderActive] = {.173f,.192f,.216f,1};
        c[ImGuiCol_Tab] = {.125f,.137f,.157f,1};
        c[ImGuiCol_TabHovered] = {.149f,.165f,.184f,1};
        c[ImGuiCol_TabSelected] = {.149f,.163f,.188f,1}; // #262a30
        c[ImGuiCol_TitleBgActive] = {.102f,.114f,.125f,1};
        c[ImGuiCol_TableHeaderBg] = {.125f,.137f,.157f,1};
        c[ImGuiCol_TableRowBgAlt] = {1,1,1,.025f};
        c[ImGuiCol_ScrollbarBg] = {.102f,.114f,.125f,1};
        c[ImGuiCol_ScrollbarGrab] = {.180f,.200f,.220f,1};
        c[ImGuiCol_ScrollbarGrabHovered] = {.220f,.239f,.259f,1};
        c[ImGuiCol_ScrollbarGrabActive] = {.941f,.643f,.192f,1};
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_PlotLines] = accent;
        c[ImGuiCol_NavCursor] = accent;
        s.ScaleAllSizes(uiScale);
        return;
    }
    if (choice == 1) {
        accent = {.08f,.32f,.61f,1};
        c[ImGuiCol_Text] = {.08f,.10f,.13f,1};
        c[ImGuiCol_TextDisabled] = {.35f,.38f,.42f,1};
        c[ImGuiCol_WindowBg] = {.935f,.945f,.955f,1};
        c[ImGuiCol_ChildBg] = {1,1,1,1};
        c[ImGuiCol_PopupBg] = {.95f,.96f,.97f,1};
        c[ImGuiCol_Border] = {.64f,.68f,.73f,1};
        c[ImGuiCol_FrameBg] = {1,1,1,1};
        c[ImGuiCol_FrameBgHovered] = {.91f,.95f,1,1};
        c[ImGuiCol_FrameBgActive] = {.84f,.91f,.99f,1};
        c[ImGuiCol_Button] = {.97f,.98f,.99f,1};
        c[ImGuiCol_ButtonHovered] = {.85f,.92f,1,1};
        c[ImGuiCol_ButtonActive] = {.73f,.85f,.98f,1};
        c[ImGuiCol_Header] = {.84f,.90f,.97f,1};
        c[ImGuiCol_HeaderHovered] = {.80f,.88f,.98f,1};
        c[ImGuiCol_HeaderActive] = {.73f,.84f,.96f,1};
        c[ImGuiCol_Tab] = {.87f,.89f,.92f,1};
        c[ImGuiCol_TabHovered] = {.79f,.87f,.97f,1};
        c[ImGuiCol_TabSelected] = {1,1,1,1};
        c[ImGuiCol_TitleBgActive] = {.86f,.90f,.95f,1};
        c[ImGuiCol_TableHeaderBg] = {.86f,.89f,.93f,1};
        c[ImGuiCol_TableRowBgAlt] = {.10f,.25f,.45f,.045f};
    } else {
        accent = {.48f,.74f,1,1};
        c[ImGuiCol_Text] = {.95f,.96f,.98f,1};
        c[ImGuiCol_TextDisabled] = {.70f,.74f,.80f,1};
        c[ImGuiCol_WindowBg] = choice == 2 ? ImVec4{.07f,.08f,.10f,1} : ImVec4{.14f,.16f,.19f,1};
        c[ImGuiCol_ChildBg] = {.10f,.12f,.15f,1};
        c[ImGuiCol_PopupBg] = {.16f,.18f,.22f,1};
        c[ImGuiCol_Border] = {.39f,.44f,.51f,1};
        c[ImGuiCol_FrameBg] = {.09f,.11f,.14f,1};
        c[ImGuiCol_Button] = {.23f,.27f,.33f,1};
        c[ImGuiCol_Header] = {.25f,.34f,.45f,1};
        c[ImGuiCol_TabSelected] = {.28f,.35f,.44f,1};
    }
    c[ImGuiCol_CheckMark] = accent; c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_PlotHistogram] = accent; c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    s.ScaleAllSizes(uiScale);
}
static void heading(const char *title) {
    ImGui::Spacing();
    auto p = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFontSize()+U(12);
    auto *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p,{p.x+width,p.y+height},ImGui::GetColorU32(ImGuiCol_Tab),U(3));
    if (headingFont) ImGui::PushFont(headingFont);
    d->AddText({p.x+U(8),p.y+U(6)},ImGui::GetColorU32(ImGuiCol_Text),title);
    if (headingFont) ImGui::PopFont();
    ImGui::Dummy({width,height});
}
struct Loaded {
    Dataset data;
    std::vector<Frame> frames;
    std::filesystem::path path;
    int frame;
    std::optional<document::View> documentView;
};
struct HistogramPlotView {
    int firstBin = 0;
    int lastBin = -1;
    float yMaximum = 0; // zero means automatic
};
struct PipelineJobResult {
    PipelineResult result;
    size_t checkpointNode = SIZE_MAX;
    std::optional<PipelineResult> checkpoint;
};
// Intersection polygon of the slice plane n.r = d with the simulation cell,
// returned as a fan-triangulated overlay mesh. The polygon is shrunk slightly
// toward its centroid so it does not coincide with the cell edges. An empty
// result means the plane misses the cell (or the parameters are unusable) and
// the viewport overlay is skipped.
static std::vector<DirectX::XMFLOAT3> slicePlaneTriangles(const float normalIn[3],
                                                          double distance,
                                                          const Dataset &data) {
    std::vector<DirectX::XMFLOAT3> triangles;
    double n[3]{normalIn[0], normalIn[1], normalIn[2]};
    const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(length > 0) || !std::isfinite(distance))
        return triangles;
    for (double &component : n) component /= length;
    const double origin[3]{data.origin.x, data.origin.y, data.origin.z};
    double corners[8][3]{}, heights[8]{};
    for (int corner = 0; corner < 8; ++corner) {
        const double weights[3]{double(corner & 1), double(corner & 2) * .5,
                                double(corner & 4) * .25};
        for (int c = 0; c < 3; ++c)
            corners[corner][c] = origin[c] + weights[0] * data.cell[c] +
                                 weights[1] * data.cell[3 + c] + weights[2] * data.cell[6 + c];
        heights[corner] = n[0] * corners[corner][0] + n[1] * corners[corner][1] +
                          n[2] * corners[corner][2] - distance;
    }
    double scale = 1;
    for (int corner = 0; corner < 8; ++corner)
        for (int c = 0; c < 3; ++c)
            scale = std::max(scale, std::abs(corners[corner][c]));
    std::vector<std::array<double, 3>> points;
    const double epsilon = scale * 1e-9;
    for (int bit = 1; bit < 8; bit <<= 1)
        for (int corner = 0; corner < 8; ++corner) {
            if (!(corner & bit)) continue;
            const int other = corner ^ bit;
            if ((heights[corner] > 0) == (heights[other] > 0)) continue;
            const double t = heights[corner] / (heights[corner] - heights[other]);
            std::array<double, 3> point{};
            for (int c = 0; c < 3; ++c)
                point[c] = corners[corner][c] + (corners[other][c] - corners[corner][c]) * t;
            const bool duplicate = std::any_of(points.begin(), points.end(),
                [&](const std::array<double, 3> &known) {
                    return std::abs(known[0] - point[0]) < epsilon &&
                           std::abs(known[1] - point[1]) < epsilon &&
                           std::abs(known[2] - point[2]) < epsilon;
                });
            if (!duplicate) points.push_back(point);
        }
    if (points.size() < 3) return triangles;
    std::array<double, 3> centroid{};
    for (const auto &point : points)
        for (int c = 0; c < 3; ++c) centroid[c] += point[c] / double(points.size());
    double u[3]{n[1] - n[2], n[2] - n[0], n[0] - n[1]};
    double uLength = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(uLength > 0)) {
        // The normal is parallel to (1,1,1); fall back to any fixed helper axis.
        const double helper[3]{std::abs(n[2]) < .9 ? 0. : 1., 0., std::abs(n[2]) < .9 ? 1. : 0.};
        u[0] = n[1] * helper[2] - n[2] * helper[1];
        u[1] = n[2] * helper[0] - n[0] * helper[2];
        u[2] = n[0] * helper[1] - n[1] * helper[0];
        uLength = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(uLength > 0)) return triangles;
    }
    for (double &component : u) component /= uLength;
    const double v[3]{n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2],
                      n[0] * u[1] - n[1] * u[0]};
    std::sort(points.begin(), points.end(), [&](const auto &a, const auto &b) {
        const auto angle = [&](const std::array<double, 3> &p) {
            double d[3]{p[0] - centroid[0], p[1] - centroid[1], p[2] - centroid[2]};
            return std::atan2(d[0] * v[0] + d[1] * v[1] + d[2] * v[2],
                              d[0] * u[0] + d[1] * u[1] + d[2] * u[2]);
        };
        return angle(a) < angle(b);
    });
    constexpr double inset = .985;
    std::vector<DirectX::XMFLOAT3> polygon;
    polygon.reserve(points.size());
    for (auto point : points) {
        for (int c = 0; c < 3; ++c)
            point[c] = centroid[c] + (point[c] - centroid[c]) * inset;
        polygon.push_back({float(point[0]), float(point[1]), float(point[2])});
    }
    triangles.reserve((polygon.size() - 2) * 3);
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        triangles.push_back(polygon[0]);
        triangles.push_back(polygon[i]);
        triangles.push_back(polygon[i + 1]);
    }
    return triangles;
}
// Catalog category of a modifier operation; shared by the pipeline node
// coloring and the Quick command search registry so the two listings can
// never drift apart.
static const char *opCategory(Op op) {
    return op == Op::ColorCoding || op == Op::ColorType || op == Op::AssignColor
               ? "Coloring"
               : op == Op::SelectType || op == Op::SelectIndex ||
                         op == Op::SelectRange || op == Op::Invert ||
                         op == Op::Clear || op == Op::ExpandSelection ||
                         op == Op::SelectOverlapping || op == Op::ExpressionSelect ||
                         op == Op::ManualSelection
                     ? "Selection"
                     : op == Op::CoordinationAnalysis || op == Op::ClusterAnalysis ||
                               op == Op::RadialDistribution || op == Op::Histogram ||
                               op == Op::ReduceProperty || op == Op::CommonNeighborAnalysis ||
                               op == Op::CentrosymmetryParameter ||
                               op == Op::DisplacementVectors ||
                               op == Op::BondLengthDistribution || op == Op::BondAngleDistribution ||
                               op == Op::ScatterPlot
                         ? "Analysis"
                         : "Modification";
}

// One executable entry of the Quick command search (Ctrl+P). The single
// static table feeds the palette; actions map onto the same code paths the
// toolbar buttons and the modifier catalog use.
enum class CommandAction {
    Modifier, OpenFile, ExportFile, SaveSessionState, Snapshot, Render, Settings,
    ToggleQuad, FitAll, ToolZoom, ToolPan, ToolOrbit, ToolFov, LayerBuilder,
    ViewTop, ViewBottom, ViewFront, ViewBack, ViewLeft, ViewRight, ViewOrtho, ViewPerspective
};
struct Command {
    CommandAction action;
    const char *category;
    const char *label;
    Op op = Op::Translate;
};
static const std::vector<Command> &commandRegistry() {
    static const std::vector<Command> commands = [] {
        std::vector<Command> list;
        for (Op op : {Op::Slice, Op::Translate, Op::Scale, Op::Rotate, Op::Replicate,
                      Op::AffineTransform, Op::Wrap, Op::UnwrapTrajectories, Op::ComputeProperty,
                      Op::RemoveProperty, Op::FreezeProperty, Op::Delete, Op::EditCell,
                      Op::EditType, Op::CreateBonds, Op::CommonNeighborAnalysis,
                      Op::CentrosymmetryParameter, Op::CoordinationAnalysis, Op::ClusterAnalysis,
                      Op::RadialDistribution, Op::Histogram, Op::ReduceProperty, Op::ScatterPlot,
                      Op::BondLengthDistribution, Op::BondAngleDistribution, Op::DisplacementVectors,
                      Op::SelectType, Op::SelectRange, Op::ExpressionSelect, Op::SelectOverlapping,
                      Op::ExpandSelection, Op::ManualSelection, Op::Invert, Op::Clear,
                      Op::ColorCoding, Op::ColorType, Op::AssignColor})
            list.push_back({CommandAction::Modifier, opCategory(op), opName(op), op});
        list.push_back({CommandAction::OpenFile, "File", "Open File"});
        list.push_back({CommandAction::ExportFile, "File", "Export File"});
        list.push_back({CommandAction::SaveSessionState, "File", "Save Session State"});
        list.push_back({CommandAction::Snapshot, "Tools", "Snapshot"});
        list.push_back({CommandAction::Render, "Tools", "Render"});
        list.push_back({CommandAction::Settings, "Tools", "Application Settings"});
        list.push_back({CommandAction::LayerBuilder, "Build", "Build Layers"});
        list.push_back({CommandAction::ToggleQuad, "Tools", "Toggle single / four views"});
        list.push_back({CommandAction::FitAll, "Tools", "Fit all viewports"});
        list.push_back({CommandAction::ToolZoom, "Tools", "Zoom tool"});
        list.push_back({CommandAction::ToolPan, "Tools", "Pan tool"});
        list.push_back({CommandAction::ToolOrbit, "Tools", "Orbit tool"});
        list.push_back({CommandAction::ToolFov, "Tools", "FOV tool"});
        list.push_back({CommandAction::ViewTop, "View", "Top view"});
        list.push_back({CommandAction::ViewBottom, "View", "Bottom view"});
        list.push_back({CommandAction::ViewFront, "View", "Front view"});
        list.push_back({CommandAction::ViewBack, "View", "Back view"});
        list.push_back({CommandAction::ViewLeft, "View", "Left view"});
        list.push_back({CommandAction::ViewRight, "View", "Right view"});
        list.push_back({CommandAction::ViewOrtho, "View", "Orthographic view"});
        list.push_back({CommandAction::ViewPerspective, "View", "Perspective view"});
        return list;
    }();
    return commands;
}
// Application version surfaced by Help > About and System Information.
static const char *atomxVersion = "1.3.0-dev";
struct App {
    HWND window;
    desktop::Preferences preferences;
    ComPtr<ID3D11ShaderResourceView> logo;
    HICON icon = nullptr;
    bool showSettings = false, refreshFont = false, selectAnalysis = false, selectPipeline = false;
    // Help menu dialogs (deferred-open pattern shared with showSettings).
    bool showAbout = false, showSystemInfo = false;
    // Quick command search (Ctrl+P): paletteRequested arms keyboard focus on
    // the toolbar search field for the next frame; paletteActive keeps the
    // dropdown open while the field or an entry owns the interaction.
    char commandSearch[128]{};
    bool paletteRequested = false, paletteActive = false;
    int paletteIndex = 0;
    ImVec2 paletteAnchor{};
    // Smoke flags: open the File menu / the palette with a pre-filled query
    // for the automated screenshot runs.
    bool smokeMenuFile = false;
    ImVec2 catalogAnchor{};
    char modifierSearch[128]{};
    bool showWorkspace = false, indexing = false;
    float workspacePane = 220, pipelinePane = 268, propertiesPane = 316;
    float inspectorPane = 180, timelinePane = 94;
    bool layoutDirty = false;
    int pendingFrame = -1;
    Renderer &gpu;
    Dataset source = crystal(24);
    PipelineResult result;
    PipelineGraph modifierGraph;
    std::vector<ModifierNode> &mods = modifierGraph.nodes;
    std::vector<std::vector<ModifierNode>> undo, redo;
    InteractiveEditCheckpoint editCheckpoint;
    uint64_t nextModifierId = 1;
    std::filesystem::path path;
    std::vector<Frame> frames;
    int current = 0, budget = 2000000;
    bool playing = false, quad = true, particles = true, cell = true, showTable = false,
         showCatalog = false;
    bool loopPlayback = true; // P12 timeline loop toggle (design: Loop control).
    bool histogramPreviewTab = false;
    bool globalAttributesTab = false;
    bool bondsTab = false;
    // Data inspector table state: per-page filter text plus cached filtered
    // row-index lists so scrolling tables with 100k+ rows never re-filters
    // unless the text or the row count actually changed. cellViewMode
    // switches the Simulation Cell page between the vector and parameter
    // readouts.
    char particleFilter[128] = "", bondFilter[128] = "", globalAttributeFilter[128] = "";
    std::string particleFilterCache = "\x01", bondFilterCache = "\x01",
                globalAttributeFilterCache = "\x01";
    size_t particleFilterRows = size_t(-1), bondFilterRows = size_t(-1),
           globalAttributeFilterRows = size_t(-1);
    std::vector<uint32_t> filteredParticles, filteredBonds, filteredAttributes;
    int cellViewMode = 0;
    std::map<std::string, HistogramPlotView> histogramPlotViews;
    float radius = .32f, bg[4] = {0, 0, 0, 1}, fps = 12;
    int particleShape = 0;
    int appearanceType = 0;
    std::vector<std::string> appearanceNames;
    std::unordered_map<std::string, ParticleStyle> appearanceMemory;
    std::unordered_map<std::string, float> customRadiusMemory;

    void syncAppearance(const std::vector<std::string>& names) {
        std::string selected;
        if (appearanceType >= 0 && size_t(appearanceType) < appearanceNames.size())
            selected = appearanceNames[appearanceType];
        // During an asynchronous file change the last displayed dataset may
        // briefly resize renderer styles. Those entries belong to the old
        // dataset, so never remember them under the incoming element names.
        if (gpu.styles.size() == std::max<size_t>(appearanceNames.size(), 1))
            for (size_t i = 0; i < appearanceNames.size(); ++i)
                appearanceMemory[appearanceNames[i]] = gpu.styles[i];
        if (names != appearanceNames || gpu.styles.size() != std::max<size_t>(names.size(), 1)) {
            gpu.resetStyles(names.size(), &names);
            for (size_t i = 0; i < names.size(); ++i) {
                auto found = appearanceMemory.find(names[i]);
                if (found != appearanceMemory.end()) gpu.styles[i] = found->second;
            }
            appearanceNames = names;
            auto found = std::find(names.begin(), names.end(), selected);
            appearanceType = found == names.end() ? 0 : int(found - names.begin());
        }
    }
    void setDefaultRadius(const std::string& name, ParticleStyle& style, bool inherit) {
        if (inherit) {
            if (style.visual[0] > 0) customRadiusMemory[name] = style.visual[0];
            style.visual[0] = 0;
        } else {
            auto found = customRadiusMemory.find(name);
            if (found == customRadiusMemory.end())
                // First edit of an element type starts from its covalent
                // radius so the slider continues from the displayed default.
                if (const auto *element = atomx::elements::find(name))
                    found = customRadiusMemory.emplace(name, element->covalent).first;
            style.visual[0] = found == customRadiusMemory.end() ? radius : found->second;
        }
    }
    bool focusParticleAppearance = false;
    int renderMode = 0;
    float cellColor[4] = {0.82f,0.9f,1.f,0.88f};
    float cellWidth = 1.25f, cellGlow = 0.35f;
    bool cellDashed = false, cellLabels = false;
    int cellDimension = 1;
    bool colorCoding = false, colorDiscrete = false, colorSelectedOnly = false, colorAutoRange = false,
         colorSymmetricRange = false, colorReverse = false;
    bool colorLegend = true;
    int colorAxis = 0, colorGradient = 0;
    float colorMin = 0, colorMax = 1;
    std::string colorRangeProperty = "Position.X";
    int active = 3, propertyAxis = 2, tab = 0;
    int viewportTool = 2; // 0 zoom, 1 pan, 2 orbit, 3 perspective
    // Right panel section switcher (icon row that replaced the text tabs):
    // 0 pipeline, 1 render, 2 analysis, 3 system.
    int rightTab = 0;
    // P12 left-panel selection: -1 data source, -2 particles, -3 simulation
    // cell, >=0 selected modifier row.
    int panelSelection = -1;
    // Latest status message shown as a fading overlay in the active viewport
    // (replaces the removed full-width status bar).
    std::string overlayStatus;
    double overlayStatusAt = -1e9;
    bool animationSettings = false, autoKey = false;
    Camera cameras[4];
    Target targets[4];
    std::future<Loaded> job;
    std::future<PipelineJobResult> pipelineJob;
    std::future<PipelineResult> inspectorJob;
    std::optional<PipelineResult> inspectorResult;
    std::atomic<bool> pipelineCancel{false};
    std::atomic<bool> inspectorCancel{false};
    std::atomic<size_t> pipelineActiveNode{0};
    std::atomic<size_t> inspectorActiveNode{0};
    bool pipelineBusy = false;
    bool inspectorBusy = false;
    bool pipelineDeferredForInspector = false;
    int inspectorNode = -1;
    uint64_t inspectorGeneration = 0;
    std::string inspectorNodeId;
    std::filesystem::path deferredLoadPath;
    int deferredLoadFrame = 0;
    uint64_t pipelineGeneration = 0, pipelineJobGeneration = 0;
    std::vector<std::string> pipelineJobNodeIds;
    // Unwrap trajectories per-node accumulator: node id -> (frame, unwrapped
    // output positions of that frame). Persisted across pipeline launches so
    // frame-by-frame playback chains continuously; cleared when a new file is
    // loaded. Only touched by the single active pipeline worker thread.
    std::map<std::string, std::pair<int, std::vector<Vec3>>> unwrapAccumulators;
    std::shared_ptr<const PipelineResult> pipelineCheckpoint;
    size_t pipelineCheckpointNode = SIZE_MAX;
    std::atomic<float> progress{0};
    std::atomic<bool> cancel{false};
    bool busy = false, staleResult = false, staleBeforeLoad = false;
    std::string status = "Ready", error;
    std::string dropNotice; // Appended to the load status for multi-file drops.
    std::string readerName = "Generated crystal";
    double lastFrame = 0;
    int exportW = 1920, exportH = 1080;
    bool showDataExport = false, exportRange = false, exportSequence = false, exporting = false,
         exportPreview = false;
    int exportFormat = 0, exportFirst = 0, exportLast = 0, exportStep = 1;
    io::ExportOptions exportOptions;
    std::future<std::string> exportJob;
    std::filesystem::path exportedDocumentPath;
    uint64_t exportedDocumentTab = 0;
    std::atomic<bool> exportCancel{false};
    std::atomic<float> exportProgress{0};
    std::string filter;
    struct UiTestItem { ImGuiID id=0; ImVec2 min{}, max{}; bool hovered=false, clicked=false; };
    bool captureUiTestItems = false;
    std::unordered_map<std::string,UiTestItem> uiTestItems;
    Statistics cachedStats[3];
    size_t selectedCount = 0;
    float cutoff = .8f;
    std::optional<NeighborAnalysis> analysis;
    std::future<NeighborAnalysis> analysisJob;
    bool analyzing = false;
    uint64_t revision = 0, analysisRevision = 0;
    std::optional<DXAResult> dxa;
    std::future<DXAResult> dxaJob;
    bool dxaRunning = false;
    float dxaCutoff = .8f;
    std::future<std::pair<double, double>> colorRangeJob;
    std::atomic<bool> colorRangeCancel{false};
    std::atomic<float> colorRangeProgress{0};
    bool colorRangeRunning = false;
    uint64_t colorRangePipelineGeneration = 0;
    std::string colorRangeNodeId;
    std::filesystem::path colorRangePath;
    // One structure per browser-style tab. The live document stays in the
    // fields above; a tab is only a snapshot taken when leaving it.
    enum class CreationTool { Select, Rotate, Pan, Move, Sketch, Ring, Fragment, Distance, Angle, Torsion, Zoom };
    enum class CreationDrag { None, Box, Move, Spin, Rotate, Pan, Sketch, Ring, Fragment, Fusion, Geometry, Zoom };
    std::vector<int> geometryPending;
    std::optional<geometry::Plan> geometryDragPlans[2];
    double geometryTarget=0,geometryDragTarget=0;
    bool geometryInverted=false,geometryDragInverted=false;
    int geometryPanelMonitor=-1;
    int creationMonitorFilter=0;
    char creationPropertyFilter[128]{};
    bool openMeasurementConstraints=false;
    uint64_t measurementConstraintTab=0,measurementConstraintGeneration=0;
    std::vector<geometry::Monitor> measurementConstraintSource;
    std::vector<int> measurementConstraintRows;
    std::array<int,3> measurementConstraintChoices{};
    std::string measurementConstraintMessage;
    struct CreationHistoryState {
        std::optional<Dataset> data; // Display-only history does not copy a large structure.
        creation::Display display;
        std::string action;
    };
    struct CreationSnapshot {
        std::shared_ptr<const Dataset> data;
        std::string name, time;
        creation::Display display;
    };
    struct StructureTab {
        uint64_t id = 0, sourceTabId = 0;
        std::string title = "Structure";
        bool home = false;
        std::string basedOn;
        Dataset source;
        PipelineGraph graph;
        std::vector<std::vector<ModifierNode>> undo, redo;
        std::filesystem::path path;
        std::filesystem::path documentPath;
        std::vector<Frame> frames;
        int current = 0;
        std::string readerName = "Generated crystal";
        Camera cameras[4]{};
        std::vector<ParticleStyle> styles;
        float radius = .32f;
        int particleShape = 0, viewportTool = 0;
        bool cell = true, particles = true, quad = false;
        bool creationMode = false;
        bool creationSketch = false;
        bool sketchContinuous = true;
        bool autoHydrogens = false;
        int sketchOrder = 1;
        int ringSize = 6;
        std::string fragmentKey="builtin/methyl";
        int fragmentConnector=1;
        CreationTool creationTool = CreationTool::Select;
        std::vector<int> selection, bondSelection;
        int picked = -1;
        int measure = -1;
        int angleAtom = -1;
        int dihedralAtom = -1;
        std::vector<CreationHistoryState> authorUndo, authorRedo;
        creation::Display display;
        std::vector<CreationSnapshot> snapshots;
        int selectedSnapshot = -1, propertyPage = 0, monitorFilter = 0;
        std::string propertyFilter;
        bool propertiesOpen = true;
        std::optional<symmetry::Info> symmetryInfo;
        bool symmetryChecked = false;
        char element[16] = "O";
    };
    struct ProjectResult { std::vector<StructureTab> tabs;int active=0;std::string message;std::filesystem::path path; };
    std::future<ProjectResult> projectJob;
    std::atomic<bool> projectCancel{false};
    bool projectBusy=false;
    std::filesystem::path projectPath;
    static document::View projectView(const StructureTab &t) {
        document::View v;v.creation=t.creationMode;v.cell=t.cell;v.particles=t.particles;v.radius=t.radius;
        v.title=t.title;v.basedOn=t.basedOn;v.shape=t.particleShape;v.tool=int(t.creationTool);
        v.order=t.sketchOrder;v.ringSize=t.ringSize;v.fragmentKey=t.fragmentKey;v.fragmentConnector=t.fragmentConnector;
        v.element=t.element;v.continuous=t.sketchContinuous;v.autoHydrogens=t.autoHydrogens;
        v.propertiesOpen=t.propertiesOpen;v.propertyPage=t.propertyPage;
        if(t.creationMode){v.display=t.display;v.selection.assign(t.selection.begin(),t.selection.end());v.bondSelection.assign(t.bondSelection.begin(),t.bondSelection.end());}
        const auto &c=t.cameras[3];v.camera={c.yaw,c.pitch,c.zoom,c.panX,c.panY};v.cameraRoll=c.roll;
        v.cameraMode=c.mode;v.fitSelected=c.fitSelected;v.fitLo=c.fitLo;v.fitHi=c.fitHi;
        if(t.styles.size()==t.source.species.size())for(const auto &s:t.styles)v.styles.push_back({s.color,s.visual,s.axes});
        return v;
    }
    static void writeProject(const std::filesystem::path &destination,const std::vector<StructureTab> &saved,int active,std::atomic<bool> *cancel=nullptr) {
        document::valid(!saved.empty() && saved.size()<=64 && active>=0 && size_t(active)<saved.size());
        auto temporary=destination;temporary+=L".tmp-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file)throw std::runtime_error("Cannot create project file");
            project::Writer w{file,cancel};w.bytes("ATOMXPRJ",8);w.value(uint32_t(2));w.value(int32_t(active));w.value(uint64_t(saved.size()));
            for(const auto &t:saved) {
                w.value(t.id);w.value(t.sourceTabId);w.flag(t.home);
                w.text(t.path.empty()?"":utf8(std::filesystem::absolute(t.path).wstring()));w.text(t.documentPath.empty()?"":utf8(std::filesystem::absolute(t.documentPath).wstring()));w.text(t.readerName);
                project::writeDocument(w,t.source,projectView(t));
                for(const auto &c:t.cameras)project::writeCamera(w,c);
                w.flag(t.quad);w.value(int32_t(t.viewportTool));w.value(int32_t(t.current));
                w.value(int32_t(t.picked));w.value(int32_t(t.measure));w.value(int32_t(t.angleAtom));w.value(int32_t(t.dihedralAtom));
                w.value(int32_t(t.monitorFilter));w.text(t.propertyFilter);
                w.value(int32_t(t.selectedSnapshot));w.value(uint64_t(t.graph.selected));project::writeGraph(w,t.graph.nodes);
                for(const auto *stack:{&t.undo,&t.redo}) {w.value(uint64_t(stack->size()));for(const auto &nodes:*stack)project::writeGraph(w,nodes);}
                w.value(uint64_t(t.frames.size()));for(const auto &f:t.frames){w.value(f.count);w.value(int64_t(f.offset));w.text(f.comment);}
                std::error_code ec;const auto bytes=t.frames.empty()?uintmax_t(0):std::filesystem::file_size(t.path,ec);
                const bool external=!t.frames.empty() && !ec;w.flag(external);
                if(external){w.value(uint64_t(bytes));const auto stamp=std::filesystem::last_write_time(t.path,ec);
                    if(ec)throw std::runtime_error("Cannot inspect trajectory dependency");w.value(int64_t(stamp.time_since_epoch().count()));}
                w.value(uint64_t(t.snapshots.size()));for(const auto &snap:t.snapshots) {
                    document::valid(bool(snap.data));w.text(snap.name);w.text(snap.time);
                    document::View v;v.creation=true;v.display=snap.display;project::writeDocument(w,*snap.data,v);
                }
                for(const auto *stack:{&t.authorUndo,&t.authorRedo}) {
                    w.value(uint64_t(stack->size()));const Dataset *state=&t.source;
                    // Resolve display-only entries from newest to oldest without cloning their datasets.
                    std::vector<const Dataset *> states(stack->size());
                    for(size_t i=stack->size();i>0;--i){const auto &h=(*stack)[i-1];if(h.data)state=&*h.data;states[i-1]=state;}
                    for(size_t i=0;i<stack->size();++i){const auto &h=(*stack)[i];w.flag(bool(h.data));w.text(h.action);
                        document::View v;v.creation=true;v.display=h.display;project::writeDocument(w,*states[i],v);}
                }
            }
            w.bytes("DONE",4);file.flush();document::checkpoint(cancel);
            if(!file || uint64_t(file.tellp())>project::byteLimit)throw std::runtime_error("Project exceeds 2 GiB or could not be written");
            file.close();
            if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish project file");
        }catch(...){std::error_code ec;std::filesystem::remove(temporary,ec);throw;}
    }
    static ProjectResult readProject(const std::filesystem::path &path,uint64_t atomBudget,std::atomic<bool> *cancel=nullptr) {
        std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open project");
        file.seekg(0,std::ios::end);const auto size=file.tellg();document::valid(size>=24 && uint64_t(size)<=project::byteLimit);file.seekg(0);
        project::Reader r{file,uint64_t(size),cancel};char signature[8];r.bytes(signature,8);
        document::valid(std::string_view(signature,8)=="ATOMXPRJ");const auto version=r.value<uint32_t>();document::valid(version>=1 && version<=2);
        ProjectResult result;result.path=path;result.active=r.value<int32_t>();result.tabs.resize(r.count(8,64));
        document::valid(!result.tabs.empty() && result.active>=0 && size_t(result.active)<result.tabs.size());std::set<uint64_t> ids;
        bool missingTrajectory=false;
        for(auto &t:result.tabs) {
            t.id=r.value<uint64_t>();t.sourceTabId=r.value<uint64_t>();t.home=r.flag();document::valid(t.id>0 && t.id<UINT64_MAX && ids.insert(t.id).second);
            t.path=project::pathFromUtf8(r.text());t.documentPath=project::pathFromUtf8(r.text());t.readerName=r.text();
            auto content=project::readDocument(r,atomBudget);auto &v=content.view;t.source=std::move(content.data);
            t.title=v.title;t.basedOn=v.basedOn;t.creationMode=v.creation;t.cell=v.cell;t.particles=v.particles;t.radius=v.radius;
            t.particleShape=v.shape;t.creationTool=CreationTool(v.tool);t.creationSketch=t.creationTool==CreationTool::Sketch;
            t.sketchOrder=v.order;t.ringSize=v.ringSize;t.fragmentKey=v.fragmentKey;t.fragmentConnector=v.fragmentConnector;
            t.sketchContinuous=v.continuous;t.autoHydrogens=v.autoHydrogens;t.propertiesOpen=v.propertiesOpen;t.propertyPage=v.propertyPage;
            snprintf(t.element,sizeof(t.element),"%s",v.element.c_str());t.selection.assign(v.selection.begin(),v.selection.end());
            t.bondSelection.assign(v.bondSelection.begin(),v.bondSelection.end());t.display=std::move(v.display);
            for(const auto &s:v.styles)t.styles.push_back({s.color,s.visual,s.axes});
            for(auto &c:t.cameras)c=project::readCamera(r);
            t.quad=r.flag();t.viewportTool=r.value<int32_t>();t.current=r.value<int32_t>();
            t.picked=r.value<int32_t>();t.measure=r.value<int32_t>();t.angleAtom=r.value<int32_t>();t.dihedralAtom=r.value<int32_t>();
            if(version>=2){t.monitorFilter=r.value<int32_t>();t.propertyFilter=r.text();document::valid(t.monitorFilter>=0 && t.monitorFilter<=3 && t.propertyFilter.size()<128);}
            t.selectedSnapshot=r.value<int32_t>();const auto selected=r.value<uint64_t>();t.graph.nodes=project::readGraph(r);
            document::valid((t.graph.nodes.empty()?selected==0:selected<t.graph.nodes.size()) && t.viewportTool>=0 && t.viewportTool<=3);t.graph.selected=size_t(selected);
            for(auto *stack:{&t.undo,&t.redo}){stack->resize(r.count(8,128));for(auto &nodes:*stack)nodes=project::readGraph(r);}
            t.frames.resize(r.count(24,1000000));for(auto &f:t.frames){f.count=r.value<uint64_t>();f.offset=std::streamoff(r.value<int64_t>());f.comment=r.text();document::valid(f.offset>=0);}
            document::valid(t.current>=0 && (t.frames.empty()?t.current==0:size_t(t.current)<t.frames.size()));
            const bool external=r.flag();uint64_t dependencySize=0;int64_t dependencyStamp=0;
            if(external){dependencySize=r.value<uint64_t>();dependencyStamp=r.value<int64_t>();}
            t.snapshots.resize(r.count(16,24));for(auto &snap:t.snapshots){snap.name=r.text();snap.time=r.text();
                auto c=project::readDocument(r,atomBudget);snap.display=std::move(c.view.display);snap.data=std::make_shared<const Dataset>(std::move(c.data));}
            document::valid(t.selectedSnapshot>=-1 && (t.selectedSnapshot<0 || size_t(t.selectedSnapshot)<t.snapshots.size()));
            for(auto *stack:{&t.authorUndo,&t.authorRedo}){stack->resize(r.count(9,64));for(auto &h:*stack){
                const bool hasData=r.flag();h.action=r.text();auto c=project::readDocument(r,atomBudget);h.display=std::move(c.view.display);if(hasData)h.data=std::move(c.data);}}
            if(!t.frames.empty()) {
                std::error_code ec;const auto bytes=std::filesystem::file_size(t.path,ec);bool matches=external && !ec && bytes==dependencySize;
                if(matches){const auto stamp=std::filesystem::last_write_time(t.path,ec);matches=!ec && int64_t(stamp.time_since_epoch().count())==dependencyStamp;}
                if(!matches){t.frames.clear();t.current=0;missingTrajectory=true;}
            }
        }
        char end[4];r.bytes(end,4);document::valid(std::string_view(end,4)=="DONE" && r.remaining==0);
        result.message=missingTrajectory?"工作区已恢复；外部轨迹缺失或已改变，保留内嵌当前帧":"工作区已恢复（标签、管线、暂存与历史）";return result;
    }
    void saveProject(const std::filesystem::path &destination) {
        if(documentsBusy() || exporting || creationDrag!=CreationDrag::None){status="Wait for the current operation before saving a workspace";return;}
        if(lowerExtension(destination)!=".atomx-project"){status="Use the .atomx-project extension";return;}
        tabs[size_t(activeTab)]=captureTab();projectCancel=false;
        projectJob=std::async(std::launch::async,[saved=tabs,active=activeTab,destination,this] {
            writeProject(destination,saved,active,&projectCancel);ProjectResult r;r.path=destination;r.message="工作区已保存";return r;
        });projectBusy=true;status="正在保存工作区…";
    }
    void loadProject(const std::filesystem::path &file) {
        if(documentsBusy() || exporting || creationDrag!=CreationDrag::None){status="Wait for the current operation before opening a workspace";return;}
        projectCancel=false;projectJob=std::async(std::launch::async,[file,atomBudget=uint64_t(budget),this]{return readProject(file,atomBudget,&projectCancel);});
        projectBusy=true;status="正在打开工作区…";
    }
    void projectDialog(bool save) {
        const auto chosen=dialog(window,save,L"AtomX workspace (*.atomx-project)\0*.atomx-project\0\0",L"atomx-project");
        if(chosen.empty())return;if(save)saveProject(chosen);else loadProject(chosen);
    }
    std::vector<StructureTab> tabs;
    int activeTab = 0;
    int displayedTab = -1;
    uint64_t nextTabId = 1;
    uint64_t pendingTabId = 0;
    bool homeMode = false;
    int creationReturnTab = -1;
    std::string creationBasedOn;
    bool creationMode = false;
    bool creationSketch = false;
    bool creationSketchContinuous = true, creationSketchPreviewValid = false, creationSketchDoubleClick = false;
    bool creationAutoHydrogens=false;
    std::string creationAutoHydrogenMessage;
    int creationSketchOrder = 1, creationSketchAnchor = -1, creationSketchStartHit = -1;
    int creationSketchLastPlaced = -1;
    bool creationSketchInsert=false;
    int creationInsertHover=-1;
    ImVec2 creationInsertMouse{-1,-1},creationInsertSize{},creationInsertOrigin{};
    DirectX::XMFLOAT4X4 creationInsertMatrix{};
    uint64_t creationInsertGeneration=0;
    Vec3 creationSketchPreview{};
    int creationRingSize=6;
    bool creationRingPreviewValid=false;
    authoring::RingSketch creationRingPreview{}, creationRingStart{};
    ImVec2 creationRingMouse{-1,-1};
    std::vector<fragments::Template> fragmentLibrary=fragments::builtins();
    std::string creationFragmentKey="builtin/methyl";
    int creationFragmentConnector=1;
    bool creationFragmentPreviewValid=false,showFragmentBrowser=false,openFragmentDefine=false;
    fragments::Placement creationFragmentPreview,creationFragmentStart;
    std::shared_ptr<const fragments::FusionSeed> creationFusionSeed;
    std::optional<fragments::Fusion> creationFusionPreview,creationFusionStart;
    int creationFusionTarget=-2;
    bool creationFusionPick=false;
    ImVec2 creationFragmentMouse{-1,-1};
    float fragmentPreviewYaw=.5f,fragmentPreviewPitch=.3f,fragmentPreviewZoom=1.f;
    ImVec2 fragmentPreviewPan{};
    char fragmentSearch[128]{},fragmentName[128]{},fragmentCategory[128]="我的片段";
    std::optional<fragments::Template> fragmentDefineDraft;
    std::string fragmentLibraryMessage;
    std::filesystem::path fragmentLibraryDirectory;
    bool fragmentLibraryRequested=false,fragmentSaveComplete=false;
    struct FragmentLoad { std::vector<fragments::Template> entries; std::string message; };
    std::future<FragmentLoad> fragmentLibraryJob;
    std::future<fragments::Template> fragmentSaveJob;
    uint64_t fragmentSaveTabId=0;
    CreationTool creationTool = CreationTool::Select;
    std::vector<int> creationSelection, creationBondSelection;
    std::vector<CreationSnapshot> creationSnapshots;
    int creationSnapshotSelected = -1, creationPropertyPage = 0;
    bool creationPropertiesOpen = true;
    std::optional<symmetry::Info> creationSymmetry;
    bool creationSymmetryChecked = false;
    std::array<double,6> creationCellParameters{};
    double creationCellOrigin[3]{};
    std::array<bool,3> creationCellPbc{};
    bool creationCellPreserveFractional = false;
    int creationHover = -1;
    ImVec2 creationLastHoverMouse{-1,-1};
    CreationDrag creationDrag = CreationDrag::None;
    ImVec2 creationDragStart{}, creationDragPrevious{};
    Camera creationDragCamera{};
    bool creationDragEdgeRoll=false;
    double creationDragRollAngle=0;
    bool creationDragMoved = false, creationDragShift = false;
    int creationDragButton = ImGuiMouseButton_Left;
    bool creationDragToggle = false;
    std::vector<std::pair<int,Vec3>> creationDragAtoms;
    Vec3 creationMoveDelta{};
    Vec3 creationDragCenter{};
    DirectX::XMMATRIX creationDragMatrix{};
    bool creationPositionFractional = false, openCreationPosition = false;
    int creationPositionIndex = -1;
    uint64_t creationPositionTabId = 0;
    float creationPositionDraft[3]{};
    bool openCreationMovement = false, creationMovementScreenAxes = true;
    bool creationMovementMassCenter=false,creationMovementHasMass=false,showMotionGroups=false,motionGroupBusy=false;
    bool showCreationStyles=false,creationStyleAll=false;
    int creationStylePreset=0;
    creation::Display creationStyleDraft;
    creation::ColorRule creationColorDraft;
    bool creationColorBusy=false;
    std::future<std::pair<double,double>> creationColorRangeJob;
    std::string creationColorMessage;
    bool showHydrogenAdjust=false,hydrogenBusy=false;
    uint64_t hydrogenTabId=0;
    hydrogens::Options hydrogenOptions;
    bool openCreationChemistry=false,chemistrySetCharge=false;
    int chemistryCharge=0,chemistryHybridChoice=0;
    uint64_t chemistryTabId=0,chemistryGeneration=0;
    std::vector<int> chemistrySelection;
    std::string chemistrySummary,chemistryMessage;
    bool openAtomProperties=false,atomPropertyMissing=false;
    uint64_t atomPropertyTabId=0,atomPropertyGeneration=0;
    std::vector<int> atomPropertySelection;
    std::vector<std::string> atomPropertyNames;
    std::string atomPropertyName,atomPropertySummary,atomPropertyMessage;
    double atomPropertyValue=0;
    bool openAtomConstraints=false;
    uint64_t constraintTabId=0,constraintGeneration=0;
    std::vector<int> constraintSelection;
    constraints::Draft constraintDraft;
    constraints::Summary constraintSummary;
    std::string constraintMessage;
    std::optional<hydrogens::Plan> hydrogenPlan;
    std::future<hydrogens::Plan> hydrogenJob;
    std::string hydrogenMessage;
    uint64_t motionRevision=0,motionCacheRevision=UINT64_MAX;
    std::vector<motion::Group> motionGroupCache;
    std::future<motion::Assignment> motionGroupJob;
    char motionGroupName[256]="我的分组";
    int motionGroupSelected=0;
    std::string motionGroupMessage;
    bool creationMovementPercent = false;
    float creationMovementDistance = 1.f, creationMovementAngle = 45.f;
    uint64_t creationMovementTabId = 0;
    uint64_t creationMovementGeneration = 0;
    std::vector<int> creationMovementSelection;
    std::vector<int> creationMovementBonds;
    bool creationMovementPeriodic=false;
    Vec3 creationMovementAxes[3]{};
    float creationMovementScreenSpan = 0;
    ImVec2 creationViewportSize{};
    std::string creationMovementMessage;
    creation::Display creationDisplay;
    bool openCreationLabels = false, creationLabelAll = false;
    uint64_t creationLabelTabId = 0;
    int creationLabelKind = int(creation::LabelKind::ElementIndex);
    char creationLabelText[128]{};
    std::vector<creation::LabelField> creationLabelFields;
    int32_t creationLabelPrecision=6;
    creation::Display creationLabelDraft;
    std::vector<int> creationLabelSelection;
    bool openCreationBondLabels=false,creationBondLabelAll=true;
    uint64_t creationBondLabelTabId=0,creationBondLabelGeneration=0;
    creation::BondLabels creationBondLabelDraft;
    creation::BondLabel creationBondLabelRule;
    std::vector<int> creationBondLabelSelection, creationBondLabelRows;
    size_t creationBondLabelCount=0;
    char creationBondLabelText[1025]{};
    std::string creationBondLabelMessage;
    int creationPick = -1;
    int creationMeasure = -1;
    int creationAngle = -1;
    int creationDihedral = -1;
    std::vector<CreationHistoryState> authorUndo, authorRedo;
    char creationElement[16] = "O";
    bool showLatticePanel = false;
    bool openNewCell = false;
    bool openTriclinicCell = false;
    bool openCrystalDialog = false, openSupercellDialog = false, openSurfaceDialog = false;
    int crystalPreset = 1, supercellFactor[3] = {2,2,1};
    int crystalSpaceGroup = 225;
    int surfaceMiller[3] = {0,0,1};
    int surfaceLayers = 3;
    float crystalA = 5.64f, crystalB = 5.64f, crystalC = 5.64f;
    float crystalAlpha = 90.f, crystalBeta = 90.f, crystalGamma = 90.f;
    float surfaceVacuum = 15.f;
    bool openLayerBuilder=false,layerBuildBusy=false,layerBuildCompleted=false;
    uint64_t layerBuilderTabId=0;
    int layerCount=2;
    uint64_t layerSourceIds[3]{};
    char layerNames[3][128]{"Layer 1","Layer 2","Layer 3"};
    double layerGaps[3]{3,3,3},layerOffsets[3][2]{};
    int layerCleaves[3]{},layerFlips[3]{};
    layers::Options layerOptions;
    std::future<layers::Built> layerBuildJob;
    std::string layerBuilderMessage;
    bool crystalReplaceCurrent = true;
    struct CrystalBasis { char element[8] = "Na"; float fractional[3]{}; };
    std::vector<CrystalBasis> crystalBasis;
    float newCellA = 5.f, newCellB = 5.f, newCellC = 5.f;
    float newAlpha = 90.f, newBeta = 90.f, newGamma = 90.f;
    App(HWND w, Renderer &r) : window(w), gpu(r) {
        preferences.load();
        workspacePane = float(preferences.workspacePane);
        pipelinePane = float(preferences.pipelinePane);
        propertiesPane = float(preferences.propertiesPane);
        inspectorPane = float(preferences.inspectorPane);
        timelinePane = float(preferences.timelinePane);
        theme(preferences.theme);
        refreshFont = true;
        logo = desktop::loadLogo(gpu.device.Get(), icon);
        SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        desktop::initTray(window, icon);
        cameras[0].mode = 0;
        cameras[1].mode = 2;
        cameras[2].mode = 4;
        update();
        auto initial = captureTab();
        initial.id = nextTabId++;
        tabs.push_back(std::move(initial));
        activeTab = 0;
    }
    ~App() {
        projectCancel=true;if(projectJob.valid())projectJob.wait();
        Shell_NotifyIconW(NIM_DELETE, &desktop::tray);
        if (icon) DestroyIcon(icon);
        cancel = true;
        pipelineCancel = true;
        if (job.valid())
            job.wait();
        if (pipelineJob.valid()) pipelineJob.wait();
        inspectorCancel = true;
        if (inspectorJob.valid()) inspectorJob.wait();
        if (analysisJob.valid())
            analysisJob.wait();
        if (dxaJob.valid()) dxaJob.wait();
        colorRangeCancel = true;
        if (colorRangeJob.valid()) colorRangeJob.wait();
        if (creationColorRangeJob.valid()) creationColorRangeJob.wait();
        if (hydrogenJob.valid()) hydrogenJob.wait();
        exportCancel = true;
        if (exportJob.valid())
            exportJob.wait();
    }
    // Refreshes the slice-plane viewport overlay from the last enabled Slice
    // node that requests it. Overlay failures degrade to "no plane" rather
    // than failing the pipeline result.
    void updateSlicePlaneVisual() {
        std::vector<DirectX::XMFLOAT3> triangles;
        for (auto it = mods.rbegin(); it != mods.rend(); ++it)
            if (it->enabled && it->op == Op::Slice && it->sliceShowPlane) {
                triangles = slicePlaneTriangles(it->sliceNormal, it->sliceDistance, result.data);
                break;
            }
        try {
            gpu.setSlicePlane(triangles);
        } catch (...) {
            try { gpu.setSlicePlane({}); } catch (...) {}
        }
    }
    void publishPipeline(PipelineResult next) {
        try {
            auto activeColor = std::find_if(mods.rbegin(), mods.rend(), [](const Modifier &m) {
                return m.enabled && (m.op == Op::ColorCoding || m.op == Op::ColorType || m.op == Op::AssignColor);
            });
            colorCoding = activeColor != mods.rend() && activeColor->op == Op::ColorCoding;
            if (colorCoding) {
                colorGradient = activeColor->colorGradient;
                colorAutoRange = activeColor->colorAutoRange;
                colorSymmetricRange = activeColor->colorSymmetricRange;
                colorReverse = activeColor->colorReverse;
                colorLegend = activeColor->colorLegend;
                colorDiscrete = activeColor->colorDiscrete;
                colorSelectedOnly = activeColor->colorSelectedOnly;
                colorRangeProperty = activeColor->property;
                colorMin = activeColor->colorMin;
                colorMax = activeColor->colorMax;
            }
            if (colorCoding && activeColor->colorAutoRange) {
                double lo = activeColor->colorAllFramesRange ? activeColor->colorMin
                    : std::numeric_limits<double>::infinity();
                double hi = activeColor->colorAllFramesRange ? activeColor->colorMax
                    : -std::numeric_limits<double>::infinity();
                if (!activeColor->colorAllFramesRange) {
                    if (auto it = next.data.scalarProperties.find("Color coding");
                        it != next.data.scalarProperties.end() && it->second.size() == next.data.atoms.size()) {
                        for (double value : it->second)
                            if (std::isfinite(value)) { lo = std::min(lo, value); hi = std::max(hi, value); }
                    } else {
                        for (const auto &a : next.data.atoms) {
                            double value = coordinate(a, colorAxis);
                            lo = std::min(lo, value); hi = std::max(hi, value);
                        }
                    }
                }
                if (std::isfinite(lo) && std::isfinite(hi)) {
                    if (lo == hi) {
                        const float center = float(lo);
                        const double delta = std::max(std::abs(double(center)) * .01, .5);
                        const double lowValue = double(center) - delta;
                        const double highValue = double(center) + delta;
                        const double floatLimit = std::numeric_limits<float>::max();
                        float expandedLow = lowValue < -floatLimit
                            ? std::nextafter(center, -std::numeric_limits<float>::infinity())
                            : float(lowValue);
                        float expandedHigh = highValue > floatLimit
                            ? std::nextafter(center, std::numeric_limits<float>::infinity())
                            : float(highValue);
                        if (std::isfinite(expandedLow)) lo = expandedLow;
                        if (std::isfinite(expandedHigh)) hi = expandedHigh;
                    }
                    if (colorSymmetricRange) {
                        double extent = std::max(std::abs(lo), std::abs(hi));
                        lo = -extent; hi = extent;
                    }
                    colorMin = float(lo); colorMax = float(hi);
                }
            }
            syncAppearance(next.data.species);
            uploadCreationDisplay(next.data, next.selected, next.colorSelected);
            result = std::move(next);
            updateSlicePlaneVisual();
            for (auto &camera : cameras)
                if (camera.fitSelected) refreshSelectionFit(camera,result.data,result.selected);
            for (size_t i = 0; i < modifierGraph.nodes.size(); ++i) {
                auto &node = modifierGraph.nodes[i];
                node.dirty = false;
                node.running = false;
                node.error.clear();
                node.outputs.clear();
                if (!node.enabled) continue;
                node.outputs=modifierOutputs(node,i);
            }
            for (int k = 0; k < 3; k++)
                cachedStats[k] = statistics(result.data, k);
            selectedCount = std::count(result.selected.begin(), result.selected.end(), 1);
            analysis.reset();
            staleResult = false;
            revision++;
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    void refreshSelectionFit(Camera &camera, const Dataset &data,
                             const std::vector<uint8_t> &selection, bool force = false) {
        if (!force && !camera.fitSelected) return;
        Vec3 lo{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        Vec3 hi{-lo.x,-lo.y,-lo.z};
        bool found=false;
        for (size_t i=0;i<data.atoms.size() && i<selection.size();++i) {
            if (!selection[i]) continue;
            found=true;
            const auto &atom=data.atoms[i];
            lo.x=std::min(lo.x,atom.x); lo.y=std::min(lo.y,atom.y); lo.z=std::min(lo.z,atom.z);
            hi.x=std::max(hi.x,atom.x); hi.y=std::max(hi.y,atom.y); hi.z=std::max(hi.z,atom.z);
        }
        if (!found) { camera.fitSelected=false; return; }
        const float pad=std::max(radius*1.4f,.18f);
        camera.fitLo={lo.x-pad,lo.y-pad,lo.z-pad};
        camera.fitHi={hi.x+pad,hi.y+pad,hi.z+pad};
        camera.fitSelected=true;
    }
    void fitCamera(int index, bool selectionOnly) {
        auto &camera=cameras[std::clamp(index,0,3)];
        camera.zoom=1;
        camera.panX=camera.panY=0;
        if (selectionOnly) {
            refreshSelectionFit(camera,result.data,result.selected,true);
            if (!camera.fitSelected) {
                status="Select one or more particles before Fit selected";
                return;
            }
        } else camera.fitSelected=false;
    }
    void launchPipeline() {
        if (pipelineBusy) return;
        std::vector<Modifier> executable;
        executable.reserve(mods.size());
        pipelineJobNodeIds.clear();
        for (const auto &node : mods) {
            executable.push_back(static_cast<const Modifier &>(node));
            pipelineJobNodeIds.push_back(node.id);
        }
        const size_t checkpointTarget=mods.empty() ? SIZE_MAX :
            std::min(modifierGraph.selected,mods.size()-1);
        size_t firstNode=0;
        std::shared_ptr<const PipelineResult> cachedPrefix;
        if (pipelineCheckpoint && pipelineCheckpointNode<=checkpointTarget &&
            pipelineCheckpointNode<=executable.size()) {
            firstNode=pipelineCheckpointNode;
            cachedPrefix=pipelineCheckpoint;
        }
        Dataset input;
        if (!cachedPrefix) input=source;
        pipelineJobGeneration = pipelineGeneration;
        pipelineCancel = false;
        pipelineActiveNode = 0;
        pipelineBusy = true;
        error.clear();
        status = "Updating pipeline; previous evaluated result remains visible...";
        for (auto &node : mods) node.running = node.enabled;
        const int currentFrame = current;
        const auto inputPath = path;
        std::vector<Frame> frameSnapshot = frames;
        const auto readBudget = budget;
        pipelineJob = std::async(std::launch::async,
            [this, input = std::move(input), executable = std::move(executable),
             cachedPrefix = std::move(cachedPrefix), firstNode, checkpointTarget, currentFrame,
             inputPath, frameSnapshot = std::move(frameSnapshot), readBudget]() mutable {
                // Displacement vectors nodes read their reference configuration
                // from the raw data source at the requested frame; Freeze
                // property nodes re-evaluate their upstream chain at the
                // reference frame. Datasets are produced lazily, once per
                // node, inside this worker thread.
                std::map<size_t, Dataset> rawFrameReferences;
                std::map<size_t, Dataset> freezeReferences;
                std::function<const Dataset &(size_t)> referenceProvider;
                const bool hasDisplacement = std::any_of(executable.begin(), executable.end(),
                    [](const Modifier &m) { return m.op == Op::DisplacementVectors; });
                const bool hasFreeze = std::any_of(executable.begin(), executable.end(),
                    [](const Modifier &m) { return m.op == Op::FreezeProperty; });
                Dataset freezeSource;
                if (hasFreeze) freezeSource = source;
                if (hasDisplacement || hasFreeze)
                    referenceProvider = [&](size_t node) -> const Dataset & {
                        const Modifier &m = executable.at(node);
                        if (m.op == Op::FreezeProperty) {
                            const auto cached = freezeReferences.find(node);
                            if (cached != freezeReferences.end()) return cached->second;
                            std::vector<Modifier> prefix(executable.begin(),
                                                         executable.begin() + node);
                            const size_t prefixCount = freezeSource.atoms.size();
                            PipelineResult prefixInput{freezeSource,
                                                       std::vector<uint8_t>(prefixCount),
                                                       std::vector<uint8_t>(prefixCount, 1)};
                            // Nested cross-frame modifiers inside the prefix
                            // fall back to their default (uncached) behavior.
                            auto evaluated = evaluateFrom(std::move(prefixInput), prefix, 0,
                                                          &pipelineCancel);
                            return freezeReferences
                                .emplace(node, std::move(evaluated.data))
                                .first->second;
                        }
                        const int frame = resolveDisplacementReferenceFrame(
                            m, currentFrame, int(frameSnapshot.size()));
                        return rawFrameReferences
                            .emplace(node, io::read(inputPath, frameSnapshot.at(size_t(frame)),
                                                    readBudget, nullptr, &pipelineCancel))
                            .first->second;
                    };
                // Unwrap trajectories nodes chain their per-frame output
                // through a per-node accumulator owned by the app, keyed by
                // node id so two unwrap nodes never share state.
                ModifierFrameState frameState;
                if (std::any_of(executable.begin(), executable.end(), [](const Modifier &m) {
                        return m.op == Op::UnwrapTrajectories;
                    })) {
                    frameState.previousUnwrapped =
                        [this, currentFrame](size_t node) -> const std::vector<Vec3> * {
                            const auto found =
                                unwrapAccumulators.find(pipelineJobNodeIds.at(node));
                            if (found == unwrapAccumulators.end() ||
                                found->second.first != currentFrame - 1)
                                return nullptr;
                            return &found->second.second;
                        };
                    frameState.storeUnwrapped = [this, currentFrame](
                                                    size_t node, std::vector<Vec3> positions) {
                        unwrapAccumulators.insert_or_assign(pipelineJobNodeIds.at(node),
                                                            std::make_pair(currentFrame,
                                                                           std::move(positions)));
                    };
                }
                PipelineResult initial;
                if (cachedPrefix) initial=*cachedPrefix;
                else {
                    const size_t count=input.atoms.size();
                    initial={std::move(input),std::vector<uint8_t>(count),
                             std::vector<uint8_t>(count,1)};
                }
                std::optional<PipelineResult> checkpoint;
                auto saveCheckpoint=[&](size_t,const PipelineResult &state) {
                    if (pipelineResultWithinCacheBudget(state)) checkpoint=state;
                };
                auto evaluated=evaluateFrom(std::move(initial),executable,firstNode,
                                             &pipelineCancel,&pipelineActiveNode,
                                             checkpointTarget,saveCheckpoint,referenceProvider,
                                             currentFrame,frameState);
                return PipelineJobResult{std::move(evaluated),checkpointTarget,
                                         std::move(checkpoint)};
            });
    }
    void inspectPipelineNode(int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= int(mods.size()) || busy || indexing || pipelineBusy ||
            inspectorBusy || inspectorJob.valid())
            return;
        std::vector<Modifier> executable;
        executable.reserve(size_t(nodeIndex) + 1);
        for (int i = 0; i <= nodeIndex; ++i)
            executable.push_back(static_cast<const Modifier &>(mods[size_t(i)]));
        Dataset input = source;
        inspectorNode = nodeIndex;
        inspectorNodeId = mods[size_t(nodeIndex)].id;
        inspectorGeneration = pipelineGeneration;
        inspectorCancel = false;
        inspectorActiveNode = 0;
        inspectorBusy = true;
        inspectorResult.reset();
        status = "Evaluating selected pipeline node for data inspection...";
        const int currentFrame = current;
        const auto inputPath = path;
        std::vector<Frame> frameSnapshot = frames;
        const auto readBudget = budget;
        inspectorJob = std::async(std::launch::async,
            [this, input = std::move(input), executable = std::move(executable), currentFrame,
             inputPath, frameSnapshot = std::move(frameSnapshot), readBudget]() mutable {
                std::map<size_t, Dataset> rawFrameReferences;
                std::map<size_t, Dataset> freezeReferences;
                std::function<const Dataset &(size_t)> referenceProvider;
                const bool hasDisplacement = std::any_of(executable.begin(), executable.end(),
                    [](const Modifier &m) { return m.op == Op::DisplacementVectors; });
                const bool hasFreeze = std::any_of(executable.begin(), executable.end(),
                    [](const Modifier &m) { return m.op == Op::FreezeProperty; });
                Dataset freezeSource;
                if (hasFreeze) freezeSource = input;
                if (hasDisplacement || hasFreeze)
                    referenceProvider = [&](size_t node) -> const Dataset & {
                        const Modifier &m = executable.at(node);
                        if (m.op == Op::FreezeProperty) {
                            const auto cached = freezeReferences.find(node);
                            if (cached != freezeReferences.end()) return cached->second;
                            std::vector<Modifier> prefix(executable.begin(),
                                                         executable.begin() + node);
                            const size_t prefixCount = freezeSource.atoms.size();
                            PipelineResult prefixInput{freezeSource,
                                                       std::vector<uint8_t>(prefixCount),
                                                       std::vector<uint8_t>(prefixCount, 1)};
                            auto evaluated = evaluateFrom(std::move(prefixInput), prefix, 0,
                                                          &inspectorCancel);
                            return freezeReferences
                                .emplace(node, std::move(evaluated.data))
                                .first->second;
                        }
                        const int frame = resolveDisplacementReferenceFrame(
                            m, currentFrame, int(frameSnapshot.size()));
                        return rawFrameReferences
                            .emplace(node, io::read(inputPath, frameSnapshot.at(size_t(frame)),
                                                    readBudget, nullptr, &inspectorCancel))
                            .first->second;
                    };
                return evaluate(std::move(input), executable, &inspectorCancel,
                                &inspectorActiveNode, referenceProvider, currentFrame);
            });
    }
    void update(size_t dirtyFrom = SIZE_MAX, bool preserveColorRanges = false) {
        // Modifier output can reorder/replace bonds even with unchanged atom
        // count. Source-row selections must not refer to evaluated bond rows.
        if(!mods.empty())creationBondSelection.clear();
        // With no modifier graph there is no reusable prefix: source/history
        // edits must not revive the input cached before an old node zero.
        else {pipelineCheckpoint.reset();pipelineCheckpointNode=SIZE_MAX;}
        ++motionRevision;
        const size_t first = dirtyFrom == SIZE_MAX
            ? (mods.empty() ? 0 : std::min(modifierGraph.selected, mods.size() - 1))
            : dirtyFrom;
        if (pipelineCheckpoint && first < pipelineCheckpointNode) {
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
        }
        if (!preserveColorRanges) {
            for (size_t i = first; i < mods.size(); ++i)
                if (mods[i].op == Op::ColorCoding && mods[i].colorAllFramesRange && i > first)
                    mods[i].colorAllFramesRange = false;
        }
        modifierGraph.markDirtyFrom(first);
        ++pipelineGeneration;
        if (inspectorBusy) inspectorCancel = true;
        inspectorResult.reset();
        inspectorNode = -1;
        inspectorNodeId.clear();
        staleResult = true;
        if (pipelineBusy) {
            pipelineCancel = true;
            status = "Cancelling outdated pipeline evaluation...";
            return;
        }
        if (inspectorBusy) {
            pipelineDeferredForInspector = true;
            status = "Waiting for cancelled node inspection before updating the pipeline...";
            return;
        }
        launchPipeline();
    }
    ModifierNode makeNode(const Modifier &modifier) {
        ModifierNode node(modifier);
        node.id = std::to_string(nextModifierId++);
        node.displayName = opName(modifier.op);
        node.category = opCategory(modifier.op);
        return node;
    }
    void checkpoint() {
        if (!editCheckpoint.shouldRecord(ImGui::IsMouseDown(ImGuiMouseButton_Left)))
            return;
        if (undo.size() >= 128)
            undo.erase(undo.begin());
        undo.push_back(mods);
        redo.clear();
    }
    void add(Op op) {
        structureEditIsLatest = false;
        checkpoint();
        Modifier m{op};
        if (op == Op::Slice) {
            m.sliceNormal[0] = 1; m.sliceNormal[1] = 0; m.sliceNormal[2] = 0;
            const double center[3]{
                double(result.data.origin.x) +
                    (result.data.cell[0] + result.data.cell[3] + result.data.cell[6]) * .5,
                double(result.data.origin.y) +
                    (result.data.cell[1] + result.data.cell[4] + result.data.cell[7]) * .5,
                double(result.data.origin.z) +
                    (result.data.cell[2] + result.data.cell[5] + result.data.cell[8]) * .5};
            m.sliceDistance = m.sliceNormal[0] * center[0] + m.sliceNormal[1] * center[1] +
                              m.sliceNormal[2] * center[2];
        }
        if (op == Op::Scale) m.value = 1;
        if (op == Op::Replicate) {
            m.replicateN[0] = 1; m.replicateN[1] = 1; m.replicateN[2] = 1;
            m.replicateAdjustBox = true;
        }
        if (op == Op::Rotate) m.value = 90;
        if (op == Op::SelectRange) {
            m.value = result.data.lo.z; m.upper = result.data.hi.z;
        }
        if (op == Op::EditCell) {
            m.editedCell = result.data.cell;
            m.editedOrigin = result.data.origin;
            m.editedPbc = result.data.pbc;
        }
        if (op == Op::ColorType) { colorCoding = true; colorAxis = 0; colorAutoRange = true; }
        if (op == Op::ColorCoding) { colorCoding = true; colorAutoRange = true; }
        if (op == Op::CommonNeighborAnalysis || op == Op::CreateBonds ||
            op == Op::CoordinationAnalysis || op == Op::ClusterAnalysis ||
            op == Op::RadialDistribution || op == Op::SelectOverlapping) m.value = cutoff;
        if (op == Op::CreateBonds) {
            // Dense structures can produce millions of pairs at 3.2 A. Require
            // an explicit physical cutoff instead of starting a costly job.
            m.value = result.data.atoms.size() > 10000 ? 0.f : 3.2f;
            if (result.data.atoms.size() > 10000) m.bondCylinders = false;
        }
        if (op == Op::Histogram) { m.type = 64; m.property = "Position.X"; }
        if (op == Op::BondLengthDistribution) m.type = 64;
        if (op == Op::BondAngleDistribution) m.type = 90;
        if (op == Op::ReduceProperty) m.property = "Position.X";
        if (op == Op::SelectOverlapping) m.property = "Radius";
        if (op == Op::ExpandSelection) {
            m.value = 3.2f; // OVITO default cutoff distance
            m.type = 1;
            m.expandNeighbors = 10;
        }
        if (op == Op::SelectType) m.selectedTypes = {0};
        if (op == Op::ExpressionSelect) m.property = "Position.X > 0";
        if (op == Op::ComputeProperty) m.property = "x*x + y*y + z*z";
        modifierGraph.insert(makeNode(m));
        update();
        status = op == Op::CreateBonds && result.data.atoms.size() > 10000
            ? "Large structure: set a bond cutoff explicitly; fast lines are the default"
            : std::string("Added ") + opName(op);
    }
    // Display-only synchronous preview of the simulation cell entering pipeline
    // node `nodeIndex`: applies the cell-affecting modifiers upstream without
    // touching particles. Used by the affine panel's read-only cell readout.
    std::pair<std::array<double, 9>, Vec3> pipelineCellBefore(size_t nodeIndex) const {
        std::array<double, 9> cellVectors = source.cell;
        Vec3 origin = source.origin;
        const size_t end = std::min(nodeIndex, mods.size());
        for (size_t i = 0; i < end; ++i) {
            const auto &mod = mods[i];
            if (!mod.enabled) continue;
            if (mod.op == Op::EditCell) {
                cellVectors = mod.editedCell;
                origin = mod.editedOrigin;
            } else if (mod.op == Op::AffineTransform) {
                std::array<double, 9> next{};
                for (int row = 0; row < 3; ++row)
                    for (int component = 0; component < 3; ++component)
                        next[row * 3 + component] =
                            mod.affineTransform[row * 4] * cellVectors[component] +
                            mod.affineTransform[row * 4 + 1] * cellVectors[3 + component] +
                            mod.affineTransform[row * 4 + 2] * cellVectors[6 + component];
                cellVectors = next;
                origin = {float(mod.affineTransform[0] * origin.x + mod.affineTransform[1] * origin.y +
                                mod.affineTransform[2] * origin.z + mod.affineTransform[3]),
                          float(mod.affineTransform[4] * origin.x + mod.affineTransform[5] * origin.y +
                                mod.affineTransform[6] * origin.z + mod.affineTransform[7]),
                          float(mod.affineTransform[8] * origin.x + mod.affineTransform[9] * origin.y +
                                mod.affineTransform[10] * origin.z + mod.affineTransform[11])};
            } else if (mod.op == Op::Scale) {
                for (auto &v : cellVectors) v *= mod.value;
                origin = {float(origin.x * mod.value), float(origin.y * mod.value),
                          float(origin.z * mod.value)};
            } else if (mod.op == Op::Rotate) {
                const int u = (mod.axis + 1) % 3, v = (mod.axis + 2) % 3;
                const double angle = mod.value * 0.0174532925199433;
                for (int row = 0; row < 3; ++row) {
                    const double x = cellVectors[row * 3 + u], y = cellVectors[row * 3 + v];
                    cellVectors[row * 3 + u] = x * std::cos(angle) - y * std::sin(angle);
                    cellVectors[row * 3 + v] = x * std::sin(angle) + y * std::cos(angle);
                }
            } else if (mod.op == Op::Replicate && mod.replicateAdjustBox) {
                for (int axis = 0; axis < 3; ++axis)
                    for (int component = 0; component < 3; ++component)
                        cellVectors[axis * 3 + component] *= mod.replicateN[axis];
            }
        }
        return {cellVectors, origin};
    }
    bool colorPropertyCombo(const char *label, std::string &property,
                            bool includeVectorComponents = false) {
        const auto choices = pipelineInputPropertyChoices(
            source, mods, std::min(modifierGraph.selected, mods.size()), includeVectorComponents);
        bool changed = false;
        if (ImGui::BeginCombo(label, property.c_str())) {
            for (const auto &name : choices) {
                bool selected = property == name;
                if (ImGui::Selectable(name.c_str(), selected)) {
                    property = name;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    bool structureEditIsLatest = false;
    std::string tabTitle() const {
        if (homeMode) return "新标签页";
        if (activeTab >= 0 && activeTab < int(tabs.size()) && !tabs[size_t(activeTab)].title.empty() && tabs[size_t(activeTab)].title!="Structure")
            return tabs[size_t(activeTab)].title;
        if (!path.empty()) return utf8(path.filename().wstring());
        if (!source.comment.empty()) {
            std::string text = source.comment;
            if (text.size() > 28) text.resize(28);
            return text;
        }
        return "Untitled structure";
    }
    StructureTab captureTab() const {
        StructureTab tab;
        if (activeTab >= 0 && activeTab < int(tabs.size())) {
            tab.id = tabs[size_t(activeTab)].id;
            tab.sourceTabId = tabs[size_t(activeTab)].sourceTabId;
            tab.documentPath = tabs[size_t(activeTab)].documentPath;
        }
        tab.title = tabTitle();
        tab.home = homeMode;
        tab.basedOn = creationBasedOn;
        tab.source = source;
        tab.graph = modifierGraph;
        tab.undo = undo;
        tab.redo = redo;
        tab.path = path;
        tab.frames = frames;
        tab.current = current;
        tab.readerName = readerName;
        for (int i = 0; i < 4; ++i) tab.cameras[i] = cameras[i];
        tab.styles = gpu.styles;
        tab.radius = radius;
        tab.particleShape = particleShape;
        tab.viewportTool = viewportTool;
        tab.cell = cell;
        tab.particles = particles;
        tab.quad = quad;
        tab.creationMode = creationMode;
        tab.creationSketch = creationSketch;
        tab.sketchContinuous = creationSketchContinuous; tab.sketchOrder = creationSketchOrder;
        tab.autoHydrogens=creationAutoHydrogens;
        tab.ringSize=creationRingSize;
        tab.fragmentKey=creationFragmentKey; tab.fragmentConnector=creationFragmentConnector;
        tab.creationTool = creationTool;
        tab.selection = creationSelection; tab.bondSelection=creationBondSelection;
        tab.picked = creationPick;
        tab.measure = creationMeasure;
        tab.angleAtom = creationAngle;
        tab.dihedralAtom = creationDihedral;
        tab.authorUndo = authorUndo;
        tab.authorRedo = authorRedo;
        tab.display = creationDisplay;
        tab.snapshots = creationSnapshots;
        tab.selectedSnapshot = creationSnapshotSelected;
        tab.propertyPage = creationPropertyPage;tab.monitorFilter=creationMonitorFilter;tab.propertyFilter=creationPropertyFilter;
        tab.propertiesOpen = creationPropertiesOpen;
        tab.symmetryInfo = creationSymmetry;
        tab.symmetryChecked = creationSymmetryChecked;
        snprintf(tab.element, sizeof(tab.element), "%s", creationElement);
        return tab;
    }
    void restoreTab(const StructureTab &tab) {
        showMotionGroups=false; showCreationStyles=false; motionGroupSelected=0; motionCacheRevision=UINT64_MAX;
        homeMode = tab.home;
        creationBasedOn = tab.basedOn;
        source = tab.source;
        modifierGraph = tab.graph;
        undo = tab.undo;
        redo = tab.redo;
        path = tab.path;
        frames = tab.frames;
        current = tab.current;
        readerName = tab.readerName;
        for (int i = 0; i < 4; ++i) cameras[i] = tab.cameras[i];
        radius = tab.radius;
        particleShape = tab.particleShape;
        viewportTool = tab.viewportTool;
        cell = tab.cell;
        particles = tab.particles;
        quad = tab.quad;
        creationMode = tab.creationMode;
        creationSketch = tab.creationSketch;
        creationSketchContinuous=tab.sketchContinuous; creationSketchOrder=tab.sketchOrder;
        creationAutoHydrogens=tab.autoHydrogens; creationAutoHydrogenMessage.clear();
        creationSketchInsert=false;creationInsertHover=-1;creationInsertMouse={-1,-1};
        creationRingSize=tab.ringSize; creationRingPreviewValid=false;
        creationFragmentKey=tab.fragmentKey; creationFragmentConnector=tab.fragmentConnector;
        creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion(); showFragmentBrowser=false;
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        creationTool = tab.creationTool;
        creationSelection = tab.selection; creationBondSelection=tab.bondSelection;
        creationPick = creationSelection.empty() ? tab.picked : creationSelection.back();
        creationHover = -1;
        creationLastHoverMouse={-1,-1};
        creationDrag = CreationDrag::None;
        geometryPending.clear(); geometryPanelMonitor=-1;
        geometryDragPlans[0].reset(); geometryDragPlans[1].reset();
        creationMeasure = tab.measure;
        creationAngle = tab.angleAtom;
        creationDihedral = tab.dihedralAtom;
        authorUndo = tab.authorUndo;
        authorRedo = tab.authorRedo;
        creationDisplay = tab.display;
        creationDisplay.normalize(source.atoms.size()); creationDisplay.normalizeBonds(source);
        creationSnapshots = tab.snapshots;
        creationSnapshotSelected = tab.selectedSnapshot;
        creationPropertyPage = tab.propertyPage;creationMonitorFilter=tab.monitorFilter;
        snprintf(creationPropertyFilter,sizeof(creationPropertyFilter),"%s",tab.propertyFilter.c_str());
        creationPropertiesOpen = tab.propertiesOpen;
        creationSymmetry = tab.symmetryInfo;
        creationSymmetryChecked = tab.symmetryChecked;
        snprintf(creationElement, sizeof(creationElement), "%s", tab.element);
        pipelineCheckpoint.reset();
        pipelineCheckpointNode = SIZE_MAX;
        unwrapAccumulators.clear();
        structureEditIsLatest = !authorUndo.empty();
        appearanceMemory.clear();
        appearanceNames = source.species;
        gpu.resetStyles(source.species.size(), &source.species);
        for (size_t i = 0; i < tab.styles.size() && i < gpu.styles.size(); ++i)
            gpu.styles[i] = tab.styles[i];
        for (size_t i = 0; i < appearanceNames.size() && i < gpu.styles.size(); ++i)
            appearanceMemory[appearanceNames[i]] = gpu.styles[i];
        queueCreationLabelFont();
        update();
    }
    bool documentsBusy() const { return busy || pipelineBusy || indexing || motionGroupBusy || layerBuildBusy || creationColorBusy || hydrogenBusy || projectBusy; }
    void switchTab(int index) {
        if (index < 0 || index >= int(tabs.size()) || index == activeTab) return;
        if (documentsBusy()) {
            pendingTabId=tabs[size_t(index)].id;
            status = "Switching tabs after the current operation finishes";
            return;
        }
        pendingTabId=0;
        tabs[activeTab] = captureTab();
        activeTab = index;
        restoreTab(tabs[activeTab]);
        status = "Switched to " + tabs[activeTab].title;
    }
    void newStructureTab(Dataset data, const std::string &title) {
        if (documentsBusy()) {
            status = "Wait for the current load or pipeline to finish before opening a tab";
            return;
        }
        const bool replaceHome = homeMode && !tabs.empty();
        if (!tabs.empty()) tabs[activeTab] = captureTab();
        StructureTab tab;
        tab.id = replaceHome ? tabs[size_t(activeTab)].id : nextTabId++;
        tab.source = std::move(data);
        tab.title = title;
        tab.readerName = "Authored structure";
        tab.graph = {};
        snprintf(tab.element, sizeof(tab.element), "C");
        if (replaceHome) tabs[size_t(activeTab)] = std::move(tab);
        else { tabs.push_back(std::move(tab)); activeTab = int(tabs.size()) - 1; }
        restoreTab(tabs[activeTab]);
        status = "New tab: " + title;
    }
    void newBlankCreation() {
        if(documentsBusy())return;
        auto empty=authoring::orthogonalCell(10,10,10,"C");empty.atoms.clear();empty.sourceCount=0;empty.bounds();
        newStructureTab(std::move(empty),"未命名 · 创作");creationMode=true;
        chooseCreationTool(CreationTool::Sketch);saveCreationSnapshot("空白结构");
    }
    void newHomeTab() {
        if (documentsBusy()) { status = "Wait for the current operation before opening a tab"; return; }
        if (!tabs.empty()) tabs[size_t(activeTab)] = captureTab();
        StructureTab tab;
        tab.id = nextTabId++;
        tab.home = true;
        tab.title = "新标签页";
        tabs.push_back(std::move(tab));
        activeTab = int(tabs.size()) - 1;
        restoreTab(tabs[size_t(activeTab)]);
    }
    void saveCreationSnapshot(std::string name = {}) {
        if (!creationMode) return;
        if (creationSnapshots.size()>=24) {
            status="工作区最多暂存 24 个结果；请删除不需要的快照";
            return;
        }
        if (name.empty()) name="快照 " + std::to_string(creationSnapshots.size()+1);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char time[16];
        snprintf(time,sizeof(time),"%02u:%02u",now.wHour,now.wMinute);
        creationSnapshots.push_back({std::make_shared<const Dataset>(source),std::move(name),time,creationDisplay});
        creationSnapshotSelected=int(creationSnapshots.size())-1;
        status="已暂存当前结构到工作区";
    }
    void restoreCreationSnapshot(int index) {
        if (index<0||index>=int(creationSnapshots.size()) || !creationSnapshots[size_t(index)].data)
            return;
        const auto snapshot=creationSnapshots[size_t(index)];
        adoptStructure(*snapshot.data,"已载入工作区快照："+snapshot.name);
        creationDisplay=snapshot.display;
        creationDisplay.normalize(source.atoms.size()); creationDisplay.normalizeBonds(source);
        queueCreationLabelFont();
        creationSnapshotSelected=index;
    }
    void configureCrystalPreset(int preset) {
        crystalPreset=preset;
        crystalBasis.clear();
        auto add=[&](const char *element,float x,float y,float z) {
            CrystalBasis atom;
            snprintf(atom.element,sizeof(atom.element),"%s",element);
            atom.fractional[0]=x; atom.fractional[1]=y; atom.fractional[2]=z;
            crystalBasis.push_back(atom);
        };
        crystalAlpha=crystalBeta=crystalGamma=90.f;
        if (preset==0) { crystalA=3.615f; add("Cu",0,0,0); }
        else if (preset==1) { crystalA=5.64f; add("Na",0,0,0); add("Cl",.5f,.5f,.5f); }
        else if (preset==2) { crystalA=5.431f; add("Si",0,0,0); add("Si",.25f,.25f,.25f); }
        else if (preset==3) { crystalA=2.866f; add("Fe",0,0,0); }
        else if (preset==4) { crystalA=crystalB=3.209f; crystalC=5.211f;
            crystalGamma=120.f; add("Mg",1.f/3,2.f/3,.25f); add("Mg",2.f/3,1.f/3,.75f); }
        else if (preset==5) { crystalA=crystalB=crystalC=10.f; add("C",0,0,0); }
        else { crystalA=crystalB=crystalC=5.64f; crystalSpaceGroup=225;
            add("Na",0,0,0); add("Cl",.5f,.5f,.5f); }
        if (preset<=3) crystalB=crystalC=crystalA;
    }
    Dataset crystalFromDialog() {
        if (!(crystalA>0&&crystalB>0&&crystalC>0) || crystalBasis.empty())
            throw std::runtime_error("晶格长度和原子基元必须有效");
        Dataset data=authoring::triclinicCell(crystalA,crystalB,crystalC,
            crystalAlpha,crystalBeta,crystalGamma,"C");
        data.atoms.clear(); data.species.clear();
        if (crystalPreset==6) {
            std::vector<symmetry::Site> sites;
            sites.reserve(crystalBasis.size());
            for (const auto &basis:crystalBasis)
                sites.push_back({basis.element,{basis.fractional[0],basis.fractional[1],
                    basis.fractional[2]}});
            auto expanded=symmetry::expandAsymmetricUnit(data,sites,crystalSpaceGroup);
            if (!expanded)
                throw std::runtime_error("空间群与晶胞参数不相容，或原子基元无效");
            return std::move(*expanded);
        }
        const std::array<std::array<float,3>,4> f{{{{0,0,0}},{{0,.5f,.5f}},{{.5f,0,.5f}},{{.5f,.5f,0}}}};
        const size_t copies=crystalPreset<=2?4:crystalPreset==3?2:1;
        for (const auto &basis:crystalBasis) {
            const auto type=authoring::speciesIndex(data,basis.element);
            for (size_t copy=0;copy<copies;++copy) {
                const std::array<float,3> offset=crystalPreset==3 && copy==1
                    ? std::array<float,3>{.5f,.5f,.5f} : f[copy];
                const auto wrap=[](float v){ v=std::fmod(v,1.f); return v<0?v+1.f:v; };
                const auto p=authoring::cartesian(data,
                    wrap(basis.fractional[0]+offset[0]),
                    wrap(basis.fractional[1]+offset[1]),
                    wrap(basis.fractional[2]+offset[2]));
                data.atoms.push_back({p.x,p.y,p.z,type});
            }
        }
        authoring::finish(data,"Built crystal");
        return data;
    }
    void openFileTab(const std::filesystem::path &file) {
        if(lowerExtension(file)==".atomx-project"){loadProject(file);return;}
        if (file.empty()) return;
        if (documentsBusy()) { status = "Wait for the current operation before opening a file"; return; }
        if (!tabs.empty()) tabs[size_t(activeTab)] = captureTab();
        StructureTab tab;
        tab.id = homeMode && !tabs.empty() ? tabs[size_t(activeTab)].id : nextTabId++;
        tab.title = utf8(file.filename().wstring());
        if (homeMode && !tabs.empty()) tabs[size_t(activeTab)] = std::move(tab);
        else { tabs.push_back(std::move(tab)); activeTab = int(tabs.size()) - 1; }
        restoreTab(tabs[size_t(activeTab)]);
        load(file);
    }
    void openCreationTab() {
        if (documentsBusy()) {
            status = "Wait for the current operation before opening Creation Mode";
            return;
        }
        if (source.sampled() || result.data.sampled()) {
            status = "Cannot edit a sampled preview; load the complete structure first";
            return;
        }
        const int origin = activeTab;
        const uint64_t originId = tabs[size_t(origin)].id;
        const Camera viewCamera = cameras[active];
        Dataset editable = result.data.atoms.empty() ? source : result.data;
        editable.sourceCount = editable.atoms.size();
        // Creation view previews close atomic neighbours as bonds. This is a
        // visual default on the copied structure, never a pipeline modifier
        // on the source document. Existing explicit topology wins.
        if (editable.bonds.empty() && editable.atoms.size() <= 5000) {
            std::vector<float> radii;
            radii.reserve(editable.species.size());
            float largest = 0.f;
            for (const auto &name : editable.species) {
                const auto *element = elements::find(name);
                const float value = element ? element->covalent : .8f;
                radii.push_back(value);
                largest = std::max(largest, value);
            }
            if (largest > 0) {
                try {
                    const float maxDistance = largest * 2.4f;
                    forEachNeighborPair(editable, maxDistance,
                        [&](uint32_t first, uint32_t second, double distanceSquared,
                            std::array<int32_t, 3> image) {
                            const auto a = editable.atoms[first].type;
                            const auto b = editable.atoms[second].type;
                            if (a >= radii.size() || b >= radii.size()) return;
                            const double threshold = 1.2 * (radii[a] + radii[b]);
                            if (image == std::array<int32_t, 3>{0,0,0} &&
                                distanceSquared <= threshold * threshold)
                                editable.bonds.push_back({first, second, image});
                        });
                } catch (const std::exception &) {
                    editable.bonds.clear();
                }
            }
        }
        editable.bondStyle.radius = .12f;
        editable.bondStyle.colorByType = true;
        editable.bondStyle.showPeriodicImages = false;
        const std::string basedOn = path.empty() ? tabTitle() : utf8(path.filename().wstring());
        const std::string title = tabTitle() + " · 创作";
        newStructureTab(std::move(editable), title);
        if (activeTab == origin) return;
        creationReturnTab = origin;
        tabs[size_t(activeTab)].sourceTabId = originId;
        tabs[size_t(activeTab)].basedOn = basedOn;
        creationBasedOn = basedOn;
        cameras[3] = viewCamera;
        cameras[3].mode = 7;
        active = 3;
        creationMode = true;
        creationSketch = false;
        creationTool = CreationTool::Select;
        creationSelection.clear(); creationBondSelection.clear();
        creationSnapshots.clear();
        saveCreationSnapshot("原始 · " + basedOn);
        status = "Creation Mode: " + title;
    }
    void leaveCreationTab() {
        const uint64_t sourceId = activeTab >= 0 && activeTab < int(tabs.size())
            ? tabs[size_t(activeTab)].sourceTabId : 0;
        for (int index = 0; index < int(tabs.size()); ++index)
            if (sourceId && tabs[size_t(index)].id == sourceId) { switchTab(index); return; }
        newHomeTab();
    }
    void closeTab(int index) {
        if (index < 0 || index >= int(tabs.size())) return;
        if (documentsBusy()) {
            status = "Wait for the current load or pipeline to finish before closing a tab";
            return;
        }
        if (tabs.size() == 1) {
            tabs.clear();
            activeTab = 0;
            newHomeTab();
            return;
        }
        if (index == activeTab) {
            tabs[activeTab] = captureTab();
            int next = index + 1 < int(tabs.size()) ? index + 1 : index - 1;
            StructureTab keep = tabs[next];
            tabs.erase(tabs.begin() + index);
            activeTab = next > index ? next - 1 : next;
            restoreTab(tabs[activeTab]);
            (void)keep;
        } else {
            tabs.erase(tabs.begin() + index);
            if (index < activeTab) --activeTab;
        }
        status = "Closed structure tab";
    }
    bool sameAtomCount() const { return source.atoms.size() == result.data.atoms.size(); }
    void rememberStructure(const std::string &action = {}) {
        if (authorUndo.size() >= 64) authorUndo.erase(authorUndo.begin());
        authorUndo.push_back({source,creationDisplay,action});
        authorRedo.clear();
        structureEditIsLatest = true;
    }
    void adoptStructure(Dataset data, const std::string &message) {
        if(hydrogenBusy) return;
        hydrogenPlan.reset();
        chooseCreationTool(creationTool);
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        rememberStructure(message);
        creationDisplay={};
        creationSymmetry.reset(); creationSymmetryChecked=false;
        creationSnapshotSelected=-1;
        source = std::move(data);
        frames.clear();
        current = 0;
        pendingFrame = -1;
        mods.clear();
        modifierGraph.selected = 0;
        creationPick = creationMeasure = creationAngle = creationDihedral = -1;
        creationSelection.clear(); creationBondSelection.clear();
        syncAppearance(source.species);
        update();
        status = message;
    }
    void editStructure(const std::string &message, const std::function<void(Dataset &)> &edit) {
        if(hydrogenBusy) return;
        hydrogenPlan.reset();
        if (!sameAtomCount() && !mods.empty()) {
            status = "Clear modifiers first: the pipeline changed the atom count";
            return;
        }
        geometryPanelMonitor=-1; geometryPending.clear();
        rememberStructure(message);
        creationAutoHydrogenMessage.clear();
        creationRingPreviewValid=false;
        creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion();
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        creationSymmetry.reset(); creationSymmetryChecked=false;
        creationSnapshotSelected=-1;
        creationBondSelection.clear();
        edit(source);
        creationDisplay.normalize(source.atoms.size()); creationDisplay.normalizeBonds(source);
        source.sourceCount = source.atoms.size();
        source.bounds();
        if (!mods.empty()) {
            mods.clear();
            modifierGraph.selected = 0;
        }
        syncAppearance(source.species);
        update();
        status = message;
        if(!creationAutoHydrogenMessage.empty())status+=" · "+creationAutoHydrogenMessage;
    }
    // Run only at a committed chemical edit, never during hover or dragging.
    // Hydrogen compaction also remaps the touched atoms and UI indices.
    void updateAutomaticHydrogens(Dataset &data,std::vector<int> &affected) {
        if(!creationMode || !creationAutoHydrogens || affected.empty())return;
        if(data.atoms.size()>100000 || data.bonds.size()>interactiveBondBudget) {
            creationAutoHydrogenMessage="自动氢未执行：大体系请使用 H+ 后台预览"; return;
        }
        try {
            creationDisplay.normalize(data.atoms.size());
            hydrogens::Options options; options.all=false;options.selection=affected;
            auto hidden=creationDisplay.hidden;
            for(size_t i=0;i<data.atoms.size();++i) {
                const auto type=data.atoms[i].type;
                if(!particles || (type<gpu.styles.size() && gpu.styles[type].visual[2]<=0)) {
                    if(hidden.empty())hidden.resize(data.atoms.size());hidden[i]=1;
                }
            }
            const auto plan=hydrogens::prepare(data,options,hidden);
            if(plan.changes()) {
                hydrogens::apply(data,plan);
                if(!plan.removed.empty())creationDisplay.eraseAtoms(plan.atomCount,plan.removed);
                auto remap=[&](std::vector<int> &indices) {
                    for(auto &i:indices)i=hydrogens::remapIndex(i,plan.removed);
                    indices.erase(std::remove(indices.begin(),indices.end(),-1),indices.end());
                };
                remap(affected);remap(creationSelection);
                for(int *index:{&creationPick,&creationMeasure,&creationAngle,&creationDihedral})
                    *index=hydrogens::remapIndex(*index,plan.removed);
                creationAutoHydrogenMessage="自动氢：新增 "+std::to_string(plan.added.size())+" / 删除 "+std::to_string(plan.removed.size());
            }
            const size_t skipped=plan.unsupported+plan.ambiguous+plan.invalid+plan.periodic+plan.hidden;
            if(skipped) {
                if(!creationAutoHydrogenMessage.empty())creationAutoHydrogenMessage+=" · ";
                creationAutoHydrogenMessage+="跳过 "+std::to_string(skipped)+" 个不可自动调整的位点";
            }
        } catch(const std::exception &e) { creationAutoHydrogenMessage=std::string("自动氢未执行：")+e.what(); }
    }
    static UINT structureClipboardFormat() {
        static const UINT format=RegisterClipboardFormatW(L"AtomX.Structure.v1");return format;
    }
    std::vector<int> clipboardRows() const {
        auto rows=creationSelection;
        for(int row:creationBondSelection)if(row>=0 && size_t(row)<source.bonds.size()) {
            const auto &b=source.bonds[size_t(row)];rows.push_back(int(b.a));rows.push_back(int(b.b));
        }
        rows.erase(std::remove_if(rows.begin(),rows.end(),[&](int row){return !creationAtomVisible(row);}),rows.end());
        std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());return rows;
    }
    void copyCreationSelection(bool cut=false) {
        if(!creationMode || documentsBusy() || creationDrag!=CreationDrag::None)return;
        try {
            if(!mods.empty())throw std::runtime_error("Clear creation modifiers before copying");
            auto copied=clipboard::copy(source,creationDisplay,clipboardRows());
            const auto bytes=clipboard::encode(copied.content);const uint64_t length=bytes.size();
            HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,sizeof(length)+bytes.size());
            if(!memory)throw std::runtime_error("Clipboard allocation failed");
            auto *data=static_cast<char *>(GlobalLock(memory));
            if(!data){GlobalFree(memory);throw std::runtime_error("Clipboard allocation failed");}
            memcpy(data,&length,sizeof(length));memcpy(data+sizeof(length),bytes.data(),bytes.size());GlobalUnlock(memory);
            if(!OpenClipboard(window)){GlobalFree(memory);throw std::runtime_error("Clipboard is in use; try again");}
            const UINT format=structureClipboardFormat();
            const bool accepted=format && EmptyClipboard() && SetClipboardData(format,memory);
            CloseClipboard();if(!accepted){GlobalFree(memory);throw std::runtime_error("Cannot publish structure clipboard");}
            if(cut) {
                const auto count=source.atoms.size();
                editStructure("剪切 "+std::to_string(copied.rows.size())+" 个原子",[&](Dataset &d){
                    creationDisplay.eraseAtoms(count,copied.rows);authoring::eraseAtoms(d,copied.rows);
                    creationSelection.clear();creationPick=creationMeasure=creationAngle=creationDihedral=-1;
                });
            } else status="已复制 "+std::to_string(copied.rows.size())+" 个原子（保留坐标与内部键）";
        }catch(const std::exception &e){status=e.what();}
    }
    void pasteCreationContent(const document::Content &content) {
        if(!creationMode || documentsBusy() || creationDrag!=CreationDrag::None)return;
        if(!mods.empty())throw std::runtime_error("Clear creation modifiers before pasting");
        auto prepared=clipboard::prepare(source,creationDisplay,content);
        editStructure("粘贴 "+std::to_string(prepared.rows.size())+" 个原子",[&](Dataset &d){
            d=std::move(prepared.data);creationDisplay=std::move(prepared.display);
            creationSelection=std::move(prepared.rows);creationPick=creationSelection.back();
            creationMeasure=creationAngle=creationDihedral=-1;
        });queueCreationLabelFont();
    }
    void pasteCreationSelection() {
        try {
            if(!OpenClipboard(window))throw std::runtime_error("Clipboard is in use; try again");
            std::string bytes;std::string error;
            const auto memory=GetClipboardData(structureClipboardFormat());
            const auto size=memory?GlobalSize(memory):0;
            if(size<sizeof(uint64_t))error="剪贴板没有 AtomX 体系";
            else if(const auto *data=static_cast<const char *>(GlobalLock(memory))) {
                uint64_t length=0;memcpy(&length,data,sizeof(length));
                if(length>clipboard::byteLimit || length>size-sizeof(length))error="Invalid structure clipboard size";
                else bytes.assign(data+sizeof(length),size_t(length));
                GlobalUnlock(memory);
            }else error="Cannot read structure clipboard";
            CloseClipboard();if(!error.empty())throw std::runtime_error(error);
            pasteCreationContent(clipboard::decode(bytes));
        }catch(const std::exception &e){status=e.what();}
    }
    void centerCreationSelection() {
        auto rows=clipboardRows();if(rows.empty()){status="请先选中原子或键";return;}
        auto &cam=cameras[3];const auto projected=creationProjection(result.data,cam,creationViewportSize);
        Vec3 center{};for(int row:rows)center=authoring::add(center,Vec3{result.data.atoms[size_t(row)].x,result.data.atoms[size_t(row)].y,result.data.atoms[size_t(row)].z});
        center=authoring::scale(center,1./rows.size());
        DirectX::XMFLOAT3 point;DirectX::XMStoreFloat3(&point,DirectX::XMVector3TransformCoord(
            DirectX::XMVectorSet(center.x,center.y,center.z,1),projected.view));
        // Derive the pan scale from two matrices, including cell/preset bounds.
        auto translated=cam;translated.panX+=1;
        const auto second=creationProjection(result.data,translated,creationViewportSize);
        DirectX::XMFLOAT3 shifted;DirectX::XMStoreFloat3(&shifted,DirectX::XMVector3TransformCoord(
            DirectX::XMVectorSet(center.x,center.y,center.z,1),second.view));
        const float span=shifted.x-point.x;
        if(std::isfinite(span) && span>0){cam.panX-=point.x/span;cam.panY-=point.y/span;status="选中对象已居中";}
    }
    void setCreationView(int mode) {
        if(mode<0 || mode>7)return;
        chooseCreationTool(creationTool);cameras[3].mode=mode;cameras[3].roll=0;
        fitCamera(3,false);status="标准视角";
    }
    void deletePickedAtom() {
        std::vector<int> selected=creationSelection;
        if (selected.empty() && creationPick>=0) selected.push_back(creationPick);
        selected.erase(std::remove_if(selected.begin(),selected.end(),[&](int index) {
            return index<0||size_t(index)>=source.atoms.size();
        }),selected.end());
        if(selected.empty()) {
            if(!creationBondSelection.empty())editSelectedCreationBonds(0);
            else status="请先选中原子或键";
            return;
        }
        const auto selectedBonds=creationBondSelection;
        std::sort(selected.begin(),selected.end());
        selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
        editStructure("删除 " + std::to_string(selected.size()) + " 个原子", [&](Dataset &data) {
            std::vector<int> affected;
            if(creationAutoHydrogens && data.atoms.size()<=100000 && data.bonds.size()<=interactiveBondBudget) {
                std::vector<uint8_t> removed(data.atoms.size());for(int i:selected)removed[size_t(i)]=1;
                for(const auto &b:data.bonds)if(b.a<removed.size() && b.b<removed.size()) {
                    if(removed[b.a] && !removed[b.b])affected.push_back(hydrogens::remapIndex(int(b.b),selected));
                    if(removed[b.b] && !removed[b.a])affected.push_back(hydrogens::remapIndex(int(b.a),selected));
                }
            }
            auto disconnected=creation::editBonds(data,selectedBonds,0);
            for(int atom:disconnected) {
                const int remapped=hydrogens::remapIndex(atom,selected);
                if(remapped>=0)affected.push_back(remapped);
            }
            creationDisplay.eraseAtoms(data.atoms.size(),selected);
            authoring::eraseAtoms(data,selected);
            creationSelection.clear();creationPick=creationMeasure=creationAngle=creationDihedral=-1;
            updateAutomaticHydrogens(data,affected);
        });
        creationSelection.clear();
        creationPick = creationMeasure = creationAngle = creationDihedral = -1;
    }
    void refreshAtomProperty() {
        const auto s=properties::summarize(source,atomPropertyName,atomPropertySelection);
        atomPropertyValue=s.common.value_or(0);atomPropertyMissing=false;atomPropertyMessage.clear();
        std::ostringstream value;value<<std::setprecision(12)<<atomPropertyValue;
        atomPropertySummary=s.common?"当前值："+value.str():"当前值：混合 / 缺失";
        atomPropertySummary+=" · 有值 "+std::to_string(s.present)+" · 缺失 "+std::to_string(s.missing);
    }
    void requestAtomProperties() {
        if(!creationMode || documentsBusy() || !sameAtomCount() || creationSelection.empty())return;
        try {
            atomPropertySelection=properties::selected(source,creationSelection);
            atomPropertyTabId=tabs[size_t(activeTab)].id;atomPropertyGeneration=pipelineGeneration;
            atomPropertyNames.clear();
            if(!source.vectorProperties.contains("Charge"))atomPropertyNames.push_back("Charge");
            for(const auto &[name,rows]:source.scalarProperties)
                if(name!="Charge" && properties::editable(name) && rows.size()==source.atoms.size() && !source.vectorProperties.contains(name))
                    atomPropertyNames.push_back(name);
            if(atomPropertyNames.empty()){status="没有可编辑的数值属性";return;}
            atomPropertyName=atomPropertyNames.front();refreshAtomProperty();openAtomProperties=true;
        }catch(const std::exception &e){status=e.what();}
    }
    bool validAtomProperties() const {
        return creationMode && activeTab>=0 && activeTab<int(tabs.size()) && tabs[size_t(activeTab)].id==atomPropertyTabId &&
            pipelineGeneration==atomPropertyGeneration && !documentsBusy();
    }
    bool applyAtomProperties() {
        if(!validAtomProperties()){atomPropertyMessage="体系已改变，请关闭后重新选择原子";return false;}
        try {
            const auto edit=properties::prepare(source,atomPropertyName,atomPropertySelection,
                atomPropertyMissing?std::nullopt:std::optional<double>(atomPropertyValue));
            if(!edit.changed){status="属性没有变化";return true;}
            const auto bonds=creationBondSelection;
            editStructure("修改 "+edit.name+" · "+std::to_string(edit.selection.size())+" 个原子",
                [&](Dataset &data){properties::apply(data,edit);});
            creationSelection=edit.selection;creationPick=creationSelection.back();creationBondSelection=bonds;return true;
        }catch(const std::exception &e){atomPropertyMessage=e.what();return false;}
    }
    void atomPropertiesDialog() {
        if(openAtomProperties){ImGui::OpenPopup("原子数值属性##atom-properties");openAtomProperties=false;}
        const auto *vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(),ImGuiCond_Appearing,{.5f,.5f});
        ImGui::SetNextWindowSizeConstraints({U(440),0},{std::max(U(440),vp->WorkSize.x-U(24)),vp->WorkSize.y-U(24)});
        if(!ImGui::BeginPopupModal("原子数值属性##atom-properties",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        ImGui::Text("打开时选中 %zu 个原子",atomPropertySelection.size());
        const bool valid=validAtomProperties();ImGui::BeginDisabled(!valid);
        ImGui::SetNextItemWidth(U(260));
        const bool combo=ImGui::BeginCombo("属性",atomPropertyName.c_str());recordUiTestItem("creation.property-name");
        if(combo){for(const auto &name:atomPropertyNames) {
            if(ImGui::Selectable(name.c_str(),name==atomPropertyName)){atomPropertyName=name;refreshAtomProperty();}
            recordUiTestItem("creation.property-name-"+name);
        }ImGui::EndCombo();}
        ImGui::TextWrapped("%s",atomPropertySummary.c_str());
        ImGui::Checkbox("将选中行设为缺失值",&atomPropertyMissing);recordUiTestItem("creation.property-missing");
        ImGui::BeginDisabled(atomPropertyMissing);ImGui::SetNextItemWidth(U(260));
        ImGui::InputDouble("新值",&atomPropertyValue,0,0,"%.12g");recordUiTestItem("creation.property-value");ImGui::EndDisabled();
        ImGui::TextWrapped("应用到打开时选中的原子；其他行保留。Charge 是部分电荷，不改变形式电荷，也不自动补氢。\n新建 Charge 列时，未赋值原子保持缺失。缺失值可保存在 .atomx 中；导出包含该列的 Extended XYZ 需先补齐数值。");
        if(atomPropertyName=="Mass")ImGui::TextWrapped("Mass 使用 amu，必须大于零；这里只修改文件中已有的质量列。");
        ImGui::EndDisabled();
        if(!valid)ImGui::TextWrapped("体系已改变，请关闭后重新选择原子。");
        if(!atomPropertyMessage.empty())ImGui::TextWrapped("%s",atomPropertyMessage.c_str());
        ImGui::BeginDisabled(!valid);
        if(ImGui::Button("应用",{U(110),U(30)}) && applyAtomProperties())ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.property-apply");ImGui::EndDisabled();ImGui::SameLine();
        if(ImGui::Button("取消",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.property-cancel");ImGui::EndPopup();
    }
    void requestAtomConstraints() {
        if(!creationMode || documentsBusy() || !sameAtomCount() || creationSelection.empty())return;
        try {
            constraintSelection=constraints::selected(source,creationSelection);
            constraintSummary=constraints::summarize(source,constraintSelection);
            constraintTabId=tabs[size_t(activeTab)].id;constraintGeneration=pipelineGeneration;
            constraintDraft={};constraintMessage.clear();openAtomConstraints=true;
        }catch(const std::exception &e){status=e.what();}
    }
    bool validAtomConstraints() const {
        return creationMode && activeTab>=0 && activeTab<int(tabs.size()) && tabs[size_t(activeTab)].id==constraintTabId &&
            pipelineGeneration==constraintGeneration && !documentsBusy();
    }
    bool applyAtomConstraints() {
        if(!validAtomConstraints()){constraintMessage="体系已改变，请关闭后重新选择原子";return false;}
        try {
            const auto edit=constraints::prepare(source,constraintSelection,constraintDraft);
            if(!edit.changed){status="约束没有变化";return true;}
            const auto bonds=creationBondSelection;
            editStructure("修改原子模拟约束 · "+std::to_string(edit.rows.size())+" 个原子",
                [&](Dataset &d){constraints::apply(d,edit);});
            creationSelection=edit.rows;creationPick=creationSelection.back();creationBondSelection=bonds;return true;
        }catch(const std::exception &e){constraintMessage=e.what();return false;}
    }
    void atomConstraintsDialog() {
        if(openAtomConstraints){ImGui::OpenPopup("原子模拟约束##atom-constraints");openAtomConstraints=false;}
        const auto *vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(),ImGuiCond_Appearing,{.5f,.5f});
        ImGui::SetNextWindowSizeConstraints({U(480),0},{std::max(U(480),vp->WorkSize.x-U(24)),vp->WorkSize.y-U(24)});
        if(!ImGui::BeginPopupModal("原子模拟约束##atom-constraints",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+U(480));
        ImGui::Text("打开时选中 %zu 个原子",constraintSelection.size());
        auto state=[](int s){return s<0?"混合":s?"固定":"自由";};
        ImGui::Text("当前整原子固定：%s",state(constraintSummary.cartesian));
        ImGui::Text("当前晶格方向：a %s · b %s · c %s",state(constraintSummary.fractional[0]),
            state(constraintSummary.fractional[1]),state(constraintSummary.fractional[2]));
        const bool valid=validAtomConstraints();ImGui::BeginDisabled(!valid);
        auto choice=[&](const char *label,int &value,const char *key) {
            ImGui::SetNextItemWidth(U(155));
            const char *labels[]{"保持当前值","解除固定","固定"};
            const bool open=ImGui::BeginCombo(label,labels[std::clamp(value,0,2)]);recordUiTestItem(key);
            if(open) {
                for(int i=0;i<3;++i) {if(ImGui::Selectable(labels[i],value==i))value=i;recordUiTestItem(std::string(key)+"-"+std::to_string(i));}
                ImGui::EndCombo();
            }
        };
        ImGui::SeparatorText("整原子固定");
        choice("笛卡尔 XYZ",constraintDraft.cartesian,"creation.constraint-cartesian");
        ImGui::TextWrapped("笛卡尔 XYZ 同时固定；部分轴及其旋转后的方向约束尚未支持。");
        ImGui::SeparatorText("晶格方向约束");
        const bool periodic=constraints::periodic(source);ImGui::BeginDisabled(!periodic);
        choice("a 分数分量",constraintDraft.fractional[0],"creation.constraint-a");
        choice("b 分数分量",constraintDraft.fractional[1],"creation.constraint-b");
        choice("c 分数分量",constraintDraft.fractional[2],"creation.constraint-c");
        if(ImGui::Button("固定全部晶格方向"))constraintDraft.fractional.fill(constraints::Fixed);
        recordUiTestItem("creation.constraint-fractional-all");ImGui::EndDisabled();
        if(!periodic)ImGui::TextWrapped("晶格方向固定需要有效的三维周期晶胞。");
        if(ImGui::Button("解除全部晶格方向"))constraintDraft.fractional.fill(constraints::Free);
        recordUiTestItem("creation.constraint-fractional-clear");
        ImGui::TextWrapped("这些是模拟约束属性，手工移动与旋转仍可使用；AtomX 不执行模拟求解。保持当前值可保留多选中的不同约束。");
        ImGui::TextWrapped(".atomx 保存两类约束。POSCAR 保存晶格方向 T/F；整原子笛卡尔固定需保存在 .atomx 中，导出 POSCAR 时须显式取消约束保留。");
        ImGui::EndDisabled();
        if(!valid)ImGui::TextWrapped("体系已改变，请关闭后重新选择原子。");
        if(!constraintMessage.empty())ImGui::TextWrapped("%s",constraintMessage.c_str());
        ImGui::BeginDisabled(!valid);
        if(ImGui::Button("应用",{U(110),U(30)}) && applyAtomConstraints())ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.constraint-apply");ImGui::EndDisabled();ImGui::SameLine();
        if(ImGui::Button("取消",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.constraint-cancel");ImGui::PopTextWrapPos();ImGui::EndPopup();
    }
    void requestCreationChemistry() {
        if(!creationMode || documentsBusy() || !sameAtomCount() || creationSelection.empty())return;
        chemistrySelection=creationSelection;
        chemistryTabId=tabs[size_t(activeTab)].id;chemistryGeneration=pipelineGeneration;
        chemistrySetCharge=false;chemistryHybridChoice=0;chemistryCharge=0;chemistryMessage.clear();
        try {
            const auto first=chemistrySelection.front();
            const double q=chemistry::charge(source,size_t(first)),h=chemistry::hybrid(source,size_t(first));
            bool sameQ=true,sameH=true;
            for(int i:chemistrySelection) {sameQ&=chemistry::charge(source,size_t(i))==q;sameH&=chemistry::hybrid(source,size_t(i))==h;}
            if(std::isfinite(q) && q==std::floor(q) && q>=INT_MIN && q<=INT_MAX)chemistryCharge=int(q);
            const auto qText=sameQ && std::isfinite(q)?std::to_string(q):std::string("混合 / 缺失");
            const auto hText=sameH && std::isfinite(h) && h>=0 && h<=7 && h==std::floor(h)?chemistry::hybridNames[int(h)]:"混合 / 缺失";
            chemistrySummary="当前形式电荷："+qText+" · 杂化："+hText;
            openCreationChemistry=true;
        }catch(const std::exception &e){status=e.what();}
    }
    bool applyCreationChemistry() {
        if(!creationMode || activeTab<0 || activeTab>=int(tabs.size()) || tabs[size_t(activeTab)].id!=chemistryTabId ||
           pipelineGeneration!=chemistryGeneration || documentsBusy()) {chemistryMessage="体系已改变，请关闭后重新选择原子";return false;}
        try {
            const auto edit=chemistry::prepare(source,chemistrySelection,
                chemistrySetCharge?std::optional<int>(chemistryCharge):std::nullopt,
                chemistryHybridChoice?std::optional<int>(chemistryHybridChoice-1):std::nullopt);
            if(!edit.changed){status="化学设置没有变化";return true;}
            auto selected=edit.selection;
            editStructure("修改原子化学设置 · "+std::to_string(selected.size())+" 个原子",[&](Dataset &data) {
                chemistry::apply(data,edit);updateAutomaticHydrogens(data,selected);
            });
            creationSelection=std::move(selected);creationPick=creationSelection.empty()?-1:creationSelection.back();
            return true;
        }catch(const std::exception &e){chemistryMessage=e.what();return false;}
    }
    void chemistryDialog() {
        if(openCreationChemistry){ImGui::OpenPopup("原子化学设置##chemistry");openCreationChemistry=false;}
        const auto *vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(),ImGuiCond_Appearing,{.5f,.5f});
        ImGui::SetNextWindowSizeConstraints({U(360),0},{std::max(U(360),vp->WorkSize.x-U(24)),vp->WorkSize.y-U(24)});
        if(!ImGui::BeginPopupModal("原子化学设置##chemistry",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        ImGui::Text("打开时选中 %zu 个原子",chemistrySelection.size());
        ImGui::TextWrapped("%s",chemistrySummary.c_str());
        ImGui::Checkbox("修改形式电荷",&chemistrySetCharge);recordUiTestItem("creation.chemistry-charge-enable");
        ImGui::BeginDisabled(!chemistrySetCharge);ImGui::SetNextItemWidth(U(150));
        ImGui::InputInt("整数形式电荷",&chemistryCharge,0,0);recordUiTestItem("creation.chemistry-charge");ImGui::EndDisabled();
        const char *preview=chemistryHybridChoice?chemistry::hybridNames[chemistryHybridChoice-1]:"保持原设置";
        ImGui::SetNextItemWidth(U(230));
        const bool hybridOpen=ImGui::BeginCombo("杂化",preview,ImGuiComboFlags_HeightLarge);recordUiTestItem("creation.chemistry-hybrid");
        if(hybridOpen) {
            for(int choice=0;choice<=8;++choice) {
                if(ImGui::Selectable(choice?chemistry::hybridNames[choice-1]:"保持原设置",chemistryHybridChoice==choice))chemistryHybridChoice=choice;
                recordUiTestItem("creation.chemistry-hybrid-"+std::to_string(choice));
            }ImGui::EndCombo();
        }
        ImGui::TextWrapped("形式电荷用于化学价态判断；文件中的部分电荷 Charge 保留原值。\n自动杂化由已有键判断；高配位设置只记录属性，当前不自动补氢。\n应用后按本标签的自动氢开关调整；重原子固定。大体系可用 H+ 后台预览。");
        if(!chemistryMessage.empty())ImGui::TextWrapped("%s",chemistryMessage.c_str());
        const bool valid=creationMode && activeTab>=0 && activeTab<int(tabs.size()) && tabs[size_t(activeTab)].id==chemistryTabId &&
            pipelineGeneration==chemistryGeneration;
        if(!valid)ImGui::TextWrapped("体系已改变，请关闭后重新选择原子。");
        ImGui::BeginDisabled(!valid || documentsBusy() || (!chemistrySetCharge && !chemistryHybridChoice));
        if(ImGui::Button("应用",{U(110),U(30)}) && applyCreationChemistry())ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.chemistry-apply");ImGui::EndDisabled();ImGui::SameLine();
        if(ImGui::Button("取消",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.chemistry-cancel");ImGui::EndPopup();
    }
    void replacePickedElement() {
        std::vector<int> selected=creationSelection;
        if (selected.empty() && creationPick>=0) selected.push_back(creationPick);
        if (selected.empty()) {
            status = "Pick an atom in Creation Mode first";
            return;
        }
        std::string symbol = creationElement[0] ? creationElement : "C";
        editStructure("替换 " + std::to_string(selected.size()) + " 个原子 → " + symbol, [&](Dataset &data) {
            const auto type=authoring::speciesIndex(data,symbol);
            for (int index:selected)
                if (index>=0&&size_t(index)<data.atoms.size()) data.atoms[size_t(index)].type=type;
            updateAutomaticHydrogens(data,selected);
        });
    }
    void addAtomAt(Vec3 position) {
        if(documentsBusy() || !hydrogens::finite(position))return;
        try {hydrogens::validateRows(source);} catch(const std::exception &e){status=e.what();return;}
        std::string symbol = creationElement[0] ? creationElement : "C";
        editStructure("Added " + symbol, [&](Dataset &data) {
            const size_t old=data.atoms.size();
            uint32_t type = authoring::speciesIndex(data, symbol);
            data.atoms.push_back({position.x, position.y, position.z, type});
            hydrogens::extendRows(data,old);
            creationPick = int(data.atoms.size() - 1);
            creationSelection={creationPick};
            std::vector<int> affected{creationPick};updateAutomaticHydrogens(data,affected);
        });
    }
    bool editCreationBond(int first,int last,int order,int *remappedLast=nullptr) {
        if (!creationMode || documentsBusy() || order<0 || order>4 || first<0 || last<0 || first==last ||
            size_t(first)>=source.atoms.size() || size_t(last)>=source.atoms.size()) return false;
        const int existing=authoring::directBondIndex(source,first,last);
        if ((existing<0 && order==0) ||
            (existing>=0 && source.bonds[size_t(existing)].order==order)) return false;
        if (order && existing<0) {
            const auto &a=source.atoms[size_t(first)], &b=source.atoms[size_t(last)];
            const double distance=authoring::length({a.x-b.x,a.y-b.y,a.z-b.z});
            if (!std::isfinite(distance) || distance<1e-6 || source.bonds.size()>=interactiveBondBudget) {
                status="无法连键：重合坐标或键数已达上限"; return false;
            }
        }
        editStructure(order?"设置键级 "+std::to_string(order):"断开键",[&](Dataset &data) {
            authoring::setDirectBond(data,first,last,order);
            if (order) data.bondStyle.visible=true;
            std::vector<int> affected{first,last};updateAutomaticHydrogens(data,affected);
            if(remappedLast)*remappedLast=affected.size()==2?affected.back():-1;
        });
        return true;
    }
    bool insertCreationAtom(int bond) {
        if(!creationMode || documentsBusy() || !sameAtomCount() || bond<0 || size_t(bond)>=source.bonds.size())return false;
        try {
            const auto &b=source.bonds[size_t(bond)];
            if(!creationAtomVisible(int(b.a)) || !creationAtomVisible(int(b.b)))throw std::invalid_argument("键端点不可见");
            const auto edit=insertion::prepare(source,size_t(bond),creationElement[0]?creationElement:"C",creationSketchOrder);
            int added=-1;
            editStructure("键中插入 "+edit.element,[&](Dataset &data) {
                added=insertion::apply(data,edit);data.bondStyle.visible=true;
                std::vector<int> affected{int(edit.original.a),int(edit.original.b),added};
                updateAutomaticHydrogens(data,affected);added=affected.back();
            });
            selectCreationAtom(added,false);
            creationSketchAnchor=creationSketchLastPlaced=-1;creationSketchPreviewValid=false;creationInsertHover=-1;
            return true;
        }catch(const std::exception &e){status=e.what();return false;}
    }
    void commitCreationSketch(int hit,Vec3 at,bool isolated,bool finish,bool replace) {
        if (documentsBusy() || !creationMode) return;
        const std::string symbol=creationElement[0]?creationElement:"C";
        if (!elements::find(symbol)) { status="请选择有效元素"; return; }
        if (hit>=0) {
            creationSketchLastPlaced=-1;
            if (replace) { selectCreationAtom(hit,false); replacePickedElement(); return; }
            const int anchor=creationSketchAnchor;
            if (anchor>=0 && anchor!=hit && !isolated) editCreationBond(anchor,hit,creationSketchOrder,&hit);
            if(hit<0){creationSketchAnchor=-1;return;}
            selectCreationAtom(hit,false);
            creationSketchAnchor=finish || (anchor>=0 && !creationSketchContinuous) || isolated?-1:hit;
            creationSketchPreviewValid=false;
            return;
        }
        if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) return;
        const int anchor=isolated?-1:creationSketchAnchor;
        if (anchor>=0 && source.bonds.size()>=interactiveBondBudget) {
            status="键数已达到交互显示上限"; return;
        }
        if (anchor>=0) {
            const auto &a=source.atoms[size_t(anchor)];
            if (authoring::length({at.x-a.x,at.y-a.y,at.z-a.z})<1e-6) return;
        }
        int added=-1;
        try {hydrogens::validateRows(source);} catch(const std::exception &e){status=e.what();return;}
        editStructure(anchor>=0?"绘制 "+symbol+" 并连键":"绘制 "+symbol,[&](Dataset &data) {
            const uint32_t type=authoring::speciesIndex(data,symbol);
            added=int(data.atoms.size());
            data.atoms.push_back({at.x,at.y,at.z,type});
            // New geometry has no measured per-particle properties yet.
            // Keep existing values; mark the new row as missing instead of
            // invalidating every old particle's property arrays.
            hydrogens::extendRows(data,size_t(added));
            if (anchor>=0) {
                authoring::setDirectBond(data,anchor,added,creationSketchOrder);
                data.bondStyle.visible=true;
            }
            std::vector<int> affected{anchor,added};updateAutomaticHydrogens(data,affected);
            added=affected.back();
        });
        selectCreationAtom(added,false);
        creationSketchLastPlaced=added;
        creationSketchAnchor=finish || !creationSketchContinuous || isolated?-1:added;
    }
    void commitCreationRing(const authoring::RingSketch &ring) {
        if (documentsBusy() || !creationMode || !sameAtomCount()) return;
        try {
            hydrogens::validateRows(source);
            const auto edit=authoring::prepareRing(source,ring,creationDisplay.hidden);
            std::vector<int> selected=edit.selection;
            if (!edit.changed()) { status="环已存在"; return; }
            editStructure(std::string(ring.aromatic?"绘制芳香环 · ":"绘制碳环 · ")+std::to_string(ring.size),[&](Dataset &data) {
                authoring::applyRing(data,edit); data.bondStyle.visible=true;
                updateAutomaticHydrogens(data,selected);
            });
            creationSelection=std::move(selected); creationPick=creationSelection.empty()?-1:creationSelection.back();
        } catch (const std::exception &e) { status=e.what(); }
    }
    const fragments::Template *currentFragment() const {
        for (const auto &t:fragmentLibrary) if (t.key==creationFragmentKey) return &t;
        return nullptr;
    }
    void requestFragmentLibrary() {
        if (fragmentLibraryRequested) return;
        fragmentLibraryRequested=true;
        if (fragmentLibraryDirectory.empty()) fragmentLibraryDirectory=desktop::settingsPath().parent_path()/"fragments";
        const auto directory=fragmentLibraryDirectory;
        fragmentLibraryJob=std::async(std::launch::async,[directory] {
            FragmentLoad result;
            try {
                if (!std::filesystem::exists(directory)) return result;
                size_t bytes=0,count=0,invalid=0;
                for (const auto &file:std::filesystem::directory_iterator(directory)) {
                    if (!file.is_regular_file() || file.path().extension()!=L".atomx") continue;
                    const auto size=file.file_size();
                    if (++count>128 || bytes+size>8*1024*1024) { result.message="片段库读取上限：128 个文件 / 8 MiB"; break; }
                    bytes+=size;
                    try { result.entries.push_back(fragments::load(file.path())); }
                    catch (const std::exception &) { ++invalid; }
                }
                if (invalid) result.message="已跳过 "+std::to_string(invalid)+" 个无效片段文件";
                std::sort(result.entries.begin(),result.entries.end(),[](const auto &a,const auto &b){return a.name<b.name;});
            } catch (const std::exception &e) { result.message=e.what(); }
            return result;
        });
    }
    void selectFragment(const fragments::Template &t) {
        creationFragmentKey=t.key; creationFragmentConnector=t.connector;
        creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion();
        fragmentPreviewYaw=.5f; fragmentPreviewPitch=.3f; fragmentPreviewZoom=1.f; fragmentPreviewPan={};
        chooseCreationTool(CreationTool::Fragment);
    }
    void fragmentBrowser() {
        if (!creationMode) { showFragmentBrowser=false; return; }
        if (showFragmentBrowser) {
            requestFragmentLibrary();
            ImGui::SetNextWindowSize({U(610),U(480)},ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints({U(500),U(390)},{U(900),U(760)});
            if (ImGui::Begin("片段浏览器##fragments",&showFragmentBrowser)) {
                ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##fragment-search","搜索片段...",fragmentSearch,sizeof(fragmentSearch));
                recordUiTestItem("creation.fragment-search");
                if (ImGui::BeginChild("##fragment-list",{U(210),-U(100)},ImGuiChildFlags_Borders)) {
                    std::map<std::string,std::vector<size_t>> categories;
                    for (size_t index=0;index<fragmentLibrary.size();++index)
                        if (!fragmentSearch[0] || fragmentLibrary[index].name.find(fragmentSearch)!=std::string::npos)
                            categories[fragmentLibrary[index].category].push_back(index);
                    for (const auto &[category,indexes]:categories) {
                        ImGui::SetNextItemOpen(true,ImGuiCond_Once);
                        if (ImGui::TreeNode(category.c_str())) {
                            for (auto index:indexes) {
                                const auto &t=fragmentLibrary[index]; ImGui::PushID(t.key.c_str());
                                if (ImGui::Selectable(t.name.c_str(),t.key==creationFragmentKey)) selectFragment(t);
                                recordUiTestItem("creation.fragment-entry-"+t.key);
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::EndChild(); ImGui::SameLine();
                if (ImGui::BeginChild("##fragment-detail",{0,-U(100)},ImGuiChildFlags_Borders)) {
                    if (const auto *t=currentFragment()) {
                        ImGui::TextWrapped("%s",t->name.c_str());
                        ImGui::TextDisabled("%zu 原子 · %zu 键",t->data.atoms.size(),t->data.bonds.size());
                        const ImVec2 size{ImGui::GetContentRegionAvail().x,std::max(U(140),ImGui::GetContentRegionAvail().y-U(95))};
                        const auto start=ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##fragment-preview",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
                        recordUiTestItem("creation.fragment-preview");
                        auto &io=ImGui::GetIO();
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                            fragmentPreviewYaw+=io.MouseDelta.x*.012f; fragmentPreviewPitch+=io.MouseDelta.y*.012f;
                        }
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(1)) {
                            fragmentPreviewPan.x+=io.MouseDelta.x; fragmentPreviewPan.y+=io.MouseDelta.y;
                        }
                        if (ImGui::IsItemHovered() && io.MouseWheel!=0) fragmentPreviewZoom=std::clamp(fragmentPreviewZoom*std::pow(1.12f,io.MouseWheel),.3f,4.f);
                        const Vec3 center=authoring::scale(authoring::add(t->data.lo,t->data.hi),.5);
                        const float span=std::max({t->data.hi.x-t->data.lo.x,t->data.hi.y-t->data.lo.y,t->data.hi.z-t->data.lo.z,1.f});
                        const float zoom=std::min(size.x,size.y)*.62f/span*fragmentPreviewZoom;
                        std::vector<ImVec2> points; std::vector<float> depths; std::vector<int> order;
                        for (size_t i=0;i<t->data.atoms.size();++i) {
                            auto v=authoring::sub(fragments::at(t->data,int(i)),center);
                            v=authoring::rotatedPoint(v,{}, {0,1,0},fragmentPreviewYaw);
                            v=authoring::rotatedPoint(v,{}, {1,0,0},fragmentPreviewPitch);
                            points.push_back({start.x+size.x*.5f+v.x*zoom+fragmentPreviewPan.x,start.y+size.y*.5f-v.y*zoom+fragmentPreviewPan.y});
                            depths.push_back(v.z); order.push_back(int(i));
                        }
                        std::sort(order.begin(),order.end(),[&](int a,int b){return depths[size_t(a)]<depths[size_t(b)];});
                        auto *draw=ImGui::GetWindowDrawList(); draw->PushClipRect(start,{start.x+size.x,start.y+size.y},true);
                        draw->AddRectFilled(start,{start.x+size.x,start.y+size.y},IM_COL32(0,0,0,255),U(4));
                        for (const auto &b:t->data.bonds) {
                            auto a=points[b.a],z=points[b.b]; const float len=std::max(std::hypot(z.x-a.x,z.y-a.y),1.f);
                            const int strands=b.order==4?2:b.order;
                            for (int lane=0;lane<strands;++lane) {
                                const float offset=U(3)*(lane-(strands-1)*.5f),x=-(z.y-a.y)/len*offset,y=(z.x-a.x)/len*offset;
                                draw->AddLine({a.x+x,a.y+y},{z.x+x,z.y+y},IM_COL32(180,190,202,210),U(3));
                            }
                        }
                        int hoveredAtom=-1; float nearestDepth=-FLT_MAX;
                        for (int i:order) {
                            const auto *element=elements::find(t->data.species[t->data.atoms[size_t(i)].type]);
                            const float radiusPx=std::clamp(element->covalent*zoom*.25f,U(5),U(19));
                            const auto at=points[size_t(i)];
                            for (int shade=0;shade<8;++shade) {
                                const float f=float(shade)/7,brightness=.48f+.52f*f;
                                const unsigned color=element->rgb;
                                draw->AddCircleFilled({at.x-radiusPx*f*.24f,at.y-radiusPx*f*.24f},radiusPx*(1-f*.76f),
                                    IM_COL32(int(((color>>16)&255)*brightness),int(((color>>8)&255)*brightness),int((color&255)*brightness),255),20);
                            }
                            if (i==creationFragmentConnector) {
                                draw->AddCircle(at,radiusPx+U(4),IM_COL32(255,104,88,255),24,U(2));
                                draw->AddLine({at.x-radiusPx-U(4),at.y},{at.x+radiusPx+U(4),at.y},IM_COL32(255,104,88,150));
                            }
                            if (ImGui::IsItemHovered() && std::hypot(io.MousePos.x-at.x,io.MousePos.y-at.y)<radiusPx+U(3) && depths[size_t(i)]>=nearestDepth) {
                                hoveredAtom=i; nearestDepth=depths[size_t(i)];
                            }
                        }
                        draw->PopClipRect();
                        if (hoveredAtom>=0 && ImGui::IsMouseDoubleClicked(0)) {
                            if (fragments::terminalNeighbor(t->data,hoveredAtom)>=0) {
                                creationFragmentConnector=hoveredAtom; creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion();
                            } else fragmentLibraryMessage="连接点必须是只有一条直接键的末端原子";
                        }
                        ImGui::TextDisabled("红圈：连接点 · 双击末端原子更换");
                        ImGui::TextDisabled("拖动旋转 · 右键平移 · 滚轮缩放");
                        if (ImGui::SmallButton("重置预览")) { fragmentPreviewYaw=.5f; fragmentPreviewPitch=.3f; fragmentPreviewZoom=1; fragmentPreviewPan={}; }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("开始放置")) { chooseCreationTool(CreationTool::Fragment); showFragmentBrowser=false; }
                        recordUiTestItem("creation.fragment-place");
                    } else ImGui::TextWrapped("正在加载所选片段，或片段不在本机库中。");
                }
                ImGui::EndChild();
                if (ImGui::Button("定义片段...")) defineFragment();
                recordUiTestItem("creation.fragment-define");
                ImGui::SameLine();
                if (ImGui::Button("关闭")) showFragmentBrowser=false;
                recordUiTestItem("creation.fragment-close");
                if (fragmentLibraryJob.valid()) ImGui::TextDisabled("正在读取本机片段库...");
                if (!fragmentLibraryMessage.empty()) ImGui::TextWrapped("%s",fragmentLibraryMessage.c_str());
                ImGui::TextDisabled("主视口：空白处放置 · 末端原子接枝 · 按住拖动调整朝向");
            }
            ImGui::End();
        }
        if (openFragmentDefine) { ImGui::OpenPopup("定义片段##fragment-define"); openFragmentDefine=false; }
        if (ImGui::BeginPopupModal("定义片段##fragment-define",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            if (fragmentSaveComplete) { fragmentSaveComplete=false; ImGui::CloseCurrentPopup(); }
            if (fragmentDefineDraft) {
                ImGui::Text("%zu 原子 · 连接点 #%d",fragmentDefineDraft->data.atoms.size(),fragmentDefineDraft->connector+1);
                ImGui::InputText("名称",fragmentName,sizeof(fragmentName)); recordUiTestItem("creation.fragment-name");
                ImGui::InputText("库",fragmentCategory,sizeof(fragmentCategory)); recordUiTestItem("creation.fragment-category");
                ImGui::BeginDisabled(fragmentSaveJob.valid() || !fragmentName[0] || !fragmentCategory[0]);
                if (ImGui::Button("保存到片段库")) {
                    requestFragmentLibrary();
                    auto entry=*fragmentDefineDraft; entry.name=fragmentName; entry.category=fragmentCategory;
                    const auto directory=fragmentLibraryDirectory;
                    const auto filename=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".atomx";
                    fragmentSaveTabId=tabs[size_t(activeTab)].id;
                    fragmentSaveJob=std::async(std::launch::async,[directory,filename,entry=std::move(entry)]() mutable {
                        std::filesystem::create_directories(directory);
                        entry.key="user/"+std::filesystem::path(filename).stem().string();
                        fragments::save(directory/filename,entry); return entry;
                    });
                }
                recordUiTestItem("creation.fragment-save"); ImGui::EndDisabled(); ImGui::SameLine();
                if (ImGui::Button("取消")) ImGui::CloseCurrentPopup(); recordUiTestItem("creation.fragment-cancel");
                if (!fragmentLibraryMessage.empty()) ImGui::TextWrapped("%s",fragmentLibraryMessage.c_str());
            }
            ImGui::EndPopup();
        }
    }
    void clearCreationFusion() {
        creationFusionSeed.reset(); creationFusionPreview.reset(); creationFusionStart.reset();
        creationFusionTarget=-2; creationFusionPick=false; creationFragmentMouse={-1,-1};
    }
    void beginCreationFusion(int connector) {
        if (documentsBusy() || !creationMode || !sameAtomCount()) return;
        try {
            if (!creationAtomVisible(connector)) throw std::invalid_argument("请指定可见的末端原子");
            auto seed=std::make_shared<fragments::FusionSeed>(fragments::prepareFusion(source,connector));
            clearCreationFusion(); creationFusionSeed=std::move(seed);
            creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
            status="已指定移动片段 · 点击另一片段的末端原子 · 拖动转向 · Esc 取消";
        } catch (const std::exception &e) { status=e.what(); }
    }
    void commitCreationFusion(fragments::Fusion fusion) {
        if (documentsBusy() || !creationMode || !sameAtomCount()) return;
        try {
            fragments::preflight(source,fusion,creationDisplay.hidden);
            const auto removed=fragments::fusionRemoved(fusion); std::vector<int> moved;
            editStructure("连接已有片段",[&](Dataset &data) {
                moved=fragments::apply(data,fusion);
                creationDisplay.eraseAtoms(fusion.seed->atomCount,removed);
                data.bondStyle.visible=true;
            });
            creationSelection=std::move(moved); creationPick=creationSelection.empty()?-1:creationSelection.back();
            creationMeasure=creationAngle=creationDihedral=-1;
            clearCreationFusion();
        } catch (const std::exception &e) { status=e.what(); clearCreationFusion(); }
    }
    void commitCreationFragment(const fragments::Placement &placement) {
        if (documentsBusy() || !creationMode || !sameAtomCount()) return;
        const auto *t=currentFragment(); if (!t) return;
        try {
            fragments::preflight(source,*t,placement,creationDisplay.hidden);
            std::vector<int> added;
            editStructure("放置片段 · "+t->name,[&](Dataset &data) {
                if (placement.removed>=0) creationDisplay.eraseAtoms(data.atoms.size(),{placement.removed});
                added=fragments::apply(data,*t,placement); data.bondStyle.visible=true;
            });
            creationSelection=std::move(added); creationPick=creationSelection.empty()?-1:creationSelection.back();
            creationMeasure=creationAngle=creationDihedral=-1;
        } catch (const std::exception &e) { status=e.what(); }
    }
    void defineFragment() {
        if (documentsBusy() || !creationMode || creationSelection.size()!=1) {
            fragmentLibraryMessage="请在主视口选中一个末端原子作为连接点"; return;
        }
        try {
            fragmentDefineDraft=fragments::define(source,creationSelection[0],"我的片段","我的片段");
            snprintf(fragmentName,sizeof(fragmentName),"%s",fragmentDefineDraft->name.c_str());
            openFragmentDefine=true;
        } catch (const std::exception &e) { fragmentLibraryMessage=e.what(); }
    }
    void nudgePicked(int axis, float delta) {
        if (creationPick < 0 || size_t(creationPick) >= source.atoms.size()) {
            status = "Pick an atom in Creation Mode first";
            return;
        }
        int index = creationPick;
        editStructure("Moved atom " + std::to_string(index + 1), [&](Dataset &data) {
            float &coord = axis == 0 ? data.atoms[index].x : axis == 1 ? data.atoms[index].y
                                                                        : data.atoms[index].z;
            coord += delta;
        });
        creationPick = index;
    }
    void resetView() {
        for (auto &camera : cameras) {
            camera.yaw = .65f;
            camera.pitch = .48f;
            camera.zoom = 1.f;
            camera.panX = camera.panY = 0;
            camera.roll = 0;
        }
        cameras[0].mode = 0;
        cameras[1].mode = 2;
        cameras[2].mode = 4;
        cameras[3].mode = 7;
        for (int i = 0; i < 4; ++i) fitCamera(i, false);
        status = "Reset the camera on every viewport";
    }
    void addHydrogensCommand() {
        if(documentsBusy()) return;
        if(!creationMode) { openCreationTab(); if(!creationMode) return; }
        chooseCreationTool(CreationTool::Select);
        hydrogenTabId=tabs[size_t(activeTab)].id;
        hydrogenOptions={}; hydrogenOptions.selection=creationSelection;
        hydrogenOptions.all=creationSelection.empty();
        hydrogenPlan.reset();hydrogenMessage.clear();showHydrogenAdjust=true;
    }
    std::vector<uint8_t> hydrogenVisibility() const {
        auto hidden=creationDisplay.hidden;
        if(!particles) {hidden.assign(source.atoms.size(),1);return hidden;}
        if(std::any_of(gpu.styles.begin(),gpu.styles.end(),[](const auto &style){return style.visual[2]<=.5f;})) {
            hidden.resize(source.atoms.size());
            for(size_t i=0;i<source.atoms.size();++i) {
                const auto type=source.atoms[i].type;
                if(type<gpu.styles.size() && gpu.styles[type].visual[2]<=.5f)hidden[i]=1;
            }
        }
        return hidden;
    }
    void previewHydrogens() {
        if(documentsBusy() || !creationMode || !sameAtomCount() || !mods.empty() ||
           tabs[size_t(activeTab)].id!=hydrogenTabId) return;
        const auto options=hydrogenOptions; const auto hidden=hydrogenVisibility();
        hydrogenPlan.reset();hydrogenMessage="正在后台计算显式键与局部几何…";
        hydrogenBusy=true;
        try {hydrogenJob=std::async(std::launch::async,[this,options,hidden] {return hydrogens::prepare(source,options,hidden);});}
        catch(const std::exception &e){hydrogenBusy=false;hydrogenMessage=e.what();}
    }
    void applyHydrogens() {
        if(documentsBusy() || !hydrogenPlan || !hydrogenPlan->changes() || !creationMode ||
           tabs[size_t(activeTab)].id!=hydrogenTabId || !mods.empty()) return;
        try {
            hydrogens::preflight(source,*hydrogenPlan,hydrogenVisibility());
            auto plan=std::move(*hydrogenPlan);
            const auto message="调整氢 · +"+number(plan.added.size())+" / −"+number(plan.removed.size())+" / 移动 "+number(plan.moved.size());
            editStructure(message,[&](Dataset &data) {
                creationDisplay.eraseAtoms(data.atoms.size(),plan.removed);
                creationSelection=hydrogens::apply(data,plan);
                creationPick=creationSelection.empty()?-1:creationSelection.back();
            });
            hydrogenMessage=message+" · 已记录一步历史";
        } catch(const std::exception &e) {hydrogenPlan.reset();hydrogenMessage=e.what();}
    }
    void hydrogenDialog() {
        if(!showHydrogenAdjust)return;
        if(!creationMode || tabs[size_t(activeTab)].id!=hydrogenTabId) {showHydrogenAdjust=false;hydrogenPlan.reset();return;}
        const auto *viewport=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});
        ImGui::SetNextWindowSize({U(550),std::min(U(560),viewport->WorkSize.y-U(50))},ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({U(430),U(300)},{viewport->WorkSize.x-U(30),viewport->WorkSize.y-U(30)});
        if(ImGui::Begin("调整氢原子##hydrogen-adjust",&showHydrogenAdjust,ImGuiWindowFlags_NoCollapse)) {
            ImGui::BeginChild("##hydrogen-body",{0,-U(78)});
            ImGui::TextUnformatted("按显式键级调整数量与局部 SP / SP2 / SP3 几何");
            ImGui::TextDisabled("重原子坐标不变；形式电荷与文件中的部分电荷分开处理");
            ImGui::Separator();
            ImGui::BeginDisabled(documentsBusy());
            bool changed=ImGui::Checkbox("整个可见体系",&hydrogenOptions.all);recordUiTestItem("creation.hydrogen-all");
            if(!hydrogenOptions.all) ImGui::Text("打开时的选择：%zu 原子；末端 H 对应其相连重原子",hydrogenOptions.selection.size());
            changed|=ImGui::Checkbox("只补缺少的氢（保留已有氢的位置与数量）",&hydrogenOptions.onlyAdd);recordUiTestItem("creation.hydrogen-only-add");
            const char *hybrids[]={"沿用已有设置 / 自动","SP · 直线","SP2 · 三角平面","SP3 · 四面体","重置为自动判断"};
            ImGui::SetNextItemWidth(U(260));changed|=ImGui::Combo("局部杂化",&hydrogenOptions.hybridization,hybrids,5);
            recordUiTestItem("creation.hydrogen-hybrid");
            changed|=ImGui::Checkbox("指定形式电荷",&hydrogenOptions.setCharge);recordUiTestItem("creation.hydrogen-charge-override");
            if(hydrogenOptions.setCharge) {ImGui::SetNextItemWidth(U(150));changed|=ImGui::InputInt("整数电荷",&hydrogenOptions.charge);recordUiTestItem("creation.hydrogen-charge");}
            if(changed) {hydrogenPlan.reset();hydrogenMessage.clear();}
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::TextWrapped("先确认键级。支持常见主族低价态；金属、芳香 N、跨周期连接和退化位点跳过。不会自动识别隐式键或优化整个分子，也不会更新对称等价原子。");
            if(!mods.empty())ImGui::TextWrapped("当前有修改器。请先从其最终结果新建创作副本，再调整氢。");
            ImGui::Separator();
            if(hydrogenPlan) {
                const auto &p=*hydrogenPlan;
                ImGui::Text("已检查 %zu 个可处理位点",p.sites);
                ImGui::Text("新增 %zu · 删除多余末端氢 %zu · 移动已有氢 %zu",p.added.size(),p.removed.size(),p.moved.size());
                ImGui::Text("形式电荷 / 杂化设置更新：%zu 个位点",p.settings.size());
                ImGui::TextWrapped("跳过：不支持 %zu · 价态不明确 %zu · 坐标/拓扑无效 %zu · 跨周期 %zu · 隐藏 %zu",p.unsupported,p.ambiguous,p.invalid,p.periodic,p.hidden);
                if(!p.changes())ImGui::TextUnformatted("没有需要应用的更改");
            }
            if(!hydrogenMessage.empty())ImGui::TextWrapped("%s",hydrogenMessage.c_str());
            ImGui::EndChild();ImGui::Separator();
            ImGui::BeginDisabled(documentsBusy() || !sameAtomCount() || !mods.empty() ||
                (!hydrogenOptions.all && hydrogenOptions.selection.empty()) ||
                (hydrogenOptions.setCharge && (hydrogenOptions.charge < -4 || hydrogenOptions.charge > 4)));
            if(ImGui::Button("计算调整预览",{U(155),U(32)}))previewHydrogens();recordUiTestItem("creation.hydrogen-preview");
            ImGui::SameLine();ImGui::BeginDisabled(!hydrogenPlan || !hydrogenPlan->changes());
            if(ImGui::Button("应用",{U(100),U(32)}))applyHydrogens();recordUiTestItem("creation.hydrogen-apply");
            ImGui::EndDisabled();ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button("关闭",{U(100),U(32)})){showHydrogenAdjust=false;hydrogenPlan.reset();}
            recordUiTestItem("creation.hydrogen-close");
            ImGui::TextDisabled("预览在后台完成；应用后可撤销、暂存并保存为 .atomx");
        }
        ImGui::End();
    }
    void cleanGeometryCommand() {
        if (source.atoms.size() > 2500) {
            status = "Clean Geometry is limited to 2500 atoms";
            return;
        }
        editStructure("Cleaned bond lengths toward covalent radii",
                      [](Dataset &data) { authoring::cleanGeometry(data); });
    }
    void setDisplayStyle(int style) {
        if(creationMode) {
            auto next=creationDisplay;
            const uint8_t preset=style==0?3:style==1?4:style==2?2:1;
            next.setPreset(source.atoms.size(),creationSelection,preset,creationSelection.empty());
            editCreationDisplay(std::move(next),"切换显示样式"); return;
        }
        particleShape = 0;
        if (style == 1) radius = 1.15f;
        else if (style == 2) radius = 0.12f;
        else if (style == 3) radius = 0.05f;
        else radius = 0.32f;
        status = style == 1 ? "Display style: Space filling"
                 : style == 2 ? "Display style: Stick"
                 : style == 3 ? "Display style: Line"
                              : "Display style: Ball and stick";
    }
    void history(bool forward) {
        if(creationColorBusy || hydrogenBusy) return;
        creationAutoHydrogenMessage.clear();
        hydrogenPlan.reset();
        chooseCreationTool(creationTool);
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        if (creationMode || structureEditIsLatest || !authorRedo.empty()) {
            if (!forward && !authorUndo.empty()) {
                auto previous=std::move(authorUndo.back());
                const bool visibilityChanged=!previous.display.sameGpuAppearance(creationDisplay);
                authorRedo.push_back({previous.data?std::optional<Dataset>(source):std::nullopt,creationDisplay,previous.action});
                if (previous.data) source = std::move(*previous.data);
                creationDisplay = std::move(previous.display);
                queueCreationLabelFont();
                if (previous.data) { creationSymmetry.reset(); creationSymmetryChecked=false; }
                authorUndo.pop_back();
                creationSnapshotSelected=-1;
                if (authorUndo.empty()) structureEditIsLatest = false;
                creationPick = creationMeasure = creationAngle = creationDihedral = -1;
                creationSelection.clear(); creationBondSelection.clear();
                syncAppearance(source.species);
                if (previous.data) update(); else refreshCreationDisplay(visibilityChanged);
                status = "撤销："+previous.action;
                return;
            }
            if (forward && !authorRedo.empty()) {
                auto next=std::move(authorRedo.back());
                const bool visibilityChanged=!next.display.sameGpuAppearance(creationDisplay);
                authorUndo.push_back({next.data?std::optional<Dataset>(source):std::nullopt,creationDisplay,next.action});
                if (next.data) source = std::move(*next.data);
                creationDisplay = std::move(next.display);
                queueCreationLabelFont();
                if (next.data) { creationSymmetry.reset(); creationSymmetryChecked=false; }
                authorRedo.pop_back();
                creationSnapshotSelected=-1;
                creationPick = creationMeasure = creationAngle = creationDihedral = -1;
                creationSelection.clear(); creationBondSelection.clear();
                syncAppearance(source.species);
                if (next.data) update(); else refreshCreationDisplay(visibilityChanged);
                status = "重做："+next.action;
                return;
            }
        }
        if (!applyModifierHistory(mods, undo, redo, forward)) return;
        modifierGraph.selected = mods.empty() ? 0 : std::min(modifierGraph.selected, mods.size() - 1);
        pipelineCheckpoint.reset();
        pipelineCheckpointNode=SIZE_MAX;
        structureEditIsLatest = false;
        update();
    }
    void jumpCreationHistory(size_t position) {
        if(creationColorBusy || hydrogenBusy) return;
        const size_t end=authorUndo.size()+authorRedo.size();
        if (position>end) return;
        while (authorUndo.size()>position) history(false);
        while (authorUndo.size()<position && !authorRedo.empty()) history(true);
    }
    void load(const std::filesystem::path &p, int frame = 0) {
        if (busy || creationColorBusy || hydrogenBusy || p.empty())
            return;
        if (inspectorBusy) {
            inspectorCancel = true;
            deferredLoadPath = p;
            deferredLoadFrame = frame;
            inspectorResult.reset();
            inspectorNode = -1;
            inspectorNodeId.clear();
            status = "Waiting for node inspection to stop before loading data...";
            return;
        }
        deferredLoadPath.clear();
        pipelineDeferredForInspector = false;
        pipelineCheckpoint.reset();
        pipelineCheckpointNode=SIZE_MAX;
        unwrapAccumulators.clear();
        inspectorResult.reset();
        inspectorNode = -1;
        inspectorNodeId.clear();
        staleBeforeLoad = staleResult;
        staleResult = true;
        indexing = p != path || frames.empty();
        if (indexing) pendingFrame = -1;
        busy = true;
        cancel = false;
        progress = 0;
        playing = playing && p == path;
        status = "Reading trajectory...";
        auto known = p == path ? frames : std::vector<Frame>{};
        auto b = budget;
        job =
            std::async(std::launch::async, [this, p, frame, known = std::move(known), b]() mutable {
                if (known.empty())
                    known = io::index(p, &progress, &cancel);
                if (frame < 0 || frame >= int(known.size()))
                    throw std::runtime_error("Frame out of range");
                Dataset d;
                std::optional<document::View> view;
                if (io::detect(p)==io::Format::AtomX) {
                    auto content=document::read(p,b,&cancel);
                    d=std::move(content.data); view=std::move(content.view); progress=1;
                } else d=io::read(p,known[frame],b,&progress,&cancel);
                known[frame].count = d.sourceCount;
                return Loaded{std::move(d), std::move(known), p, frame,std::move(view)};
            });
    }
    void computeColorRangeAllFrames(size_t nodeIndex) {
        if (colorRangeRunning || busy || indexing || pipelineBusy || nodeIndex >= mods.size())
            return;
        if (frames.empty() || path.empty()) {
            error = "Load a trajectory before computing an all-frame color range";
            return;
        }
        const auto &target = mods[nodeIndex];
        if (target.op != Op::ColorCoding) return;
        constexpr uint64_t maxExactAtomsPerFrame = 2000000;
        for (const auto &frame : frames) {
            if (frame.count > maxExactAtomsPerFrame) {
                error = "Exact all-frame color range is limited to 2 million atoms per frame";
                return;
            }
        }
        std::vector<Frame> frameSnapshot = frames;
        std::vector<Modifier> upstream;
        upstream.reserve(nodeIndex);
        for (size_t i = 0; i < nodeIndex; ++i)
            upstream.push_back(static_cast<const Modifier &>(mods[i]));
        const auto inputPath = path;
        const auto property = target.property;
        colorRangeNodeId = target.id;
        colorRangePath = inputPath;
        colorRangePipelineGeneration = pipelineGeneration;
        colorRangeCancel = false;
        colorRangeProgress = 0;
        colorRangeRunning = true;
        error.clear();
        status = "Computing exact color range across trajectory frames...";
        colorRangeJob = std::async(std::launch::async,
            [this, inputPath, frameSnapshot = std::move(frameSnapshot), upstream = std::move(upstream), property,
             maxExactAtomsPerFrame]() mutable {
                auto readFrame = [&](size_t index) {
                    if (colorRangeCancel) throw std::runtime_error("Cancelled");
                    const auto &frame = frameSnapshot.at(index);
                    // Static formats have one indexed frame whose particle count is unknown.
                    // Read just over the exact-work limit so the importer can report sampling
                    // or an over-limit source without silently calculating from a preview.
                    auto data = io::read(inputPath, frame, maxExactAtomsPerFrame + 1,
                                         nullptr, &colorRangeCancel);
                    if (data.sampled() || data.sourceCount > maxExactAtomsPerFrame ||
                        data.atoms.size() > maxExactAtomsPerFrame)
                        throw std::runtime_error(
                            "Exact all-frame color range is limited to 2 million atoms per frame; "
                            "the input was sampled or exceeds the limit");
                    return data;
                };
                return colorRangeAcrossFrames(frameSnapshot.size(), readFrame, upstream, property,
                                              &colorRangeCancel, &colorRangeProgress);
            });
    }
    void poll() {
        if(projectBusy && projectJob.valid() && projectJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            projectBusy=false;
            try {
                auto loaded=projectJob.get();projectPath=loaded.path;
                if(!loaded.tabs.empty()) {
                    tabs[size_t(activeTab)]=captureTab();
                    const bool replaceHome=tabs.size()==1 && homeMode;
                    if(replaceHome)tabs.clear();
                    const int offset=int(tabs.size());std::map<uint64_t,uint64_t> remap;
                    for(auto &t:loaded.tabs){const auto old=t.id;remap[old]=nextTabId++;t.id=remap[old];}
                    for(auto &t:loaded.tabs){t.sourceTabId=remap.contains(t.sourceTabId)?remap.at(t.sourceTabId):0;tabs.push_back(std::move(t));}
                    activeTab=offset+loaded.active;restoreTab(tabs[size_t(activeTab)]);
                }
                status=loaded.message;
            }catch(const std::exception &e){status=std::string("Workspace: ")+e.what();}
        }
        if(hydrogenBusy && hydrogenJob.valid() && hydrogenJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            hydrogenBusy=false;
            try {
                auto plan=hydrogenJob.get();
                if(showHydrogenAdjust && creationMode && tabs[size_t(activeTab)].id==hydrogenTabId) {
                    hydrogenPlan=std::move(plan);hydrogenMessage="预览完成，确认增删数量后应用";
                }
            } catch(const std::exception &e) {hydrogenMessage=e.what();}
        }
        if(creationColorBusy && creationColorRangeJob.valid() && creationColorRangeJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            creationColorBusy=false;
            try { const auto r=creationColorRangeJob.get(); creationColorDraft.low=r.first; creationColorDraft.high=r.second;
                creationColorMessage="已更新范围";
            } catch(const std::exception &e) {creationColorMessage=e.what();}
        }
        if (layerBuildBusy && layerBuildJob.valid() && layerBuildJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            layerBuildBusy=false;
            try {
                auto built=layerBuildJob.get();
                if (!creationMode || tabs[size_t(activeTab)].id!=layerBuilderTabId)
                    throw std::runtime_error("创作标签已改变，叠层结果未替换当前体系");
                const size_t count=built.data.atoms.size(),cut=built.cutBonds;
                adoptStructure(std::move(built.data),"构建叠层 · "+std::to_string(layerCount)+" 层");
                fitCamera(3,false);
                layerBuilderMessage="已构建 "+number(count)+" 原子 · 切断 "+number(cut)+" 条法向边界键 · 已记录一步历史";
                layerBuildCompleted=true;
                refreshFont=true;
            } catch (const std::exception &e) { layerBuilderMessage=e.what(); }
        }
        if (motionGroupBusy && motionGroupJob.valid() && motionGroupJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            motionGroupBusy=false;
            try {
                auto assignment=motionGroupJob.get();
                const size_t count=assignment.labels.size(),skipped=assignment.skipped;
                if (count) editStructure("自动创建 "+std::to_string(count)+" 个运动分组",[&](Dataset &data){motion::apply(data,std::move(assignment));});
                motionGroupMessage="已创建 "+std::to_string(count)+" 组 · 跳过周期网络中的 "+std::to_string(skipped)+" 个原子";
                refreshFont=true;
            } catch (const std::exception &e) { motionGroupMessage=e.what(); }
        }
        if (fragmentLibraryJob.valid() && fragmentLibraryJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            const auto loaded=fragmentLibraryJob.get();
            for (auto t:loaded.entries) {
                const bool exists=std::any_of(fragmentLibrary.begin(),fragmentLibrary.end(),[&](const auto &old){return old.key==t.key;});
                if (!exists) fragmentLibrary.push_back(std::move(t));
            }
            fragmentLibraryMessage=loaded.message;
            if (!loaded.entries.empty()) refreshFont=true;
            if (creationTool==CreationTool::Fragment && !currentFragment()) {
                chooseCreationTool(CreationTool::Select); status="所选自定义片段不在本机片段库中，请重新选择片段";
            }
        }
        if (fragmentSaveJob.valid() && fragmentSaveJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {
                fragmentLibrary.push_back(fragmentSaveJob.get());
                if (creationMode && tabs[size_t(activeTab)].id==fragmentSaveTabId) selectFragment(fragmentLibrary.back());
                fragmentLibraryMessage="片段已保存 · "+fragmentLibrary.back().name;
                refreshFont=true;
                showFragmentBrowser=creationMode; fragmentSaveComplete=true;
            } catch (const std::exception &e) { fragmentLibraryMessage=e.what(); }
        }
        if (inspectorBusy && inspectorJob.valid() &&
            inspectorJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            inspectorBusy = false;
            try {
                auto inspected = inspectorJob.get();
                if (inspectorGeneration == pipelineGeneration && inspectorNode >= 0 &&
                    inspectorNode < int(mods.size()) &&
                    mods[size_t(inspectorNode)].id == inspectorNodeId) {
                    inspectorResult = std::move(inspected);
                    status = "Selected pipeline node output is ready for inspection";
                } else {
                    inspectorResult.reset();
                    inspectorNode = -1;
                    inspectorNodeId.clear();
                }
            } catch (const std::exception &e) {
                inspectorResult.reset();
                inspectorNode = -1;
                inspectorNodeId.clear();
                if (std::string(e.what()) != "Cancelled") error = e.what();
            }
            if (!inspectorBusy && !deferredLoadPath.empty()) {
                auto queuedPath = std::move(deferredLoadPath);
                const int queuedFrame = deferredLoadFrame;
                deferredLoadFrame = 0;
                load(queuedPath, queuedFrame);
            } else if (!inspectorBusy && pipelineDeferredForInspector) {
                pipelineDeferredForInspector = false;
                launchPipeline();
            }
        }
        if (pipelineBusy && pipelineJob.valid() &&
            pipelineJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const uint64_t finishedGeneration = pipelineJobGeneration;
            pipelineBusy = false;
            if (finishedGeneration == pipelineGeneration) {
                try {
                    auto completed = pipelineJob.get();
                    publishPipeline(std::move(completed.result));
                    if (completed.checkpoint) {
                        pipelineCheckpoint=std::make_shared<PipelineResult>(std::move(*completed.checkpoint));
                        pipelineCheckpointNode=completed.checkpointNode;
                    }
                    for (auto &node : mods) {
                        node.running = false;
                        node.error.clear();
                    }
                    if (error.empty()) status = "Pipeline ready";
                } catch (const ModifierExecutionError &e) {
                    error = e.what();
                    for (auto &node : mods) node.running = false;
                    if (e.nodeIndex < mods.size() && e.nodeIndex < pipelineJobNodeIds.size() &&
                        mods[e.nodeIndex].id == pipelineJobNodeIds[e.nodeIndex])
                        mods[e.nodeIndex].error = error;
                    status = "Pipeline evaluation failed";
                } catch (const std::exception &e) {
                    error = e.what();
                    for (auto &node : mods) node.running = false;
                    status = "Pipeline evaluation failed";
                }
            } else {
                try { (void)pipelineJob.get(); } catch (...) {}
            }
            if (finishedGeneration != pipelineGeneration)
                launchPipeline();
        }
        if (exporting && exportJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            exporting = false;
            try {
                status = exportJob.get();
                if (!exportedDocumentPath.empty())
                    for (auto &tab : tabs) if (tab.id == exportedDocumentTab) {
                        tab.documentPath = exportedDocumentPath;
                        pushRecent(exportedDocumentPath);
                        break;
                    }
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        if (dxaRunning && dxaJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            dxaRunning=false;
            try { dxa= dxaJob.get(); status="DXA prepass completed"; } catch(const std::exception&e){error=e.what();}
        }
        if (analyzing &&
            analysisJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            analyzing = false;
            try {
                auto a = analysisJob.get();
                if (revision == analysisRevision) {
                    analysis = std::move(a);
                    status = "Neighbor analysis completed";
                } else
                    status = "Dataset changed; analysis result discarded";
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        if (colorRangeRunning && colorRangeJob.valid() &&
            colorRangeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            colorRangeRunning = false;
            try {
                auto range = colorRangeJob.get();
                auto node = std::find_if(mods.begin(), mods.end(), [&](const ModifierNode &item) {
                    return item.id == colorRangeNodeId;
                });
                if (node == mods.end() || path != colorRangePath ||
                    pipelineGeneration != colorRangePipelineGeneration) {
                    status = "Dataset or pipeline changed; all-frame color range discarded";
                } else {
                    checkpoint();
                    node->colorMin = float(range.first);
                    node->colorMax = float(range.second);
                    node->colorAutoRange = true;
                    node->colorAllFramesRange = true;
                    const size_t index = size_t(node - mods.begin());
                    update(index, true);
                    status = "Color range computed across all frames";
                }
            } catch (const std::exception &e) {
                if (std::string(e.what()) == "Cancelled") status = "All-frame color range cancelled";
                else { error = e.what(); status = "All-frame color range failed"; }
            }
        }
        if (busy && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            busy = false;
            indexing = false;
            try {
                auto l = job.get();
                readerName = io::info(io::detect(l.path)).name;
                bool changed = path != l.path;
                if (changed) {
                    appearanceMemory.clear();
                    customRadiusMemory.clear();
                    appearanceNames.clear();
                    gpu.resetStyles(0);
                }
                source = std::move(l.data);
                frames = std::move(l.frames);
                path = l.path;
                current = l.frame;
                if(changed && activeTab>=0 && activeTab<int(tabs.size()))tabs[size_t(activeTab)].title=utf8(l.path.filename().wstring());
                pushRecent(l.path);
                if (changed) {
                    appearanceType = 0;
                    exportOptions.scalarProperties.clear();
                    exportOptions.vectorProperties.clear();
                    mods.clear();
                    modifierGraph.selected = 0;
                    undo.clear();
                    redo.clear();
                    for (auto &c : cameras)
                        c.zoom = 1;
                }
                if (l.documentView) restoreDocumentView(*l.documentView);
                update(SIZE_MAX, true);
                status = "Opened " + utf8(path.filename().wstring()) + ": " +
                         number(source.sourceCount) + " atoms" + dropNotice;
                dropNotice.clear();
            } catch (const std::exception &e) {
                error = e.what();
                playing = false;
                staleResult = staleBeforeLoad;
                status = "Load stopped";
            }
        }
        if (!busy && pendingFrame >= 0) {
            int requested = pendingFrame; pendingFrame = -1;
            if (requested != current) load(path, requested);
        }
        if (!droppedFiles.empty() && !documentsBusy() && error.empty()) {
            auto file=std::move(droppedFiles.front());droppedFiles.pop_front();openFileTab(file);
        }
    }
    void open() {
        // Explorer's multi-select buffer is directory + individual filenames,
        // or one full path. Queue each file so asynchronous loads cannot cancel
        // one another, and keep all remaining files when one fails to load.
        std::vector<wchar_t> buffer(65536);OPENFILENAMEW of{};of.lStructSize=sizeof(of);of.hwndOwner=window;
        of.lpstrFile=buffer.data();of.nMaxFile=DWORD(buffer.size());of.lpstrDefExt=L"xyz";
        of.lpstrFilter=L"Atom structures\0*.xyz;*.extxyz;*.vasp;*.poscar;*.contcar;POSCAR;CONTCAR;*.cif;*.data;*.lmp;*.dump;*.lammpstrj;*.pdb;*.ent;*.gro;*.atomx;*.atomx-project\0All files\0*.*\0";
        of.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_EXPLORER|OFN_ALLOWMULTISELECT;
        const bool accepted=GetOpenFileNameW(&of)!=FALSE;
        if(ImGui::GetCurrentContext()){auto &io=ImGui::GetIO();io.ClearEventsQueue();io.ClearInputKeys();io.ClearInputMouse();}
        if(!accepted){if(CommDlgExtendedError()==FNERR_BUFFERTOOSMALL)status="Too many selected paths; open fewer files at once";return;}
        const std::filesystem::path first=buffer.data();size_t cursor=wcslen(buffer.data())+1;
        if(buffer[cursor]==L'\0')droppedFiles.push_back(first);
        else while(cursor<buffer.size() && buffer[cursor]!=L'\0') {droppedFiles.push_back(first/(buffer.data()+cursor));cursor+=wcslen(buffer.data()+cursor)+1;}
    }
    // Remember a successfully opened file for File > Recent Files (newest
    // first, deduplicated, persisted immediately). Persistence failures are
    // non-fatal: the session keeps working with an in-memory list.
    void pushRecent(const std::filesystem::path &p) {
        if (p.empty()) return;
        auto wide = p.wstring();
        auto &list = preferences.recentFiles;
        list.erase(std::remove(list.begin(), list.end(), wide), list.end());
        list.insert(list.begin(), wide);
        if (list.size() > size_t(desktop::Preferences::maxRecentFiles))
            list.resize(size_t(desktop::Preferences::maxRecentFiles));
        try {
            preferences.save();
        } catch (const std::exception &) {
        }
    }
    // Native documents preserve the evaluated result and creation display.
    document::View captureDocumentView() const {
        document::View v;
        v.creation=creationMode; v.cell=cell; v.particles=particles;
        const auto &camera=cameras[creationMode?3:active];
        v.camera={camera.yaw,camera.pitch,camera.zoom,camera.panX,camera.panY};
        v.cameraRoll=camera.roll;
        v.cameraMode=camera.mode; v.fitSelected=camera.fitSelected; v.fitLo=camera.fitLo; v.fitHi=camera.fitHi;
        v.radius=radius; v.shape=particleShape;
        v.title=tabs[size_t(activeTab)].title; v.basedOn=creationBasedOn;
        if (creationMode) {
            v.display=creationDisplay; v.selection.assign(creationSelection.begin(),creationSelection.end());
            v.bondSelection.assign(creationBondSelection.begin(),creationBondSelection.end());
            v.element=creationElement; v.tool=int(creationTool); v.order=creationSketchOrder;
            v.ringSize=creationRingSize;
            v.fragmentKey=creationFragmentKey; v.fragmentConnector=creationFragmentConnector;
            v.continuous=creationSketchContinuous; v.propertyPage=creationPropertyPage;
            v.autoHydrogens=creationAutoHydrogens;
            v.propertiesOpen=creationPropertiesOpen;
        }
        for (size_t i=0;i<result.data.species.size() && i<gpu.styles.size();++i) {
            const auto &style=gpu.styles[i]; v.styles.push_back({style.color,style.visual,style.axes});
        }
        return v;
    }
    void restoreDocumentView(const document::View &v) {
        creationMode=v.creation; homeMode=false; quad=false; active=3;
        auto &camera=cameras[3];
        camera.yaw=v.camera[0]; camera.pitch=v.camera[1]; camera.zoom=v.camera[2];
        camera.panX=v.camera[3]; camera.panY=v.camera[4]; camera.mode=v.cameraMode;
        camera.roll=std::remainder(v.cameraRoll,DirectX::XM_2PI);
        camera.fitSelected=v.fitSelected; camera.fitLo=v.fitLo; camera.fitHi=v.fitHi;
        cell=v.cell; particles=v.particles; radius=v.radius; particleShape=v.shape;
        syncAppearance(source.species);
        if (!v.styles.empty()) for (size_t i=0;i<v.styles.size();++i)
            gpu.styles[i]={v.styles[i].color,v.styles[i].visual,v.styles[i].axes};
        for (size_t i=0;i<source.species.size();++i)
            appearanceMemory[source.species[i]]=gpu.styles[i];
        creationDisplay=v.display; creationSelection.assign(v.selection.begin(),v.selection.end());
        creationBondSelection.assign(v.bondSelection.begin(),v.bondSelection.end());
        creationPick=creationSelection.empty()?-1:creationSelection.back();
        creationTool=CreationTool(v.tool); creationSketch=creationTool==CreationTool::Sketch;
        creationSketchOrder=v.order; creationSketchContinuous=v.continuous;
        creationAutoHydrogens=v.autoHydrogens; creationAutoHydrogenMessage.clear();
        creationRingSize=v.ringSize; creationRingPreviewValid=false;
        creationFragmentKey=v.fragmentKey; creationFragmentConnector=v.fragmentConnector;
        creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion(); showFragmentBrowser=false;
        if (creationTool==CreationTool::Fragment) requestFragmentLibrary();
        creationDrag=CreationDrag::None; creationDragAtoms.clear();
        snprintf(creationElement,sizeof(creationElement),"%s",v.element.c_str());
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        creationPropertiesOpen=v.propertiesOpen; creationPropertyPage=v.propertyPage;
        creationBasedOn=v.basedOn; creationReturnTab=-1;
        auto &tab=tabs[size_t(activeTab)]; tab.sourceTabId=0; tab.basedOn=v.basedOn; tab.documentPath=path;
        if (!v.title.empty()) tab.title=v.title;
        authorUndo.clear(); authorRedo.clear(); creationSnapshots.clear();
        creationSnapshotSelected=-1; creationSymmetry.reset(); creationSymmetryChecked=false;
        if (creationMode) saveCreationSnapshot("打开 · "+utf8(path.filename().wstring()));
        queueCreationLabelFont();
    }
    // Each tab remembers its native save destination without changing the
    // pipeline input path. First save / Save As uses the export dialog.
    void saveSessionState(bool askLocation) {
        const bool native=creationMode || lowerExtension(path)==".atomx";
        if (native) {
            exportFormat=int(io::Format::AtomX); exportRange=false; exportSequence=false;
        }
        const auto &documentPath=tabs[size_t(activeTab)].documentPath;
        if (askLocation || (path.empty() && documentPath.empty())) {
            showDataExport = true;
            return;
        }
        try {
            if (!native) exportFormat=0;
            const auto suffix=native?L"-creation.atomx":L"-session.xyz";
            const auto destination=native && !documentPath.empty()?documentPath:
                native && lowerExtension(path)==".atomx"?path:
                path.parent_path()/(path.stem().wstring()+suffix);
            startDataExport(destination);
            status = "Saving session state...";
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    void openUrl(const char *url) {
        ShellExecuteA(window, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
    void exportImage() {
        auto p = dialog(window, true, L"PNG image\0*.png\0", L"png");
        if (p.empty())
            return;
        Target t;
        gpu.target(t, exportW, exportH);
            const bool compact=!creationMode && result.data.bondStyle.visible && result.data.bondStyle.radius>0 && gpu.bondsUploaded();
            gpu.draw(t, result.data, cameras[active], radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell,
                creationMode?.43f:compact?.50f:1.f,creationMode && sameAtomCount()?&creationDisplay:nullptr);
        ColorLegendOptions legend{colorCoding && colorLegend, colorRangeProperty, colorGradient,
                                  colorMin, colorMax, colorReverse, colorDiscrete};
        if(creationMode && creationDisplay.hasColors()) {
            const auto &r=creationDisplay.defaultColor;
            legend={r.kind==creation::ColorKind::Property && r.legend,r.property,r.gradient,r.low,r.high,r.reverse,r.discrete};
        }
        gpu.png(t, p, legend);
        status = "Rendered " + utf8(p.filename().wstring());
    }
    void fixed(const char *name, float x, float y, float w, float h) {
        ImGui::SetNextWindowPos({x, y});
        ImGui::SetNextWindowSize({std::max(w, 1.f), std::max(h, 1.f)});
        const std::string_view panel=name;
        const bool scrollableCreationPanel=panel=="Creation snapshots" ||
            panel=="Creation file properties" || panel=="Creation selection";
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ((panel == "Title" ||
                         (creationMode && panel.starts_with("Creation") &&
                          !scrollableCreationPanel)) ?
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0));
    }
    void recordUiTestItem(const std::string &name,const char *explicitLabel=nullptr) {
        if (!captureUiTestItems) return;
        const ImGuiID id=explicitLabel ? ImGui::GetID(explicitLabel) : ImGui::GetItemID();
        uiTestItems[name]={id,ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
                           ImGui::IsItemHovered(),ImGui::IsItemClicked()};
    }
    // Records a captured item whose ImGui item context has since moved on
    // (the pipeline rows draw badges/names after their row button).
    void recordUiTestItemExplicit(const std::string &name, ImGuiID id, ImVec2 mn, ImVec2 mx,
                                  bool hovered, bool clicked, const char *explicitLabel = nullptr) {
        if (!captureUiTestItems) return;
        if (explicitLabel) id = ImGui::GetID(explicitLabel);
        uiTestItems[name] = {id, mn, mx, hovered, clicked};
    }
    // Stroke icon for the viewport navigation cluster (zoom / pan / orbit /
    // field of view). Drawn on a 16-unit grid centered in the button so the
    // four tools share one stroke weight. Segoe MDL2's magnifier and its
    // neighbors turn muddy at this size; these paths stay a single hairline.
    void drawViewportNavIcon(ImDrawList *dl, ImVec2 min, ImVec2 max, int tool, ImU32 color) const {
        const float side = std::min(max.x - min.x, max.y - min.y);
        const float iconPx = side * (16.4f / 26.f);
        const float ox = (min.x + max.x - iconPx) * .5f;
        const float oy = (min.y + max.y - iconPx) * .5f;
        const float u = iconPx / 16.f;
        const float stroke = std::max(1.15f, side * (1.65f / 26.f));
        auto P = [&](float x, float y) { return ImVec2{ox + x * u, oy + y * u}; };
        auto line = [&](float x0, float y0, float x1, float y1, float width) {
            const ImVec2 a = P(x0, y0), b = P(x1, y1);
            dl->AddLine(a, b, color, width);
            dl->AddCircleFilled(a, width * .5f, color, 12);
            dl->AddCircleFilled(b, width * .5f, color, 12);
        };
        if (tool == 0) {
            const float cx = 6.05f, cy = 6.05f, r = 3.95f;
            dl->AddCircle(P(cx, cy), r * u, color, 32, stroke);
            const float a = 0.78539816339f;
            const float start = r + (stroke / u) * .45f;
            line(cx + std::cos(a) * start, cy + std::sin(a) * start, 14.15f, 14.15f, stroke);
        } else if (tool == 1) {
            auto arrow = [&](float tx, float ty, float dx, float dy) {
                const float shaft = 2.15f, head = 2.7f, wing = 1.85f;
                line(8.f + dx * shaft, 8.f + dy * shaft, tx - dx * head, ty - dy * head, stroke);
                const float nx = -dy, ny = dx;
                line(tx, ty, tx - dx * head + nx * wing, ty - dy * head + ny * wing, stroke);
                line(tx, ty, tx - dx * head - nx * wing, ty - dy * head - ny * wing, stroke);
            };
            arrow(14.7f, 8.f, 1.f, 0.f);
            arrow(1.3f, 8.f, -1.f, 0.f);
            arrow(8.f, 1.3f, 0.f, -1.f);
            arrow(8.f, 14.7f, 0.f, 1.f);
        } else if (tool == 2) {
            const float cx = 8.f, cy = 8.05f, r = 4.85f;
            const float a0 = 0.85f, a1 = 5.35f;
            dl->PathClear();
            dl->PathArcTo(P(cx, cy), r * u, a0, a1 - .42f, 28);
            dl->PathStroke(color, 0, stroke);
            dl->AddCircleFilled(P(cx + std::cos(a0) * r, cy + std::sin(a0) * r), stroke * .48f, color, 12);
            const float tx = -std::sin(a1), ty = std::cos(a1);
            const float ex = cx + std::cos(a1) * r, ey = cy + std::sin(a1) * r;
            const float head = 2.7f, wing = 1.75f, nx = -ty, ny = tx;
            line(ex, ey, ex - tx * head + nx * wing, ey - ty * head + ny * wing, stroke);
            line(ex, ey, ex - tx * head - nx * wing, ey - ty * head - ny * wing, stroke);
            dl->AddCircleFilled(P(cx, cy), .95f * u, color, 12);
        } else {
            const float nearX = 3.15f, farX = 13.7f;
            line(nearX, 5.35f, nearX, 10.65f, stroke);
            line(farX, 2.15f, farX, 13.85f, stroke);
            line(nearX, 5.35f, farX, 2.15f, stroke);
            line(nearX, 10.65f, farX, 13.85f, stroke);
        }
    }
    // Shared chrome for the four viewport tools: one rounded tray, hairline
    // dividers, and a pale fill on the active tool. `hits` is filled by the
    // invisible buttons so hover/active are known before anything is painted.
    struct NavHit {
        ImVec2 min{}, max{};
        bool pressed = false, hovered = false, held = false;
    };
    NavHit navToolHit(const char *name, const char *tip, float square) {
        ImGui::PushID(name);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        NavHit hit;
        hit.pressed = ImGui::Button("##nav", {square, square});
        hit.hovered = ImGui::IsItemHovered();
        hit.held = ImGui::IsItemActive();
        hit.min = ImGui::GetItemRectMin();
        hit.max = ImGui::GetItemRectMax();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        if (hit.hovered) ImGui::SetTooltip("%s", tip);
        recordUiTestItem(name);
        return hit;
    }
    void paintViewportNav(const NavHit hits[4]) const {
        const bool light = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x > .5f;
        const ImU32 border = light ? IM_COL32(176, 186, 198, 255) : IM_COL32(86, 94, 106, 255);
        const ImU32 tray = light ? IM_COL32(255, 255, 255, 255) : IM_COL32(32, 36, 42, 255);
        const ImU32 divider = light ? IM_COL32(210, 216, 224, 255) : IM_COL32(70, 78, 90, 255);
        const ImU32 ink = light ? IM_COL32(42, 50, 62, 255) : IM_COL32(230, 234, 240, 255);
        const ImU32 sink = light ? IM_COL32(10, 70, 132, 255) : IM_COL32(176, 214, 255, 255);
        const float rad = U(6.f);
        const float edge = std::max(1.f, U(1.f));
        const ImVec2 origin = hits[0].min;
        const ImVec2 outerMax = hits[3].max;
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, outerMax, border, rad);
        dl->AddRectFilled({origin.x + edge, origin.y + edge}, {outerMax.x - edge, outerMax.y - edge},
                          tray, std::max(0.f, rad - edge));
        auto shadeOf = [&](bool selected, bool hovered, bool held) {
            if (light) {
                if (selected && held) return IM_COL32(186, 214, 240, 255);
                if (selected && hovered) return IM_COL32(196, 220, 242, 255);
                if (selected) return IM_COL32(206, 226, 246, 255);
                if (held) return IM_COL32(220, 232, 244, 255);
                return IM_COL32(232, 241, 250, 255);
            }
            if (selected && held) return IM_COL32(28, 70, 118, 255);
            if (selected) return IM_COL32(38, 84, 132, 255);
            return IM_COL32(48, 58, 72, 255);
        };
        for (int i = 1; i < 4; ++i) {
            const bool leftHot = (viewportTool == i - 1) || hits[i - 1].hovered;
            const bool rightHot = (viewportTool == i) || hits[i].hovered;
            if (leftHot || rightHot) continue;
            const float x = hits[i].min.x;
            dl->AddRectFilled({x - edge * .5f, origin.y + U(6.f)},
                              {x + edge * .5f, outerMax.y - U(6.f)}, divider);
        }
        for (int i = 0; i < 4; ++i) {
            const bool selected = viewportTool == i;
            if (!selected && !hits[i].hovered) continue;
            ImVec2 a = hits[i].min, b = hits[i].max;
            a.y += edge;
            b.y -= edge;
            if (i == 0) a.x += edge;
            if (i == 3) b.x -= edge;
            const ImDrawFlags corners = i == 0   ? ImDrawFlags_RoundCornersLeft
                                        : i == 3 ? ImDrawFlags_RoundCornersRight
                                                 : ImDrawFlags_RoundCornersNone;
            dl->AddRectFilled(a, b, shadeOf(selected, hits[i].hovered, hits[i].held),
                              std::max(0.f, rad - edge), corners);
        }
        for (int i = 0; i < 4; ++i)
            drawViewportNavIcon(dl, hits[i].min, hits[i].max, i,
                                viewportTool == i ? sink : ink);
    }
    // Compact flat icon button in the OVITO style. Renders the Segoe MDL2
    // glyph when the icon font loaded and carries the specific glyph; falls
    // back to the historical text label (never a blank button) otherwise. The
    // stable UI-test name, the tooltip and the action are identical in both
    // modes. highlight renders the light-blue selected background used by
    // OVITO's light theme: a pale tint with a dark glyph, so a toggled tool
    // reads as selected rather than pressed.
    bool iconButton(const char *name, unsigned glyph, const char *fallbackText,
                    const char *tip, bool highlight = false, float square = 0,
                    const char *recordName = nullptr) {
        char label[8];
        const bool useGlyph = glyphAvailable(glyph);
        if (useGlyph)
            glyphUtf8(glyph, label);
        else
            snprintf(label, sizeof(label), "%s", fallbackText);
        ImGui::PushID(name);
        if (highlight) {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.82f, 0.90f, 0.98f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.74f, 0.85f, 0.96f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.66f, 0.80f, 0.94f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {0.05f, 0.25f, 0.45f, 1.f});
        }
        const ImVec2 size = square > 0 ? ImVec2{square, square} : ImVec2{0, 0};
        const bool pressed = ImGui::Button(label, size);
        if (highlight) ImGui::PopStyleColor(4);
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        recordUiTestItem(recordName ? recordName : name);
        return pressed;
    }
    // Case-insensitive substring match used by the Data inspector filters:
    // a row shows whenever its rendered text contains the filter text.
    static bool inspectorFilterMatch(const std::string &text, const char *needleText) {
        auto lower = [](char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
        const std::string needle = needleText;
        if (needle.empty()) return true;
        if (needle.size() > text.size()) return false;
        for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
            size_t j = 0;
            while (j < needle.size() && lower(text[i + j]) == lower(needle[j])) ++j;
            if (j == needle.size()) return true;
        }
        return false;
    }
    // Rebuilds a filtered row-index list only when the filter text or the row
    // count changed; `render` appends the row's rendered text for matching.
    template <typename Render>
    void refreshInspectorFilter(const char *text, size_t rowCount, std::string &cachedText,
                                size_t &cachedRows, std::vector<uint32_t> &matches,
                                Render render) {
        if (cachedText == text && cachedRows == rowCount) return;
        cachedText = text;
        cachedRows = rowCount;
        matches.clear();
        if (!text[0]) return;
        matches.reserve(rowCount);
        std::string rowText;
        for (size_t i = 0; i < rowCount; ++i) {
            rowText.clear();
            render(i, rowText);
            if (inspectorFilterMatch(rowText, text)) matches.push_back(uint32_t(i));
        }
    }
    // The OVITO-style inspector tool row shared by the table pages: a filter
    // field on the left and the row count on the right edge of the same line.
    void inspectorToolRow(const char *id, char *filterText, size_t shown, size_t total,
                          const char *noun, const char *suffix = "") {
        (void)total;
        char count[160];
        snprintf(count, sizeof(count), "%s %s%s", number(shown).c_str(), noun, suffix);
        ImGui::SetNextItemWidth(U(220));
        ImGui::InputTextWithHint(id, "Filter...", filterText, 128);
        recordUiTestItem(std::string("inspector.filter.") + (id + 2));
        ImGui::SameLine();
        const float textWidth = ImGui::CalcTextSize(count).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > textWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - textWidth);
        ImGui::TextUnformatted(count);
        recordUiTestItem(std::string("inspector.count.") + noun);
    }
    // Small filled color square rendered before type/bond-type text cells.
    void colorSwatch(const std::array<float, 4> &color) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float size = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, {p.x + size, p.y + size},
            ImGui::GetColorU32({color[0], color[1], color[2], color[3]}));
        ImGui::Dummy({size + U(6), size});
    }
    std::array<float, 4> typeColor(size_t type) const {
        if (type < gpu.styles.size()) return gpu.styles[type].color;
        return {.76f, .57f, .38f, 1.f};
    }

    // Subtle vertical separator drawn between toolbar button groups.
    void toolbarSeparator(float rowHeight) {
        ImGui::SameLine();
        const auto p = ImGui::GetCursorScreenPos();
        auto *draw = ImGui::GetWindowDrawList();
        const float height = U(16);
        const float y = (rowHeight - height) * .5f;
        draw->AddLine({p.x + U(2), p.y + y}, {p.x + U(2), p.y + y + height},
                      ImGui::GetColorU32(ImGuiCol_Separator), U(1));
        ImGui::Dummy({U(5), 0});
        ImGui::SameLine();
    }
    bool usingIconFont() const { return iconFont != nullptr; }
    // ---- P12 redesign system widgets -------------------------------------
    // Section caption: 10px-equivalent uppercase letter-spaced label used by
    // the redesign panels (MODIFICATIONS / VISUAL ELEMENTS / RANGE / ...).
    void sectionLabel(const char *text, const char *rightText = nullptr) {
        auto *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetTextLineHeight();
        dl->AddText(p, ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        if (rightText && rightText[0]) {
            const ImVec2 ts = ImGui::CalcTextSize(rightText);
            const float avail = ImGui::GetContentRegionAvail().x;
            dl->AddText({p.x + ImGui::CalcTextSize(text).x + U(8) + std::max(0.f, avail - ts.x), p.y},
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), rightText);
        }
        ImGui::Dummy({ImGui::GetContentRegionAvail().x, height});
    }
    // Scrub bar: the 24px control well (#111316, hairline border) with an
    // amber fill and right-aligned value, cursor ew-resize. Drag or click
    // sets the value; replaces the old sliders for the six redesign controls
    // (radius scale, cell line width, slice distance/slab width, color-code
    // start/end) while operating on the SAME state variables.
    bool scrubBar(const char *id, double *value, double mn, double mx,
                  const char *format, const char *unit, float width = 0,
                  const char *leftCaption = nullptr) {
        if (width > 0) ImGui::SetNextItemWidth(width);
        ImGui::PushID(id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = U(24);
        const float w = width > 0 ? width : std::max(U(40), ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton("##scrub", {w, h});
        const bool hovered = ImGui::IsItemHovered();
        const bool itemActive = ImGui::IsItemActive();
        bool changed = false;
        if (itemActive) {
            const double t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / std::max(w, 1.f), 0.f, 1.f);
            const double next = mn + t * (mx - mn);
            if (std::abs(next - *value) > 1e-9) { *value = next; changed = true; }
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, ImGui::GetColorU32(ImGuiCol_FrameBg), U(3));
        dl->AddRect(p, {p.x + w, p.y + h},
                    ImGui::GetColorU32(hovered ? ImGuiCol_TextDisabled : ImGuiCol_Border), U(3));
        const double t = mx > mn ? std::clamp((*value - mn) / (mx - mn), 0.0, 1.0) : 0.0;
        if (t > 0)
            dl->AddRectFilled(p, {p.x + float(t * w), p.y + h}, IM_COL32(240, 164, 49, 36), U(3),
                              ImDrawFlags_RoundCornersLeft);
        if (t > 0 && t < 1)
            dl->AddLine({p.x + float(t * w), p.y}, {p.x + float(t * w), p.y + h},
                        IM_COL32(240, 164, 49, 204));
        char text[64];
        snprintf(text, sizeof(text), format, *value);
        if (unit && unit[0])
            snprintf(text + strlen(text), sizeof(text) - strlen(text), " %s", unit);
        const float textY = p.y + (h - ImGui::GetTextLineHeight()) * .5f;
        if (leftCaption && leftCaption[0])
            dl->AddText({p.x + U(8), textY}, ImGui::GetColorU32(ImGuiCol_TextDisabled), leftCaption);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText({p.x + w - ts.x - U(8), textY}, ImGui::GetColorU32(ImGuiCol_Text), text);
        if (hovered && !active) ImGui::SetTooltip("%s", id + 2); // id is "##label"
        ImGui::PopID();
        return changed;
    }
    bool scrubBar(const char *id, float *value, float mn, float mx,
                  const char *format, const char *unit, float width = 0,
                  const char *leftCaption = nullptr) {
        double v = *value;
        if (scrubBar(id, &v, mn, mx, format, unit, width, leftCaption)) {
            *value = float(v);
            return true;
        }
        return false;
    }
    // Segmented pill control: well container with flex segments; the selected
    // segment gets #2d3238 fill and amber text. Returns the selected index.
    int segmented(const char *id, int currentValue, std::initializer_list<const char *> labels,
                  float width = 0, const char **recordedNames = nullptr) {
        ImGui::PushID(id);
        if (width > 0) ImGui::SetNextItemWidth(width);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = width > 0 ? width : std::max(U(60), ImGui::GetContentRegionAvail().x);
        const float h = U(24);
        const int count = int(labels.size());
        const float pad = U(2);
        const float segment = (w - pad * 2) / std::max(count, 1);
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, ImGui::GetColorU32(ImGuiCol_FrameBg), U(3));
        dl->AddRect(p, {p.x + w, p.y + h}, ImGui::GetColorU32(ImGuiCol_Border), U(3));
        int pickedResult = currentValue;
        int index = 0;
        for (const char *label : labels) {
            const ImVec2 a{p.x + pad + segment * index, p.y + pad};
            const ImVec2 b{p.x + pad + segment * (index + 1), p.y + h - pad};
            const bool selected = index == currentValue;
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(index);
            if (ImGui::InvisibleButton("##seg", {segment, b.y - a.y})) pickedResult = index;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (selected || hovered)
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_TabSelected), U(2));
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText({(a.x + b.x - ts.x) * .5f, (a.y + b.y - ts.y) * .5f},
                        ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_TextDisabled),
                        label);
            if (recordedNames && recordedNames[index]) recordUiTestItem(recordedNames[index], label);
            ++index;
        }
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ImGui::Dummy({w, 0});
        ImGui::PopID();
        return pickedResult;
    }
    // Toggle switch (30x16 rounded, amber when on) used in the inspector
    // header for toggleable nodes.
    bool toggleSwitch(const char *id, bool *value) {
        ImGui::PushID(id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = U(30), h = U(16);
        ImGui::InvisibleButton("##switch", {w, h});
        if (ImGui::IsItemClicked(0)) *value = !*value;
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h},
                          ImGui::GetColorU32(*value ? ImGuiCol_NavCursor : ImGuiCol_Tab), U(8));
        const float knob = h - U(4);
        dl->AddCircleFilled({p.x + (*value ? w - U(2) - knob * .5f : U(2) + knob * .5f),
                             p.y + h * .5f},
                            knob * .5f, ImGui::GetColorU32(ImGuiCol_WindowBg), 12);
        const bool changed = ImGui::IsItemClicked(0);
        ImGui::PopID();
        return changed;
    }
    // 2-letter mono modifier badge with hairline border; amber when selected.
    void monoBadge(const char *text, bool selected) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = U(24), h = U(15);
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, 0, U(2));
        dl->AddRect(p, {p.x + w, p.y + h},
                    ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_Tab, selected ? 1.f : 1.f),
                    U(2));
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText({p.x + (w - ts.x) * .5f, p.y + (h - ts.y) * .5f},
                    ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_TextDisabled), text);
        ImGui::Dummy({w + U(2), h});
    }
    void control(const char *id, int kind, const char *tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
        if (ImGui::Button(id, {U(42),U(30)})) {
            if (kind == 0) ShowWindow(window, SW_MINIMIZE);
            if (kind == 1) ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            if (kind == 2) desktop::hide(window);
            if (kind == 3) PostQuitMessage(0);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        auto p = ImGui::GetItemRectMin(); auto *d = ImGui::GetWindowDrawList();
        ImVec2 c{p.x+U(21),p.y+U(15)};
        auto color = ImGui::GetColorU32(kind == 3 ? ImVec4{.94f,.40f,.40f,1} : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (kind == 0) d->AddLine({c.x-6,c.y+3},{c.x+6,c.y+3},color,1.5f);
        if (kind == 1) {
            if (IsZoomed(window)) d->AddRect({c.x-3,c.y-7},{c.x+7,c.y+3},color);
            d->AddRect({c.x-6,c.y-4},{c.x+4,c.y+6},color);
        }
        if (kind == 2) {
            d->AddLine({c.x-5,c.y-5},{c.x+5,c.y+5},color,1.5f);
            d->AddLine({c.x+5,c.y-5},{c.x-5,c.y+5},color,1.5f);
        }
        if (kind == 3) {
            d->PathArcTo(c,7,-.9f,4.04f,24); d->PathStroke(color,0,1.7f);
            d->AddLine({c.x,c.y-9},{c.x,c.y},color,1.7f);
        }
    }
    float pipelineWidth() const { return U(pipelinePane); }
    float workspaceWidth() const { return showWorkspace ? U(workspacePane) : 0.f; }
    float leftWidth() const { return workspaceWidth() + pipelineWidth(); }
    float rightWidth() const { return U(propertiesPane); }
    float statusHeight() const { return U(24); }     // P12 bottom status strip
    float titleBarHeight() const { return U(42); }
    float toolBarHeight() const { return U(34); }
    float topInset() const { return titleBarHeight() + (homeMode ? 0.f : creationMode ? U(94) : titleBarHeight() + toolBarHeight()); }
    void drawCreationIcon(ImDrawList *draw, ImVec2 center, std::string_view name, ImU32 color) {
        auto point=[&](float x,float y) { return ImVec2{center.x+U(x-12),center.y+U(y-12)}; };
        auto line=[&](float x,float y,float a,float b) {
            draw->AddLine(point(x,y),point(a,b),color,U(1.8f));
        };
        auto circle=[&](float x,float y,float r) {
            draw->AddCircle(point(x,y),U(r),color,20,U(1.8f));
        };
        auto rect=[&](float x,float y,float a,float b) {
            draw->AddRect(point(x,y),point(a,b),color,U(1),0,U(1.8f));
        };
        if (name=="undo"||name=="redo") {
            const bool flip=name=="redo";
            const float a=flip?19:5,b=flip?5:19;
            line(a,11,flip?14:10,6); line(a,11,flip?14:10,16);
            line(a,11,b,11); circle(flip?11:13,15,5);
        } else if (name=="select") {
            ImVec2 pts[]={point(5,3),point(19,11),point(12,13),point(9,20)};
            draw->AddPolyline(pts,4,color,ImDrawFlags_Closed,U(1.8f));
        } else if (name=="rotate") {
            circle(12,12,8); line(19,4,20,9); line(20,9,15,9);
        } else if (name=="zoom") {
            circle(10,10,6);line(15,15,21,21);line(7,10,13,10);line(10,7,10,13);
        } else if (name=="pan") {
            line(12,2,12,22);line(2,12,22,12);
            line(12,2,9,5);line(12,2,15,5);line(12,22,9,19);line(12,22,15,19);
            line(2,12,5,9);line(2,12,5,15);line(22,12,19,9);line(22,12,19,15);
        } else if (name=="move") {
            circle(12,12,3.5f);line(12,1,12,6);line(12,18,12,23);
            line(1,12,6,12);line(18,12,23,12);
        } else if (name=="draw") {
            line(4,19,17,5);line(17,5,21,9);line(21,9,8,22);line(8,22,4,22);
            line(4,22,4,19);line(14,8,18,12);
        } else if (name=="fragment") {
            line(7,8,16,6); line(7,8,11,18); line(11,18,20,15);
            circle(7,8,3); circle(16,6,2.5f); circle(11,18,3); circle(20,15,2.5f);
        } else if (name=="ring") {
            ImVec2 vertices[6];
            for (int i=0;i<6;++i) {
                const float angle=float(i*authoring::kPi/3);
                vertices[i]=point(12+9*std::cos(angle),12+9*std::sin(angle));
            }
            draw->AddPolyline(vertices,6,color,ImDrawFlags_Closed,U(1.8f));
        } else if (name=="delete") {
            line(4,7,20,7);line(7,7,8,20);line(8,20,16,20);line(16,20,17,7);
            line(9,7,9,4);line(9,4,15,4);line(15,4,15,7);line(10,11,10,17);line(14,11,14,17);
        } else if (name=="hydrogen") {
            draw->AddText(point(3,3),color,"H+");
        } else if (name=="auto-hydrogen") {
            line(5,6,5,17);line(11,6,11,17);line(5,11,11,11);
            line(15,4,20,7);line(20,7,20,15);line(20,15,16,19);line(16,19,12,19);
            line(16,15,16,19);line(16,19,20,19);
        } else if (name=="clean") {
            line(12,3,14,9);line(14,9,20,11);line(20,11,14,13);
            line(14,13,12,19);line(12,19,10,13);line(10,13,4,11);line(4,11,10,9);line(10,9,12,3);
        } else if (name=="distance") {
            line(3,12,21,12);line(3,8,3,16);line(21,8,21,16);
            line(8,10,8,14);line(12,9,12,15);line(16,10,16,14);
        } else if (name=="angle") {
            line(4,20,20,20);line(4,20,15,5);circle(4,20,3);
        } else if (name=="torsion") {
            line(3,18,8,7);line(8,7,16,17);line(16,17,21,6);circle(3,18,1);circle(21,6,1);
        } else if (name=="ball"||name=="fill") {
            circle(8,8,4);circle(17,16,4);
            if (name=="ball") line(10,10,15,14);
        } else if (name=="stick") {
            line(5,19,19,5);line(5,11,13,19);
        } else if (name=="cell") {
            line(4,7,12,3);line(12,3,20,7);line(20,7,20,17);
            line(20,17,12,21);line(12,21,4,17);line(4,17,4,7);
            line(4,7,12,11);line(12,11,20,7);line(12,11,12,21);
        } else if (name=="bond") {
            circle(5,12,2.5f);circle(19,12,2.5f);line(8,12,16,12);
        } else if (name=="crystal") {
            rect(4,4,20,20);line(4,12,20,12);line(12,4,12,20);
            for (float x:{8.f,16.f}) for (float y:{8.f,16.f}) circle(x,y,.7f);
        } else if (name=="supercell") {
            rect(3,3,11,11);rect(13,3,21,11);rect(3,13,11,21);rect(13,13,21,21);
        } else if (name=="surf") {
            line(3,21,21,21);line(3,17,21,17);line(3,13,21,13);
            line(12,3,12,9);line(12,3,9,6);line(12,3,15,6);
        } else if (name=="home") {
            line(3,11,12,3);line(12,3,21,11);line(5,9,5,20);line(5,20,19,20);line(19,20,19,9);
        } else if (name=="fit") {
            line(3,9,3,3);line(3,3,9,3);line(21,9,21,3);line(21,3,15,3);
            line(3,15,3,21);line(3,21,9,21);line(21,15,21,21);line(21,21,15,21);
        }
    }
    void browserTabStrip(float w) {
        const float titleH = titleBarHeight();
        fixed("Creation tabs", 0, 0, w, titleH);
        ImGui::SetCursorPosY(U(3));
        if (logo.Get()) ImGui::Image((ImTextureID)(intptr_t)logo.Get(), {U(30), U(30)});
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("AtomX  %s", atomxVersion);
        ImGui::SameLine(0, U(16));
        const float tabStart=ImGui::GetCursorPosX();
        const float tabWidth=std::max(U(180),w-tabStart-U(265));
        ImGui::BeginChild("##browser-tab-scroll",{tabWidth,titleH-U(2)},ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPosY(U(3));
        int closing = -1;
        for (int index = 0; index < int(tabs.size()); ++index) {
            ImGui::PushID(index);
            const bool selected = index == activeTab;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, {0.102f, 0.114f, 0.125f, 1.f});
            const std::string name = selected ? tabTitle() : tabs[size_t(index)].title;
            const std::string label = name;
            if (ImGui::Button(label.c_str(), {U(212), U(32)})) switchTab(index);
            recordUiTestItem("tabs.tab."+std::to_string(index),label.c_str());
            if (selected && displayedTab != activeTab) {
                ImGui::SetScrollHereX(.5f);
                displayedTab=activeTab;
            }
            const bool isHome=selected?homeMode:tabs[size_t(index)].home;
            const bool isCreation=selected?creationMode:tabs[size_t(index)].creationMode;
            if (!isHome) {
                const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddCircleFilled({lo.x+U(12),(lo.y+hi.y)*.5f},U(4),
                    isCreation?IM_COL32(70,225,134,255):IM_COL32(245,173,53,255));
            }
            if (selected) ImGui::PopStyleColor();
            ImGui::SameLine(0, U(2));
            if (ImGui::SmallButton("x")) closing = index;
            ImGui::SameLine(0, U(5));
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+")) newHomeTab();
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel!=0)
            ImGui::SetScrollX(ImGui::GetScrollX()-ImGui::GetIO().MouseWheel*U(170));
        ImGui::EndChild();
        ImGui::SetCursorPos({w-U(252),U(5)});
        if (ImGui::InvisibleButton("##all-tabs",{U(36),U(30)})) ImGui::OpenPopup("##all-tabs-popup");
        {
            const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();
            auto *draw=ImGui::GetWindowDrawList();
            draw->AddRectFilled(lo,hi,ImGui::IsItemHovered()?IM_COL32(43,48,55,255):IM_COL32(28,32,37,255),U(5));
            const ImVec2 center{(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f};
            draw->AddLine({center.x-U(5),center.y-U(2)}, {center.x,center.y+U(3)},IM_COL32(190,199,211,255),U(1.7f));
            draw->AddLine({center.x,center.y+U(3)}, {center.x+U(5),center.y-U(2)},IM_COL32(190,199,211,255),U(1.7f));
        }
        recordUiTestItem("tabs.switcher","All tabs");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("所有标签页 · Ctrl+Tab 切换");
        if (ImGui::BeginPopup("##all-tabs-popup")) {
            for (int index=0;index<int(tabs.size());++index) {
                const auto &entry=tabs[size_t(index)];
                const std::string name=index==activeTab?tabTitle():entry.title;
                if (ImGui::Selectable(name.c_str(),index==activeTab)) switchTab(index);
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorPos({w-U(210),U(5)});
        control("##minimize", 0, "Minimize to taskbar"); ImGui::SameLine();
        control("##maximize", 1, "Maximize / restore"); ImGui::SameLine();
        control("##tray", 2, "Close to system tray"); ImGui::SameLine();
        control("##exit", 3, "Exit AtomX");
        ImGui::End();
        if (closing >= 0) closeTab(closing);
    }
    void creationTop(float w) {
        const float titleH = titleBarHeight();
        browserTabStrip(w);

        fixed("Creation menu", 0, titleH, w, U(38));
        ImGui::PushStyleColor(ImGuiCol_Button, {0.22f, 0.86f, 0.48f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_Text, {0.05f, 0.07f, 0.06f, 1.f});
        if (ImGui::Button("创作模式")) leaveCreationTab();
        recordUiTestItem("creation.return-view", "Creation mode badge");
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0, U(10));
        auto menu = [&](const char *caption, const char *id, auto &&items) {
            if (ImGui::Button(caption)) ImGui::OpenPopup(id);
            if (ImGui::BeginPopup(id)) { items(); ImGui::EndPopup(); }
            ImGui::SameLine(0, U(3));
        };
        menu("文件", "##create-file", [&] {
            if (ImGui::MenuItem("新建标签", "Ctrl+T")) newHomeTab();
            if(ImGui::MenuItem("新建 3D 原子结构"))newBlankCreation();
            if (ImGui::MenuItem("打开结构...", "Ctrl+O")) open();
            if (ImGui::MenuItem("保存文档", "Ctrl+S")) saveSessionState(false);
            if (ImGui::MenuItem("文档另存为...", "Ctrl+Shift+S")) saveSessionState(true);
            ImGui::Separator();
            if(ImGui::MenuItem("保存整个工作区..."))projectDialog(true);
            if(ImGui::MenuItem("追加打开工作区..."))projectDialog(false);
            if (ImGui::MenuItem("导出结构...", "Ctrl+E")) showDataExport = true;
            if (ImGui::MenuItem("关闭创作标签")) leaveCreationTab();
        });
        menu("编辑", "##create-edit", [&] {
            if (ImGui::MenuItem("撤销", "Ctrl+Z")) history(false);
            if (ImGui::MenuItem("重做", "Ctrl+Y")) history(true);
            ImGui::Separator();
            const bool selected=!creationSelection.empty() || !creationBondSelection.empty();
            if(ImGui::MenuItem("剪切", "Ctrl+X",false,selected))copyCreationSelection(true);
            if(ImGui::MenuItem("复制", "Ctrl+C",false,selected))copyCreationSelection();
            if(ImGui::MenuItem("粘贴", "Ctrl+V",false,IsClipboardFormatAvailable(structureClipboardFormat())!=FALSE))pasteCreationSelection();
            if (ImGui::MenuItem("删除选中", "Del")) deletePickedAtom();
        });
        menu("视图", "##create-view", [&] {
            if (ImGui::MenuItem("晶胞边框", nullptr, cell)) cell = !cell;
            if (ImGui::MenuItem("化学键", nullptr, result.data.bondStyle.visible)) {
                source.bondStyle.visible = !source.bondStyle.visible; update();
            }
            if (ImGui::MenuItem("显示样式...")) requestCreationStyles();
            if (ImGui::MenuItem("适应视窗")) fitCamera(3, false);
            if(ImGui::MenuItem("选中居中",nullptr,false,!creationSelection.empty() || !creationBondSelection.empty()))centerCreationSelection();
            if(ImGui::MenuItem("持续缩放",nullptr,creationTool==CreationTool::Zoom))chooseCreationTool(CreationTool::Zoom);
            if(ImGui::MenuItem("逆时针旋转 90°"))cameras[3].roll=std::remainder(cameras[3].roll+DirectX::XM_PIDIV2,DirectX::XM_2PI);
            if(ImGui::MenuItem("顺时针旋转 90°"))cameras[3].roll=std::remainder(cameras[3].roll-DirectX::XM_PIDIV2,DirectX::XM_2PI);
            if(ImGui::BeginMenu("标准视角")) {
                const char *names[]={"顶视图","底视图","前视图","后视图","左视图","右视图","正交","透视"};
                for(int mode=0;mode<8;++mode)if(ImGui::MenuItem(names[mode],nullptr,cameras[3].mode==mode))setCreationView(mode);
                ImGui::EndMenu();
            }
            ImGui::MenuItem("体系属性栏",nullptr,&creationPropertiesOpen);
        });
        menu("修改", "##create-modify", [&] {
            if(ImGui::MenuItem("原子数值属性...",nullptr,false,!creationSelection.empty()))requestAtomProperties();
            if(ImGui::MenuItem("原子模拟约束...",nullptr,false,!creationSelection.empty()))requestAtomConstraints();
            if(ImGui::MenuItem("测量模拟约束...",nullptr,false,!creationDisplay.monitors.empty()))requestMeasurementConstraints();
            if(ImGui::MenuItem("原子化学设置...",nullptr,false,!creationSelection.empty()))requestCreationChemistry();
            if (ImGui::MenuItem("运动分组...")) requestMotionGroups();
            if (ImGui::MenuItem("替换元素")) replacePickedElement();
            if (ImGui::MenuItem("删除 / 空位")) deletePickedAtom();
            if (ImGui::MenuItem("调整氢原子…")) addHydrogensCommand();
            ImGui::MenuItem("自动更新氢",nullptr,&creationAutoHydrogens);
            if (ImGui::MenuItem("几何优化")) cleanGeometryCommand();
        });
        menu("构建", "##create-build", [&] {
            if (ImGui::MenuItem("片段浏览器...")) { requestFragmentLibrary(); showFragmentBrowser=true; }
            if (ImGui::MenuItem("定义片段...")) defineFragment();
            if (ImGui::BeginMenu("绘制碳环")) {
                for (int size=4;size<=6;++size) {
                    const auto caption=std::to_string(size)+" 元环";
                    if (ImGui::MenuItem(caption.c_str(),nullptr,creationTool==CreationTool::Ring && creationRingSize==size)) {
                        creationRingSize=size; chooseCreationTool(CreationTool::Ring);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("建晶体...")) openCrystalDialog = true;
            if (ImGui::MenuItem("超胞...")) openSupercellDialog = true;
            if (ImGui::MenuItem("切面·真空...")) openSurfaceDialog = true;
            if (ImGui::MenuItem("构建叠层...")) requestLayerBuilder();
            recordUiTestItem("creation.layers-open");
        });
        menu("工具", "##create-tools", [&] {
            if(ImGui::MenuItem("测量 / 修改距离")) chooseCreationTool(CreationTool::Distance);
            if(ImGui::MenuItem("测量 / 修改角度")) chooseCreationTool(CreationTool::Angle);
            if(ImGui::MenuItem("测量 / 修改扭转角")) chooseCreationTool(CreationTool::Torsion);
            ImGui::Separator();
            if (ImGui::MenuItem("重置视角")) resetView();
            if (ImGui::MenuItem("适应视窗")) fitCamera(3, false);
        });
        menu("窗口", "##create-window", [&] {
            for(int index=0;index<int(tabs.size());++index){ImGui::PushID(index);
                if(ImGui::MenuItem(tabs[size_t(index)].title.c_str(),nullptr,index==activeTab))switchTab(index);ImGui::PopID();}
            if(ImGui::MenuItem("关闭当前标签", "Ctrl+W"))closeTab(activeTab);
        });
        menu("帮助", "##create-help", [&] {
            if(ImGui::MenuItem("用户手册", "F1"))openUrl("https://github.com/WhiteCrosstheRiver/AtomX#readme");
            if(ImGui::MenuItem("系统信息"))showSystemInfo=true;
            if(ImGui::MenuItem("关于 AtomX"))showAbout=true;
        });
        const std::string base = "基于 " + creationBasedOn;
        const float textWidth = ImGui::CalcTextSize(base.c_str()).x;
        if (ImGui::GetCursorPosX() + textWidth < w - U(14)) {
            ImGui::SetCursorPosX(w - textWidth - U(14));
            ImGui::TextDisabled("%s", base.c_str());
        }
        ImGui::End();

        fixed("Creation tool strip", 0, titleH + U(38), w, U(56));
        ImGui::SetCursorPosY(U(7));
        auto icon = [&](const char *id, unsigned codepoint, const char *fallback, const char *tip,
                        ImVec4 tone, bool selected, auto &&action) {
            (void)codepoint; (void)fallback;
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##icon", {U(35), U(35)});
            recordUiTestItem(std::string("creation.tool-")+id);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            const ImVec4 bg = selected ? tone : ImVec4{tone.x * (hovered ? .30f : .14f),
                tone.y * (hovered ? .30f : .14f), tone.z * (hovered ? .30f : .14f), 1.f};
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(lo, hi, ImGui::GetColorU32(bg), U(10));
            draw->AddRect(lo, hi, ImGui::GetColorU32(ImVec4{tone.x*.48f,tone.y*.48f,tone.z*.48f,1}), U(10), 0, U(1));
            if (selected) draw->AddRect({lo.x-U(2),lo.y-U(2)}, {hi.x+U(2),hi.y+U(2)},
                ImGui::GetColorU32(tone), U(12), 0, U(1.5f));
            drawCreationIcon(draw, {(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f}, id,
                selected ? IM_COL32(12,18,21,255) : ImGui::GetColorU32(tone));
            if (pressed) action();
            if (hovered) ImGui::SetTooltip("%s", tip);
            ImGui::PopID();
            ImGui::SameLine(0, U(4));
        };
        auto divider = [&] { toolbarSeparator(U(35)); };
        const ImVec4 gray{.62f,.67f,.73f,1}, cyan{.11f,.65f,.95f,1}, green{.20f,.86f,.49f,1},
            orange{.95f,.57f,.18f,1}, red{.95f,.25f,.30f,1}, gold{.95f,.67f,.14f,1},
            violet{.72f,.53f,1.f,1}, blue{.58f,.68f,1.f,1};
        icon("undo",0xE7A7,"<","Undo",gray,false,[&]{history(false);});
        icon("redo",0xE7A6,">","Redo",gray,false,[&]{history(true);}); divider();
        icon("select",0xE7C9,"S","Select atoms",cyan,creationTool==CreationTool::Select,[&]{chooseCreationTool(CreationTool::Select);});
        icon("rotate",0xE7AD,"R","Rotate view (X/Y/Z constrain axis; right drag near edge rolls screen)",cyan,creationTool==CreationTool::Rotate,[&]{chooseCreationTool(CreationTool::Rotate);});
        icon("pan",0xE72A,"P","Pan view",green,creationTool==CreationTool::Pan,[&]{chooseCreationTool(CreationTool::Pan);});
        icon("zoom",0,"Z","拖动持续缩放",blue,creationTool==CreationTool::Zoom,[&]{chooseCreationTool(CreationTool::Zoom);});
        icon("move",0xE8AB,"M","Move selected atoms",orange,creationTool==CreationTool::Move,[&]{chooseCreationTool(CreationTool::Move);});
        icon("draw",0xE70F,"+","Draw atoms",green,creationTool==CreationTool::Sketch,[&]{chooseCreationTool(CreationTool::Sketch);});
        icon("ring",0,"6","绘制碳环 (4 / 5 / 6) · Alt 芳香环",green,creationTool==CreationTool::Ring,[&]{chooseCreationTool(CreationTool::Ring);});
        icon("fragment",0,"CH","片段浏览器 · 常用与自定义片段",green,creationTool==CreationTool::Fragment,[&]{
            requestFragmentLibrary(); chooseCreationTool(CreationTool::Fragment); showFragmentBrowser=true;
        }); divider();
        icon("delete",0xE74D,"X","Delete selected atom",red,false,[&]{deletePickedAtom();});
        icon("hydrogen",0xE8FA,"H+","调整氢原子 / Adjust Hydrogen",violet,false,[&]{addHydrogensCommand();});
        icon("auto-hydrogen",0,"H","自动更新氢 · 绘制 / 换元素 / 改键级 / 接环",violet,creationAutoHydrogens,[&]{creationAutoHydrogens=!creationAutoHydrogens;});
        icon("clean",0xE734,"*","Clean geometry",violet,false,[&]{cleanGeometryCommand();}); divider();
        icon("distance",0xE8A0,"D","测量 / 修改距离",gold,creationTool==CreationTool::Distance,[&]{chooseCreationTool(CreationTool::Distance);});
        icon("angle",0xE8B1,"A","测量 / 修改角度",gold,creationTool==CreationTool::Angle,[&]{chooseCreationTool(CreationTool::Angle);});
        icon("torsion",0,"T","测量 / 修改扭转角",gold,creationTool==CreationTool::Torsion,[&]{chooseCreationTool(CreationTool::Torsion);}); divider();
        icon("ball",0xE80F,"B","Ball and stick",blue,radius<.6f,[&]{setDisplayStyle(0);});
        icon("stick",0xE8A4,"I","Stick",blue,false,[&]{setDisplayStyle(2);});
        icon("fill",0xE8B7,"O","Space filling",blue,radius>=.6f,[&]{setDisplayStyle(1);}); divider();
        icon("cell",0xE8A7,"C","Show cell",cyan,cell,[&]{cell=!cell;});
        icon("bond",0xE8D7,"-","Show bonds",cyan,source.bondStyle.visible,[&]{source.bondStyle.visible=!source.bondStyle.visible;update();}); divider();
        auto buildButton=[&](const char *id,const char *caption,float width,bool primary,auto &&action) {
            ImGui::PushID(id);
            const bool pressed=ImGui::InvisibleButton("##build",{U(width),U(35)});
            const bool hovered=ImGui::IsItemHovered();
            const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();
            auto *draw=ImGui::GetWindowDrawList();
            const ImU32 fill=primary
                ? IM_COL32(155,110,244,255)
                : hovered?IM_COL32(55,35,87,255):IM_COL32(35,23,55,255);
            const ImU32 foreground=primary?IM_COL32(23,14,34,255):IM_COL32(189,145,255,255);
            draw->AddRectFilled(lo,hi,fill,U(9));
            draw->AddRect(lo,hi,primary?IM_COL32(171,134,255,255):IM_COL32(79,49,120,255),U(9));
            drawCreationIcon(draw,{lo.x+U(16),(lo.y+hi.y)*.5f},id,foreground);
            const auto textSize=ImGui::CalcTextSize(caption);
            draw->AddText({lo.x+U(32),(lo.y+hi.y-textSize.y)*.5f},foreground,caption);
            if (pressed) action();
            ImGui::PopID();
            ImGui::SameLine(0,U(4));
        };
        buildButton("crystal","建晶体",90,true,[&]{openCrystalDialog=true;});
        buildButton("supercell","超胞",76,false,[&]{openSupercellDialog=true;});
        buildButton("surf","切面·真空",116,false,[&]{openSurfaceDialog=true;});
        divider();
        icon("home",0xE80F,"H","Reset camera",gray,false,[&]{resetView();});
        icon("fit",0xE8AA,"F","Fit structure",gray,false,[&]{fitCamera(3,false);});
        ImGui::End();
    }
    void top(float w) {
        if (creationMode) { creationTop(w); return; }
        if (homeMode) { browserTabStrip(w); return; }
        const float titleH = titleBarHeight(), barH = toolBarHeight();
        browserTabStrip(w);
        fixed("Title", 0, titleH, w, titleH);
        ImGui::SetCursorPos({U(12),U(10)});
        // OVITO parity: File / Edit / Help drop-downs live in the title strip
        // next to the logo. Plain BeginMenu calls give the standard behavior
        // (click to open, hover to switch while open, click-away/Esc to
        // close) without BeginMainMenuBar, which cannot coexist with the
        // custom title bar. Disabled entries are honest about being planned.
        {
            auto enabledItem = [&](const char *label, const char *record,
                                   const char *shortcut = nullptr) {
                const bool pressed = ImGui::MenuItem(label, shortcut);
                recordUiTestItem(record, label);
                return pressed;
            };
            auto disabledItem = [&](const char *label, const char *record,
                                    const char *shortcut, const char *tip) {
                ImGui::BeginDisabled();
                ImGui::MenuItem(label, shortcut);
                ImGui::EndDisabled();
                recordUiTestItem(record, label);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tip);
            };
        // BeginMenu returns whether the popup is open (not "pressed"),
        // so the label records before the open branch. The three menus lay
        // out horizontally like a real menu bar: a plain window defaults to
        // the vertical menu layout (one full-width row per menu), so the
        // layout type is switched across just this group, exactly what
        // BeginMenuBar does for its own row.
        auto *titleWindow = ImGui::GetCurrentWindow();
        const ImGuiLayoutType savedLayout = titleWindow->DC.LayoutType;
        titleWindow->DC.LayoutType = ImGuiLayoutType_Horizontal;
            if (smokeMenuFile) ImGui::OpenPopup("File");
            const bool fileOpen = ImGui::BeginMenu("File");
            recordUiTestItem("menu.file", "File");
            if (fileOpen) {
                if(ImGui::MenuItem("New 3D Atomistic Structure"))newBlankCreation();
                if(ImGui::MenuItem("Save Workspace..."))projectDialog(true);
                if(ImGui::MenuItem("Append Workspace..."))projectDialog(false);
                if (enabledItem("New Tab", "menu.file.new-tab", "Ctrl+T"))
                    newHomeTab();
                if (enabledItem("Load File...", "menu.file.load-file", "Ctrl+I")) open();
                if (enabledItem("Load File in New Tab...", "menu.file.load-new-tab")) {
                    open();
                }
                disabledItem("Load Remote File", "menu.file.load-remote", "Ctrl+Shift+I",
                             "Not available");
                if (enabledItem("Export File...", "menu.file.export", "Ctrl+E"))
                    showDataExport = true;
                if (ImGui::BeginMenu("Recent Files")) {
                    if (preferences.recentFiles.empty()) {
                        ImGui::BeginDisabled();
                        ImGui::MenuItem("(no recent files)");
                        ImGui::EndDisabled();
                    } else {
                        int index = 0;
                        for (const auto &file : preferences.recentFiles) {
                            const auto record = "menu.file.recent." + std::to_string(index++);
                            if (enabledItem(utf8(file).c_str(), record.c_str())) openFileTab(file);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (enabledItem("Load Session State...", "menu.file.load-session", "Ctrl+O"))
                    open();
                if (enabledItem("Save Session State", "menu.file.save-session", "Ctrl+S"))
                    saveSessionState(false);
                if (enabledItem("Save Session State As...", "menu.file.save-session-as",
                                "Ctrl+Shift+S"))
                    saveSessionState(true);
                ImGui::Separator();
                disabledItem("Run Python Script...", "menu.file.run-python", nullptr,
                             "Python script deferred");
                disabledItem("Generate Python Script...", "menu.file.generate-python", nullptr,
                             "Python script deferred");
                ImGui::Separator();
                disabledItem("New Program Window", "menu.file.new-window", "Ctrl+N",
                             "Single instance");
                if (enabledItem("Quit", "menu.file.quit")) PostQuitMessage(0);
                ImGui::EndMenu();
            }
            const bool editOpen = ImGui::BeginMenu("Edit");
            recordUiTestItem("menu.edit", "Edit");
            if (editOpen) {
                if (enabledItem("Undo", "menu.edit.undo", "Ctrl+Z")) history(false);
                if (enabledItem("Redo", "menu.edit.redo", "Ctrl+Y")) history(true);
                ImGui::Separator();
                if (enabledItem("Application Settings...", "menu.edit.settings"))
                    showSettings = true;
                ImGui::EndMenu();
            }
            const bool helpOpen = ImGui::BeginMenu("Help");
            recordUiTestItem("menu.help", "Help");
            if (helpOpen) {
                if (enabledItem("User Manual", "menu.help.user-manual", "F1"))
                    openUrl("https://github.com/WhiteCrosstheRiver/AtomX#readme");
                disabledItem("Scripting Reference", "menu.help.scripting", nullptr, "Deferred");
                if (enabledItem("Request a Feature", "menu.help.request-feature"))
                    openUrl("https://github.com/WhiteCrosstheRiver/AtomX/issues/new");
                if (enabledItem("System Information...", "menu.help.system-info"))
                    showSystemInfo = true;
                if (enabledItem("About AtomX", "menu.help.about")) showAbout = true;
                ImGui::EndMenu();
            }
            const bool viewOpen = ImGui::BeginMenu("View");
            recordUiTestItem("menu.view", "View");
            if (viewOpen) {
                if (enabledItem("Reset Camera", "menu.view.reset-camera")) resetView();
                if (enabledItem("Fit Structure in View", "menu.view.fit")) {
                    for (int i = 0; i < 4; ++i) fitCamera(i, false);
                }
                if (enabledItem("Fit Selected in View", "menu.view.fit-selected", nullptr))
                    fitCamera(active, true);
                if (enabledItem(showWorkspace ? "Hide Project Panel" : "Show Project Panel",
                                "menu.view.project"))
                    showWorkspace = !showWorkspace;
                if (enabledItem(showLatticePanel ? "Hide Lattice Properties" : "Show Lattice Properties",
                                "menu.view.lattice"))
                    showLatticePanel = !showLatticePanel;
                if (enabledItem(cell ? "Hide Unit Cell" : "Show Unit Cell", "menu.view.cell"))
                    cell = !cell;
                if (enabledItem("Ball and Stick", "menu.view.ball-and-stick")) setDisplayStyle(0);
                if (enabledItem("Space Filling", "menu.view.space-filling")) setDisplayStyle(1);
                if (enabledItem("Stick", "menu.view.stick")) setDisplayStyle(2);
                if (enabledItem("Line", "menu.view.line")) setDisplayStyle(3);
                ImGui::EndMenu();
            }
            const bool modifyOpen = ImGui::BeginMenu("Modify");
            recordUiTestItem("menu.modify", "Modify");
            if (modifyOpen) {
                if (enabledItem(creationMode ? "Exit Creation Mode" : "Enter Creation Mode",
                                "menu.modify.creation-mode"))
                    creationMode ? leaveCreationTab() : openCreationTab();
                if (enabledItem("Delete Picked Atom", "menu.modify.delete-atom")) deletePickedAtom();
                if (enabledItem("Create Vacancy at Picked Atom", "menu.modify.vacancy")) deletePickedAtom();
                if (enabledItem("Replace Picked Atom", "menu.modify.replace-atom")) replacePickedElement();
                if (enabledItem("Dope Picked Atom with Element", "menu.modify.dope")) replacePickedElement();
                if (enabledItem("Adjust Hydrogens...", "menu.modify.add-hydrogens")) addHydrogensCommand();
                if (enabledItem("Clean Geometry", "menu.modify.clean")) cleanGeometryCommand();
                ImGui::EndMenu();
            }
            const bool buildOpen = ImGui::BeginMenu("Build");
            recordUiTestItem("menu.build", "Build");
            if (buildOpen) {
                if (enabledItem("New Orthogonal Cell...", "menu.build.orthogonal-cell")) openNewCell = true;
                if (enabledItem("New Triclinic Cell...", "menu.build.triclinic-cell")) openTriclinicCell = true;
                if (enabledItem("Supercell 2 x 2 x 1", "menu.build.supercell-221"))
                    adoptStructure(authoring::replicate(source, 2, 2, 1), "Built a 2 x 2 x 1 supercell");
                if (enabledItem("Supercell 2 x 2 x 2", "menu.build.supercell-222"))
                    adoptStructure(authoring::replicate(source, 2, 2, 2), "Built a 2 x 2 x 2 supercell");
                if (enabledItem("Cleave Along C and Add Vacuum", "menu.build.cleave"))
                    adoptStructure(authoring::cleaveAndVacuum(source, 2, 0.5, 15),
                                   "Cleaved the cell along c and added 15 A of vacuum");
                if (enabledItem("Add 15 A Vacuum Along C", "menu.build.vacuum"))
                    adoptStructure(authoring::addVacuum(source, 2, 15), "Added 15 A of vacuum along c");
                if (enabledItem("Build Layers...", "menu.build.layer")) requestLayerBuilder();
                if (enabledItem("Graphene Sheet", "menu.build.graphene"))
                    newStructureTab(authoring::grapheneSheet(6, 4), "Graphene sheet");
                if (enabledItem("Zigzag Nanotube (8,0)", "menu.build.nanotube"))
                    newStructureTab(authoring::zigzagNanotube(8, 6), "Zigzag nanotube (8,0)");
                if (enabledItem("FCC Nanoparticle", "menu.build.nanoparticle"))
                    newStructureTab(authoring::fccNanoparticle("Cu", 8), "FCC nanoparticle");
                if (enabledItem("Place Water Above Structure", "menu.build.water"))
                    adoptStructure(authoring::placeWater(source, 3), "Placed a water molecule 3 A above the structure");
                ImGui::EndMenu();
            }
            const bool toolsOpen = ImGui::BeginMenu("Tools");
            recordUiTestItem("menu.tools", "Tools");
            if (toolsOpen) {
                if (enabledItem("Show Lattice Parameters", "menu.tools.lattice")) showLatticePanel = true;
                if (enabledItem("Measure Distance Between Two Atoms", "menu.tools.measure")) {
                    if (creationPick >= 0 && creationMeasure >= 0)
                        status = "Distance " + std::to_string(authoring::distance(source, creationPick, creationMeasure)) + " angstrom";
                    else status = "Click the first atom, then Shift-click the second atom";
                }
                if (enabledItem("Measure Bond Angle of Three Atoms", "menu.tools.angle")) {
                    if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0)
                        status = "Bond angle " + std::to_string(authoring::bondAngle(source, creationPick, creationMeasure, creationAngle)) + " degrees (vertex is the second atom)";
                    else status = "Click, Shift-click the vertex, then Ctrl-click the third atom";
                }
                if (enabledItem("Measure Dihedral Angle of Four Atoms", "menu.tools.dihedral")) {
                    if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0 && creationDihedral >= 0)
                        status = "Dihedral " + std::to_string(authoring::dihedralAngle(source, creationPick, creationMeasure, creationAngle, creationDihedral)) + " degrees";
                    else status = "Click, Shift-click, Ctrl-click, then Alt-click the fourth atom";
                }
                ImGui::EndMenu();
            }
            titleWindow->DC.LayoutType = savedLayout;
        }
        // P12: centered "file — fileInfo" caption between the menus and the
        // window controls (file in secondary text, the rest faint).
        {
            auto *dl = ImGui::GetWindowDrawList();
            const std::string fileName = creationMode ? tabTitle() : path.empty()
                ? std::string("Untitled structure")
                : utf8(path.filename().wstring());
            const std::string info = readerName + " · " + std::to_string(std::max<size_t>(frames.size(), 1)) +
                (frames.size() > 1 ? " frames" : " frame");
            const ImVec2 dash = ImGui::CalcTextSize("  —  ");
            const ImVec2 fs = ImGui::CalcTextSize(fileName.c_str());
            const float total = fs.x + dash.x + ImGui::CalcTextSize(info.c_str()).x;
            float x = (w - total) * .5f;
            const float y = titleH + U(13);
            dl->AddText({x, y}, IM_COL32(174, 180, 187, 255), fileName.c_str());
            x += fs.x;
            dl->AddText({x, y}, ImGui::GetColorU32(ImGuiCol_TextDisabled, .62f), "  -  ");
            x += dash.x;
            dl->AddText({x, y}, ImGui::GetColorU32(ImGuiCol_TextDisabled, .62f), info.c_str());
        }
        // P12: amber "View mode" marker chip at the right edge, in front of
        // the min/max/close cluster.
        {
            auto *dl = ImGui::GetWindowDrawList();
            const float chipX = w - U(215) - U(96);
            const float cy = titleH + U(20);
            dl->AddRectFilled({chipX, cy - U(3)}, {chipX + U(6), cy + U(3)},
                              ImGui::GetColorU32(ImGuiCol_NavCursor), U(1));
            dl->AddText({chipX + U(12), titleH + U(13)}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        "View mode");
        }
        // OVITO parity: the "Pipelines: <source>" selector lives in the
        // window's top strip, right-aligned before the min/max/close cluster
        // (which occupies the right ~U(215) of the row).
        const std::string sourceLabel = creationMode ? tabTitle() : path.empty()
            ? readerName
            : utf8(path.filename().wstring()) + "  [" + readerName + "]";
        {
            const float comboWidth = U(280);
            ImGui::AlignTextToFramePadding();
            const float labelWidth = ImGui::CalcTextSize("Pipelines:").x;
            const float rowRight = w - U(215);
            const float blockWidth = labelWidth + U(8) + comboWidth;
            ImGui::SetCursorPosY(U(9));
            ImGui::SetCursorPosX(std::max(U(220), rowRight - blockWidth));
            ImGui::TextUnformatted("Pipelines:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(comboWidth);
            if (ImGui::BeginCombo("##pipeline-source", sourceLabel.c_str())) {
                ImGui::Selectable(sourceLabel.c_str(), true);
                ImGui::EndCombo();
            }
            recordUiTestItem("pipeline.source-selector");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Data source feeding the pipeline");
        }
        ImGui::End();
        if (creationMode) {
            fixed("Creation mode header", 0, titleH, w, U(38));
            ImGui::PushStyleColor(ImGuiCol_Button, {0.494f, 0.871f, 0.494f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {0.086f, 0.094f, 0.106f, 1.f});
            if (ImGui::Button("创作模式")) leaveCreationTab();
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("返回视图")) leaveCreationTab();
            recordUiTestItem("creation.return-view", "Return to view");
            ImGui::SameLine();
            if (ImGui::Button("File")) ImGui::OpenPopup("creation-file");
            if (ImGui::BeginPopup("creation-file")) {
                if (ImGui::MenuItem("Export structure...")) showDataExport = true;
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Edit")) ImGui::OpenPopup("creation-edit");
            if (ImGui::BeginPopup("creation-edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) history(false);
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) history(true);
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Build")) ImGui::OpenPopup("creation-build");
            if (ImGui::BeginPopup("creation-build")) {
                if (ImGui::MenuItem("Orthogonal cell...")) openNewCell = true;
                if (ImGui::MenuItem("Triclinic cell...")) openTriclinicCell = true;
                if (ImGui::MenuItem("Supercell 2 x 2 x 2"))
                    adoptStructure(authoring::replicate(source, 2, 2, 2), "Built supercell");
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Based on %s", creationBasedOn.c_str());
            ImGui::End();
            fixed("Creation mode tools", 0, titleH + U(38), w, U(56));
            auto tool = [&](const char *label, const char *tip, auto &&fn) {
                if (ImGui::Button(label, {0, U(34)})) fn();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                ImGui::SameLine(0, U(5));
            };
            tool("Select", "Pick an atom in the viewport", [&] { creationSketch = false; });
            if (creationSketch) {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.18f, 0.55f, 0.31f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.23f, 0.65f, 0.37f, 1.f});
            }
            tool("Draw atom", "Click empty space to place the chosen element", [&] {
                creationSketch = true;
            });
            if (creationSketch) ImGui::PopStyleColor(2);
            tool("Add atom", "Add the chosen element at the center of the cell", [&] {
                addAtomAt(authoring::cartesian(source, .5, .5, .5));
            });
            tool("Delete", "Delete the selected atom", [&] { deletePickedAtom(); });
            ImGui::SetNextItemWidth(U(48));
            ImGui::InputText("##creation-element-toolbar", creationElement, sizeof(creationElement));
            ImGui::SameLine();
            tool("Replace", "Replace the selected atom with this element", [&] { replacePickedElement(); });
            tool("Hydrogens", "Adjust hydrogens", [&] { addHydrogensCommand(); });
            tool("Clean", "Clean geometry", [&] { cleanGeometryCommand(); });
            tool("Ball + stick", "Ball and stick display", [&] { setDisplayStyle(0); });
            tool("Space fill", "Space filling display", [&] { setDisplayStyle(1); });
            tool("Fit", "Fit the structure", [&] { fitCamera(3, false); });
            ImGui::End();
            return;
        }
        // Compact OVITO-style toolbar: grouped icon-only flat buttons with
        // subtle separators; every action, tooltip and recorded UI-test name
        // of the former text toolbar is preserved.
        fixed("Top", 0, titleH * 2, w, barH);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(6), U(4)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(2), U(4)});
        ImGui::SetCursorPosY(U(3));
        if (iconButton("toolbar.open", 0xE8E5, "Open",
                       "Open a structure file (Ctrl+O)"))
            open();
        ImGui::SameLine();
        if (iconButton("toolbar.import", 0xE8B5, "Import",
                       "Import data from a structure file"))
            open();
        toolbarSeparator(barH);
        if (iconButton("toolbar.undo", 0xE7A7, "Undo",
                       "Undo the last pipeline edit (Ctrl+Z)"))
            history(false);
        ImGui::SameLine();
        if (iconButton("toolbar.redo", 0xE7A6, "Redo",
                       "Redo an undone pipeline edit (Ctrl+Y)"))
            history(true);
        toolbarSeparator(barH);
        if (iconButton("toolbar.select", 0xE799, "Select", "Selection tool"))
            active = active;
        ImGui::SameLine();
        if (iconButton("toolbar.orbit", 0xE7B8, "Orbit",
                       "Orbit the active viewport"))
            cameras[active].mode = 7;
        ImGui::SameLine();
        if (iconButton("toolbar.rotate", 0xE72C, "Rotate",
                       "Rotate the view"))
            cameras[active].yaw += .35f;
        toolbarSeparator(barH);
        // P12 tool pill group: Orbit / Pan / Zoom pills over the existing
        // viewportTool state (design: Orbit/Pan/Select pill container).
        {
            struct Pill { const char *name; const char *label; int tool; };
            const Pill pills[] = {
                {"toolbar.pill.orbit", "Orbit", 2},
                {"toolbar.pill.pan", "Pan", 1},
                {"toolbar.pill.zoom", "Zoom", 0},
            };
            const float pillH = U(24);
            for (const auto &pill : pills) {
                const bool selected = viewportTool == pill.tool;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.941f, 0.643f, 0.192f, 1.f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.957f, 0.694f, 0.263f, 1.f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.957f, 0.694f, 0.263f, 1.f});
                    ImGui::PushStyleColor(ImGuiCol_Text, {0.086f, 0.094f, 0.106f, 1.f});
                }
                if (ImGui::Button(pill.label, {0, pillH}))
                    viewportTool = pill.tool;
                if (selected) ImGui::PopStyleColor(4);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s tool — drag in the active viewport",
                                                              pill.label);
                recordUiTestItem(pill.name, pill.label);
                ImGui::SameLine();
            }
        }
        toolbarSeparator(barH);
        if (iconButton("toolbar.snapshot", 0xE722, "Snapshot",
                       "Save a snapshot image"))
            exportImage();
        ImGui::SameLine();
        if (iconButton("toolbar.views", quad ? 0xE8A7 : 0xE8A9,
                       quad ? "Single view" : "Four views",
                       quad ? "Maximize active viewport" : "Show four viewports"))
            quad = !quad;
        ImGui::SameLine();
        if (iconButton("toolbar.fit", 0xE8AA, "Fit all",
                       "Fit all viewports to the data")) {
            for (int i=0;i<4;++i) fitCamera(i,false);
        }
        toolbarSeparator(barH);
        if (iconButton("toolbar.modifiers", 0xE81E, "Modifiers",
                       "Show the modifier catalog"))
            showCatalog = true;
        ImGui::SameLine();
        if (iconButton("toolbar.render", 0xE7F4, "Render",
                       "Render the active viewport to an image"))
            exportImage();
        toolbarSeparator(barH);
        if (iconButton("toolbar.settings", 0xE713, "Settings",
                       "Open the settings dialog"))
            showSettings = true;
        ImGui::SameLine();
        if (iconButton("toolbar.workspace", 0xE91B, "Workspace",
                       "Toggle the workspace side panel"))
            showWorkspace = !showWorkspace;
        ImGui::SameLine();
        // P12 primary buttons: green 创作模式 (Creation Mode) and amber Render.
        {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.494f, 0.871f, 0.494f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.573f, 0.906f, 0.573f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.573f, 0.906f, 0.573f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {0.086f, 0.094f, 0.106f, 1.f});
            if (ImGui::Button(creationMode ? "退出创作模式" : "创作模式", {0, U(28)}))
                creationMode ? leaveCreationTab() : openCreationTab();
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open this structure in Creation Mode (atom editing)");
            recordUiTestItem("toolbar.creation-mode", creationMode ? "退出创作模式" : "创作模式");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.941f, 0.643f, 0.192f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.957f, 0.694f, 0.263f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.957f, 0.694f, 0.263f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {0.086f, 0.094f, 0.106f, 1.f});
            if (ImGui::Button("Render", {0, U(28)}))
                exportImage();
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Render the active viewport to a PNG image");
            recordUiTestItem("toolbar.render-primary", "Render");
        }
        // Quick command search (Ctrl+P) pinned to the right edge of the row;
        // the matching entry list renders in a floating window drawn after
        // every other window so nothing can cover it.
        {
            const float searchWidth = U(260);
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > searchWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - searchWidth);
            ImGui::SetCursorPosY(U(3));
            if (paletteRequested) {
                paletteRequested = false;
                paletteActive = true;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputTextWithHint(
                "##command-search", "Quick command search (Ctrl+P)", commandSearch,
                sizeof(commandSearch));
            recordUiTestItem("palette.search");
            // Anchor always tracks the field so the dropdown appears beneath
            // it even while focus is still being acquired.
            paletteAnchor = ImGui::GetItemRectMin();
            // Typing directly in the field also opens the dropdown; the
            // empty field after an executed command keeps it closed.
            if (ImGui::IsItemActive() && commandSearch[0]) paletteActive = true;
        }
        ImGui::PopStyleVar(2);
        ImGui::End();
    }
    void left(float h) {
        fixed("Workspace", 0, topInset(), workspaceWidth(), h - topInset() - statusHeight());
        heading("WORKSPACE");
        ImGui::TextColored(accent, "ATOMIC STRUCTURES");
        ImGui::Spacing();
        ImGui::Selectable(path.empty() ? "  Cu-Ni / FCC specimen"
                                       : ("  " + utf8(path.filename().wstring())).c_str(),
                          true);
        ImGui::TextDisabled("    %s", path.empty() ? "Generated dataset" : "XYZ trajectory");
        ImGui::Spacing();
        if (ImGui::Button("+ Import dataset", {-1, U(32)}))
            open();
        heading("SCENE");
        ImGui::Checkbox("Particles", &particles);
        ImGui::Checkbox("Simulation cell", &cell);
        ImGui::Checkbox("Data inspector", &showTable);
        heading("PARTICLE TYPES");
        for (size_t i = 0; i < source.species.size(); ++i) {
            ImGui::BulletText("%s", source.species[i].c_str());
        }
        heading("DATASET");
        ImGui::TextDisabled("Source atoms");
        ImGui::Text("%s", number(source.sourceCount).c_str());
        ImGui::TextDisabled("Displayed atoms");
        ImGui::Text("%s", number(result.data.atoms.size()).c_str());
        if (source.sampled())
            ImGui::TextColored({.9f, .73f, .42f, 1}, "SAMPLED  /  1 in %llu", source.stride);
        ImGui::TextDisabled("Frames");
        ImGui::Text("%zu", std::max<size_t>(frames.size(), 1));
        heading("QUICK ACTIONS");
        if (ImGui::Button("Slice specimen", {-1, U(30)}))
            add(Op::Slice);
        if (ImGui::Button("Select particle type", {-1, U(30)}))
            add(Op::SelectType);
        if (ImGui::Button("Reset pipeline", {-1, U(30)})) {
            checkpoint();
            mods.clear();
            modifierGraph.selected = 0;
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
            update();
        }
        if (ImGui::Button("Load demo crystal", {-1, U(30)}) && !documentsBusy()) {
            source = crystal(24);
            path.clear();
            frames.clear();
            current = 0;
            mods.clear();
            modifierGraph.selected = 0;
            pipelineCheckpoint.reset();
            pipelineCheckpointNode=SIZE_MAX;
            undo.clear();
            redo.clear();
            update();
        }
        ImGui::End();
    }
    // P12 left PIPELINE panel (268px): header with the node action strip,
    // the "Add modification" button, then MODIFICATIONS / VISUAL ELEMENTS /
    // DATA SOURCE rows in the redesign row style (checkbox, mono badge,
    // name, right meta). All recorded names and actions are identical to the
    // previous pipeline stack + action strip.
    static std::string nodeBadgeText(const ModifierNode &m) {
        const char *name = m.displayName.empty() ? opName(m.op) : m.displayName.c_str();
        std::string letters;
        bool lastSpace = true;
        for (const char *p = name; *p && letters.size() < 2; ++p) {
            if (isalpha((unsigned char)*p)) {
                if (lastSpace || letters.empty()) letters += char(toupper((unsigned char)*p));
                lastSpace = false;
            } else if (*p == ' ') {
                lastSpace = true;
            }
        }
        while (letters.size() < 2 && name[0]) letters += char(toupper((unsigned char)name[1]));
        return letters;
    }
    void pipelineRow(const char *id, const char *badge, const char *name, const char *meta,
                     bool *enabled, bool selected, bool isModifier, int index,
                     const char *enabledRecordName = nullptr) {
        ImGui::PushID(id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rowH = U(30);
        const float w = ImGui::GetContentRegionAvail().x;
        const bool rowClicked = ImGui::InvisibleButton("##row", {w, rowH});
        const bool hovered = ImGui::IsItemHovered();
        const bool rowPress = ImGui::IsItemClicked(0); // down-transition flag for the UI harness
        const ImGuiID rowId = ImGui::GetItemID();
        const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
        auto *dl = ImGui::GetWindowDrawList();
        if (selected)
            dl->AddRectFilled(p, {p.x + w, p.y + rowH}, ImGui::GetColorU32(ImGuiCol_Header));
        else if (hovered)
            dl->AddRectFilled(p, {p.x + w, p.y + rowH}, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        if (selected && isModifier)
            dl->AddRectFilled(p, {p.x + U(2), p.y + rowH}, IM_COL32(240, 164, 49, 255));
        float x = p.x + U(14);
        if (index == -1) {
            // Data source row: green 6px dot instead of a checkbox.
            const ImVec2 c{x, p.y + (rowH - U(6)) * .5f};
            dl->AddRectFilled(c, {c.x + U(6), c.y + U(6)}, IM_COL32(87, 171, 90, 255), U(1));
        } else {
        // Checkbox (14px, amber when on)
        {
            const ImVec2 c{x, p.y + (rowH - U(14)) * .5f};
            if (*enabled) {
                dl->AddRectFilled(c, {c.x + U(14), c.y + U(14)}, ImGui::GetColorU32(ImGuiCol_NavCursor), U(2));
                float cx0 = c.x + U(3), cy0 = c.y + U(7.2f);
                dl->AddLine({cx0, cy0}, {cx0 + U(2.2f), cy0 + U(2.4f)}, IM_COL32(22, 24, 27, 255), U(1.6f));
                dl->AddLine({cx0 + U(2.2f), cy0 + U(2.4f)}, {cx0 + U(7.4f), c.y + U(3.6f)}, IM_COL32(22, 24, 27, 255), U(1.6f));
            } else {
                dl->AddRectFilled(c, {c.x + U(14), c.y + U(14)}, ImGui::GetColorU32(ImGuiCol_FrameBg), U(2));
                dl->AddRect(c, {c.x + U(14), c.y + U(14)}, ImGui::GetColorU32(ImGuiCol_Tab), U(2));
            }
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(c);
            ImGui::InvisibleButton("##toggle", {U(14), U(14)});
            const bool togglePress = ImGui::IsItemClicked(0);
            const bool toggleHover = ImGui::IsItemHovered();
            const ImGuiID toggleId = ImGui::GetItemID();
            const ImVec2 toggleMin = ImGui::GetItemRectMin(), toggleMax = ImGui::GetItemRectMax();
            if (togglePress && isModifier && enabled) {
                checkpoint();
                *enabled = !*enabled;
                update(size_t(index));
            }
            if (enabledRecordName)
                recordUiTestItemExplicit(enabledRecordName, toggleId, toggleMin, toggleMax,
                                         toggleHover, togglePress);
            ImGui::SetCursorScreenPos(cursor);
        }
        }
        x += U(14) + U(9);
        // Badge, name and meta are drawn directly on the row's draw list so
        // the mid-row cursor juggling for the checkbox cannot wrap lines.
        ImGui::SetCursorScreenPos({x, p.y + (rowH - U(15)) * .5f});
        monoBadge(badge, selected);
        x += U(24) + U(9);
        {
            const ImVec2 ts = ImGui::CalcTextSize(name);
            dl->AddText({x, p.y + (rowH - ts.y) * .5f},
                        ImGui::GetColorU32(*enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                        name);
            if (meta && meta[0]) {
                const ImVec2 ms = ImGui::CalcTextSize(meta);
                dl->AddText({p.x + w - ms.x - U(12), p.y + (rowH - ms.y) * .5f},
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), meta);
            }
        }
        // The row button already advanced the flow; pin the cursor to the row
        // end so the badge Dummy cannot push following rows down.
        ImGui::SetCursorScreenPos({p.x, p.y + rowH});
        if (rowClicked) {
            if (isModifier) {
                modifierGraph.selected = size_t(index);
                panelSelection = index;
            } else {
                panelSelection = index; // -1 source, -2 particles, -3 cell
            }
        }
        recordUiTestItemExplicit(std::string("pipeline.node.select.") + id, rowId, rowMin, rowMax,
                                 hovered, rowPress, name);
        ImGui::PopID();
    }
    void pipelinePanel(float h) {
        fixed("Pipeline", workspaceWidth(), topInset(), pipelineWidth(),
              h - topInset() - statusHeight());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(4), U(3)});
        // Header: PIPELINE caption + node action strip (up/down/duplicate/
        // delete acting on the selected node; identical record names).
        {
            const float headerH = U(36);
            auto *dl = ImGui::GetWindowDrawList();
            const ImVec2 hp = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            dl->AddRectFilled(hp, {hp.x + w, hp.y + headerH}, ImGui::GetColorU32(ImGuiCol_WindowBg));
            dl->AddLine({hp.x, hp.y + headerH}, {hp.x + w, hp.y + headerH},
                        ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::SetCursorPos({ImGui::GetCursorPosX() + U(14), ImGui::GetCursorPosY() + U(11)});
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
            ImGui::TextUnformatted("PIPELINE");
            ImGui::PopStyleColor();
            const bool isMod = modifierGraph.selected < mods.size();
            const std::string selectId =
                isMod ? mods[modifierGraph.selected].id : std::string();
            ImGui::SameLine();
            // Right-aligned 24px icon cluster.
            const float btn = U(24);
            float right = hp.x + w - U(8);
            auto place = [&](float width) { right -= width; ImGui::SetCursorScreenPos({right, hp.y + U(6)}); };
            place(btn);
            if (iconButton("pipeline.strip.delete", 0xE74D, "x",
                           "Delete the selected modification", false, btn,
                           ("pipeline.node.delete." + selectId).c_str())) {
                checkpoint();
                modifierGraph.erase(modifierGraph.selected);
                update(modifierGraph.selected);
            }
            place(btn);
            ImGui::BeginDisabled(!isMod || modifierGraph.selected + 1 >= mods.size());
            if (iconButton("pipeline.strip.up", 0xE70E, "^",
                           "Move the selected modification up (later in the pipeline)", false, btn,
                           ("pipeline.node.up." + selectId).c_str())) {
                checkpoint();
                modifierGraph.move(modifierGraph.selected, modifierGraph.selected + 1);
                update(modifierGraph.selected);
            }
            ImGui::EndDisabled();
            place(btn);
            ImGui::BeginDisabled(!isMod || modifierGraph.selected == 0);
            if (iconButton("pipeline.strip.down", 0xE70D, "v",
                           "Move the selected modification down (earlier in the pipeline)", false, btn,
                           ("pipeline.node.down." + selectId).c_str())) {
                checkpoint();
                modifierGraph.move(modifierGraph.selected, modifierGraph.selected - 1);
                update(modifierGraph.selected - 1);
            }
            ImGui::EndDisabled();
            place(btn);
            if (iconButton("pipeline.strip.copy", 0xE8C8, "Copy",
                           "Duplicate the selected modification", false, btn,
                           ("pipeline.node.copy." + selectId).c_str())) {
                checkpoint();
                auto copy = mods[modifierGraph.selected];
                copy.id = std::to_string(nextModifierId++);
                copy.error.clear();
                copy.outputs.clear();
                copy.dirty = true;
                modifierGraph.insert(std::move(copy), modifierGraph.selected + 1);
                update();
            }
            ImGui::SetCursorScreenPos({hp.x + U(1), hp.y + headerH + U(10)});
        }
        // Add modification button (30px, amber plus, kbd M chip).
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - U(20);
            const float bH = U(30);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Tab));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Button("   Add modification        M", {w, bH}))
                showCatalog = true;
            ImGui::PopStyleColor(5);
            auto *dl = ImGui::GetWindowDrawList();
            dl->AddLine({p.x + U(10), p.y + bH * .5f}, {p.x + U(24), p.y + bH * .5f},
                        ImGui::GetColorU32(ImGuiCol_NavCursor), U(1.5f));
            dl->AddLine({p.x + U(17), p.y + bH * .5f - U(7)}, {p.x + U(17), p.y + bH * .5f + U(7)},
                        ImGui::GetColorU32(ImGuiCol_NavCursor), U(1.5f));
            recordUiTestItem("pipeline.add-modification");
            catalogAnchor = {ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + 2};
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + U(10));
        }
        // MODIFICATIONS rows.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + U(4));
        sectionLabel("MODIFICATIONS",
                     std::to_string(mods.size()).c_str());
        if (mods.empty())
            ImGui::TextWrapped("No modifiers. Data flows unchanged from source.");
        for (int i = int(mods.size()) - 1; i >= 0; i--) {
            auto &m = mods[size_t(i)];
            const std::string meta = m.error.empty() ? std::string() : "! " + m.error;
            bool enabled = m.enabled;
            const std::string name = m.displayName.empty() ? opName(m.op) : m.displayName;
            ImGui::PushID(m.id.c_str());
            const std::string enabledRecord = std::string("pipeline.node.enabled.") + m.id;
            pipelineRow(m.id.c_str(), nodeBadgeText(m).c_str(), name.c_str(), meta.c_str(),
                        &enabled, panelSelection == i, true, i, enabledRecord.c_str());
            if (enabled != m.enabled) {
                checkpoint();
                m.enabled = enabled;
                update(size_t(i));
            }
            if (!m.error.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", m.error.c_str());
            ImGui::PopID();
        }
        // VISUAL ELEMENTS rows.
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + U(4));
        sectionLabel("VISUAL ELEMENTS");
        {
            std::string meta = std::to_string(result.data.atoms.size());
            bool on = particles;
            pipelineRow("particles", "PA", "Particles", meta.c_str(), &on, panelSelection == -2, false, -2);
            if (on != particles) particles = on;
            bool onCell = cell;
            const double a = std::hypot(result.data.cell[0], result.data.cell[1], result.data.cell[2]);
            char cellMeta[32];
            snprintf(cellMeta, sizeof(cellMeta), "%.2f A", a);
            pipelineRow("cell", "SC", "Simulation cell", a > 0 ? cellMeta : "", &onCell,
                        panelSelection == -3, false, -3);
            if (onCell != cell) cell = onCell;
        }
        // DATA SOURCE row.
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + U(4));
        sectionLabel("DATA SOURCE");
        {
            const std::string fileName =
                path.empty() ? "Generated crystal" : utf8(path.filename().wstring());
            char fr[32];
            snprintf(fr, sizeof(fr), "%zu fr", std::max<size_t>(frames.size(), 1));
            bool always = true;
            pipelineRow("src", "XYZ", fileName.c_str(), fr, &always, panelSelection == -1, false, -1);
        }
        ImGui::PopStyleVar();
        ImGui::End();
    }
    const char *views = "Top\0Bottom\0Front\0Back\0Left\0Right\0Ortho\0Perspective\0";
    static bool isGeometryTool(CreationTool tool) {
        return tool==CreationTool::Distance || tool==CreationTool::Angle || tool==CreationTool::Torsion;
    }
    static const char *geometryName(uint8_t count) { return count==2?"距离":count==3?"角度":"扭转角"; }
    void activateGeometry(int index) {
        creationDisplay.activeMonitor=index; geometryPanelMonitor=-1; geometryPending.clear();
    }
    void addGeometryMonitor(geometry::Monitor m) {
        if(!geometry::valid(m,source.atoms.size())) return;
        if(!geometry::value(source,m)) { status="测量点重合或扭转角未定义"; geometryPending.clear(); return; }
        auto next=creationDisplay;
        const auto found=std::find_if(next.monitors.begin(),next.monitors.end(),[&](const auto &other){return geometry::sameMeasurement(other,m);});
        if(found!=next.monitors.end()) { activateGeometry(int(found-next.monitors.begin())); return; }
        if(next.monitors.size()>=geometry::monitorLimit) { status="测量标记最多 256 个"; geometryPending.clear(); return; }
        next.monitors.push_back(m); next.activeMonitor=int(next.monitors.size())-1; next.monitorsVisible=true;
        editCreationDisplay(std::move(next),std::string("创建测量 · ")+geometryName(m.count));
    }
    void removeGeometryMonitor() {
        const int i=creationDisplay.activeMonitor;
        if(i<0 || size_t(i)>=creationDisplay.monitors.size()) return;
        auto next=creationDisplay; next.monitors.erase(next.monitors.begin()+i); next.activeMonitor=-1;
        editCreationDisplay(std::move(next),"移除测量标记");
    }
    void applyGeometryValue() {
        if(documentsBusy() || !sameAtomCount()) return;
        const int i=creationDisplay.activeMonitor;
        if(i<0 || size_t(i)>=creationDisplay.monitors.size()) return;
        try {
            const auto plan=geometry::prepare(source,creationDisplay.monitors[size_t(i)],geometryInverted);
            (void)geometry::positions(plan,geometryTarget);
            if(std::abs(geometry::delta(plan,geometryTarget))<1e-7) return;
            editStructure(std::string("修改")+geometryName(plan.monitor.count),[&](Dataset &data){geometry::apply(data,plan,geometryTarget);});
            geometryPanelMonitor=-1;
        } catch(const std::exception &e) { status=e.what(); }
    }
    void requestMeasurementConstraints() {
        if(!creationMode || documentsBusy() || !sameAtomCount() || creationDisplay.monitors.empty())return;
        measurementConstraintTab=tabs[size_t(activeTab)].id;measurementConstraintGeneration=pipelineGeneration;
        measurementConstraintSource=creationDisplay.monitors;measurementConstraintRows.clear();
        if(creationDisplay.activeMonitor>=0 && size_t(creationDisplay.activeMonitor)<measurementConstraintSource.size())
            measurementConstraintRows.push_back(creationDisplay.activeMonitor);
        measurementConstraintChoices={};measurementConstraintMessage.clear();openMeasurementConstraints=true;
    }
    bool validMeasurementConstraints() const {
        return creationMode && activeTab>=0 && activeTab<int(tabs.size()) && tabs[size_t(activeTab)].id==measurementConstraintTab &&
            pipelineGeneration==measurementConstraintGeneration && !documentsBusy() && creationDisplay.monitors==measurementConstraintSource;
    }
    bool applyMeasurementConstraints() {
        if(!validMeasurementConstraints()){measurementConstraintMessage="体系或测量已改变，请重新打开";return false;}
        try {auto next=creationDisplay;
            if(geometry::setFixed(next.monitors,measurementConstraintRows,measurementConstraintChoices))
                editCreationDisplay(std::move(next),"修改测量模拟约束");
            else status="测量约束没有变化";
            return true;
        }catch(const std::exception &e){measurementConstraintMessage=e.what();return false;}
    }
    void measurementConstraintsDialog() {
        if(openMeasurementConstraints){ImGui::OpenPopup("测量模拟约束##measurement-constraints");openMeasurementConstraints=false;}
        const auto *vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetWorkCenter(),ImGuiCond_Appearing,{.5f,.5f});
        ImGui::SetNextWindowSizeConstraints({U(500),0},{std::max(U(500),vp->WorkSize.x-U(24)),vp->WorkSize.y-U(24)});
        if(!ImGui::BeginPopupModal("测量模拟约束##measurement-constraints",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+U(500));
        const bool valid=validMeasurementConstraints();ImGui::BeginDisabled(!valid);
        ImGui::TextUnformatted("选择已有测量，可同时修改多种类型");
        if(ImGui::Button("全选测量")){measurementConstraintRows.resize(measurementConstraintSource.size());std::iota(measurementConstraintRows.begin(),measurementConstraintRows.end(),0);}
        recordUiTestItem("creation.measurement-select-all");ImGui::SameLine();
        if(ImGui::Button("清空测量选择"))measurementConstraintRows.clear();recordUiTestItem("creation.measurement-select-none");
        ImGui::BeginChild("##measurement-selection",{U(500),U(std::min(160.f,30.f*float(measurementConstraintSource.size())+8))},ImGuiChildFlags_Borders);
        for(size_t i=0;i<measurementConstraintSource.size();++i) {
            const auto &m=measurementConstraintSource[i];const auto measured=geometry::value(source,m);
            std::string label=std::string(geometryName(m.count))+" "+std::to_string(i+1)+" · ";
            for(size_t j=0;j<m.count;++j)label+="#"+std::to_string(m.atoms[j])+(j+1<m.count?" → ":"");
            char number[64];if(measured)snprintf(number,sizeof(number)," · %.3f %s",*measured,m.count==2?"Å":"°");else snprintf(number,sizeof(number)," · 未定义");
            label+=number;label+=m.fixed?" · 固定":" · 自由";label+="##constraint-"+std::to_string(i);
            auto it=std::find(measurementConstraintRows.begin(),measurementConstraintRows.end(),int(i));bool selected=it!=measurementConstraintRows.end();
            if(ImGui::Checkbox(label.c_str(),&selected)){if(selected)measurementConstraintRows.push_back(int(i));else measurementConstraintRows.erase(it);}
            recordUiTestItem("creation.measurement-row-"+std::to_string(i));
        }ImGui::EndChild();
        const auto summary=geometry::fixedSummary(measurementConstraintSource,measurementConstraintRows);
        ImGui::Text("已选 %zu 个测量",measurementConstraintRows.size());
        for(int k=0;k<3;++k) {
            ImGui::PushID(k);ImGui::BeginDisabled(summary[size_t(k)]==-2);
            ImGui::Text("%s：%s",geometryName(uint8_t(k+2)),summary[size_t(k)]==-2?"未选择":summary[size_t(k)]==-1?"混合":summary[size_t(k)]?"固定":"自由");
            ImGui::SameLine();ImGui::SetNextItemWidth(U(155));const char *labels[]{"保持当前值","解除固定","固定"};
            const bool open=ImGui::BeginCombo("##choice",labels[measurementConstraintChoices[size_t(k)]]);
            recordUiTestItem("creation.measurement-choice-"+std::to_string(k));
            if(open){for(int c=0;c<3;++c){if(ImGui::Selectable(labels[c],measurementConstraintChoices[size_t(k)]==c))measurementConstraintChoices[size_t(k)]=c;
                recordUiTestItem("creation.measurement-choice-"+std::to_string(k)+"-"+std::to_string(c));}ImGui::EndCombo();}
            ImGui::EndDisabled();ImGui::PopID();
        }
        ImGui::TextWrapped("固定距离、角度或扭转角是模拟属性；仍可手工修改几何。当前值随坐标更新，AtomX 不执行模拟求解。");
        ImGui::TextWrapped("约束随测量保存在 .atomx 中；标准结构导出不保留测量。移除测量或删除其端点会同时移除该约束。");
        ImGui::EndDisabled();
        if(!valid)ImGui::TextWrapped("体系或测量已改变，请重新打开。");
        else if(!measurementConstraintMessage.empty())ImGui::TextWrapped("%s",measurementConstraintMessage.c_str());
        ImGui::BeginDisabled(!valid || measurementConstraintRows.empty());
        if(ImGui::Button("应用",{U(110),U(30)}) && applyMeasurementConstraints())ImGui::CloseCurrentPopup();recordUiTestItem("creation.measurement-apply");
        ImGui::EndDisabled();ImGui::SameLine();
        if(ImGui::Button("取消",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();recordUiTestItem("creation.measurement-cancel");
        ImGui::PopTextWrapPos();ImGui::EndPopup();
    }
    // Small fixed monitor budget; these overlays never scan all atoms.
    void geometryOverlay(ImVec2 p,ImVec2 size,Camera &cam,ImDrawList *draw=nullptr,int *hit=nullptr) {
        if(!sameAtomCount() || !creationDisplay.monitorsVisible) return;
        const auto matrix=creationProjection(result.data,cam,size).combined;
        auto project=[&](Vec3 at,ImVec2 &out) {
            DirectX::XMFLOAT4 q; DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),matrix));
            if(q.w<=0 || q.z<=0 || q.z>=q.w) return false;
            out={p.x+(q.x/q.w+1)*size.x*.5f,p.y+(1-q.y/q.w)*size.y*.5f}; return true;
        };
        for(size_t step=0;step<creationDisplay.monitors.size();++step) {
            const size_t active=size_t(creationDisplay.activeMonitor);
            const size_t i=active>=creationDisplay.monitors.size()?step:
                step+1==creationDisplay.monitors.size()?active:step<active?step:step+1;
            const auto &m=creationDisplay.monitors[i];
            if(!geometry::valid(m,result.data.atoms.size())) continue;
            ImVec2 pts[4]; bool visible=true;
            for(size_t j=0;j<m.count;++j) visible=visible && creationAtomVisible(m.atoms[j]) && project(geometry::at(result.data,m.atoms[j]),pts[j]);
            if(!visible) continue;
            const auto measured=geometry::value(result.data,m);
            char caption[80]; if(measured) snprintf(caption,sizeof(caption),m.count==2?"%.3f Å%s":"%.2f°%s",*measured,m.fixed?" · 固定":""); else snprintf(caption,sizeof(caption),"未定义%s",m.fixed?" · 固定":"");
            ImVec2 anchor=m.count==2?ImVec2{(pts[0].x+pts[1].x)*.5f,(pts[0].y+pts[1].y)*.5f}:
                m.count==3?pts[1]:ImVec2{(pts[1].x+pts[2].x)*.5f,(pts[1].y+pts[2].y)*.5f};
            anchor.x+=U(8); anchor.y+=U(m.count==4?8.f:-24.f);
            const auto textSize=ImGui::CalcTextSize(caption);
            if(hit) {
                const auto mouse=ImGui::GetIO().MousePos;
                if(mouse.x>=anchor.x-U(5) && mouse.x<=anchor.x+textSize.x+U(5) && mouse.y>=anchor.y-U(4) && mouse.y<=anchor.y+textSize.y+U(4)) *hit=int(i);
            }
            if(!draw) continue;
            const ImU32 color=int(i)==creationDisplay.activeMonitor?IM_COL32(255,85,96,255):IM_COL32(76,219,155,230);
            for(size_t j=1;j<m.count;++j) {
                const float dx=pts[j].x-pts[j-1].x,dy=pts[j].y-pts[j-1].y;
                const float length=std::hypot(dx,dy); const int segments=std::min(512,std::max(1,int(length/U(9))));
                for(int k=0;k<segments;++k) {
                    const float a=float(k)/segments,b=(k+.55f)/segments;
                    draw->AddLine({pts[j-1].x+dx*a,pts[j-1].y+dy*a},{pts[j-1].x+dx*b,pts[j-1].y+dy*b},color,U(1.5f));
                }
            }
            if(m.count==3 && measured) {
                const Vec3 center=geometry::at(result.data,m.atoms[1]);
                const auto a=authoring::sub(geometry::at(result.data,m.atoms[0]),center),b=authoring::sub(geometry::at(result.data,m.atoms[2]),center);
                const double arcRadius=std::min(authoring::length(a),authoring::length(b))*.28;
                auto axis=authoring::cross(a,b);
                if(authoring::length(axis)<1e-8) axis=authoring::cross(a,std::abs(a.x)<std::abs(a.y)?Vec3{1,0,0}:Vec3{0,1,0});
                const Vec3 start=authoring::add(center,authoring::scale(a,arcRadius/authoring::length(a)));
                ImVec2 prior{}; bool priorVisible=project(start,prior);
                for(int k=1;k<=24;++k) {
                    ImVec2 next{}; const bool nextVisible=project(authoring::rotatedPoint(start,center,axis,*measured*authoring::kPi/180*k/24),next);
                    if(priorVisible && nextVisible) draw->AddLine(prior,next,color,U(1.5f)); prior=next; priorVisible=nextVisible;
                }
            }
            if(m.count==4 && measured) {
                const auto a=geometry::at(result.data,m.atoms[0]),b=geometry::at(result.data,m.atoms[1]);
                const auto c=geometry::at(result.data,m.atoms[2]),d=geometry::at(result.data,m.atoms[3]);
                const auto axis=authoring::scale(authoring::sub(c,b),1/authoring::length(authoring::sub(c,b)));
                const auto center=authoring::scale(authoring::add(b,c),.5);
                const auto first=authoring::sub(authoring::sub(a,b),authoring::scale(axis,authoring::dot(authoring::sub(a,b),axis)));
                const auto last=authoring::sub(authoring::sub(d,c),authoring::scale(axis,authoring::dot(authoring::sub(d,c),axis)));
                const double r=std::min(authoring::length(first),authoring::length(last))*.3;
                const auto start=authoring::add(center,authoring::scale(first,r/authoring::length(first)));
                const auto end=authoring::add(center,authoring::scale(last,r/authoring::length(last)));
                ImVec2 pivot{},from{},to{};
                if(project(center,pivot) && project(start,from) && project(end,to)) {
                    draw->AddLine(pivot,from,color,U(1.5f)); draw->AddLine(pivot,to,color,U(1.5f));
                    draw->AddLine(from,to,color,U(1));
                }
            }
            draw->AddRectFilled({anchor.x-U(4),anchor.y-U(3)},{anchor.x+textSize.x+U(4),anchor.y+textSize.y+U(3)},IM_COL32(12,16,19,210),U(3));
            draw->AddText(anchor,color,caption);
        }
        if(draw) for(size_t i=0;i<geometryPending.size();++i) {
            if(geometryPending[i]<0 || size_t(geometryPending[i])>=result.data.atoms.size()) continue;
            ImVec2 out; if(project(geometry::at(result.data,geometryPending[i]),out)) {
                draw->AddCircle(out,U(10),IM_COL32(255,211,91,255),24,U(2));
                const auto label=std::to_string(i+1); draw->AddText({out.x+U(12),out.y},IM_COL32(255,211,91,255),label.c_str());
            }
        }
    }
    void geometryPanel() {
        if(!isGeometryTool(creationTool) && creationDisplay.monitors.empty()) return;
        ImGui::PushTextWrapPos(0);
        ImGui::SeparatorText("测量 / 修改几何");
        if(isGeometryTool(creationTool)) {
            const int count=creationTool==CreationTool::Distance?2:creationTool==CreationTool::Angle?3:4;
            ImGui::Text("%s · 已选 %zu / %d 点",geometryName(uint8_t(count)),geometryPending.size(),count);
            ImGui::TextDisabled("依次点击原子 · 距离也可点击键");
            ImGui::TextDisabled("角度顶点为第 2 点 · Esc 取消");
        }
        if(!creationDisplay.monitors.empty()) {
            ImGui::BeginChild("##monitors",{0,U(std::min(130.f,float(creationDisplay.monitors.size())*29+6))},ImGuiChildFlags_Borders);
            for(size_t i=0;i<creationDisplay.monitors.size();++i) {
                const auto &m=creationDisplay.monitors[i]; const auto measured=geometry::value(result.data,m);
                char label[160]; if(measured) snprintf(label,sizeof(label),"%s %zu  %.3f %s%s##%zu",geometryName(m.count),i+1,*measured,m.count==2?"Å":"°",m.fixed?" · 固定":"",i);
                else snprintf(label,sizeof(label),"%s %zu  未定义%s##%zu",geometryName(m.count),i+1,m.fixed?" · 固定":"",i);
                if(ImGui::Selectable(label,creationDisplay.activeMonitor==int(i))) activateGeometry(int(i));
                recordUiTestItem("creation.monitor-"+std::to_string(i));
            }
            ImGui::EndChild();
            bool visible=creationDisplay.monitorsVisible;
            if(ImGui::Checkbox("显示测量标记",&visible)) { auto next=creationDisplay; next.monitorsVisible=visible; editCreationDisplay(std::move(next),"显示测量标记"); }
            if(ImGui::Button("测量模拟约束...",{-1,U(30)}))requestMeasurementConstraints();recordUiTestItem("creation.edit-measurement-constraints");
        }
        const int i=creationDisplay.activeMonitor;
        if(i>=0 && size_t(i)<creationDisplay.monitors.size()) {
            const auto &m=creationDisplay.monitors[size_t(i)];
            if(geometryPanelMonitor!=i) { geometryTarget=geometry::value(source,m).value_or(0); geometryPanelMonitor=i; geometryInverted=false; }
            std::string atoms; for(size_t j=0;j<m.count;++j) atoms+="#"+std::to_string(m.atoms[j])+(j+1<m.count?" → ":"");
            ImGui::TextDisabled("%s",atoms.c_str());
            ImGui::SetNextItemWidth(U(125)); ImGui::InputDouble(m.count==2?"目标 Å":"目标 °",&geometryTarget,0,0,"%.4f");
            recordUiTestItem("creation.geometry-value");
            ImGui::Checkbox("反向移动另一侧",&geometryInverted); recordUiTestItem("creation.geometry-invert");
            ImGui::BeginDisabled(documentsBusy());
            if(ImGui::Button("应用数值")) applyGeometryValue(); recordUiTestItem("creation.geometry-apply");
            ImGui::SameLine(); if(ImGui::Button("移除标记")) removeGeometryMonitor(); recordUiTestItem("creation.geometry-remove");
            ImGui::EndDisabled();
            ImGui::TextDisabled("点击标记 · 空白处向上 / 右拖增大");
            ImGui::TextDisabled("Alt 移动另一侧 · Esc 恢复");
            ImGui::TextDisabled("按实际坐标测量，不取周期最短距离");
        }
        ImGui::Separator();
        ImGui::PopTextWrapPos();
    }
    void chooseCreationTool(CreationTool tool) {
        creationSketchInsert=false;creationInsertHover=-1;
        geometryPending.clear(); geometryPanelMonitor=-1;
        geometryDragPlans[0].reset(); geometryDragPlans[1].reset();
        creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
        if (!pipelineBusy && !creationDragAtoms.empty()) {
            for (const auto &[index,original]:creationDragAtoms) if (index>=0 && size_t(index)<result.data.atoms.size()) {
                auto &a=result.data.atoms[size_t(index)]; a.x=original.x; a.y=original.y; a.z=original.z;
            }
            uploadCreationDisplay(result.data,result.selected,result.colorSelected);
        }
        creationRingPreviewValid=false; creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion(); creationDrag=CreationDrag::None; creationDragAtoms.clear();
        creationTool = tool;
        creationSketch = tool == CreationTool::Sketch;
        status = tool == CreationTool::Select ? "选择原子或拖动框选" :
                 isGeometryTool(tool) ? "依次点击测量点 · 点击标记后拖动空白处修改 · Alt 反向 · Esc 取消" :
                 tool == CreationTool::Rotate ? "拖动旋转" :
                 tool == CreationTool::Pan ? "拖动平移" :
                 tool == CreationTool::Zoom ? "上下拖动缩放 · Esc 返回选择" :
                 tool == CreationTool::Move ? "拖动选中原子" :
                 tool == CreationTool::Fragment ? "点击放置片段 · Alt 点击连接已有片段 · 拖动调整朝向 · Esc 取消" :
                 tool == CreationTool::Ring ? "点击放置碳环 · 原子或键上接环 · 拖动调整朝向 · Alt 芳香环" : "点击绘制或连键 · 双击结束 · Esc 取消";
    }
    void selectCreationAtom(int index, bool shift, bool toggle = false) {
        if (index>=0 && !creationAtomVisible(index)) return;
        if(!shift)creationBondSelection.clear();
        if (index < 0) { if (!shift) creationSelection.clear(); }
        else if (!shift) creationSelection = {index};
        else {
            auto found = std::find(creationSelection.begin(), creationSelection.end(), index);
            if (found == creationSelection.end()) creationSelection.push_back(index);
            else if (toggle) creationSelection.erase(found);
        }
        creationPick = creationSelection.empty() ? -1 : creationSelection.back();
    }
    bool creationBondVisible(int row) const {
        if(row<0 || size_t(row)>=source.bonds.size() || !mods.empty() || !source.bondStyle.visible ||
            source.bonds.size()>interactiveBondBudget || gpu.bondsOmittedForPerformance() ||
            creationDisplay.bondVisibility.isHidden(size_t(row)))return false;
        const auto &b=source.bonds[size_t(row)];
        return creationAtomVisible(int(b.a)) && creationAtomVisible(int(b.b)) &&
            (b.image==std::array<int32_t,3>{} || source.bondStyle.showPeriodicImages) &&
            (creationDisplay.presetAt(b.a)!=4 || creationDisplay.presetAt(b.b)!=4);
    }
    void selectCreationBond(int row,bool shift,bool toggle=false) {
        if(!creationBondVisible(row))return;
        if(!shift) {creationSelection.clear();creationPick=-1;creationBondSelection.clear();}
        auto found=std::find(creationBondSelection.begin(),creationBondSelection.end(),row);
        if(found==creationBondSelection.end())creationBondSelection.push_back(row);
        else if(toggle)creationBondSelection.erase(found);
    }
    bool editSelectedCreationBonds(int order) {
        if(!creationMode || documentsBusy() || !sameAtomCount() || creationBondSelection.empty() || order<0 || order>4)return false;
        const auto rows=creationBondSelection;
        std::set<creation::BondKey> keys;bool changed=false;
        for(int row:rows) {
            if(row<0 || size_t(row)>=source.bonds.size())return false;
            keys.insert(creation::bondKey(source.bonds[size_t(row)]));
            changed|=source.bonds[size_t(row)].order!=order;
        }
        if(!changed)return false;
        editStructure((order?"修改选中键级 · ":"断开选中键 · ")+std::to_string(rows.size()),[&](Dataset &d) {
            auto affected=creation::editBonds(d,rows,order);
            updateAutomaticHydrogens(d,affected);
        });
        // Hydrogen compaction may remap endpoints. Keep selection only when
        // identities are unchanged; never reinterpret old row indices.
        if(order && !creationAutoHydrogens)for(size_t i=0;i<source.bonds.size();++i)
            if(keys.contains(creation::bondKey(source.bonds[i])))creationBondSelection.push_back(int(i));
        return true;
    }
    void selectCreationFragment(int index) {
        creationBondSelection.clear();
        creationSelection=authoring::fragment(source,index);
        creationSelection.erase(std::remove_if(creationSelection.begin(),creationSelection.end(),
            [&](int atom){return !creationAtomVisible(atom);}),creationSelection.end());
        creationPick=creationSelection.empty()?-1:index;
        status="选中连接片段 · "+std::to_string(creationSelection.size())+" 个原子";
    }
    std::vector<int> creationTransformSelection() const {
        try {
            auto selected=creation::selectedAtoms(source,creationSelection,creationBondSelection);
            selected.erase(std::remove_if(selected.begin(),selected.end(),[&](int i){return !creationAtomVisible(i);}),selected.end());
            return selected;
        } catch(const std::invalid_argument &) {return {};}
    }
    void selectCreationFragments() {
        const auto seeds=creationTransformSelection();
        creationSelection=authoring::fragments(source,seeds);
        creationSelection.erase(std::remove_if(creationSelection.begin(),creationSelection.end(),
            [&](int i){return !creationAtomVisible(i);}),creationSelection.end());
        creationBondSelection.clear();
        creationPick=creationSelection.empty()?-1:creationSelection.back();
        status="选中连接片段 · "+std::to_string(creationSelection.size())+" 个原子";
    }
    void invertCreationSelection() {
        std::vector<uint8_t> atoms(source.atoms.size(),0);
        for(int i:creationSelection)if(i>=0 && size_t(i)<atoms.size())atoms[size_t(i)]=1;
        creationSelection.clear();
        for(size_t i=0;i<atoms.size();++i)if(!atoms[i] && creationAtomVisible(int(i)))creationSelection.push_back(int(i));
        if(source.bonds.size()<=interactiveBondBudget) {
            std::vector<uint8_t> bonds(source.bonds.size(),0);
            for(int i:creationBondSelection)if(i>=0 && size_t(i)<bonds.size())bonds[size_t(i)]=1;
            creationBondSelection.clear();
            for(size_t i=0;i<bonds.size();++i)if(!bonds[i] && creationBondVisible(int(i)))creationBondSelection.push_back(int(i));
        } else creationBondSelection.clear();
        creationPick=creationSelection.empty()?-1:creationSelection.back();
    }
    bool creationAtomVisible(int index) const {
        if (!particles || index<0 || size_t(index)>=source.atoms.size() || creationDisplay.isHidden(size_t(index)))
            return false;
        const auto type=source.atoms[size_t(index)].type;
        return type>=gpu.styles.size() || gpu.styles[type].visual[2]>.5f;
    }
    void uploadCreationDisplay(const Dataset &data,const std::vector<uint8_t> &selected,
                               const std::vector<uint8_t> &colorSelected) {
        static const std::vector<uint8_t> empty;
        gpu.upload(data,selected,colorSelected,
            creationMode && data.atoms.size()==source.atoms.size()?creationDisplay.hidden:empty,
            creationMode && data.atoms.size()==source.atoms.size()?&creationDisplay:nullptr);
    }
    void queueCreationLabelFont() {
        // File loading can finish while hidden, before the first ImGui frame.
        // Defer glyph inspection until a current font has been selected.
        ImFont *currentFont=ImGui::GetFont();
        if(!currentFont){refreshFont=true;return;}
        auto missingGlyph=[&](const std::string &text) {
            const char *cursor=text.c_str();
            while (*cursor) {
                unsigned character=0;
                const int length=ImTextCharFromUtf8(&character,cursor,nullptr);
                if (length<=0) break;
                if (!currentFont->FindGlyphNoFallback(ImWchar(character))) refreshFont=true;
                cursor+=length;
            }
        };
        auto glyphs=[&](const creation::Label &label) { missingGlyph(label.text);
            for(const auto &field:label.fields) missingGlyph(field.property); };
        glyphs(creationDisplay.defaultLabel);
        for (const auto &[index,label]:creationDisplay.labels) { (void)index; glyphs(label); }
        missingGlyph(creationDisplay.defaultColor.property);
        missingGlyph(creationDisplay.bondLabels.defaultLabel.text);
        for(const auto &[key,label]:creationDisplay.bondLabels.labels) { (void)key;missingGlyph(label.text); }
    }
    void refreshCreationDisplay(bool visibilityChanged=true) {
        geometryPanelMonitor=-1; geometryPending.clear();
        creationRingPreviewValid=false;
        creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1}; clearCreationFusion();
        creationDisplay.normalize(source.atoms.size()); creationDisplay.normalizeBonds(source);
        creationSelection.erase(std::remove_if(creationSelection.begin(),creationSelection.end(),
            [&](int index){return !creationAtomVisible(index);}),creationSelection.end());
        creationPick=creationSelection.empty()?-1:creationSelection.back();
        creationBondSelection.erase(std::remove_if(creationBondSelection.begin(),creationBondSelection.end(),
            [&](int row){return !creationBondVisible(row);}),creationBondSelection.end());
        creationHover=-1; creationLastHoverMouse={-1,-1};
        queueCreationLabelFont();
        if (visibilityChanged && !documentsBusy() && sameAtomCount())
            uploadCreationDisplay(result.data,result.selected,result.colorSelected);
    }
    void editCreationDisplay(creation::Display next,const std::string &message) {
        if (!creationMode || documentsBusy()) return;
        next.normalize(source.atoms.size()); next.normalizeBonds(source);
        if (next==creationDisplay) return;
        const bool visibilityChanged=!next.sameGpuAppearance(creationDisplay);
        if (authorUndo.size()>=64) authorUndo.erase(authorUndo.begin());
        authorUndo.push_back({std::nullopt,creationDisplay,message});
        authorRedo.clear(); structureEditIsLatest=true;
        creationSnapshotSelected=-1;
        creationDisplay=std::move(next);
        refreshCreationDisplay(visibilityChanged);
        status=message;
    }
    void creationVisibility(int mode) {
        if (mode!=2 && creationSelection.empty() && creationBondSelection.empty()) return;
        auto next=creationDisplay;
        if(mode==2) {
            next.visibility(source.atoms.size(),{},2);next.bondVisibility.visibility(source,{},2);
        } else if(mode==0) {
            if(!creationSelection.empty())next.visibility(source.atoms.size(),creationSelection,0);
            if(!creationBondSelection.empty())next.bondVisibility.visibility(source,creationBondSelection,0);
        } else {
            // Retain the selected bond endpoints so Show Only never produces
            // an empty viewport from a bond-only selection (as observed in MS).
            const auto atoms=creation::selectedAtoms(source,creationSelection,creationBondSelection);
            next.visibility(source.atoms.size(),atoms,1);
            if(!creationBondSelection.empty())next.bondVisibility.visibility(source,creationBondSelection,1);
        }
        editCreationDisplay(std::move(next),mode==0?"隐藏选中对象":mode==1?"仅显示选中对象及键端点":"显示全部原子与键");
    }
    void requestCreationLabels() {
        if (!creationMode || documentsBusy()) return;
        creationLabelTabId=tabs[size_t(activeTab)].id;
        creationLabelSelection=creationSelection;
        creationLabelAll=creationLabelSelection.empty();
        creationLabelDraft={};
        creationLabelDraft.fontSize=creationDisplay.fontSize;
        creationLabelDraft.color=creationDisplay.color;
        creationLabelDraft.bold=creationDisplay.bold;
        creationLabelDraft.labelsVisible=creationDisplay.labelsVisible;
        creationLabelDraft.labelBudget=creationDisplay.labelBudget;
        const auto label=creationPick>=0?creationDisplay.labelAt(creationPick):creationDisplay.defaultLabel;
        creationLabelKind=int(label.kind==creation::LabelKind::None?creation::LabelKind::ElementIndex:label.kind);
        snprintf(creationLabelText,sizeof(creationLabelText),"%s",label.text.c_str());
        creationLabelFields=label.fields.empty()?std::vector<creation::LabelField>{{creation::LabelFieldKind::Element,{}},{creation::LabelFieldKind::Index,{}}}:label.fields;
        creationLabelPrecision=label.precision;
        openCreationLabels=true;
    }
    void requestCreationBondLabels() {
        if(!creationMode || documentsBusy())return;
        creationBondLabelTabId=tabs[size_t(activeTab)].id;creationBondLabelGeneration=pipelineGeneration;
        creationBondLabelSelection=creationSelection;creationBondLabelRows=creationBondSelection;
        creationBondLabelAll=creationSelection.empty() && creationBondSelection.empty();
        creationBondLabelDraft=creationDisplay.bondLabels;
        creationBondLabelRule=creationBondLabelDraft.defaultLabel;
        const std::unordered_set<int> selected(creationSelection.begin(),creationSelection.end());
        creationBondLabelCount=creationBondLabelRows.size();bool first=creationBondLabelRows.empty();
        if(!first)creationBondLabelRule=creationBondLabelDraft.at(source,size_t(creationBondLabelRows.front()));
        for(size_t i=0;creationBondLabelRows.empty() && !selected.empty() && i<source.bonds.size();++i) {
            const auto &b=source.bonds[i];if(selected.contains(int(b.a)) && selected.contains(int(b.b))) {
                ++creationBondLabelCount;if(first) {creationBondLabelRule=creationBondLabelDraft.at(source,i);first=false;}
            }
        }
        if(creationBondLabelRule.empty())creationBondLabelRule.fields={creation::BondField::Length};
        snprintf(creationBondLabelText,sizeof(creationBondLabelText),"%s",creationBondLabelRule.text.c_str());
        creationBondLabelMessage.clear();openCreationBondLabels=true;
    }
    void requestCreationPosition() {
        if (creationPick<0 || size_t(creationPick)>=source.atoms.size()) return;
        creationPositionIndex=creationPick;
        creationPositionTabId=tabs[size_t(activeTab)].id;
        const auto &atom=source.atoms[size_t(creationPick)];
        creationPositionDraft[0]=atom.x; creationPositionDraft[1]=atom.y;
        creationPositionDraft[2]=atom.z;
        creationPositionFractional=false;
        openCreationPosition=true;
    }
    void requestMotionGroups() {
        if (!creationMode || documentsBusy()) return;
        chooseCreationTool(CreationTool::Select);
        showMotionGroups=true; motionGroupSelected=0; motionGroupMessage.clear(); motionCacheRevision=UINT64_MAX;
    }
    bool selectMotionGroup(int key,bool movement=false) {
        const auto members=motion::members(source,key); if (members.empty()) return false;
        creationBondSelection.clear();
        creationSelection.clear();
        for (int index:members) if (creationAtomVisible(index)) creationSelection.push_back(index);
        creationPick=creationSelection.empty()?-1:creationSelection.back();
        if (movement && creationSelection.size()!=members.size()) {
            motionGroupMessage="分组有隐藏成员，请先显示全部再整体移动"; return false;
        }
        return !creationSelection.empty();
    }
    void requestCreationStyles() {
        if(!creationMode || documentsBusy()) return;
        chooseCreationTool(CreationTool::Select);
        creationStyleAll=creationSelection.empty();
        creationStylePreset=creationStyleAll?creationDisplay.defaultPreset:creationDisplay.presetAt(creationSelection.back());
        creationStyleDraft=creationDisplay;
        creationColorDraft=creationStyleAll?creationDisplay.defaultColor:creationDisplay.colorAt(creationSelection.back());
        creationColorMessage.clear();
        showCreationStyles=true;
    }
    void creationStylesDialog() {
        if(!creationMode) {showCreationStyles=false;return;}
        if(!showCreationStyles) return;
        ImGui::SetNextWindowSize({U(440),U(560)},ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({U(300),U(250)},{U(700),std::max(U(250),ImGui::GetMainViewport()->WorkSize.y-U(24))});
        if(ImGui::Begin("显示样式##creation-styles",&showCreationStyles)) {
            ImGui::BeginDisabled(documentsBusy());
            ImGui::Checkbox("整个体系",&creationStyleAll); recordUiTestItem("creation.style-all");
            ImGui::EndDisabled();
            if(!creationStyleAll) ImGui::Text("选中 %zu 个原子",creationSelection.size());
            if(ImGui::BeginTabBar("##appearance-pages")) {
            if(ImGui::BeginTabItem("样式")) {
            const char *names[]={"原有外观","线","棒","球棒","CPK"};
            for(int i=0;i<5;++i) {
                ImGui::RadioButton(names[i],&creationStylePreset,i);
                recordUiTestItem(("creation.style-preset-"+std::to_string(i)).c_str());
                if(i<4) ImGui::SameLine();
            }
            ImGui::Separator();
            ImGui::SliderFloat("球半径 / Å",&creationStyleDraft.ballRadius,.02f,5.f,"%.2f");
            ImGui::SliderFloat("棒半径 / Å",&creationStyleDraft.stickRadius,.01f,creationStyleDraft.ballRadius,"%.2f");
            ImGui::SliderFloat("CPK 比例",&creationStyleDraft.cpkScale,.05f,3.f,"%.2f");
            ImGui::SliderFloat("线宽 / px",&creationStyleDraft.lineWidth,.5f,10.f,"%.1f");
            creationStyleDraft.stickRadius=std::min(creationStyleDraft.stickRadius,creationStyleDraft.ballRadius);
            ImGui::TextWrapped("半径和比例为当前标签的共享参数；样式可分别应用到不同原子。CPK 使用范德华半径，不显示对应半键。");
            ImGui::TextWrapped("线样式保留微小原子点便于选择。大体系沿用显示预算，超限键自动降为线或省略。");
            ImGui::BeginDisabled(documentsBusy() || (!creationStyleAll && creationSelection.empty()));
            if(ImGui::Button("应用",{U(110),U(30)})) {
                auto next=creationDisplay;
                next.ballRadius=creationStyleDraft.ballRadius; next.stickRadius=creationStyleDraft.stickRadius;
                next.cpkScale=creationStyleDraft.cpkScale; next.lineWidth=creationStyleDraft.lineWidth;
                next.setPreset(source.atoms.size(),creationSelection,uint8_t(creationStylePreset),creationStyleAll);
                editCreationDisplay(std::move(next),std::string("显示样式 · ")+names[creationStylePreset]);
            }
            recordUiTestItem("creation.style-apply"); ImGui::EndDisabled(); ImGui::SameLine();
            if(ImGui::Button("关闭",{U(110),U(30)})) showCreationStyles=false;
            recordUiTestItem("creation.style-close");
            ImGui::EndTabItem();
            }
            const bool colorsPage=ImGui::BeginTabItem("着色"); recordUiTestItem("creation.style-colors-page");
            if(colorsPage) {
            ImGui::SeparatorText("原子与半键着色");
            ImGui::BeginDisabled(documentsBusy());
            const char *colorKinds[]={"来源颜色","元素颜色","自定义颜色","属性渐变","分类编号"};
            int colorKind=int(creationColorDraft.kind);
            if(ImGui::BeginCombo("着色方式",colorKinds[colorKind])) {
                for(int k=0;k<5;++k) {
                    if(ImGui::Selectable(colorKinds[k],k==colorKind)) creationColorDraft.kind=creation::ColorKind(k);
                    recordUiTestItem("creation.color-kind-"+std::to_string(k));
                } ImGui::EndCombo();
            } recordUiTestItem("creation.color-kind");
            auto &cr=creationColorDraft;
            if(cr.kind==creation::ColorKind::Custom) ImGui::ColorEdit3("自定义色",&cr.rgb.x,ImGuiColorEditFlags_NoInputs);
            if(cr.kind>=creation::ColorKind::Property) {
                if(ImGui::BeginCombo("属性",cr.property.c_str())) {
                    std::unordered_set<std::string> offered;
                    auto choice=[&](const std::string &name) {
                        if(!offered.insert(name).second) return;
                        if(ImGui::Selectable(name.c_str(),cr.property==name)) cr.property=name;
                        recordUiTestItem("creation.color-property-"+name);
                    };
                    for(const auto &[name,v]:source.scalarProperties) {(void)v;choice(name);}
                    if(cr.kind==creation::ColorKind::Property) {
                        for(const char *p:{"Position.X","Position.Y","Position.Z"}) choice(p);
                        for(const auto &[name,v]:source.vectorProperties) { (void)v;
                            choice(name+".X");choice(name+".Y");choice(name+".Z");choice("|"+name+"|"); }
                    }
                    ImGui::EndCombo();
                } recordUiTestItem("creation.color-property");
                if(cr.kind==creation::ColorKind::Property) {
                    if(ImGui::Button("按当前范围调整")) {
                        const bool all=creationStyleAll; auto selection=all?std::vector<int>{}:creationSelection;
                        const std::string property=cr.property;
                        creationColorBusy=true;creationColorMessage="正在后台读取范围…";
                        creationColorRangeJob=std::async(std::launch::async,[this,all,selection=std::move(selection),property] {
                            return creation::colorRange(creation::ColorValues(source,property),source.atoms.size(),selection,all);
                        });
                    } recordUiTestItem("creation.color-range");
                    ImGui::SetNextItemWidth(U(210)); ImGui::InputDouble("下限",&cr.low,0,0,"%.6g");
                    ImGui::SetNextItemWidth(U(210)); ImGui::InputDouble("上限",&cr.high,0,0,"%.6g");
                    ImGui::Combo("渐变",&cr.gradient,"Rainbow\0Blue-White-Red\0Cyclic Rainbow\0Fast\0Grayscale\0Hot\0Jet\0Magma\0Viridis\0Plasma\0");
                    ImGui::Checkbox("反转颜色",&cr.reverse); ImGui::SameLine(); ImGui::Checkbox("12 段",&cr.discrete);
                    ImGui::Checkbox("显示全体系图例",&cr.legend);
                } else ImGui::TextWrapped("非负整数编号保持固定颜色；可选择运动分组或层编号，其他值为灰色。");
                ImGui::TextDisabled("缺失值为灰色；颜色随数据更新，范围仅手动调整。");
            }
            if(!creationColorMessage.empty()) ImGui::TextWrapped("%s",creationColorMessage.c_str());
            ImGui::BeginDisabled(!creation::validColor(cr) || (!creationStyleAll && creationSelection.empty()));
            if(ImGui::Button("应用着色",{U(110),U(30)})) {
                auto next=creationDisplay; next.setColor(source.atoms.size(),creationSelection,cr,creationStyleAll);
                editCreationDisplay(std::move(next),"编辑原子着色");
            } recordUiTestItem("creation.color-apply"); ImGui::EndDisabled(); ImGui::EndDisabled();
            ImGui::SameLine(); if(ImGui::Button("关闭",{U(110),U(30)})) showCreationStyles=false;
            recordUiTestItem("creation.color-close");
            ImGui::TextWrapped("来源颜色保留文件配色；元素颜色使用当前元素外观。着色不修改科学属性或键拓扑。");
            ImGui::EndTabItem();
            } ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }
    void motionGroupsDialog() {
        if (!creationMode) { showMotionGroups=false; return; }
        if (!showMotionGroups) return;
        if (motionCacheRevision!=motionRevision) {
            try { motionGroupCache=motion::catalog(source); }
            catch (const std::exception &e) { motionGroupCache.clear(); motionGroupMessage=e.what(); }
            motionCacheRevision=motionRevision; refreshFont=true;
        }
        ImGui::SetNextWindowSize({U(570),U(450)},ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({U(530),U(410)},{U(900),U(800)});
        if (ImGui::Begin("运动分组##motion-groups",&showMotionGroups)) {
            ImGui::TextDisabled("保留原子与显式键 · 分组不自动限制手工编辑");
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##group-name",motionGroupName,sizeof(motionGroupName));
            recordUiTestItem("creation.group-name");
            auto attempt=[&](auto action) { try { action(); } catch (const std::exception &e) { motionGroupMessage=e.what(); } };
            ImGui::BeginDisabled(documentsBusy());
            ImGui::BeginDisabled(creationSelection.empty() || !motionGroupName[0]);
            if (ImGui::Button("从选择创建")) attempt([&] {
                const int key=motion::prepare(source,creationSelection,motionGroupName);
                const auto selected=creationSelection; const std::string name=motionGroupName;
                editStructure("创建运动分组 · "+name,[&](Dataset &data){motion::create(data,selected,name,key);});
                motionGroupSelected=key; motionGroupMessage="已创建分组 · "+name;
            }); recordUiTestItem("creation.group-create"); ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("按独立片段自动分组")) attempt([&] {
                if (source.sampled() || !motion::catalog(source).empty())
                    throw std::invalid_argument("请使用完整体系，并先取消已有分组");
                const size_t count=source.atoms.size(); auto bonds=source.bonds;
                motionGroupJob=std::async(std::launch::async,[count,bonds=std::move(bonds)]{return motion::automatic(count,bonds);});
                motionGroupBusy=true; motionGroupMessage="正在查找连接分量...";
            }); recordUiTestItem("creation.group-auto"); ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::BeginChild("##group-list",{0,std::max(U(120),ImGui::GetContentRegionAvail().y-U(130))},ImGuiChildFlags_Borders)) {
                ImGuiListClipper clipper; clipper.Begin(int(motionGroupCache.size()));
                while (clipper.Step()) for (int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index) {
                    const auto &group=motionGroupCache[size_t(index)]; ImGui::PushID(group.id);
                    const std::string label=group.name+" · "+std::to_string(group.count)+" 原子";
                    if (ImGui::Selectable(label.c_str(),motionGroupSelected==group.id)) {
                        motionGroupSelected=group.id; snprintf(motionGroupName,sizeof(motionGroupName),"%s",group.name.c_str());
                    } recordUiTestItem("creation.group-row-"+std::to_string(group.id)); ImGui::PopID();
                }
                if (motionGroupCache.empty()) ImGui::TextDisabled("暂无分组");
            } ImGui::EndChild();
            ImGui::BeginDisabled(documentsBusy() || motionGroupSelected<=0);
            if (ImGui::Button("选中整组")) attempt([&]{selectMotionGroup(motionGroupSelected);});
            recordUiTestItem("creation.group-select"); ImGui::SameLine();
            if (ImGui::Button("整体移动 / 旋转...")) attempt([&] {
                if (selectMotionGroup(motionGroupSelected,true)) {
                    requestCreationMovement(); creationMovementMassCenter=bool(motion::massCenter(source,creationSelection)); showMotionGroups=false;
                }
            }); recordUiTestItem("creation.group-move"); ImGui::SameLine();
            if (ImGui::Button("改名")) attempt([&] {
                if (motion::members(source,motionGroupSelected).empty() || !motionGroupName[0])
                    throw std::invalid_argument("请选中有效分组并填写名称");
                const auto labels=motion::names(source);
                if (labels.contains(motionGroupSelected) && labels.at(motionGroupSelected)==motionGroupName) return;
                editStructure("运动分组改名",[&](Dataset &data){motion::rename(data,motionGroupSelected,motionGroupName);});
            }); recordUiTestItem("creation.group-rename"); ImGui::EndDisabled();
            ImGui::BeginDisabled(documentsBusy() || motionGroupSelected<=0);
            if (ImGui::Button("取消分组（保留原子）")) attempt([&] {
                if (motion::members(source,motionGroupSelected).empty()) return;
                editStructure("取消运动分组",[&](Dataset &data){motion::erase(data,motionGroupSelected);}); motionGroupSelected=0;
            }); recordUiTestItem("creation.group-remove"); ImGui::EndDisabled(); ImGui::SameLine();
            ImGui::BeginDisabled(documentsBusy() || motionGroupCache.empty());
            if (ImGui::Button("取消全部分组")) {
                editStructure("取消全部运动分组",[&](Dataset &data){
                    data.scalarProperties.erase(motion::property); motion::setNames(data,{});
                }); motionGroupSelected=0;
            } recordUiTestItem("creation.group-clear"); ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("关闭")) showMotionGroups=false; recordUiTestItem("creation.group-close");
            if (!motionGroupMessage.empty()) ImGui::TextWrapped("%s",motionGroupMessage.c_str());
        }
        ImGui::End();
    }
    const Dataset *layerSource(uint64_t id) const {
        for (size_t i=0;i<tabs.size();++i) if (tabs[i].id==id && !tabs[i].home)
            return int(i)==activeTab?&source:&tabs[i].source;
        return nullptr;
    }
    void requestLayerBuilder() {
        if (documentsBusy()) return;
        if (!creationMode) { openCreationTab(); if (!creationMode) return; }
        chooseCreationTool(CreationTool::Select);
        layerBuilderTabId=tabs[size_t(activeTab)].id;
        layerSourceIds[0]=layerSourceIds[1]=layerSourceIds[2]=layerBuilderTabId;
        for (const auto &tab:tabs) if (tab.id!=layerBuilderTabId && !tab.home) {
            try { (void)layers::frame(*layerSource(tab.id)); layerSourceIds[1]=tab.id; break; }
            catch (const std::exception &) {}
        }
        layerCount=2; layerOptions={}; layerBuilderMessage.clear(); layerBuildCompleted=false;
        for (int i=0;i<3;++i) { layerGaps[i]=3; layerOffsets[i][0]=layerOffsets[i][1]=0; layerCleaves[i]=layerFlips[i]=0; }
        openLayerBuilder=true;
    }
    std::vector<const Dataset *> selectedLayerSources() const {
        std::vector<const Dataset *> inputs;
        for (int i=0;i<layerCount;++i) inputs.push_back(layerSource(layerSourceIds[i]));
        return inputs;
    }
    std::vector<layers::Detail> selectedLayerDetails() const {
        std::vector<layers::Detail> details;
        for (int i=0;i<layerCount;++i) details.push_back({layerNames[i],layerGaps[i],layerOffsets[i][0],layerOffsets[i][1],
            layers::Cleave(layerCleaves[i]),layers::Flip(layerFlips[i])});
        return details;
    }
    void startLayerBuild() {
        if (documentsBusy() || !creationMode || tabs[size_t(activeTab)].id!=layerBuilderTabId) return;
        try {
            const auto inputs=selectedLayerSources(); auto details=selectedLayerDetails(); (void)layers::match(inputs,layerOptions,details);
            std::vector<Dataset> snapshots;
            size_t total=0;
            for (int i=0;i<layerCount;++i) {
                const auto *input=inputs[size_t(i)];
                if (input->atoms.size()>size_t(budget) || total>size_t(budget)-input->atoms.size())
                    throw std::invalid_argument("叠层原子数超过当前完整载入上限");
                total+=input->atoms.size(); snapshots.push_back(*input);
            }
            auto options=layerOptions; options.atomLimit=size_t(budget);
            layerBuildJob=std::async(std::launch::async,[inputs=std::move(snapshots),details=std::move(details),options]() {
                std::vector<const Dataset *> sources; for (const auto &input:inputs) sources.push_back(&input);
                return layers::build(sources,details,options);
            });
            layerBuildBusy=true; layerBuilderMessage="正在构建叠层...";
        } catch (const std::exception &e) { layerBuilderMessage=e.what(); }
    }
    void layerBuilderDialog() {
        if (openLayerBuilder) { ImGui::OpenPopup("构建叠层##layer-builder"); openLayerBuilder=false; }
        ImGui::SetNextWindowSize({U(630),U(545)},ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("构建叠层##layer-builder",nullptr,ImGuiWindowFlags_NoSavedSettings)) return;
        if (layerBuildCompleted) {
            layerBuildCompleted=false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return;
        }
        ImGui::TextWrapped("从已打开的标签选择 2–3 层。来源使用当前帧的源体系；需要修改器结果时，先创建其创作副本。");
        ImGui::BeginDisabled(layerBuildBusy);
        bool valid=creationMode && activeTab>=0 && tabs[size_t(activeTab)].id==layerBuilderTabId;
        std::string validation;
        layers::Match preview;
        size_t count=0;
        try {
            auto inputs=selectedLayerSources(); preview=layers::match(inputs,layerOptions,selectedLayerDetails());
            for (const auto *input:inputs) count+=input->atoms.size();
            if (count>size_t(budget)) throw std::invalid_argument("叠层原子数超过当前完整载入上限");
            for (int i=0;i<layerCount;++i) if (!layerNames[i][0] || !std::isfinite(layerGaps[i]) || layerGaps[i]<0 ||
                !std::isfinite(layerOffsets[i][0]) || !std::isfinite(layerOffsets[i][1]) ||
                std::abs(layerOffsets[i][0])>1000000 || std::abs(layerOffsets[i][1])>1000000)
                throw std::invalid_argument("请填写名称、非负真空和有限的面内偏移");
        } catch (const std::exception &e) { valid=false; validation=e.what(); }
        if (ImGui::BeginTabBar("##layer-pages")) {
            if (ImGui::BeginTabItem("定义层")) {
                if (ImGui::RadioButton("两层",layerCount==2)) { layerCount=2; if(layerOptions.matching==2)layerOptions.matching=-1; if(layerOptions.orientation==2)layerOptions.orientation=0; }
                recordUiTestItem("creation.layers-two"); ImGui::SameLine();
                if (ImGui::RadioButton("三层",layerCount==3)) layerCount=3;
                recordUiTestItem("creation.layers-three");
                for (int i=0;i<layerCount;++i) {
                    ImGui::PushID(i); ImGui::Separator();
                    ImGui::Text("层 %d",i+1);
                    std::string title="选择来源标签";
                    for (const auto &tab:tabs) if (tab.id==layerSourceIds[i]) title=tab.title;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##source",title.c_str())) {
                        for (const auto &tab:tabs) if (!tab.home && layerSource(tab.id)) {
                            ImGui::PushID(int(tab.id));
                            if (ImGui::Selectable(tab.title.c_str(),tab.id==layerSourceIds[i])) layerSourceIds[i]=tab.id;
                            recordUiTestItem("creation.layers-source-"+std::to_string(i)+"-"+std::to_string(tab.id));
                            ImGui::PopID();
                        } ImGui::EndCombo();
                    } recordUiTestItem("creation.layers-source-"+std::to_string(i));
                    ImGui::SetNextItemWidth(-1); ImGui::InputText("##name",layerNames[i],sizeof(layerNames[i]));
                    recordUiTestItem("creation.layers-name-"+std::to_string(i)); ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::RadioButton("3D 周期晶体",!layerOptions.surface)) layerOptions.surface=false;
                recordUiTestItem("creation.layers-crystal"); ImGui::SameLine();
                if (ImGui::RadioButton("2D 表面",layerOptions.surface)) layerOptions.surface=true;
                recordUiTestItem("creation.layers-surface"); ImGui::EndTabItem();
            } recordUiTestItem("creation.layers-define-tab");
            if (ImGui::BeginTabItem("层参数")) {
                ImGui::TextWrapped("真空加在该层上方。偏移使用匹配后 a、b 的分数坐标；表面输出不加末层真空。");
                ImGui::BeginChild("##layer-detail-scroll",{0,U(290)});
                for (int i=0;i<layerCount;++i) {
                    ImGui::PushID(i); ImGui::Separator(); ImGui::Text("层 %d · %s",i+1,layerNames[i]);
                    ImGui::BeginDisabled(layerOptions.surface && i==layerCount-1);
                    ImGui::InputDouble("上方真空 (Å)",&layerGaps[i],0,0,"%.4f");
                    recordUiTestItem("creation.layers-gap-"+std::to_string(i)); ImGui::EndDisabled();
                    ImGui::InputDouble("面内偏移 a",&layerOffsets[i][0],0,0,"%.4f");
                    recordUiTestItem("creation.layers-offset-a-"+std::to_string(i));
                    ImGui::InputDouble("面内偏移 b",&layerOffsets[i][1],0,0,"%.4f");
                    recordUiTestItem("creation.layers-offset-b-"+std::to_string(i));
                    const auto *input=layerSource(layerSourceIds[i]);
                    ImGui::BeginDisabled(!input || !input->pbc[2]);
                    const char *cleaves[]={"原子切割","完整分子（质心归属）"};
                    if (ImGui::BeginCombo("切割规则",cleaves[layerCleaves[i]])) {
                        for (int choice=0;choice<2;++choice) {
                            if (ImGui::Selectable(cleaves[choice],layerCleaves[i]==choice)) layerCleaves[i]=choice;
                            recordUiTestItem("creation.layers-cleave-"+std::to_string(i)+"-"+std::to_string(choice));
                        } ImGui::EndCombo();
                    }
                    recordUiTestItem("creation.layers-cleave-"+std::to_string(i)); ImGui::EndDisabled();
                    const char *flips[]={"不翻转","A / u","B / v"};
                    if (ImGui::BeginCombo("翻转",flips[layerFlips[i]])) {
                        for (int choice=0;choice<3;++choice) {
                            if (ImGui::Selectable(flips[choice],layerFlips[i]==choice)) layerFlips[i]=choice;
                            recordUiTestItem("creation.layers-flip-"+std::to_string(i)+"-"+std::to_string(choice));
                        } ImGui::EndCombo();
                    }
                    recordUiTestItem("creation.layers-flip-"+std::to_string(i)); ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            } recordUiTestItem("creation.layers-details-tab");
            if (ImGui::BeginTabItem("晶格匹配")) {
                if (ImGui::RadioButton("a、b、γ 取各层平均",layerOptions.matching<0)) layerOptions.matching=-1;
                recordUiTestItem("creation.layers-average");
                for (int i=0;i<layerCount;++i) {
                    const auto label="采用层 "+std::to_string(i+1)+" 的 a、b、γ";
                    if (ImGui::RadioButton(label.c_str(),layerOptions.matching==i)) layerOptions.matching=i;
                    recordUiTestItem("creation.layers-match-"+std::to_string(i));
                }
                ImGui::Separator();
                if (ImGui::RadioButton("保持体积（调整晶体层厚度）",layerOptions.constantVolume)) layerOptions.constantVolume=true;
                recordUiTestItem("creation.layers-volume");
                if (ImGui::RadioButton("保持厚度（允许体积改变）",!layerOptions.constantVolume)) layerOptions.constantVolume=false;
                recordUiTestItem("creation.layers-thickness");
                ImGui::TextWrapped("晶体原子的分数坐标保持不变。表面来源保持原子法向跨度，并去掉来源晶胞中的外部真空。");
                ImGui::EndTabItem();
            } recordUiTestItem("creation.layers-matching-tab");
            if (ImGui::BeginTabItem("选项")) {
                ImGui::BeginChild("##layer-options-scroll",{0,U(290)});
                for (int i=0;i<layerCount;++i) {
                    const auto label="采用层 "+std::to_string(i+1)+" 的空间朝向";
                    if (ImGui::RadioButton(label.c_str(),layerOptions.orientation==i)) layerOptions.orientation=i;
                    recordUiTestItem("creation.layers-orient-"+std::to_string(i));
                }
                ImGui::InputDouble("晶格变形提示 (%)",&layerOptions.warningPercent,0,0,"%.2f");
                ImGui::TextWrapped("原子切割按 c 分数坐标 0–1 取层。完整分子仅按已有显式键识别，无键的原子视为独立原子；以质心归属保留整分子，厚度计入范德华半径。无限周期键网络需用原子切割。表面来源不再切割。");
                ImGui::TextWrapped("A / B 翻转反转层的上下方向与另一面内轴，γ 变为 180°−γ，再参与匹配。输出 P1 / p1，不自动重建跨层键或对称性。");
                ImGui::TextWrapped("原子属性值保留；各层矢量属性仍按来源坐标系解释。来源坐标系记录在 LayerBases 数据表。");
                ImGui::EndChild();
                ImGui::EndTabItem();
            } recordUiTestItem("creation.layers-options-tab");
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (!validation.empty()) ImGui::TextWrapped("%s",validation.c_str());
        else {
            ImGui::Text("%s 原子 · a %.4f Å · b %.4f Å · γ %.3f°",number(count).c_str(),preview.al,preview.bl,preview.gamma);
            for (size_t i=0;i<preview.mismatch.size();++i) if (preview.mismatch[i]>layerOptions.warningPercent)
                ImGui::TextColored({1.f,.68f,.2f,1.f},"层 %zu 晶格变形 %.2f%%，超过提示阈值",i+1,preview.mismatch[i]);
        }
        ImGui::BeginDisabled(!valid || documentsBusy());
        if (ImGui::Button("构建",{U(110),U(32)})) startLayerBuild();
        recordUiTestItem("creation.layers-build"); ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("关闭",{U(110),U(32)})) ImGui::CloseCurrentPopup();
        recordUiTestItem("creation.layers-close"); ImGui::EndDisabled();
        if (!layerBuilderMessage.empty()) ImGui::TextWrapped("%s",layerBuilderMessage.c_str());
        ImGui::EndPopup();
    }
    void requestCreationMovement() {
        if (!creationMode || documentsBusy()) return;
        creationMovementSelection=creationTransformSelection();
        if(creationMovementSelection.empty())return;
        creationMovementBonds=creationBondSelection;
        creationMovementPeriodic=std::any_of(creationMovementBonds.begin(),creationMovementBonds.end(),
            [&](int i){return source.bonds[size_t(i)].image!=std::array<int32_t,3>{};});
        creationMovementTabId=tabs[size_t(activeTab)].id;
        creationMovementGeneration=pipelineGeneration;
        creationMovementMessage.clear();
        const auto projection=creationProjection(source,cameras[3],creationViewportSize);
        const auto inverseView=DirectX::XMMatrixInverse(nullptr,projection.view);
        for (int axis=0;axis<3;++axis) {
            DirectX::XMFLOAT3 direction;
            DirectX::XMStoreFloat3(&direction,DirectX::XMVector3TransformNormal(
                DirectX::XMVectorSet(axis==0?1.f:0.f,axis==1?1.f:0.f,axis==2?1.f:0.f,0),inverseView));
            creationMovementAxes[axis]={direction.x,direction.y,direction.z};
        }
        double x=0,y=0,z=0;
        for (int index:creationMovementSelection) {
            if (index<0 || size_t(index)>=source.atoms.size()) return;
            const auto &atom=source.atoms[size_t(index)]; x+=atom.x; y+=atom.y; z+=atom.z;
        }
        const double count=double(creationMovementSelection.size());
        DirectX::XMFLOAT4 q;
        DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
            DirectX::XMVectorSet(float(x/count),float(y/count),float(z/count),1),projection.combined));
        creationMovementScreenSpan=0;
        if (q.w>0 && creationViewportSize.x>0 && creationViewportSize.y>0) {
            const float extent=std::min(creationViewportSize.x,creationViewportSize.y);
            const auto a=creationWorldAt({0,0},q.z/q.w,{},creationViewportSize,projection.combined);
            const auto b=creationWorldAt({extent,0},q.z/q.w,{},creationViewportSize,projection.combined);
            creationMovementScreenSpan=float(authoring::length(authoring::sub(b,a)));
        }
        creationMovementMassCenter=false;
        creationMovementHasMass=bool(motion::massCenter(source,creationMovementSelection));
        openCreationMovement=true;
    }
    bool validCreationMovement() const {
        return creationMode && activeTab>=0 && activeTab<int(tabs.size()) &&
            tabs[size_t(activeTab)].id==creationMovementTabId && creationMovementGeneration==pipelineGeneration && !creationMovementSelection.empty();
    }
    void applyCreationMovement(int axis, int direction, bool rotate) {
        if (!validCreationMovement() || documentsBusy() || axis<0 || axis>2) return;
        Vec3 vector=creationMovementScreenAxes?creationMovementAxes[axis]:
            Vec3{axis==0?1.f:0.f,axis==1?1.f:0.f,axis==2?1.f:0.f};
        const double step=creationMovementPercent?
            double(creationMovementDistance)*creationMovementScreenSpan/100:creationMovementDistance;
        try {
            const auto positions=authoring::transformedSelection(source,creationMovementSelection,
                rotate?Vec3{}:authoring::scale(vector,step*direction),
                rotate?vector:Vec3{},rotate?double(creationMovementAngle)*direction:0,
                creationMovementMassCenter?motion::massCenter(source,creationMovementSelection):std::nullopt);
            if (positions.empty()) { creationMovementMessage="坐标未改变"; return; }
            const std::string message=(rotate?"精准旋转 ":"精准移动 ")+std::to_string(positions.size())+" 个原子";
            editStructure(message,[&](Dataset &data) {
                for (const auto &[index,at]:positions) {
                    auto &atom=data.atoms[size_t(index)]; atom.x=at.x; atom.y=at.y; atom.z=at.z;
                }
            });
            // This edit changes only coordinates, so captured source edge rows
            // retain identity. Other source edits invalidate the dialog.
            creationMovementGeneration=pipelineGeneration;
            creationBondSelection=creationMovementBonds;
            creationMovementMessage=message+" · 已记录一步历史";
        } catch (const std::exception &e) { creationMovementMessage=e.what(); }
    }
    struct CreationProjection {
        DirectX::XMMATRIX view, projection, combined;
    };
    CreationProjection creationProjection(const Dataset &data, const Camera &camera, ImVec2 size) {
        CreationProjection projected;
        projected.combined=gpu.matrix(data,camera,size.x/std::max(size.y,1.f),cell,radius,
            &projected.view,&projected.projection,.43f);
        return projected;
    }
    float creationScreenRadius(const Atom &atom, const CreationProjection &projection,
                               ImVec2 size,int index=-1) const {
        float worldRadius=radius*.43f;
        if (atom.type<gpu.styles.size()) {
            const auto &style=gpu.styles[atom.type];
            const float base=style.visual[0]>0?style.visual[0]
                :style.visual[3]>0?style.visual[3]:radius;
            worldRadius=base*.43f*std::max({style.axes[0],style.axes[1],style.axes[2],.05f});
        }
        const auto preset=index>=0?creationDisplay.presetAt(size_t(index)):0;
        if(preset==1) worldRadius=.04f;
        if(preset==2) worldRadius=creationDisplay.stickRadius;
        if(preset==3) worldRadius=creationDisplay.ballRadius;
        if(preset==4) {
            const auto *e=atom.type<source.species.size()?elements::find(source.species[atom.type]):nullptr;
            const float covalent=atom.type<gpu.styles.size()?gpu.styles[atom.type].visual[3]:0;
            worldRadius=(e && e->vdw>0?e->vdw:covalent>0?covalent:radius)*creationDisplay.cpkScale;
        }
        using namespace DirectX;
        const auto center=XMVector4Transform(XMVectorSet(atom.x,atom.y,atom.z,1),projection.view);
        const auto projected=XMVector4Transform(center,projection.projection);
        const auto projectedX=XMVector4Transform(XMVectorAdd(center,XMVectorSet(worldRadius,0,0,0)),projection.projection);
        const auto projectedY=XMVector4Transform(XMVectorAdd(center,XMVectorSet(0,worldRadius,0,0)),projection.projection);
        XMFLOAT4 q{},qx{},qy{};
        XMStoreFloat4(&q,projected); XMStoreFloat4(&qx,projectedX); XMStoreFloat4(&qy,projectedY);
        if (q.w<=0||qx.w<=0||qy.w<=0) return U(12);
        const float rx=std::abs(qx.x/qx.w-q.x/q.w)*size.x*.5f;
        const float ry=std::abs(qy.y/qy.w-q.y/q.w)*size.y*.5f;
        return std::max(U(6),std::max(rx,ry)*1.12f+U(3));
    }
    int creationHit(ImVec2 p, ImVec2 size, const Camera &cam, ImVec2 mouse) {
        if (result.data.atoms.empty()) return -1;
        const auto projection=creationProjection(result.data,cam,size);
        const auto matrix=projection.combined;
        int hit=-1;
        float bestDepth=FLT_MAX, bestPixel=FLT_MAX;
        for (size_t index=0; index<result.data.atoms.size(); ++index) {
            if (!creationAtomVisible(int(index))) continue;
            const auto &atom=result.data.atoms[index];
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                DirectX::XMVectorSet(atom.x,atom.y,atom.z,1),matrix));
            if (q.w<=0 || q.z<=0) continue;
            const float x=p.x+(q.x/q.w+1)*size.x*.5f;
            const float y=p.y+(1-q.y/q.w)*size.y*.5f;
            const float dx=x-mouse.x,dy=y-mouse.y, distance=dx*dx+dy*dy;
            const float hitRadius=creationScreenRadius(atom,projection,size,int(index));
            if (distance>hitRadius*hitRadius) continue;
            const float depth=q.z/q.w;
            if (depth<bestDepth-1e-4f || (std::abs(depth-bestDepth)<1e-4f && distance<bestPixel)) {
                hit=int(index); bestDepth=depth; bestPixel=distance;
            }
        }
        return hit;
    }
    Vec3 creationWorldAt(ImVec2 screen, float depth, ImVec2 p, ImVec2 size,
                         const DirectX::XMMATRIX &matrix) {
        const float nx=2.f*(screen.x-p.x)/std::max(size.x,1.f)-1.f;
        const float ny=1.f-2.f*(screen.y-p.y)/std::max(size.y,1.f);
        const auto inverse=DirectX::XMMatrixInverse(nullptr,matrix);
        DirectX::XMFLOAT4 q;
        DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
            DirectX::XMVectorSet(nx,ny,depth,1.f),inverse));
        const float w=std::abs(q.w)>1e-6f?q.w:1.f;
        return {q.x/w,q.y/w,q.z/w};
    }
    struct CreationBondHalf { Vec3 p1,p2;uint8_t preset=0; };
    std::vector<CreationBondHalf> creationBondHalves(int row) const {
        std::vector<CreationBondHalf> halves;
        if(row<0 || size_t(row)>=source.bonds.size() || creationDisplay.bondVisibility.isHidden(size_t(row)))return halves;
        const auto &b=source.bonds[size_t(row)];
        if(b.a>=result.data.atoms.size() || b.b>=result.data.atoms.size())return halves;
        const auto &a=result.data.atoms[b.a],&z=result.data.atoms[b.b];
        const auto segments=bondSegments(result.data.cell,{a.x,a.y,a.z},{z.x,z.y,z.z},b.image);
        const bool periodic=b.image!=std::array<int32_t,3>{};
        for(size_t i=0;i<segments.size();++i) {
            const auto &segment=segments[i];
            if(periodic) {
                const auto preset=creationDisplay.presetAt(i==0?b.a:b.b);
                if(preset!=4)halves.push_back({segment.p1,segment.p2,preset});
            } else {
                const auto middle=authoring::scale(authoring::add(segment.p1,segment.p2),.5);
                const auto pa=creationDisplay.presetAt(b.a),pb=creationDisplay.presetAt(b.b);
                if(pa!=4)halves.push_back({segment.p1,middle,pa});
                if(pb!=4)halves.push_back({middle,segment.p2,pb});
            }
        }
        return halves;
    }
    // Match the renderer's lane spacing, endpoint presets and translated
    // periodic stubs. Sketch/geometry tools still request direct edges only.
    int creationBondHit(ImVec2 p,ImVec2 size,const Camera &cam,ImVec2 mouse,bool includePeriodic=false) {
        if(!source.bondStyle.visible || source.bonds.size()>interactiveBondBudget || gpu.bondsOmittedForPerformance())return -1;
        const auto projection=creationProjection(result.data,cam,size);
        const auto matrix=projection.combined;
        DirectX::XMFLOAT3 forward;
        DirectX::XMStoreFloat3(&forward,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0,0,1,0),
            DirectX::XMMatrixInverse(nullptr,projection.view)));
        auto project=[&](Vec3 at,ImVec2 &point,float &depth) {
            DirectX::XMFLOAT4 q;DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),matrix));
            if(q.w<=0 || q.z<=0 || q.z>=q.w)return false;
            point={p.x+(q.x/q.w+1)*size.x*.5f,p.y+(1-q.y/q.w)*size.y*.5f};depth=q.z/q.w;return true;
        };
        int found=-1;float bestDepth=FLT_MAX;
        for(size_t i=0;i<source.bonds.size();++i) {
            const auto &bond=source.bonds[i];
            if(!creationBondVisible(int(i)) || (!includePeriodic && bond.image!=std::array<int32_t,3>{}))continue;
            const int order=bond.order==4?2:std::clamp(int(bond.order),1,3);
            for(const auto &segment:creationBondHalves(int(i))) {
                const auto delta=authoring::sub(segment.p2,segment.p1);
                const double len=authoring::length(delta);if(len<1e-6 || !std::isfinite(len))continue;
                const auto axis=authoring::scale(delta,1/len);
                auto side=authoring::cross(axis,{forward.x,forward.y,forward.z});
                if(authoring::length(side)<1e-5)side=authoring::cross(axis,std::abs(axis.z)<.85f?Vec3{0,0,1}:Vec3{0,1,0});
                side=authoring::scale(side,1/authoring::length(side));
                const auto preset=segment.preset;
                float radius=creationDisplay.bondRadius(bond.order,preset,source.bondStyle.radius);
                if(gpu.renderedBondLaneCount()>cylinderBondBudget)radius=0;
                const float width=preset?creationDisplay.lineWidth:source.bondStyle.width;
                ImVec2 boundA,boundB;float depth;
                if(!project(segment.p1,boundA,depth) || !project(segment.p2,boundB,depth))continue;
                float padding=U(6)+width*std::max(3.f,float(order)*1.5f);
                if(radius>0) {
                    const double extent=radius*(1+1.5*order);ImVec2 edge;
                    for(const auto at:{segment.p1,segment.p2})if(project(authoring::add(at,authoring::scale(side,extent)),edge,depth)) {
                        ImVec2 center;if(project(at,center,depth))padding=std::max(padding,U(6)+std::hypot(edge.x-center.x,edge.y-center.y));
                    }
                }
                if(mouse.x<std::min(boundA.x,boundB.x)-padding || mouse.x>std::max(boundA.x,boundB.x)+padding ||
                    mouse.y<std::min(boundA.y,boundB.y)-padding || mouse.y>std::max(boundA.y,boundB.y)+padding)continue;
                for(int lane=-1;lane<order;++lane) {
                    // The centerline is also a convenient target between lanes.
                    const float offset=lane<0?0:bond.order==4?(lane?.5f:-.5f):float(lane)-float(order-1)*.5f;
                    const auto shift=authoring::scale(side,offset*radius*3);
                    ImVec2 a,b;float za,zb;
                    if(!project(authoring::add(segment.p1,shift),a,za) || !project(authoring::add(segment.p2,shift),b,zb))continue;
                    const float dx=b.x-a.x,dy=b.y-a.y,lengthSquared=dx*dx+dy*dy;if(lengthSquared<1)continue;
                    const float norm=std::sqrt(lengthSquared);
                    if(radius==0) {
                        const float pixels=offset*std::max(width*3,4.f);
                        a.x+=dy/norm*pixels;a.y-=dx/norm*pixels;b.x+=dy/norm*pixels;b.y-=dx/norm*pixels;
                    }
                    const float t=std::clamp(((mouse.x-a.x)*dx+(mouse.y-a.y)*dy)/lengthSquared,0.f,1.f);
                    float tolerance=U(6)+width*.5f;
                    if(radius>0) {
                        const auto center=authoring::add(authoring::add(segment.p1,shift),authoring::scale(delta,t));
                        ImVec2 c,edge;
                        if(project(center,c,depth) && project(authoring::add(center,authoring::scale(side,radius)),edge,depth))
                            tolerance=U(6)+std::hypot(edge.x-c.x,edge.y-c.y);
                    }
                    const float distance=std::hypot(mouse.x-a.x-t*dx,mouse.y-a.y-t*dy),z=za+(zb-za)*t;
                    if(distance<=tolerance && z<bestDepth) {bestDepth=z;found=int(i);}
                }
            }
        }
        return found;
    }
    bool creationPointer(ImVec2 p, ImVec2 size, Camera &cam, bool hovered) {
        auto &io=ImGui::GetIO();
        if(io.AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape)) { geometryPending.clear(); geometryPanelMonitor=-1; }
        if (creationTool!=CreationTool::Fragment) clearCreationFusion();
        if ((io.AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape)) &&
            (creationFusionSeed || creationFusionPick)) {
            const bool previewOnly=creationDrag==CreationDrag::None || creationDrag==CreationDrag::Fusion;
            clearCreationFusion(); creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
            if (previewOnly) { creationDrag=CreationDrag::None; return false; }
        }
        if (!hovered || io.MouseWheel!=0 || (creationDrag!=CreationDrag::None && creationDrag!=CreationDrag::Fusion)) {
            creationFusionPreview.reset(); creationFusionTarget=-2;
        }
        creationSketchPreviewValid=false;
        if (creationTool!=CreationTool::Fragment || !hovered || io.AppFocusLost || io.MouseWheel!=0 ||
            (creationDrag!=CreationDrag::None && creationDrag!=CreationDrag::Fragment)) {
            creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
        }
        if (creationTool!=CreationTool::Ring || !hovered || io.AppFocusLost || io.MouseWheel!=0 ||
            (creationDrag!=CreationDrag::None && creationDrag!=CreationDrag::Ring)) creationRingPreviewValid=false;
        if (io.AppFocusLost && (creationDrag==CreationDrag::Ring || creationDrag==CreationDrag::Fragment)) {
            creationDrag=CreationDrag::None; return false;
        }
        if (io.AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            creationSketchAnchor=creationSketchLastPlaced=-1;
            creationSketchInsert=false;creationInsertHover=-1;
        }
        // The first click may change the automatic camera bounds or start
        // an asynchronous pipeline update. Finish on the second press before
        // either can make the old screen coordinate create another atom.
        if (hovered && creationTool==CreationTool::Sketch &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            (creationSketchAnchor>=0 || creationSketchLastPlaced>=0)) {
            creationSketchAnchor=creationSketchLastPlaced=-1;
            creationDrag=CreationDrag::None; creationDragAtoms.clear();
            return false;
        }
        if (documentsBusy() || staleResult || !sameAtomCount()) {
            // A history/tab/data change can finish while a button is held.
            // Discard the old gesture rather than miss its single release
            // event and retain atom indexes from an outdated document.
            creationDrag=CreationDrag::None; creationDragAtoms.clear();
            creationRingPreviewValid=false;
            creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
            creationLastHoverMouse={-1,-1}; clearCreationFusion();
            creationInsertHover=-1;creationInsertMouse={-1,-1};
            return false;
        }
        const ImVec2 mouse=io.MousePos;
        bool contextRequested=false;
        if (creationDrag!=CreationDrag::None && (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.AppFocusLost)) {
            if(creationDrag==CreationDrag::Rotate || creationDrag==CreationDrag::Pan || creationDrag==CreationDrag::Zoom) cam=creationDragCamera;
            for (const auto &[index,original]:creationDragAtoms) {
                auto &a=result.data.atoms[size_t(index)];
                a.x=original.x; a.y=original.y; a.z=original.z;
            }
            if (!creationDragAtoms.empty()) uploadCreationDisplay(result.data,result.selected,result.colorSelected);
            creationDragAtoms.clear(); creationDrag=CreationDrag::None;
            creationRingPreviewValid=false;
            creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
            creationLastHoverMouse={-1,-1};
            return false;
        }
        if ((creationDrag==CreationDrag::None || creationDrag==CreationDrag::Sketch) && hovered &&
            (mouse.x!=creationLastHoverMouse.x || mouse.y!=creationLastHoverMouse.y)) {
            creationHover=creationHit(p,size,cam,mouse);
            creationLastHoverMouse=mouse;
        }
        if (!hovered && creationDrag==CreationDrag::None) {
            creationHover=-1;
            creationLastHoverMouse={-1,-1};
        }
        // Only an insertion gesture needs this preview. Cache by pointer,
        // projection and dataset generation; an idle frame never scans bonds.
        if(creationTool==CreationTool::Sketch && hovered && (creationSketchInsert || io.KeyAlt) &&
            (creationDrag==CreationDrag::None || creationDrag==CreationDrag::Sketch) && creationHover<0) {
            DirectX::XMFLOAT4X4 matrix;
            DirectX::XMStoreFloat4x4(&matrix,creationProjection(result.data,cam,size).combined);
            if(mouse.x!=creationInsertMouse.x || mouse.y!=creationInsertMouse.y ||
                size.x!=creationInsertSize.x || size.y!=creationInsertSize.y || p.x!=creationInsertOrigin.x || p.y!=creationInsertOrigin.y || pipelineGeneration!=creationInsertGeneration ||
                std::memcmp(&matrix,&creationInsertMatrix,sizeof(matrix))!=0) {
                creationInsertHover=creationBondHit(p,size,cam,mouse);
                creationInsertMouse=mouse;creationInsertSize=size;creationInsertOrigin=p;creationInsertMatrix=matrix;creationInsertGeneration=pipelineGeneration;
            }
        }else{creationInsertHover=-1;creationInsertMouse={-1,-1};}
        auto ringAt=[&](ImVec2 pointer) {
            const auto projection=creationProjection(result.data,cam,size);
            const int hit=creationHover;
            const int bond=hit<0?creationBondHit(p,size,cam,pointer):-1;
            Vec3 center=authoring::cartesian(source,.5,.5,.5);
            if (hit>=0) {
                const auto &a=source.atoms[size_t(hit)]; center={a.x,a.y,a.z};
            }
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                DirectX::XMVectorSet(center.x,center.y,center.z,1),projection.combined));
            if (q.w<=0) throw std::invalid_argument("绘制位置在视图之外");
            const auto inverse=DirectX::XMMatrixInverse(nullptr,projection.view);
            DirectX::XMFLOAT3 right,normal;
            DirectX::XMStoreFloat3(&right,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(1,0,0,0),inverse));
            DirectX::XMStoreFloat3(&normal,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0,0,-1,0),inverse));
            return authoring::ringSketch(source,creationRingSize,
                creationWorldAt(pointer,q.z/q.w,p,size,projection.combined),
                {right.x,right.y,right.z},{normal.x,normal.y,normal.z},hit,bond,io.KeyAlt);
        };
        if (creationTool==CreationTool::Ring && creationDrag==CreationDrag::None && hovered && !io.AppFocusLost &&
            (!creationRingPreviewValid || mouse.x!=creationRingMouse.x || mouse.y!=creationRingMouse.y ||
             creationRingPreview.aromatic!=io.KeyAlt)) {
            try { creationRingPreview=ringAt(mouse); creationRingPreviewValid=true; creationRingMouse=mouse; }
            catch (const std::exception &) { creationRingPreviewValid=false; }
        }
        if (creationTool==CreationTool::Fragment && (creationFusionSeed || creationFusionPick || io.KeyAlt)) {
            creationFragmentPreviewValid=false; creationFragmentMouse={-1,-1};
        }
        if (creationTool==CreationTool::Fragment && creationFusionSeed && creationDrag==CreationDrag::None &&
            hovered && creationFusionTarget!=creationHover) {
            creationFusionTarget=creationHover; creationFusionPreview.reset();
            if (creationHover>=0) try {
                const auto projection=creationProjection(result.data,cam,size); DirectX::XMFLOAT3 normal;
                DirectX::XMStoreFloat3(&normal,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0,0,-1,0),
                    DirectX::XMMatrixInverse(nullptr,projection.view)));
                creationFusionPreview=fragments::alignFusion(source,creationFusionSeed,creationHover,{normal.x,normal.y,normal.z});
            } catch (const std::exception &e) { status=e.what(); }
        }
        if (creationTool==CreationTool::Fragment && !creationFusionSeed && !creationFusionPick && !io.KeyAlt &&
            creationDrag==CreationDrag::None && hovered && !io.AppFocusLost &&
            (mouse.x!=creationFragmentMouse.x || mouse.y!=creationFragmentMouse.y)) {
            creationFragmentMouse=mouse;
            try {
                const auto *t=currentFragment(); if (!t) throw std::invalid_argument("片段库尚未加载");
                const auto projection=creationProjection(result.data,cam,size);
                Vec3 center=creationHover>=0?fragments::at(source,creationHover):authoring::cartesian(source,.5,.5,.5);
                DirectX::XMFLOAT4 q;
                DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(center.x,center.y,center.z,1),projection.combined));
                if (q.w<=0) throw std::invalid_argument("绘制位置在视图之外");
                const auto inverse=DirectX::XMMatrixInverse(nullptr,projection.view);
                DirectX::XMFLOAT3 right,normal;
                DirectX::XMStoreFloat3(&right,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(1,0,0,0),inverse));
                DirectX::XMStoreFloat3(&normal,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0,0,-1,0),inverse));
                creationFragmentPreview=fragments::place(source,*t,creationFragmentConnector,
                    creationWorldAt(mouse,q.z/q.w,p,size,projection.combined),{right.x,right.y,right.z},{normal.x,normal.y,normal.z},creationHover);
                creationFragmentPreviewValid=true;
            } catch (const std::exception &) { creationFragmentPreviewValid=false; }
        }
        auto captureAtoms=[&]() {
            creationDragAtoms.clear(); creationDragCenter={};
            const auto selected=creationTransformSelection();
            for (int index:selected)
                if (index>=0 && size_t(index)<source.atoms.size()) {
                    const auto &a=source.atoms[size_t(index)];
                    const Vec3 at{a.x,a.y,a.z};
                    creationDragAtoms.push_back({index,at});
                    creationDragCenter=authoring::add(creationDragCenter,at);
                }
            if (!creationDragAtoms.empty())
                creationDragCenter=authoring::scale(creationDragCenter,1.0/creationDragAtoms.size());
            creationDragMatrix=creationProjection(result.data,cam,size).combined;
            creationMoveDelta={};
        };
        // MS 2020 installed Help: mouseandkeyboardactions.htm. Keep the v2
        // middle-button orbit, and add the documented right-button gestures.
        if (hovered && creationDrag==CreationDrag::None &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
             ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            creationDragButton=ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                ? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
            creationDragStart=creationDragPrevious=mouse;
            creationDragMoved=false;
            creationDragCamera=cam;
            creationDragAtoms.clear();
            creationDragRollAngle=0;
            // Capture the edge region at mouse down. Crossing the boundary
            // during a drag must not suddenly change the rotation axis.
            creationDragEdgeRoll=creationDragButton==ImGuiMouseButton_Right &&
                std::max(std::abs((mouse.x-p.x)/std::max(size.x,1.f)*2-1),
                         std::abs((mouse.y-p.y)/std::max(size.y,1.f)*2-1))>.8f;
            if (io.KeyShift && (io.KeyAlt || creationDragButton==ImGuiMouseButton_Middle)) {
                creationDrag=CreationDrag::Move; captureAtoms();
            } else if (io.KeyShift) {
                creationDrag=CreationDrag::Spin; captureAtoms();
            } else creationDrag=io.KeyAlt?CreationDrag::Pan:CreationDrag::Rotate;
        }
        if (hovered && creationDrag==CreationDrag::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            creationDragButton=ImGuiMouseButton_Left;
            creationDragStart=creationDragPrevious=mouse;
            creationDragMoved=false;
            creationDragCamera=cam;
            creationDragAtoms.clear();
            creationDragEdgeRoll=false;
            creationDragRollAngle=0;
            creationDragShift=io.KeyShift||io.KeyCtrl;
            creationDragToggle=io.KeyCtrl;
            const int hit=creationHit(p,size,cam,mouse);
            int monitorHit=-1; geometryOverlay(p,size,cam,nullptr,&monitorHit);
            if(monitorHit>=0) {
                const auto count=creationDisplay.monitors[size_t(monitorHit)].count;
                chooseCreationTool(count==2?CreationTool::Distance:count==3?CreationTool::Angle:CreationTool::Torsion);
                activateGeometry(monitorHit);
            } else if(isGeometryTool(creationTool)) {
                const int count=creationTool==CreationTool::Distance?2:creationTool==CreationTool::Angle?3:4;
                if(hit>=0) {
                    if(std::find(geometryPending.begin(),geometryPending.end(),hit)==geometryPending.end()) {
                        geometryPending.push_back(hit); creationDisplay.activeMonitor=-1;
                    }
                    if(int(geometryPending.size())==count) {
                        geometry::Monitor m; m.count=uint8_t(count);
                        for(int i=0;i<count;++i) m.atoms[size_t(i)]=geometryPending[size_t(i)];
                        addGeometryMonitor(m); geometryPending.clear();
                    }
                } else if(count==2 && geometryPending.empty() && creationBondHit(p,size,cam,mouse)>=0) {
                    const auto &b=source.bonds[size_t(creationBondHit(p,size,cam,mouse))];
                    addGeometryMonitor({2,{int32_t(b.a),int32_t(b.b),-1,-1}});
                } else if(geometryPending.empty() && creationDisplay.monitorsVisible && creationDisplay.activeMonitor>=0) {
                    const auto &m=creationDisplay.monitors[size_t(creationDisplay.activeMonitor)];
                    geometryDragPlans[0].reset(); geometryDragPlans[1].reset();
                    std::string errors[2];
                    for(int side=0;side<2;++side) try { geometryDragPlans[side]=geometry::prepare(source,m,side!=0); }
                        catch(const std::exception &e) { errors[side]=e.what(); }
                    if(!geometryDragPlans[io.KeyAlt?1:0]) { status=errors[io.KeyAlt?1:0]; g_cursorOverride=LoadCursor(nullptr,IDC_NO); }
                    else {
                        creationDrag=CreationDrag::Geometry; geometryDragInverted=io.KeyAlt;
                        geometryDragTarget=geometry::value(source,m).value_or(0); creationDragAtoms.clear();
                        for(const auto &plan:geometryDragPlans) if(plan)
                            creationDragAtoms.insert(creationDragAtoms.end(),plan->originals.begin(),plan->originals.end());
                    }
                }
            } else if (creationTool==CreationTool::Rotate) creationDrag=CreationDrag::Rotate;
            else if (creationTool==CreationTool::Pan) creationDrag=CreationDrag::Pan;
            else if (creationTool==CreationTool::Zoom) creationDrag=CreationDrag::Zoom;
            else if (creationTool==CreationTool::Move && hit>=0) {
                if (std::find(creationSelection.begin(),creationSelection.end(),hit)==creationSelection.end())
                    selectCreationAtom(hit,false);
                creationDrag=CreationDrag::Move;
                captureAtoms();
            } else if (creationTool==CreationTool::Move) {
                const int bond=creationBondHit(p,size,cam,mouse,true);
                if(bond>=0) {
                    if(std::find(creationBondSelection.begin(),creationBondSelection.end(),bond)==creationBondSelection.end())selectCreationBond(bond,false);
                    creationDrag=CreationDrag::Move;captureAtoms();
                } else creationDrag=CreationDrag::Rotate;
            }
            else if (creationTool==CreationTool::Sketch) {
                creationDrag=CreationDrag::Sketch;
                creationSketchStartHit=hit;
                creationSketchDoubleClick=ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            }
            else if (creationTool==CreationTool::Ring) {
                if (creationRingPreviewValid) {
                    creationRingStart=creationRingPreview; creationDrag=CreationDrag::Ring;
                }
            }
            else if (creationTool==CreationTool::Fragment) {
                if (io.KeyAlt || creationFusionPick) beginCreationFusion(hit);
                else if (creationFusionSeed) {
                    if (creationFusionPreview && creationFusionPreview->target==hit) {
                        creationFusionStart=creationFusionPreview; creationDrag=CreationDrag::Fusion;
                    } else status="请点击另一独立片段的末端原子 · Esc 取消";
                } else if (creationFragmentPreviewValid) {
                    creationFragmentStart=creationFragmentPreview; creationDrag=CreationDrag::Fragment;
                } else status="请点击空白处、孤立原子或末端原子放置片段";
            }
            else if (hit>=0) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) selectCreationFragment(hit);
                else selectCreationAtom(hit,io.KeyShift||io.KeyCtrl,io.KeyCtrl);
                creationDrag=CreationDrag::None;
            }
            else {
                const int bond=creationBondHit(p,size,cam,mouse,true);
                if(bond>=0) {
                    if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))selectCreationFragment(int(source.bonds[size_t(bond)].a));
                    else selectCreationBond(bond,creationDragShift,creationDragToggle);
                }
                else {if(!creationDragShift)selectCreationAtom(-1,false);creationDrag=CreationDrag::Box;}
            }
        }
        if (creationDrag!=CreationDrag::None &&
            (ImGui::IsMouseDown(creationDragButton) || ImGui::IsMouseReleased(creationDragButton))) {
            // Windows may deliver the last move and release in the same frame.
            // Apply that final position before committing/canceling the gesture.
            if (std::abs(mouse.x-creationDragStart.x)+std::abs(mouse.y-creationDragStart.y)>U(3))
                creationDragMoved=true;
            const float dx=mouse.x-creationDragPrevious.x,dy=mouse.y-creationDragPrevious.y;
            if(creationDrag==CreationDrag::Geometry && creationDragMoved) {
                const auto &plan=geometryDragPlans[io.KeyAlt?1:0];
                if(!plan) {
                    g_cursorOverride=LoadCursor(nullptr,IDC_NO);
                    if(geometryDragInverted!=io.KeyAlt) {
                        for(const auto &[index,original]:creationDragAtoms) {
                            auto &a=result.data.atoms[size_t(index)]; a.x=original.x; a.y=original.y; a.z=original.z;
                        }
                        geometryDragInverted=io.KeyAlt;
                        uploadCreationDisplay(result.data,result.selected,result.colorSelected);
                    }
                }
                else {
                    const double amount=(mouse.x-creationDragStart.x-(mouse.y-creationDragStart.y))/U(1);
                    double target=plan->initial+amount*(plan->monitor.count==2?.01:.4);
                    target=plan->monitor.count==2?std::max(.001,target):plan->monitor.count==3?std::clamp(target,0.,180.):std::remainder(target,360.);
                    if(target!=geometryDragTarget || io.KeyAlt!=geometryDragInverted) {
                        for(const auto &[index,original]:creationDragAtoms) {
                            auto &a=result.data.atoms[size_t(index)]; a.x=original.x; a.y=original.y; a.z=original.z;
                        }
                        for(const auto &[index,at]:geometry::positions(*plan,target)) {
                            auto &a=result.data.atoms[size_t(index)]; a.x=at.x; a.y=at.y; a.z=at.z;
                        }
                        geometryDragTarget=target; geometryDragInverted=io.KeyAlt;
                        uploadCreationDisplay(result.data,result.selected,result.colorSelected);
                    }
                }
            }
            if (creationDrag==CreationDrag::Ring) {
                const double angle=(double(mouse.x-creationDragStart.x)+double(mouse.y-creationDragStart.y))*.012;
                creationRingPreview=authoring::rotatedRing(creationRingStart,angle);
                creationRingPreviewValid=hovered;
            }
            if (creationDrag==CreationDrag::Fusion && creationFusionStart) {
                if (dx!=0 || dy!=0 || !creationFusionPreview) {
                    const double angle=(double(mouse.x-creationDragStart.x)+double(mouse.y-creationDragStart.y))*.012;
                    creationFusionPreview=fragments::rotated(*creationFusionStart,angle);
                }
            }
            if (creationDrag==CreationDrag::Fragment) {
                const double angle=(double(mouse.x-creationDragStart.x)+double(mouse.y-creationDragStart.y))*.012;
                creationFragmentPreview=fragments::rotated(creationFragmentStart,angle);
                creationFragmentPreviewValid=hovered;
            }
            if (creationDrag==CreationDrag::Sketch && creationDragMoved) {
                if (creationSketchStartHit>=0 && !io.KeyAlt && !creationSketchInsert) {
                    selectCreationAtom(creationSketchStartHit,false);
                    creationDrag=CreationDrag::Move; captureAtoms();
                } else if (creationSketchAnchor<0) creationDrag=CreationDrag::Rotate;
            }
            if (creationDrag==CreationDrag::Rotate && creationDragMoved) {
                if(ImGui::IsKeyDown(ImGuiKey_Z) || (creationDragEdgeRoll &&
                    !ImGui::IsKeyDown(ImGuiKey_X) && !ImGui::IsKeyDown(ImGuiKey_Y))) {
                    const ImVec2 center{p.x+size.x*.5f,p.y+size.y*.5f};
                    const double angle=creationDragEdgeRoll && !ImGui::IsKeyDown(ImGuiKey_Z)
                        ? std::remainder(std::atan2(mouse.y-center.y,mouse.x-center.x)-
                            std::atan2(creationDragPrevious.y-center.y,creationDragPrevious.x-center.x),double(DirectX::XM_2PI))
                        : dx*.008;
                    cam.roll=std::remainder(cam.roll-float(angle),DirectX::XM_2PI);
                } else {
                    if(!ImGui::IsKeyDown(ImGuiKey_X)) cam.yaw-=dx*.008f;
                    if(!ImGui::IsKeyDown(ImGuiKey_Y)) cam.pitch=std::clamp(cam.pitch+dy*.008f,-1.55f,1.55f);
                    if (cam.mode<6) cam.mode=6;
                }
            } else if (creationDrag==CreationDrag::Pan && creationDragMoved) {
                cam.panX+=dx/std::max(size.x,1.f)*cam.zoom;
                cam.panY-=dy/std::max(size.y,1.f)*cam.zoom;
            } else if (creationDrag==CreationDrag::Zoom && creationDragMoved) {
                cam.zoom=std::clamp(cam.zoom*std::exp(dy*.008f),.02f,100.f);
            } else if (creationDrag==CreationDrag::Move && creationDragMoved && !creationDragAtoms.empty()) {
                const auto matrix=creationDragMatrix;
                const auto &anchor=creationDragAtoms.front().second;
                DirectX::XMFLOAT4 q;
                DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(anchor.x,anchor.y,anchor.z,1),matrix));
                if (q.w>0) {
                    const auto a=creationWorldAt(creationDragStart,q.z/q.w,p,size,matrix);
                    ImVec2 constrained=mouse;
                    if (ImGui::IsKeyDown(ImGuiKey_X)) constrained.y=creationDragStart.y;
                    if (ImGui::IsKeyDown(ImGuiKey_Y)) constrained.x=creationDragStart.x;
                    auto b=creationWorldAt(constrained,q.z/q.w,p,size,matrix);
                    if (ImGui::IsKeyDown(ImGuiKey_Z)) {
                        const auto projected=creationProjection(source,cam,size);
                        DirectX::XMFLOAT3 axis;
                        DirectX::XMStoreFloat3(&axis,DirectX::XMVector3TransformNormal(
                            DirectX::XMVectorSet(0,0,1,0),DirectX::XMMatrixInverse(nullptr,projected.view)));
                        const auto extent=creationWorldAt({creationDragStart.x+U(100),creationDragStart.y},q.z/q.w,p,size,matrix);
                        const auto delta=authoring::scale({axis.x,axis.y,axis.z},
                            (mouse.y-creationDragStart.y)/U(100)*authoring::length(authoring::sub(extent,a)));
                        b=authoring::add(a,delta);
                    }
                    creationMoveDelta={b.x-a.x,b.y-a.y,b.z-a.z};
                    for (const auto &[index,original]:creationDragAtoms) {
                        auto &atom=result.data.atoms[size_t(index)];
                        atom.x=original.x+creationMoveDelta.x;
                        atom.y=original.y+creationMoveDelta.y;
                        atom.z=original.z+creationMoveDelta.z;
                    }
                    if (dx!=0 || dy!=0) uploadCreationDisplay(result.data,result.selected,result.colorSelected);
                }
            } else if (creationDrag==CreationDrag::Spin && creationDragMoved && !creationDragAtoms.empty()) {
                const auto projected=creationProjection(source,cam,size);
                const auto inverseView=DirectX::XMMatrixInverse(nullptr,projected.view);
                auto worldAxis=[&](float x,float y,float z) {
                    DirectX::XMFLOAT3 axis;
                    DirectX::XMStoreFloat3(&axis,DirectX::XMVector3TransformNormal(
                        DirectX::XMVectorSet(x,y,z,0),inverseView));
                    return Vec3{axis.x,axis.y,axis.z};
                };
                const double ax=(mouse.y-creationDragStart.y)*.008;
                const double ay=(mouse.x-creationDragStart.x)*.008;
                const ImVec2 center{p.x+size.x*.5f,p.y+size.y*.5f};
                creationDragRollAngle-=std::remainder(std::atan2(mouse.y-center.y,mouse.x-center.x)-
                    std::atan2(creationDragPrevious.y-center.y,creationDragPrevious.x-center.x),double(DirectX::XM_2PI));
                for (const auto &[index,original]:creationDragAtoms) {
                    Vec3 at=original;
                    if (ImGui::IsKeyDown(ImGuiKey_Z) || (creationDragEdgeRoll &&
                        !ImGui::IsKeyDown(ImGuiKey_X) && !ImGui::IsKeyDown(ImGuiKey_Y)))
                        at=authoring::rotatedPoint(at,creationDragCenter,worldAxis(0,0,1),
                            ImGui::IsKeyDown(ImGuiKey_Z)?ay:creationDragRollAngle);
                    else {
                        if (!ImGui::IsKeyDown(ImGuiKey_Y))
                            at=authoring::rotatedPoint(at,creationDragCenter,worldAxis(1,0,0),ax);
                        if (!ImGui::IsKeyDown(ImGuiKey_X))
                            at=authoring::rotatedPoint(at,creationDragCenter,worldAxis(0,1,0),ay);
                    }
                    auto &atom=result.data.atoms[size_t(index)];
                    atom.x=at.x; atom.y=at.y; atom.z=at.z;
                }
                if (dx!=0 || dy!=0) uploadCreationDisplay(result.data,result.selected,result.colorSelected);
            }
            creationDragPrevious=mouse;
        }
        Vec3 sketchAt{};
        bool sketchPositionValid=false;
        int sketchHit=-1;
        if (creationTool==CreationTool::Sketch && hovered &&
            (creationDrag==CreationDrag::None || creationDrag==CreationDrag::Sketch)) {
            const auto projection=creationProjection(result.data,cam,size);
            sketchHit=creationHover;
            const bool anchored=creationSketchAnchor>=0 && size_t(creationSketchAnchor)<source.atoms.size();
            Vec3 center=authoring::cartesian(source,.5,.5,.5);
            if (anchored) {
                const auto &a=source.atoms[size_t(creationSketchAnchor)]; center={a.x,a.y,a.z};
            }
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                DirectX::XMVectorSet(center.x,center.y,center.z,1),projection.combined));
            if (q.w>0) {
                sketchPositionValid=true;
                sketchAt=creationWorldAt(mouse,q.z/q.w,p,size,projection.combined);
                if (anchored) {
                    if (sketchHit>=0 && sketchHit!=creationSketchAnchor) {
                        const auto &a=source.atoms[size_t(sketchHit)]; sketchAt={a.x,a.y,a.z};
                    } else {
                        DirectX::XMFLOAT3 towardViewer;
                        DirectX::XMStoreFloat3(&towardViewer,DirectX::XMVector3TransformNormal(
                            DirectX::XMVectorSet(0,0,-1,0),DirectX::XMMatrixInverse(nullptr,projection.view)));
                        sketchAt=authoring::sketchPosition(center,sketchAt,
                            {towardViewer.x,towardViewer.y,towardViewer.z},
                            authoring::sketchBondLength(source,creationSketchAnchor,creationElement),
                            io.KeyAlt,io.KeyAlt && io.KeyShift);
                    }
                    creationSketchPreview=sketchAt;
                    creationSketchPreviewValid=true;
                }
            }
        }
        if (creationDrag!=CreationDrag::None &&
            ImGui::IsMouseReleased(creationDragButton)) {
            contextRequested=creationDragButton==ImGuiMouseButton_Right &&
                !creationDragMoved && !io.KeyAlt && !io.KeyShift;
            if(creationDrag==CreationDrag::Geometry && creationDragMoved) {
                const auto &plan=geometryDragPlans[geometryDragInverted?1:0];
                if(plan && std::abs(geometry::delta(*plan,geometryDragTarget))>1e-7)
                    editStructure(std::string("拖动修改")+geometryName(plan->monitor.count),[&](Dataset &data){geometry::apply(data,*plan,geometryDragTarget);});
                geometryPanelMonitor=-1;
            } else if (creationDrag==CreationDrag::Box && creationDragMoved) {
                const auto matrix=creationProjection(result.data,cam,size).combined;
                const float x0=std::min(creationDragStart.x,mouse.x),x1=std::max(creationDragStart.x,mouse.x);
                const float y0=std::min(creationDragStart.y,mouse.y),y1=std::max(creationDragStart.y,mouse.y);
                if (!creationDragShift) {creationSelection.clear();creationBondSelection.clear();}
                std::vector<uint8_t> already(result.data.atoms.size(),0);
                for (int selected:creationSelection)
                    if (selected>=0 && size_t(selected)<already.size()) already[size_t(selected)]=1;
                for (size_t index=0;index<result.data.atoms.size();++index) {
                    if (!creationAtomVisible(int(index))) continue;
                    const auto &atom=result.data.atoms[index];
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                        DirectX::XMVectorSet(atom.x,atom.y,atom.z,1),matrix));
                    if (q.w<=0||q.z<=0) continue;
                    const float x=p.x+(q.x/q.w+1)*size.x*.5f,y=p.y+(1-q.y/q.w)*size.y*.5f;
                    if (x>=x0&&x<=x1&&y>=y0&&y<=y1)
                        already[index]=creationDragToggle?!already[index]:1;
                }
                creationSelection.clear();
                for (size_t index=0;index<already.size();++index)
                    if (already[index]) creationSelection.push_back(int(index));
                creationPick=creationSelection.empty()?-1:creationSelection.back();
                // A bond is inside the box when both endpoints of a displayed
                // segment are inside it; periodic stubs use rendered geometry.
                auto inside=[&](Vec3 at) {
                    DirectX::XMFLOAT4 q;DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),matrix));
                    if(q.w<=0 || q.z<=0 || q.z>=q.w)return false;
                    const float x=p.x+(q.x/q.w+1)*size.x*.5f,y=p.y+(1-q.y/q.w)*size.y*.5f;
                    return x>=x0 && x<=x1 && y>=y0 && y<=y1;
                };
                std::set<int> selectedBonds(creationBondSelection.begin(),creationBondSelection.end());
                if(source.bonds.size()<=interactiveBondBudget)for(size_t row=0;row<source.bonds.size();++row) {
                    if(!creationBondVisible(int(row)))continue;
                    bool contained=false;
                    for(const auto &segment:creationBondHalves(int(row)))
                        contained|=inside(segment.p1) && inside(segment.p2);
                    if(!contained)continue;
                    if(creationDragToggle && selectedBonds.contains(int(row)))selectedBonds.erase(int(row));else selectedBonds.insert(int(row));
                }
                creationBondSelection.assign(selectedBonds.begin(),selectedBonds.end());
            } else if (creationDrag==CreationDrag::Move && creationDragMoved && !creationDragAtoms.empty()) {
                const auto edits=creationDragAtoms;
                const auto delta=creationMoveDelta;
                editStructure("移动 " + std::to_string(edits.size()) + " 个原子",[&](Dataset &data) {
                    for (const auto &[index,original]:edits) {
                        auto &atom=data.atoms[size_t(index)];
                        atom.x=original.x+delta.x; atom.y=original.y+delta.y; atom.z=original.z+delta.z;
                    }
                });
            } else if (creationDrag==CreationDrag::Spin && creationDragMoved && !creationDragAtoms.empty()) {
                std::vector<std::pair<int,Vec3>> positions;
                for (const auto &[index,original]:creationDragAtoms) {
                    (void)original;
                    const auto &a=result.data.atoms[size_t(index)];
                    positions.push_back({index,{a.x,a.y,a.z}});
                }
                editStructure("旋转 "+std::to_string(positions.size())+" 个原子",[&](Dataset &data) {
                    for (const auto &[index,at]:positions) {
                        auto &a=data.atoms[size_t(index)]; a.x=at.x; a.y=at.y; a.z=at.z;
                    }
                });
            } else if (creationDrag==CreationDrag::Ring && hovered && creationRingPreviewValid) {
                const auto ring=creationRingPreview;
                commitCreationRing(ring); creationRingPreviewValid=false;
            } else if (creationDrag==CreationDrag::Fusion) {
                if (hovered && creationFusionPreview) commitCreationFusion(*creationFusionPreview);
                else clearCreationFusion();
            } else if (creationDrag==CreationDrag::Fragment && hovered && creationFragmentPreviewValid) {
                const auto placement=creationFragmentPreview;
                commitCreationFragment(placement); creationFragmentPreviewValid=false;
            } else if (creationDrag==CreationDrag::Sketch && hovered && sketchPositionValid) {
                const bool inserting=creationSketchInsert || (io.KeyAlt && sketchHit<0);
                const int bond=sketchHit<0 && (inserting || creationSketchAnchor<0) && !creationDragMoved?
                    creationBondHit(p,size,cam,mouse):-1;
                if(inserting && bond>=0)insertCreationAtom(bond);
                else if(creationSketchInsert)status="请点击可见的直接键；Esc 退出键中插入";
                else if (bond>=0) {
                    const auto existing=source.bonds[size_t(bond)];
                    editCreationBond(int(existing.a),int(existing.b),existing.order==4?1:int(existing.order)%3+1);
                } else {
                    if (io.KeyAlt && !creationDragMoved && sketchHit<0) {
                        const auto projection=creationProjection(result.data,cam,size);
                        const auto center=authoring::cartesian(source,.5,.5,.5);
                        DirectX::XMFLOAT4 q;
                        DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                            DirectX::XMVectorSet(center.x,center.y,center.z,1),projection.combined));
                        if (q.w>0) sketchAt=creationWorldAt(mouse,q.z/q.w,p,size,projection.combined);
                    }
                    commitCreationSketch(sketchHit,sketchAt,io.KeyAlt && !creationDragMoved,
                        creationSketchDoubleClick,io.KeyAlt && sketchHit>=0);
                }
            }
            creationDrag=CreationDrag::None;
            creationDragAtoms.clear();
            creationLastHoverMouse={-1,-1};
        }
        if (hovered) {
            if (isGeometryTool(creationTool) || creationTool==CreationTool::Sketch || creationTool==CreationTool::Ring || creationTool==CreationTool::Fragment) {
                if(!g_cursorOverride) g_cursorOverride=LoadCursor(nullptr,IDC_CROSS);
            }
            else if(creationTool==CreationTool::Zoom)g_cursorOverride=g_magnifierCursor?g_magnifierCursor:LoadCursor(nullptr,IDC_SIZEALL);
            else if (creationHover>=0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            else if (creationTool==CreationTool::Pan||creationTool==CreationTool::Move)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            else if (creationTool==CreationTool::Rotate)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        return contextRequested;
    }
    void viewport(int i, float w, float h) {
        ImGui::PushID(i);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,creationMode ? ImVec2{0,0} : ImVec2{U(4),U(4)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,{0,0,0,1});
        ImGui::BeginChild("view", {w, h}, creationMode ? ImGuiChildFlags_None : ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto &cam = cameras[i];
        auto p = ImGui::GetCursorScreenPos();
        auto avail = ImGui::GetContentRegionAvail();
        const bool compactBondView = !creationMode && result.data.bondStyle.visible &&
            result.data.bondStyle.radius > 0 && gpu.bondsUploaded();
        gpu.target(targets[i], int(avail.x), int(avail.y));
            gpu.draw(targets[i], result.data, cam, radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell,
                creationMode?.43f:compactBondView?.50f:1.f,creationMode && sameAtomCount()?&creationDisplay:nullptr);
        ImGui::Image((ImTextureID)(intptr_t)targets[i].srv.Get(), avail);
        if (creationMode) creationViewportSize=avail;
        const bool viewportHovered = ImGui::IsItemHovered();
        if (creationMode) recordUiTestItem("creation.viewport","creation.viewport");
        const bool creationContextRequested=creationMode && creationPointer(p,avail,cam,viewportHovered);
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::GetIO().MouseWheel)
                active = i;
            const auto &io=ImGui::GetIO();
            const auto rightStart=io.MouseClickedPos[ImGuiMouseButton_Right];
            const bool viewContextRequested=!creationMode &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right]<U(3)*U(3) &&
                rightStart.x>=p.x && rightStart.x<=p.x+avail.x &&
                rightStart.y>=p.y && rightStart.y<=p.y+avail.y;
            if (creationContextRequested || viewContextRequested) {
                if (creationMode && creationHover>=0 &&
                    std::find(creationSelection.begin(),creationSelection.end(),creationHover)==creationSelection.end())
                    selectCreationAtom(creationHover,false);
                else if(creationMode && creationHover<0) {
                    const int bond=creationBondHit(p,avail,cam,io.MousePos,true);
                    if(bond>=0 && std::find(creationBondSelection.begin(),creationBondSelection.end(),bond)==creationBondSelection.end())
                        selectCreationBond(bond,false);
                }
                ImGui::OpenPopup("viewport-structure-menu");
            }
            if (!creationMode && ImGui::IsMouseDragging(0) && viewportTool == 2) {
                cam.yaw -= ImGui::GetIO().MouseDelta.x * .008f;
                cam.pitch =
                    std::clamp(cam.pitch + ImGui::GetIO().MouseDelta.y * .008f, -1.55f, 1.55f);
                if (cam.mode < 6)
                    cam.mode = 6;
            }
            if (!creationMode && ((ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) || (ImGui::IsMouseDragging(0) && viewportTool == 1))) {
                cam.panX += ImGui::GetIO().MouseDelta.x / std::max(avail.x, 1.f) * cam.zoom;
                cam.panY -= ImGui::GetIO().MouseDelta.y / std::max(avail.y, 1.f) * cam.zoom;
            }
            // Dragging up (negative delta) zooms in, matching the wheel
            // (wheel up -> zoom in) and OVITO's zoom tool.
            if (!creationMode && viewportTool == 0 && ImGui::GetIO().MouseWheel == 0 && ImGui::IsMouseDragging(0))
                cam.zoom = std::clamp(cam.zoom * powf(.985f, -ImGui::GetIO().MouseDelta.y), .01f, 50.f);
            cam.zoom = std::clamp(cam.zoom * powf(.85f, ImGui::GetIO().MouseWheel), .01f, 50.f);
        }
        auto *draw = ImGui::GetWindowDrawList();
        if (cell) {
            using namespace DirectX;
            auto m = creationMode ? creationProjection(result.data,cam,avail).combined
                : gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f), true, radius);
            ImVec2 corners[8];
            bool valid[8];
            bool lattice = source.cell[0] != 0 || source.cell[4] != 0 || source.cell[8] != 0;
            for (int j = 0; j < 8; j++) {
                float x, y, z;
                if (lattice) {
                    auto &c = result.data.cell;
                    x = float((j & 1 ? c[0] : 0) + (j & 2 ? c[3] : 0) + (j & 4 ? c[6] : 0));
                    y = float((j & 1 ? c[1] : 0) + (j & 2 ? c[4] : 0) + (j & 4 ? c[7] : 0));
                    z = float((j & 1 ? c[2] : 0) + (j & 2 ? c[5] : 0) + (j & 4 ? c[8] : 0));
                    x += result.data.origin.x;
                    y += result.data.origin.y;
                    z += result.data.origin.z;
                } else {
                    x = j & 1 ? result.data.hi.x : result.data.lo.x;
                    y = j & 2 ? result.data.hi.y : result.data.lo.y;
                    z = j & 4 ? result.data.hi.z : result.data.lo.z;
                }
                XMFLOAT4 q;
                XMStoreFloat4(&q, XMVector4Transform(XMVectorSet(x, y, z, 1), m));
                valid[j] = q.w > 0 && q.z > 0;
                corners[j] = {p.x + (q.x / q.w + 1) * avail.x * .5f,
                              p.y + (1 - q.y / q.w) * avail.y * .5f};
            }
            draw->PushClipRect(p, {p.x + avail.x, p.y + avail.y}, true);
            for (int j = 0; j < 8; j++)
                for (int k = 1; k <= 4; k *= 2)
                    if (!(j & k) && valid[j] && valid[j | k] &&
                        (cellDimension == 1 || (!(j & 4) && !((j | k) & 4)))) {
                        auto c = creationMode ? IM_COL32(90,95,102,55) :
                            ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]*cellGlow});
                        auto hi = creationMode ? IM_COL32(114,119,125,128) :
                            ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]});
                        if (!creationMode) draw->AddLine(corners[j], corners[j | k], c, cellWidth+4.f);
                        if (!cellDashed) draw->AddLine(corners[j], corners[j | k], hi, creationMode ? U(1) : cellWidth);
                        else { auto a=corners[j], b=corners[j|k]; for(int q=0;q<8;q+=2){float t0=q/8.f,t1=(q+1)/8.f;draw->AddLine({a.x+(b.x-a.x)*t0,a.y+(b.y-a.y)*t0},{a.x+(b.x-a.x)*t1,a.y+(b.y-a.y)*t1},hi,cellWidth);} }
                    }
            if (creationMode) {
                draw->AddText(corners[1], IM_COL32(232,75,70,255), "a");
                draw->AddText(corners[2], IM_COL32(42,207,100,255), "b");
                draw->AddText(corners[4], IM_COL32(32,170,237,255), "c");
            } else if (cellLabels) { draw->AddText(corners[0], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "O"); draw->AddText(corners[1], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "A"); draw->AddText(corners[2], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "B"); draw->AddText(corners[4], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "C"); }
            draw->PopClipRect();
        }
        if (creationMode) {
            using namespace DirectX;
            const ImVec2 o{p.x+U(44),p.y+avail.y-U(44)};
            const auto view=creationProjection(result.data,cam,avail).view;
            const ImU32 colors[]={IM_COL32(231,58,62,255),IM_COL32(37,209,89,255),IM_COL32(34,172,237,255)};
            const char *names[]={"x","y","z"};
            XMFLOAT3 axes[3];
            for(int axis=0;axis<3;++axis) XMStoreFloat3(&axes[axis],XMVector3TransformNormal(
                XMVectorSet(axis==0?1.f:0.f,axis==1?1.f:0.f,axis==2?1.f:0.f,0),view));
            std::array<int,3> order{0,1,2};
            std::sort(order.begin(),order.end(),[&](int a,int b){return axes[a].z<axes[b].z;});
            for(int axis:order) {
                const auto v=axes[axis];const float length=std::hypot(v.x,v.y);
                const ImVec2 end{o.x+v.x*U(26),o.y-v.y*U(26)};
                if(length<.1f) {
                    draw->AddCircle(o,U(3),colors[axis],12,U(1.5f));
                    draw->AddText({o.x+U(7),o.y+U(7)},colors[axis],names[axis]);
                } else {
                    draw->AddLine(o,end,colors[axis],U(2));
                    const auto text=ImGui::CalcTextSize(names[axis]);
                    draw->AddText({end.x+v.x/length*U(8)-text.x*.5f,
                                   end.y-v.y/length*U(8)-text.y*.5f},colors[axis],names[axis]);
                }
            }
        }
        const auto &creationColor=creationDisplay.defaultColor;
        const bool creationLegend=creationMode && creationColor.kind==creation::ColorKind::Property && creationColor.legend;
        const bool useCreationColors=creationMode && creationDisplay.hasColors();
        if ((creationLegend || (!useCreationColors && colorCoding && colorLegend)) && avail.x >= U(220) && avail.y >= U(110)) {
            const auto &legendProperty=creationLegend?creationColor.property:colorRangeProperty;
            const int legendGradient=creationLegend?creationColor.gradient:colorGradient;
            const bool legendReverse=creationLegend?creationColor.reverse:colorReverse;
            const bool legendDiscrete=creationLegend?creationColor.discrete:colorDiscrete;
            const double legendLow=creationLegend?creationColor.low:double(colorMin);
            const double legendHigh=creationLegend?creationColor.high:double(colorMax);
            const float sx = U(174), sy = U(64), pad = U(8);
            ImVec2 a{p.x + avail.x - sx - U(12), p.y + U(12)};
            ImVec2 b{a.x + sx, a.y + sy};
            draw->AddRectFilled(a,b,IM_COL32(15,19,25,238),U(4));
            draw->AddRect(a,b,IM_COL32(104,119,138,255),U(4));
            std::string title = legendProperty;
            if (title.size() > 24) title = title.substr(0,21) + "...";
            draw->AddText({a.x+pad,a.y+U(5)},IM_COL32(238,243,250,255),title.c_str());
            const float barX=a.x+pad, barY=a.y+U(25), barW=sx-pad*2, barH=U(12);
            const int segments=legendDiscrete?12:96;
            for (int segment=0; segment<segments; ++segment) {
                float u=segments>1?float(segment)/float(segments-1):0;
                if (legendReverse) u=1-u;
                if(creationLegend && legendLow==legendHigh) u=.5f;
                if (legendDiscrete) u=std::min(std::floor(u*12),11.f)/11.f;
                auto c=sampleColorGradient(legendGradient,u);
                auto packed=ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],1});
                const float x0=barX+barW*segment/segments;
                const float x1=barX+barW*(segment+1)/segments+.5f;
                draw->AddRectFilled({x0,barY},{x1,barY+barH},packed);
            }
            char low[48]{}, high[48]{};
            snprintf(low,sizeof(low),"%.5g",legendLow);
            snprintf(high,sizeof(high),"%.5g",legendHigh);
            draw->AddText({barX,a.y+U(42)},IM_COL32(218,226,237,255),low);
            auto highSize=ImGui::CalcTextSize(high);
            draw->AddText({barX+barW-highSize.x,a.y+U(42)},IM_COL32(218,226,237,255),high);
        }
        // P12 viewport chrome: name + projection tag top-left, fit /
        // maximize buttons top-right, pick-info chip bottom-right, amber
        // ring on the active viewport. The camera selector itself lives in
        // the View menu and the Render panel; the tool identity surfaces
        // through the OS cursor and the toolbar pill group.
        if (!creationMode) {
            draw->PushClipRect(p, {p.x + avail.x, p.y + avail.y}, true);
            static const char *viewNames[8] = {"TOP", "BOTTOM", "FRONT", "BACK",
                                               "LEFT", "RIGHT", "ORTHO", "PERSPECTIVE"};
            const int mode = std::clamp(cam.mode, 0, 7);
            const bool activeVp = i == active;
            const ImVec2 lp{p.x + U(10), p.y + U(8)};
            const ImVec2 nameSize = ImGui::CalcTextSize(viewNames[mode]);
            draw->AddText(lp, activeVp ? IM_COL32(240, 164, 49, 255) : IM_COL32(174, 180, 187, 235),
                          viewNames[mode]);
            draw->AddText({lp.x + nameSize.x + U(8), lp.y + U(1)}, IM_COL32(93, 100, 108, 255),
                          mode < 6 ? "ortho" : "fov 35°");
            const float btn = U(22);
            auto vpBtn = [&](const char *id, ImVec2 min, const char *tip) {
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(id);
                const bool pressed = ImGui::InvisibleButton(id, {btn, btn});
                ImGui::PopID();
                if (ImGui::IsItemHovered()) {
                    draw->AddRectFilled(min, {min.x + btn, min.y + btn}, IM_COL32(255, 255, 255, 20), U(3));
                    ImGui::SetTooltip("%s", tip);
                }
                return pressed;
            };
            const ImVec2 fitMin{p.x + avail.x - U(6) - btn * 2 - U(1), p.y + U(5)};
            const ImVec2 maxMin{fitMin.x + btn + U(1), fitMin.y};
            if (vpBtn("##vpfit", fitMin, "Zoom to extents (this viewport)"))
                fitCamera(i, false);
            recordUiTestItem("viewport.fit." + std::to_string(i));
            if (vpBtn("##vpmax", maxMin, "Maximize / restore viewport")) {
                active = i;
                quad = !quad;
            }
            recordUiTestItem("viewport.max." + std::to_string(i));
            // corner-bracket fit icon + maximize arrows
            {
                auto bracket = [&](ImVec2 bmin, float size) {
                    const float L = U(4);
                    draw->AddLine(bmin, {bmin.x + L, bmin.y}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine(bmin, {bmin.x, bmin.y + L}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x + size, bmin.y}, {bmin.x + size - L, bmin.y}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x + size, bmin.y}, {bmin.x + size, bmin.y + L}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x, bmin.y + size}, {bmin.x + L, bmin.y + size}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x, bmin.y + size}, {bmin.x, bmin.y + size - L}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x + size, bmin.y + size}, {bmin.x + size - L, bmin.y + size}, IM_COL32(150, 156, 164, 255));
                    draw->AddLine({bmin.x + size, bmin.y + size}, {bmin.x + size, bmin.y + size - L}, IM_COL32(150, 156, 164, 255));
                };
                bracket({fitMin.x + U(4), fitMin.y + U(4)}, btn - U(8));
                // maximize icon: two diagonal arrows
                const float inset = U(6);
                draw->AddLine({maxMin.x + inset, maxMin.y + btn - inset},
                              {maxMin.x + btn - inset, maxMin.y + inset}, IM_COL32(150, 156, 164, 255));
                draw->AddLine({maxMin.x + btn - inset, maxMin.y + inset},
                              {maxMin.x + btn - inset - U(4), maxMin.y + inset}, IM_COL32(150, 156, 164, 255));
                draw->AddLine({maxMin.x + btn - inset, maxMin.y + inset},
                              {maxMin.x + btn - inset, maxMin.y + inset + U(4)}, IM_COL32(150, 156, 164, 255));
                draw->AddLine({maxMin.x + inset, maxMin.y + btn - inset},
                              {maxMin.x + inset + U(4), maxMin.y + btn - inset}, IM_COL32(150, 156, 164, 255));
                draw->AddLine({maxMin.x + inset, maxMin.y + btn - inset},
                              {maxMin.x + inset, maxMin.y + btn - inset - U(4)}, IM_COL32(150, 156, 164, 255));
            }
            // Pick info chip (design: #id type x y z, bottom-right).
            if (activeVp && creationPick >= 0 &&
                size_t(creationPick) < result.data.atoms.size()) {
                const auto &atom = result.data.atoms[size_t(creationPick)];
                const std::string typeName = atom.type < result.data.species.size()
                    ? result.data.species[atom.type] : "?";
                char chip[160];
                snprintf(chip, sizeof(chip), "#%d %s  x %.3f  y %.3f  z %.3f", creationPick,
                         typeName.c_str(), atom.x, atom.y, atom.z);
                const ImVec2 ts = ImGui::CalcTextSize(chip);
                const ImVec2 cmin{p.x + avail.x - ts.x - U(18), p.y + avail.y - ts.y - U(15)};
                draw->AddRectFilled(cmin, {cmin.x + ts.x + U(18), cmin.y + ts.y + U(10)},
                                    IM_COL32(15, 17, 19, 224), U(3));
                draw->AddRect(cmin, {cmin.x + ts.x + U(18), cmin.y + ts.y + U(10)},
                              IM_COL32(44, 49, 55, 255), U(3));
                draw->AddText({cmin.x + U(9), cmin.y + U(5)}, IM_COL32(174, 180, 187, 255), chip);
            }
            draw->PopClipRect();
        }
        // 3D-tool cursor: while the pointer is over any viewport area, the
        // cursor reflects the active tool. Pan/orbit ride the ImGui cursor
        // (mapped to the stock IDC_HAND / IDC_SIZEALL by the win32 backend);
        // the zoom magnifier has no stock shape and goes through the Win32
        // WM_SETCURSOR override. The ui() frame start resets both, so leaving
        // the viewport (or no tool) restores the normal arrow.
        if (!creationMode && ImGui::IsWindowHovered()) {
            switch (cursorForViewportTool(viewportTool)) {
            case ViewportCursor::Hand: ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); break;
            case ViewportCursor::ResizeAll: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
            case ViewportCursor::Magnifier:
                g_cursorOverride = g_magnifierCursor ? g_magnifierCursor
                                                     : LoadCursor(nullptr, IDC_SIZEALL);
                break;
            case ViewportCursor::Arrow: break;
            }
        }
        if (creationMode) {
            const auto projection=creationProjection(result.data,cam,avail);
            auto mvp=projection.combined;
            draw->PushClipRect(p,{p.x+avail.x,p.y+avail.y},true);
            geometryOverlay(p,avail,cam,draw);
            if(!pipelineBusy && !staleResult) {
                const size_t count=creationBondSelection.size(),limit=std::min(count,size_t(2000));
                for(size_t j=0;j<limit;++j) {
                    const int row=creationBondSelection[j*(count/limit)+(j*(count%limit))/limit];
                    if(!creationBondVisible(row))continue;
                    for(const auto &segment:creationBondHalves(row)) {
                        ImVec2 points[2];bool visible=true;int k=0;
                        for(const auto at:{segment.p1,segment.p2}) {
                            DirectX::XMFLOAT4 q;DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                            visible&=q.w>0 && q.z>0 && q.z<q.w;
                            if(q.w>0)points[k]={p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};++k;
                        }
                        if(visible) {
                            draw->AddLine(points[0],points[1],IM_COL32(255,204,81,85),U(9));
                            draw->AddLine(points[0],points[1],IM_COL32(255,204,81,255),U(2));
                        }
                    }
                }
            }
            if (creationFusionSeed && !pipelineBusy) {
                auto screen=[&](Vec3 at,ImVec2 &point) {
                    DirectX::XMFLOAT4 q; DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                        DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                    if (q.w<=0 || q.z<=0 || q.z>=q.w) return false;
                    point={p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f}; return true;
                };
                ImVec2 marker;
                if (screen(fragments::at(source,creationFusionSeed->connector),marker)) {
                    draw->AddRect({marker.x-U(9),marker.y-U(9)},{marker.x+U(9),marker.y+U(9)},IM_COL32(255,80,80,255),0,0,U(2));
                    draw->AddText({marker.x+U(14),marker.y-U(12)},IM_COL32(255,110,110,255),"移动片段连接点");
                }
                if (creationFusionPreview) {
                    const auto &fusion=*creationFusionPreview; ImVec2 a,b;
                    for (const auto &edge:creationFusionSeed->previewEdges)
                        if (screen(fragments::fusionPoint(fusion,size_t(edge[0])),a) && screen(fragments::fusionPoint(fusion,size_t(edge[1])),b))
                            draw->AddLine(a,b,IM_COL32(255,218,103,210),U(2));
                    const size_t step=std::max(size_t(1),(creationFusionSeed->indices.size()+2047)/2048);
                    for (size_t local=0;local<creationFusionSeed->indices.size();local+=step)
                        if (creationFusionSeed->indices[local]!=creationFusionSeed->removed && screen(fragments::fusionPoint(fusion,local),a))
                            draw->AddCircle(a,U(5),IM_COL32(255,218,103,210),12,U(1.5f));
                    if (screen(fusion.anchorPosition,a) && screen(fusion.pivot,b)) {
                        draw->AddLine(a,b,IM_COL32(255,218,103,255),U(3));
                        draw->AddText({b.x+U(12),b.y-U(24)},IM_COL32(255,218,103,255),"松开连接 · 拖动转向");
                    }
                }
            }
            if (creationFragmentPreviewValid && !pipelineBusy) {
                if (const auto *t=currentFragment()) {
                    const auto &placement=creationFragmentPreview;
                    std::vector<ImVec2> points(placement.points.size()); std::vector<uint8_t> visible(points.size(),0);
                    for (size_t index=0;index<points.size();++index) {
                        const auto at=placement.points[index]; DirectX::XMFLOAT4 q;
                        DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                        visible[index]=q.w>0 && q.z>0 && q.z<q.w;
                        if (visible[index]) points[index]={p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};
                    }
                    for (const auto &b:t->data.bonds) {
                        if (placement.anchor>=0 && (int(b.a)==placement.connector || int(b.b)==placement.connector)) continue;
                        if (visible[b.a] && visible[b.b]) draw->AddLine(points[b.a],points[b.b],IM_COL32(255,218,103,210),U(2));
                    }
                    for (size_t index=0;index<points.size();++index) if (visible[index] && !(placement.anchor>=0 && int(index)==placement.connector))
                        draw->AddCircle(points[index],U(5),IM_COL32(255,218,103,210),16,U(1.5f));
                    if (placement.anchor>=0 && visible[size_t(placement.root)]) {
                        DirectX::XMFLOAT4 q; const auto at=placement.anchorPosition;
                        DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                        if (q.w>0 && q.z>0 && q.z<q.w) draw->AddLine({p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f},
                            points[size_t(placement.root)],IM_COL32(255,218,103,210),U(2));
                    }
                    if (visible[size_t(placement.root)]) draw->AddText({points[size_t(placement.root)].x+U(12),points[size_t(placement.root)].y-U(24)},
                        IM_COL32(255,218,103,230),t->name.c_str());
                }
            }
            if (creationRingPreviewValid && !pipelineBusy) {
                const auto &ring=creationRingPreview;
                ImVec2 projected[6]; bool visible[6]{};
                for (int vertex=0;vertex<ring.size;++vertex) {
                    const auto at=ring.points[size_t(vertex)];
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                    visible[vertex]=q.w>0 && q.z>0 && q.z<q.w;
                    if (visible[vertex]) projected[vertex]={p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};
                }
                for (int vertex=0;vertex<ring.size;++vertex) {
                    const int next=(vertex+1)%ring.size;
                    if (visible[vertex] && visible[next]) draw->AddLine(projected[vertex],projected[next],IM_COL32(255,218,103,210),U(2));
                    if (visible[vertex]) draw->AddCircle(projected[vertex],U(5),IM_COL32(255,218,103,210),16,U(1.5f));
                }
                if (visible[0]) {
                    const auto caption=std::to_string(ring.size)+(ring.aromatic?" 元芳香环":" 元碳环");
                    draw->AddText({projected[0].x+U(12),projected[0].y-U(24)},IM_COL32(255,218,103,230),caption.c_str());
                }
            }
            if(creationInsertHover>=0 && size_t(creationInsertHover)<source.bonds.size() && !pipelineBusy) {
                const auto &b=source.bonds[size_t(creationInsertHover)];
                const auto at=authoring::add(authoring::scale(hydrogens::point(source,int(b.a)),.5),
                                            authoring::scale(hydrogens::point(source,int(b.b)),.5));
                DirectX::XMFLOAT4 q;DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(at.x,at.y,at.z,1),mvp));
                if(q.w>0 && q.z>0 && q.z<q.w) {
                    const ImVec2 center{p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};
                    draw->AddCircle(center,U(9),IM_COL32(255,218,103,255),24,U(2));
                    draw->AddText({center.x+U(13),center.y-U(18)},IM_COL32(255,218,103,255),"插入原子");
                }
            }
            if (creationSketchPreviewValid && creationSketchAnchor>=0 && !pipelineBusy && !creationSketchInsert && creationInsertHover<0) {
                const auto &anchor=source.atoms[size_t(creationSketchAnchor)];
                auto project=[&](Vec3 world,ImVec2 &at) {
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                        DirectX::XMVectorSet(world.x,world.y,world.z,1),mvp));
                    if (q.w<=0 || q.z<=0 || q.z>=q.w) return false;
                    at={p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};
                    return true;
                };
                ImVec2 from,to;
                if (project({anchor.x,anchor.y,anchor.z},from) && project(creationSketchPreview,to)) {
                    const float dx=to.x-from.x,dy=to.y-from.y,norm=std::max(std::hypot(dx,dy),1.f);
                    for (int lane=0;lane<creationSketchOrder;++lane) {
                        const float offset=U(4)*(lane-(creationSketchOrder-1)*.5f);
                        draw->AddLine({from.x-dy/norm*offset,from.y+dx/norm*offset},
                            {to.x-dy/norm*offset,to.y+dx/norm*offset},IM_COL32(255,218,103,190),U(2));
                    }
                    draw->AddCircleFilled(to,U(9),IM_COL32(255,218,103,80),24);
                    draw->AddCircle(to,U(9),IM_COL32(255,218,103,230),24,U(2));
                }
            }
            const auto &bondLabels=creationDisplay.bondLabels;
            if(sameAtomCount() && result.data.bondStyle.visible && bondLabels.visible && !gpu.bondsOmittedForPerformance()) {
                ImFont *font=bondLabels.bold && headingFont?headingFont:ImGui::GetFont();
                const float fontSize=U(bondLabels.fontSize);
                const auto color=ImGui::ColorConvertFloat4ToU32({bondLabels.color[0],bondLabels.color[1],bondLabels.color[2],bondLabels.color[3]});
                size_t drawn=0;
                for(auto index:bondLabels.candidates(result.data.bonds.size())) {
                    if(drawn>=size_t(bondLabels.budget) || index>=result.data.bonds.size())break;
                    const auto &b=result.data.bonds[index];
                    if(creationDisplay.bondVisibility.isHidden(result.data,index) || !creationAtomVisible(int(b.a)) || !creationAtomVisible(int(b.b)))continue;
                    const bool periodic=b.image[0] || b.image[1] || b.image[2];
                    if(periodic && !result.data.bondStyle.showPeriodicImages)continue;
                    const auto text=creation::bondLabelText(result.data,index,bondLabels.at(result.data,index));if(text.empty())continue;
                    const auto &a=result.data.atoms[b.a],&z=result.data.atoms[b.b];
                    // Use the same segments as Renderer::upload, including its periodic stubs.
                    const auto segments=bondSegments(result.data.cell,{a.x,a.y,a.z},{z.x,z.y,z.z},b.image);
                    for(size_t j=0;j<segments.size() && drawn<size_t(bondLabels.budget);++j) {
                        const auto presetA=creationDisplay.presetAt(b.a),presetB=creationDisplay.presetAt(b.b);
                        if(periodic?(creationDisplay.presetAt(j==0?b.a:b.b)==4):(presetA==4 && presetB==4))continue;
                        const auto &s=segments[j];if(bondSegmentLength(s)<=1e-6)continue;float t=.5f;
                        if(!periodic) {if(presetA==4)t=.75f;else if(presetB==4)t=.25f;}
                        const Vec3 middle{s.p1.x+(s.p2.x-s.p1.x)*t,s.p1.y+(s.p2.y-s.p1.y)*t,s.p1.z+(s.p2.z-s.p1.z)*t};
                        DirectX::XMFLOAT4 q;DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(DirectX::XMVectorSet(middle.x,middle.y,middle.z,1),mvp));
                        if(q.w<=0 || q.z<=0 || q.z>=q.w || std::abs(q.x)>q.w || std::abs(q.y)>q.w)continue;
                        const ImVec2 at{p.x+(q.x/q.w+1)*avail.x*.5f+U(5),p.y+(1-q.y/q.w)*avail.y*.5f-fontSize-U(4)};
                        draw->AddText(font,fontSize,{at.x+U(1),at.y+U(1)},IM_COL32(0,0,0,230),text.c_str());
                        draw->AddText(font,fontSize,at,color,text.c_str());++drawn;
                    }
                }
            }
            if (sameAtomCount() && creationDisplay.labelsVisible &&
                (creationDisplay.defaultLabel.kind!=creation::LabelKind::None || !creationDisplay.explicitLabels.empty())) {
                const auto candidates=creationDisplay.candidates(source.atoms.size());
                std::unordered_set<int> drawn;
                const size_t budget=size_t(std::max(0,creationDisplay.labelBudget));
                ImFont *font=creationDisplay.bold && headingFont?headingFont:ImGui::GetFont();
                const float fontSize=U(creationDisplay.fontSize);
                const ImU32 color=ImGui::ColorConvertFloat4ToU32({creationDisplay.color[0],creationDisplay.color[1],
                    creationDisplay.color[2],creationDisplay.color[3]});
                auto label=[&](int index) {
                    if (drawn.size()>=budget || !creationAtomVisible(index) || drawn.contains(index)) return;
                    const auto text=creation::labelText(result.data,index,creationDisplay.labelAt(index));
                    if (text.empty()) return;
                    const auto &atom=result.data.atoms[size_t(index)];
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                        DirectX::XMVectorSet(atom.x,atom.y,atom.z,1),mvp));
                    if (q.w<=0 || q.z<=0 || q.z>=q.w || std::abs(q.x)>q.w || std::abs(q.y)>q.w) return;
                    const float offset=creationScreenRadius(atom,projection,avail,index)+U(3);
                    const ImVec2 at{p.x+(q.x/q.w+1)*avail.x*.5f+offset,p.y+(1-q.y/q.w)*avail.y*.5f-fontSize*.5f};
                    draw->AddText(font,fontSize,{at.x+U(1),at.y+U(1)},IM_COL32(0,0,0,230),text.c_str());
                    draw->AddText(font,fontSize,at,color,text.c_str());
                    drawn.insert(index);
                };
                for (size_t item=0;item<std::min(creationSelection.size(),budget);++item) label(creationSelection[item]);
                for (int index:candidates) label(index);
            }
            auto ring=[&](int index,ImU32 color,float width) {
                if (!creationAtomVisible(index)||size_t(index)>=result.data.atoms.size()) return;
                const auto &atom=result.data.atoms[size_t(index)];
                DirectX::XMFLOAT4 q;
                DirectX::XMStoreFloat4(&q,DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(atom.x,atom.y,atom.z,1),mvp));
                if (q.w<=0||q.z<=0) return;
                const ImVec2 at{p.x+(q.x/q.w+1)*avail.x*.5f,p.y+(1-q.y/q.w)*avail.y*.5f};
                const float screenRadius=creationScreenRadius(atom,projection,avail,index);
                draw->AddCircle(at,screenRadius,color,48,width);
            };
            for (int index:creationSelection) ring(index,IM_COL32(29,155,240,255),U(2));
            if (creationHover>=0 && std::find(creationSelection.begin(),creationSelection.end(),creationHover)==creationSelection.end())
                ring(creationHover,IM_COL32(221,228,236,180),U(1));
            if (creationDrag==CreationDrag::Box && creationDragMoved) {
                const auto mouse=ImGui::GetIO().MousePos;
                const ImVec2 lo{std::min(creationDragStart.x,mouse.x),std::min(creationDragStart.y,mouse.y)};
                const ImVec2 hi{std::max(creationDragStart.x,mouse.x),std::max(creationDragStart.y,mouse.y)};
                draw->AddRectFilled(lo,hi,IM_COL32(29,155,240,35));
                draw->AddRect(lo,hi,IM_COL32(29,155,240,255));
            }
            draw->PopClipRect();
        }
        if (ImGui::BeginPopup("viewport-structure-menu")) {
            if (!creationMode) {
                ImGui::TextDisabled("%s · %zu 原子",tabTitle().c_str(),result.data.atoms.size());
                ImGui::Separator();
                if (ImGui::MenuItem("✎ 在创作模式中打开")) openCreationTab();
                ImGui::TextDisabled("新标签页 · 增删改原子、建晶体、超胞");
                ImGui::Separator();
                if (ImGui::MenuItem("复位视角")) fitCamera(i,false);
                if (ImGui::MenuItem("导出结构...")) showDataExport=true;
            } else {
                ImGui::TextDisabled("%zu 原子 · %zu 键已选中",creationSelection.size(),creationBondSelection.size());
                ImGui::Separator();
                if(ImGui::BeginMenu("修改选中键",!creationBondSelection.empty())) {
                    int order=1;
                    for(const char *caption:{"单键","双键","三键","芳香键"}) {
                        if(ImGui::MenuItem(caption))editSelectedCreationBonds(order);
                        ++order;
                    }
                    if(ImGui::MenuItem("断开选中键"))editSelectedCreationBonds(0);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("删除选中原子 / 键",nullptr,false,!creationSelection.empty() || !creationBondSelection.empty())) deletePickedAtom();
                if (ImGui::MenuItem("选中同元素",nullptr,false,creationPick>=0)) {
                    const uint32_t type=source.atoms[size_t(creationPick)].type;
                    creationSelection.clear();creationBondSelection.clear();
                    for (size_t index=0;index<source.atoms.size();++index)
                        if (source.atoms[index].type==type && creationAtomVisible(int(index))) creationSelection.push_back(int(index));
                    creationPick=creationSelection.empty()?-1:creationSelection.back();
                }
                if (ImGui::MenuItem("选中连接片段",nullptr,false,!creationSelection.empty() || !creationBondSelection.empty()))
                    selectCreationFragments();
                recordUiTestItem("creation.context-fragments");
                if (ImGui::MenuItem("编辑坐标...",nullptr,false,creationPick>=0)) requestCreationPosition();
                if(ImGui::MenuItem("原子数值属性...",nullptr,false,!creationSelection.empty()))requestAtomProperties();
                if(ImGui::MenuItem("原子模拟约束...",nullptr,false,!creationSelection.empty()))requestAtomConstraints();
                if(ImGui::MenuItem("测量模拟约束...",nullptr,false,!creationDisplay.monitors.empty()))requestMeasurementConstraints();
                if(ImGui::MenuItem("原子化学设置...",nullptr,false,!creationSelection.empty()))requestCreationChemistry();
                if (ImGui::MenuItem("精准移动 / 旋转...",nullptr,false,!creationSelection.empty() || !creationBondSelection.empty()))
                    requestCreationMovement();
                recordUiTestItem("creation.context-movement");
                const bool hasSelection=!creationSelection.empty() || !creationBondSelection.empty();
                if (ImGui::MenuItem("隐藏选中",nullptr,false,hasSelection)) creationVisibility(0);
                if (ImGui::MenuItem("仅显示选中及键端点",nullptr,false,hasSelection)) creationVisibility(1);
                if (ImGui::MenuItem("显示全部",nullptr,false,creationDisplay.hiddenCount>0 || creationDisplay.bondVisibility.hiddenCount>0)) creationVisibility(2);
                if (ImGui::MenuItem("显示样式...")) requestCreationStyles();
                if (ImGui::MenuItem("原子标签...")) requestCreationLabels();
                if (ImGui::MenuItem("键标签...")) requestCreationBondLabels();
                if (ImGui::MenuItem("运动分组...")) requestMotionGroups();
                if (ImGui::MenuItem("反选")) invertCreationSelection();
                recordUiTestItem("creation.context-invert");
                ImGui::Separator();
                ImGui::TextDisabled("右键拖动旋转 · Alt+右键平移");
                ImGui::TextDisabled("Shift+Alt+右键移动选中原子");
                ImGui::TextDisabled("Shift+右键旋转选中原子 · X/Y/Z 约束");
                ImGui::Separator();
                if (ImGui::MenuItem("在此添加原子")) {
                    chooseCreationTool(CreationTool::Sketch);
                    status="在空白处点击以添加原子";
                }
                if (ImGui::MenuItem("复位视角")) fitCamera(i,false);
                if (ImGui::MenuItem("复制为新创作标签")) openCreationTab();
            }
            ImGui::EndPopup();
        }
        if (i == active && !creationMode) {
            draw->AddRect(p, {p.x + avail.x, p.y + avail.y}, IM_COL32(240, 164, 49, 191));
            // Fading status overlay at the viewport's bottom-left (replaces the
            // removed status bar); hides after ~4 s or on the next message.
            if (!overlayStatus.empty()) {
                const double age = ImGui::GetTime() - overlayStatusAt;
                if (age >= 0 && age < 4.0) {
                    const float fade = float(std::min(4.0 - age, 1.0));
                    char line[256];
                    snprintf(line, sizeof(line), "%s", overlayStatus.c_str());
                    const ImVec2 size = ImGui::CalcTextSize(line);
                    const ImVec2 min{p.x + U(12), p.y + avail.y - U(60)};
                    const ImVec2 max{std::min(p.x + avail.x - U(12), min.x + size.x + U(18)),
                                     min.y + size.y + U(10)};
                    draw->AddRectFilled(min, max, IM_COL32(12, 16, 22, int(212 * fade)), U(5));
                    draw->AddRect(min, max, IM_COL32(118, 134, 156, int(120 * fade)), U(5));
                    draw->PushClipRect(p, {p.x + avail.x, p.y + avail.y}, true);
                    draw->AddText({min.x + U(9), min.y + U(5)},
                                  IM_COL32(226, 233, 241, int(235 * fade)), line);
                    draw->PopClipRect();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(); ImGui::PopStyleVar();
        ImGui::PopID();
    }
    void seekFrame(int frame) {
        if (frames.empty()) return;
        playing = false;
        pendingFrame = std::clamp(frame, 0, int(frames.size())-1);
    }
    bool transport(const char *name, unsigned glyph, int kind, const char *tip,
                   bool highlight = false) {
        // Compact transport icon button; keeps the legacy vector-drawn arrows
        // when the Segoe MDL2 icon font is unavailable so the cluster never
        // degrades to blank or text-only buttons. highlight renders the amber
        // active state (design: the amber Play button while playing).
        char label[8];
        const bool useGlyph = glyphAvailable(glyph);
        if (useGlyph)
            glyphUtf8(glyph, label);
        if (highlight) {
            ImGui::PushStyleColor(ImGuiCol_Button, {0.941f, 0.643f, 0.192f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.957f, 0.694f, 0.263f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.957f, 0.694f, 0.263f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {0.086f, 0.094f, 0.106f, 1.f});
        }
        ImGui::PushID(name);
        const bool pressed = useGlyph ? ImGui::Button(label, {U(26), U(26)})
                                      : ImGui::Button("##transport", {U(26), U(26)});
        const auto p = ImGui::GetItemRectMin();
        ImGui::PopID();
        if (highlight) ImGui::PopStyleColor(4);
        if (!useGlyph) {
            auto *d = ImGui::GetWindowDrawList();
            auto color = highlight ? IM_COL32(22, 24, 27, 255) : ImGui::GetColorU32(ImGuiCol_Text);
            ImVec2 c{p.x+U(13),p.y+U(13)};
            if (kind == 2 && playing) {
                d->AddRectFilled({c.x-U(4),c.y-U(5)},{c.x-U(1),c.y+U(5)},color);
                d->AddRectFilled({c.x+U(1),c.y-U(5)},{c.x+U(4),c.y+U(5)},color);
            } else {
                float direction = kind < 2 ? -1.f : 1.f;
                d->AddTriangleFilled({c.x+direction*U(4),c.y},{c.x-direction*U(3),c.y-U(5)},
                                     {c.x-direction*U(3),c.y+U(5)},color);
                if (kind == 0 || kind == 4)
                    d->AddLine({c.x+direction*U(7),c.y-U(6)}, {c.x+direction*U(7),c.y+U(6)},color,U(2));
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tip);
        return pressed;
    }
    void timeline(float height) {
        ImGui::BeginChild("Trajectory timeline", {-1,height}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        // Compact OVITO-style spacing for the whole timeline area.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(4), U(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(3), U(4)});
        int last = std::max(0,int(frames.size())-1);
        int selected = pendingFrame >= 0 ? pendingFrame : current;
        auto &style = ImGui::GetStyle();
        const float gap = style.ItemSpacing.x;
        const float square = U(26), separatorWidth = gap + U(5) + gap;
        // The frame spinner group contains the field and both steppers; its
        // total width equals the item width requested below.
        const float frameField = U(96);
        const std::string totalLabel = " / " + std::to_string(std::max<size_t>(frames.size(), 1));
        const float totalWidth = ImGui::CalcTextSize(totalLabel.c_str()).x + gap;
        // Exact width of the right-aligned transport/tool cluster.
        const float cluster =
            5*square + 4*gap + separatorWidth + frameField + totalWidth + square + gap +
            separatorWidth + 4*square + separatorWidth + 2*square + gap;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(accent, "TRAJECTORY");
        const float labelEnd = ImGui::GetItemRectMax().x;
        const float windowRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x;
        const float clusterStart = std::max(labelEnd + U(10), windowRight - cluster);
        // P11: the scrub hint was redundant with the obvious ruler; keep only
        // the single-frame note when there is nothing to scrub.
        const char *hintText = frames.size() > 1 ? nullptr : "Single frame";
        const float hintWidth = hintText ? ImGui::CalcTextSize(hintText).x + gap : 0;
        if (hintText && labelEnd + U(6) + hintWidth <= clusterStart) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", hintText);
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), clusterStart - ImGui::GetWindowPos().x));
        ImGui::BeginDisabled(frames.size()<2 || indexing && busy);
        if (transport("##first",0xE892,0,"First frame (Home)")) seekFrame(0); ImGui::SameLine();
        if (transport("##previous",0xE76B,1,"Previous frame (Left)")) seekFrame(selected-1); ImGui::SameLine();
        if (transport("##play",playing ? 0xE769 : 0xE768,2,"Play / pause",playing)) playing = !playing; ImGui::SameLine();
        if (transport("##next",0xE76C,3,"Next frame (Right)")) seekFrame(selected+1); ImGui::SameLine();
        if (transport("##last",0xE893,4,"Last frame (End)")) seekFrame(last);
        ImGui::SameLine();
        // P12 loop toggle: vector-drawn cycle arrows, amber while active
        // (design: Loop transport control).
        {
            ImGui::PushID("timeline.loop");
            if (loopPlayback) {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.941f, 0.643f, 0.192f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.957f, 0.694f, 0.263f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.957f, 0.694f, 0.263f, 1.f});
            }
            const bool pressed = ImGui::Button("##loop", {square, square});
            if (loopPlayback) ImGui::PopStyleColor(3);
            const auto pr = ImGui::GetItemRectMin();
            auto *dl = ImGui::GetWindowDrawList();
            const ImVec2 c{pr.x + square * .5f, pr.y + square * .5f};
            const float r = square * .30f;
            const ImU32 ink = loopPlayback ? IM_COL32(22, 24, 27, 255)
                                           : ImGui::GetColorU32(ImGuiCol_Text);
            dl->PathClear();
            dl->PathArcTo(c, r, 0.7f, 5.6f, 20);
            dl->PathStroke(ink, 0, std::max(1.4f, U(1.4f)));
            for (float a : {0.7f, 5.6f}) {
                const ImVec2 t{c.x + std::cos(a) * r, c.y + std::sin(a) * r};
                const ImVec2 dir{std::cos(a), std::sin(a)};
                const float s = square * .11f;
                dl->AddTriangleFilled(t,
                                      {t.x - dir.x * s - dir.y * s, t.y - dir.y * s + dir.x * s},
                                      {t.x - dir.x * s + dir.y * s, t.y - dir.y * s - dir.x * s},
                                      ink);
            }
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loop playback");
            recordUiTestItem("timeline.loop");
            if (pressed) loopPlayback = !loopPlayback;
        }
        toolbarSeparator(ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(U(96));
        int edit = selected + 1;
        if (ImGui::InputInt("##frame number", &edit, 1, 100)) seekFrame(std::max(0,edit-1));
        ImGui::SameLine();
        ImGui::Text("%s", totalLabel.c_str());
        if (frames.empty()) ImGui::SetItemTooltip("No trajectory frames are loaded");
        ImGui::SameLine();
        if (iconButton("timeline.clock", 0xE823, "Clock", "Animation settings", false, square))
            animationSettings = true;
        ImGui::EndDisabled();
        toolbarSeparator(ImGui::GetFrameHeight());
        ImGui::SameLine();
        // Zoom / pan / orbit / field-of-view as one segmented control. The
        // buttons touch (no ItemSpacing) so the tray width stays 4*square,
        // which is what the cluster measurement above accounts for.
        const char *navNames[4] = {"timeline.zoom", "timeline.pan", "timeline.orbit", "timeline.fov"};
        const char *navTips[4] = {
            "Zoom active viewport (drag or wheel)",
            "Pan active viewport",
            "Orbit active viewport",
            "Adjust perspective field of view"};
        NavHit navHits[4];
        for (int tool = 0; tool < 4; ++tool) {
            if (tool) ImGui::SameLine(0, 0);
            navHits[tool] = navToolHit(navNames[tool], navTips[tool], square);
            if (navHits[tool].pressed) viewportTool = tool;
        }
        paintViewportNav(navHits);
        toolbarSeparator(ImGui::GetFrameHeight());
        ImGui::SameLine();
        if (iconButton("timeline.views", quad ? 0xE8A7 : 0xE8A9, quad ? "Max" : "Views",
                       quad ? "Maximize active viewport" : "Show four viewports", false, square))
            quad = !quad;
        ImGui::SameLine();
        if (iconButton("timeline.key", 0xE192, "Key", "Toggle auto-key mode", autoKey, square))
            autoKey = !autoKey;
        ImGui::PopStyleVar(2);
        if (height < U(90)) {
            ImGui::EndChild();
            return;
        }
        auto p = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x, rulerHeight = U(47);
        ImGui::InvisibleButton("##frame ruler", {width,rulerHeight}, ImGuiButtonFlags_EnableNav);
        bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        recordUiTestItem("timeline.ruler");
        auto *d = ImGui::GetWindowDrawList();
        // P12 redesign ruler: 8px track near the top with the played portion
        // amber-tinted, minor/major ticks below it (played ticks warm),
        // labels above, and a 1px amber playhead with a diamond knob. All
        // colors derive from the redesign palette; the 1/2/5-decade label
        // step and the click/drag seek behavior are unchanged.
        const float left = p.x+U(2), right = p.x+width-U(2);
        const float trackTop = p.y+U(22), trackBottom = trackTop+U(8);
        const float trackWidth = std::max(1.f, right-left);
        const int64_t major = timelineLabelStep(last, trackWidth);
        int64_t minor = major >= 5 ? major/5 : 1;
        while (last > 0 && minor < last && trackWidth*float(minor)/float(last) < U(3))
            minor *= 2;
        auto xFor = [&](int frame) { return left+trackWidth*(last ? float(frame)/last : 0.f); };
        d->AddRectFilled({left,trackTop},{right,trackBottom},ImGui::GetColorU32(ImGuiCol_FrameBg),U(1));
        d->AddRect({left,trackTop},{right,trackBottom},ImGui::GetColorU32(ImGuiCol_Border),U(1));
        const float knobX = xFor(selected);
        if (knobX > left+1.f)
            d->AddRectFilled({left,trackTop},{knobX,trackBottom},IM_COL32(240,164,49,56),U(1),
                             ImDrawFlags_RoundCornersLeft);
        for (int64_t frame=0;frame<=last;frame+=minor) {
            const float x=xFor(int(frame));
            const bool isMajor = frame%major==0;
            const ImU32 tickColor = frame <= selected ? IM_COL32(138,106,58,255)
                                                      : IM_COL32(61,67,74,255);
            d->AddLine({x,trackBottom},{x,trackBottom+U(isMajor?8.f:4.f)},tickColor,U(1));
            if (isMajor) {
                const auto label=std::to_string(frame); // 0-based, matching the spinner's "/ total".
                const auto size=ImGui::CalcTextSize(label.c_str());
                d->AddText({std::clamp(x-size.x*.5f,p.x+2.f,p.x+width-size.x-2.f),
                            p.y+U(4)},IM_COL32(107,114,122,255),label.c_str());
            }
        }
        d->AddLine({knobX,p.y+U(12)},{knobX,p.y+rulerHeight-U(2)},IM_COL32(240,164,49,255),U(1));
        {
            const float cy = p.y + U(14.5f), r = U(4.5f);
            d->AddQuadFilled({knobX, cy - r}, {knobX + r, cy}, {knobX, cy + r}, {knobX - r, cy},
                             IM_COL32(240, 164, 49, 255));
        }
        const bool scrubbing = ImGui::IsItemActive() && ImGui::IsMouseDown(0);
        if (scrubbing && last>0)
            seekFrame(int(std::lround(std::clamp((ImGui::GetIO().MousePos.x-left)/trackWidth,0.f,1.f)*last)));
        // Floating frame hint follows the mouse only while hovering the
        // ruler or scrubbing; it is hidden the rest of the time.
        if ((hovered || scrubbing) && last>0) {
            const int frame=int(std::lround(std::clamp((ImGui::GetIO().MousePos.x-left)/trackWidth,0.f,1.f)*last));
            const std::string name = path.filename().string();
            ImGui::SetTooltip("%s (Frame %d)",
                              name.empty() ? "Trajectory" : name.c_str(), frame);
        }
        if (focused && last>0) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seekFrame(selected-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seekFrame(selected+1);
            if (ImGui::IsKeyPressed(ImGuiKey_Home)) seekFrame(0);
            if (ImGui::IsKeyPressed(ImGuiKey_End)) seekFrame(last);
        }
        ImGui::EndChild();
    }
    void documentTabBar() {
        if (tabs.empty()) tabs.push_back(captureTab());
        if (activeTab >= 0 && activeTab < int(tabs.size())) tabs[activeTab].title = tabTitle();
        ImGui::BeginChild("##document-tabs", {-1, U(30)}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        int closing = -1;
        for (int i = 0; i < int(tabs.size()); ++i) {
            ImGui::PushID(i);
            std::string label = tabs[i].title.empty() ? "Untitled structure" : tabs[i].title;
            if (i == activeTab && creationMode) label += "  [Creation]";
            if (ImGui::Selectable(label.c_str(), i == activeTab, 0, {U(168), U(22)})) switchTab(i);
            recordUiTestItem("document.tab." + std::to_string(i), label.c_str());
            ImGui::SameLine(0, U(2));
            if (ImGui::SmallButton("x")) closing = i;
            recordUiTestItem("document.tab.close." + std::to_string(i));
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("New Tab"))
            newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
        recordUiTestItem("document.tab.new", "New Tab");
        ImGui::EndChild();
        if (closing >= 0) closeTab(closing);
    }
    void modelingToolbar() {
        ImGui::BeginChild("##modeling-toolbar", {-1, U(72)}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        auto action = [&](const char *name, const char *label, const char *tip, auto &&fn) {
            const float width = ImGui::CalcTextSize(label).x + U(28);
            if (ImGui::GetCursorPosX() > U(12) && ImGui::GetContentRegionAvail().x < width)
                ImGui::NewLine();
            if (ImGui::Button(label)) fn();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            recordUiTestItem(name, label);
            ImGui::SameLine();
        };
        action("model.creation-mode", creationMode ? "Exit Creation Mode" : "Enter Creation Mode",
               "Turn atom editing on or off for this structure", [&] { creationMode = !creationMode; });
        action("model.add-atom", "Add Atom", "Add the typed element at the center of the cell", [&] {
            Vec3 p = authoring::cartesian(source, 0.5, 0.5, 0.5);
            addAtomAt(p);
        });
        action("model.delete-atom", "Delete Atom", "Delete the picked atom", [&] { deletePickedAtom(); });
        ImGui::SetNextItemWidth(U(48));
        ImGui::InputText("##creation-element", creationElement, sizeof(creationElement));
        recordUiTestItem("model.element", "Element");
        ImGui::SameLine();
        action("model.replace-element", "Replace Element",
               "Change the picked atom to the element symbol in the box", [&] { replacePickedElement(); });
        action("model.add-hydrogens", "Add Hydrogens",
               "Preview hydrogen count and geometry from explicit bond orders", [&] { addHydrogensCommand(); });
        action("model.clean", "Clean Geometry",
               "Nudge close pairs toward the sum of their covalent radii", [&] { cleanGeometryCommand(); });
        action("model.ball-and-stick", "Ball and Stick", "Normal sphere radius", [&] { setDisplayStyle(0); });
        action("model.space-filling", "Space Filling", "Large van-der-Waals-like spheres", [&] { setDisplayStyle(1); });
        action("model.stick", "Stick", "Thin bonds-style spheres", [&] { setDisplayStyle(2); });
        action("model.line", "Line", "Very small spheres", [&] { setDisplayStyle(3); });
        action("model.fit", "Fit Structure", "Frame the structure in every viewport", [&] {
            for (int i = 0; i < 4; ++i) fitCamera(i, false);
        });
        action("model.reset-view", "Reset View", "Restore the default cameras and fit the structure",
               [&] { resetView(); });
        action("model.measure-distance", "Measure Distance",
               "Report the distance between the clicked atom and the Shift-clicked atom", [&] {
                   if(creationMode) { chooseCreationTool(CreationTool::Distance); return; }
                   if (creationPick >= 0 && creationMeasure >= 0)
                       status = "Distance " + std::to_string(authoring::distance(source, creationPick, creationMeasure)) + " angstrom";
                   else status = "Click the first atom, then Shift-click the second atom";
               });
        action("model.measure-angle", "Measure Bond Angle",
               "Report the angle at the Shift-clicked atom", [&] {
                   if(creationMode) { chooseCreationTool(CreationTool::Angle); return; }
                   if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0)
                       status = "Bond angle " + std::to_string(authoring::bondAngle(source, creationPick, creationMeasure, creationAngle)) + " degrees";
                   else status = "Click, Shift-click the vertex, then Ctrl-click the third atom";
               });
        action("model.measure-dihedral", "Measure Dihedral Angle",
               "Report the dihedral of four picked atoms", [&] {
                   if(creationMode) { chooseCreationTool(CreationTool::Torsion); return; }
                   if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0 && creationDihedral >= 0)
                       status = "Dihedral " + std::to_string(authoring::dihedralAngle(source, creationPick, creationMeasure, creationAngle, creationDihedral)) + " degrees";
                   else status = "Click, Shift-click, Ctrl-click, then Alt-click the fourth atom";
               });
        action("model.vacancy", "Create Vacancy", "Remove the picked atom and leave a vacancy", [&] { deletePickedAtom(); });
        if (creationMode) {
            action("model.nudge-x", "Move Plus X", "Move the picked atom by 0.2 angstrom along +X", [&] { nudgePicked(0, 0.2f); });
            action("model.nudge-y", "Move Plus Y", "Move the picked atom by 0.2 angstrom along +Y", [&] { nudgePicked(1, 0.2f); });
            action("model.nudge-z", "Move Plus Z", "Move the picked atom by 0.2 angstrom along +Z", [&] { nudgePicked(2, 0.2f); });
            action("model.nudge-x-neg", "Move Minus X", "Move the picked atom by 0.2 angstrom along -X", [&] { nudgePicked(0, -0.2f); });
            action("model.nudge-y-neg", "Move Minus Y", "Move the picked atom by 0.2 angstrom along -Y", [&] { nudgePicked(1, -0.2f); });
            action("model.nudge-z-neg", "Move Minus Z", "Move the picked atom by 0.2 angstrom along -Z", [&] { nudgePicked(2, -0.2f); });
        }
        ImGui::NewLine();
        if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0 && creationDihedral >= 0) {
            ImGui::Text("Distance %.3f A | Bond angle %.2f deg | Dihedral %.2f deg",
                        authoring::distance(source, creationPick, creationMeasure),
                        authoring::bondAngle(source, creationPick, creationMeasure, creationAngle),
                        authoring::dihedralAngle(source, creationPick, creationMeasure, creationAngle, creationDihedral));
        } else if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0) {
            ImGui::Text("Distance %.3f A | Bond angle %.2f deg",
                        authoring::distance(source, creationPick, creationMeasure),
                        authoring::bondAngle(source, creationPick, creationMeasure, creationAngle));
        } else if (creationPick >= 0 && creationMeasure >= 0) {
            ImGui::Text("Distance: %.3f A", authoring::distance(source, creationPick, creationMeasure));
        } else if (creationMode) {
            ImGui::TextDisabled("Creation Mode: click an atom. Shift-click measures distance. Ctrl-click sets the bond angle. Alt-click sets the dihedral.");
        } else {
            ImGui::TextDisabled("Right-click the structure and choose Enter Creation Mode to add, move, replace, or delete atoms.");
        }
        ImGui::EndChild();
    }
    void latticePanel() {
        const auto lattice = authoring::latticeOf(result.data.cell[0] != 0 || result.data.cell[4] != 0 ||
                                                           result.data.cell[8] != 0
                                                       ? result.data
                                                       : source);
        ImGui::BeginChild("Lattice properties", {-1, U(78)}, ImGuiChildFlags_Borders);
        ImGui::Text("Lattice  %s", lattice.system);
        ImGui::SameLine();
        ImGui::Text("a %.3f A   b %.3f A   c %.3f A", lattice.a, lattice.b, lattice.c);
        ImGui::Text("alpha %.2f deg   beta %.2f deg   gamma %.2f deg   volume %.2f A^3",
                    lattice.alpha, lattice.beta, lattice.gamma, lattice.volume);
        ImGui::TextDisabled("1 A = 0.1 nm. Group and space-group labels are not assigned yet.");
        recordUiTestItem("lattice.properties");
        ImGui::EndChild();
    }
    void horizontalSplitter(const char *id, float &size, float minimum, float maximum,
                            bool growsUp, const char *tooltip) {
        ImGui::PushID(id);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##splitter", {-1, U(7)});
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        recordUiTestItem(std::string("layout.") + id + "-splitter");
        if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (hovered) ImGui::SetTooltip("%s", tooltip);
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            const float delta = ImGui::GetIO().MouseDelta.y / uiScale * (growsUp ? -1.f : 1.f);
            if (delta != 0.f) {
                size = std::clamp(size + delta, minimum, maximum);
                layoutDirty = true;
            }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            size = !strcmp(id, "inspector") ? 180.f : 94.f;
            layoutDirty = true;
        }
        const ImU32 color = active || hovered ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_Separator);
        ImGui::GetWindowDrawList()->AddLine({start.x, start.y + U(3)},
                                            {start.x + ImGui::GetItemRectSize().x, start.y + U(3)}, color);
        ImGui::PopID();
    }
    void center(float w, float h) {
        fixed("Viewport workspace", leftWidth(), topInset(), w - leftWidth() - rightWidth(),
              h - topInset() - statusHeight());
        // The reference workspace opens directly onto the viewport. Keep
        // document tabs visible only when there is another tab to switch to.
        if (creationMode) modelingToolbar();
        if (showLatticePanel) latticePanel();
        auto avail = ImGui::GetContentRegionAvail();
        const float handleH = U(7);
        float gap = ImGui::GetStyle().ItemSpacing.y;
        const float timelineH = std::clamp(U(timelinePane), U(56), std::max(U(56), avail.y * .32f));
        const float maxDataH = std::max(U(90), avail.y - timelineH - ImGui::GetFrameHeight() - U(140) - U(35));
        const float dataH = showTable ? std::clamp(U(inspectorPane), U(90), maxDataH) : 0.f;
        // Exact stack: viewports, button row, optional inspector, timeline.
        float sceneH = std::max(U(120),
                                avail.y - dataH - timelineH - ImGui::GetFrameHeight() -
                                    gap * (showTable ? 5 : 4) - handleH * (showTable ? 2 : 1) - U(18));
        if (quad) {
            float vw = (avail.x - ImGui::GetStyle().ItemSpacing.x) * .5f, vh = (sceneH - gap) * .5f;
            viewport(0, vw, vh);
            ImGui::SameLine();
            viewport(1, vw, vh);
            viewport(2, vw, vh);
            ImGui::SameLine();
            viewport(3, vw, vh);
        } else
            viewport(active, avail.x, sceneH);
        if (ImGui::Button(showTable ? "Hide data inspector" : "Data inspector")) showTable = !showTable;
        ImGui::SameLine();
        ImGui::Text("%s particles  |  %zu selected", number(result.data.atoms.size()).c_str(), selectedCount);
        if (showTable) {
            horizontalSplitter("inspector", inspectorPane, 90.f, maxDataH / uiScale, true,
                               "Drag to resize data inspector; double-click to reset");
            ImGui::BeginChild("Inspector", {-1, dataH}, ImGuiChildFlags_Borders);
            std::string inspectLabel = "Final pipeline output";
            if (inspectorNode >= 0 && inspectorNode < int(mods.size()))
                inspectLabel = "Node " + std::to_string(inspectorNode + 1) + ": " +
                               mods[size_t(inspectorNode)].displayName;
            if (ImGui::BeginCombo("Inspect output", inspectLabel.c_str())) {
                if (ImGui::Selectable("Final pipeline output", inspectorNode < 0)) {
                    inspectorCancel = true;
                    inspectorNode = -1;
                    inspectorNodeId.clear();
                    inspectorResult.reset();
                }
                for (size_t i = 0; i < mods.size(); ++i) {
                    ImGui::PushID(mods[i].id.c_str());
                    std::string label = "Node " + std::to_string(i + 1) + ": " + mods[i].displayName;
                    ImGui::BeginDisabled(busy || indexing || pipelineBusy || inspectorBusy);
                    const bool selected = inspectorNode == int(i);
                    if (ImGui::Selectable(label.c_str(), selected)) inspectPipelineNode(int(i));
                    ImGui::EndDisabled();
                    if (mods[i].outputs.empty()) {
                        ImGui::Indent();
                        ImGui::TextDisabled("%s", mods[i].enabled ? "Output not evaluated" : "Disabled");
                        ImGui::Unindent();
                    } else {
                        ImGui::Indent();
                        if (mods[i].dirty) ImGui::TextDisabled("Last result (stale):");
                        for (const auto &output : mods[i].outputs) {
                            const char *kind=output.kind==DataObject::Kind::Particles ? "Particles" :
                                output.kind==DataObject::Kind::Bonds ? "Bonds" :
                                output.kind==DataObject::Kind::Cell ? "Simulation cell" :
                                output.kind==DataObject::Kind::Table ? "Data table" :
                                output.kind==DataObject::Kind::GlobalAttributes ? "Global attributes" : "Data object";
                            ImGui::TextDisabled("↳ %s (%s)",output.name.c_str(),kind);
                        }
                        ImGui::Unindent();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const PipelineResult *inspected = inspectorNode < 0 ? &result :
                inspectorResult ? &*inspectorResult : nullptr;
            if (inspectorBusy && inspectorNode >= 0) {
                ImGui::TextDisabled("Evaluating the selected node output... this may temporarily use additional memory.");
                const size_t stageCount = size_t(std::max(inspectorNode + 1, 1));
                const size_t activeStage = std::min(inspectorActiveNode.load(), stageCount);
                const float inspectionProgress = float(activeStage) / float(stageCount);
                char overlay[48]{};
                snprintf(overlay, sizeof(overlay), "%zu / %zu stages", activeStage, stageCount);
                ImGui::ProgressBar(inspectionProgress, ImVec2(-1, 0), overlay);
                if (ImGui::Button("Cancel node inspection")) inspectorCancel = true;
            } else if (inspectorBusy) {
                ImGui::TextDisabled("Cancelling the previous node inspection...");
            } else if (!inspected) {
                ImGui::TextDisabled("Node output is unavailable. Select it again after the pipeline is ready.");
            }
            if (inspected && (!inspectorBusy || inspectorNode < 0) && ImGui::BeginTabBar("Data")) {
                if (ImGui::BeginTabItem("Particles")) {
                    const size_t atomCount = inspected->data.atoms.size();
                    const size_t inspectedSelected = std::count(inspected->selected.begin(),
                                                                 inspected->selected.end(), uint8_t(1));
                    refreshInspectorFilter(particleFilter, atomCount, particleFilterCache,
                                           particleFilterRows, filteredParticles,
                                           [&](size_t i, std::string &text) {
                                               const auto &a = inspected->data.atoms[i];
                                               if (a.type < inspected->data.species.size())
                                                   text += inspected->data.species[a.type];
                                               char value[96];
                                               snprintf(value, sizeof(value), " %.4f %.4f %.4f",
                                                        a.x, a.y, a.z);
                                               text += value;
                                           });
                    const bool filtering = particleFilter[0];
                    const std::string particleSuffix = inspectedSelected
                        ? "  |  " + std::to_string(inspectedSelected) + " selected"
                            + (inspected->data.sampled() ? "  |  sampled preview" : "")
                        : inspected->data.sampled() ? "  |  sampled preview" : "";
                    inspectorToolRow("##particle-filter", particleFilter,
                                     filtering ? filteredParticles.size() : atomCount,
                                     atomCount, "particles", particleSuffix.c_str());
                    if (ImGui::BeginTable("Atoms", 3,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV,
                                          {-1, 0})) {
                        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, U(74));
                        ImGui::TableSetupColumn("Particle Type", ImGuiTableColumnFlags_WidthFixed, U(150));
                        ImGui::TableSetupColumn("Position [X Y Z]");
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip;
                        clip.Begin(int(filtering ? filteredParticles.size() : atomCount));
                        while (clip.Step())
                            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                                const int j = filtering ? int(filteredParticles[size_t(row)])
                                                        : row;
                                auto a = inspected->data.atoms[j];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                if (inspectorNode < 0 && ImGui::Selectable(std::to_string(j).c_str(),
                                                      inspected->selected[j] != 0,
                                                      ImGuiSelectableFlags_SpanAllColumns)) {
                                    checkpoint();
                                    size_t nodeIndex = mods.size();
                                    if (!mods.empty() && mods.back().enabled && mods.back().op == Op::ManualSelection)
                                        nodeIndex = mods.size()-1;
                                    if (nodeIndex == mods.size()) {
                                        Modifier manual{Op::ManualSelection};
                                        if (ImGui::GetIO().KeyCtrl)
                                            for (size_t k=0;k<result.selected.size();++k)
                                                if (result.selected[k]) manual.manualSelection.push_back(uint32_t(k));
                                        modifierGraph.insert(makeNode(manual));
                                        nodeIndex = mods.size()-1;
                                    }
                                    auto &indices = mods[nodeIndex].manualSelection;
                                    if (!ImGui::GetIO().KeyCtrl) indices.assign(1,uint32_t(j));
                                    else {
                                        auto found=std::lower_bound(indices.begin(),indices.end(),uint32_t(j));
                                        if (found!=indices.end() && *found==uint32_t(j)) indices.erase(found);
                                        else indices.insert(found,uint32_t(j));
                                    }
                                    modifierGraph.selected=nodeIndex;
                                    update(nodeIndex);
                                } else if (inspectorNode >= 0) ImGui::Text("%d", j);
                                ImGui::TableNextColumn();
                                colorSwatch(typeColor(a.type));
                                ImGui::SameLine();
                                ImGui::Text("%u (%s)", a.type,
                                            a.type < inspected->data.species.size()
                                                ? inspected->data.species[a.type].c_str() : "?");
                                ImGui::TableNextColumn();
                                ImGui::Text("%.4f %.4f %.4f", a.x, a.y, a.z);
                            }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                const bool bondsOpen = ImGui::BeginTabItem("Bonds", nullptr,
                    bondsTab ? ImGuiTabItemFlags_SetSelected : 0);
                // Recorded outside the selected-tab branch so the harness can
                // reach the tab even while another page is shown.
                recordUiTestItem("inspector.bonds-tab");
                if (bondsOpen) {
                    bondsTab = false;
                    const size_t bondCount = inspected->data.bonds.size();
                    refreshInspectorFilter(bondFilter, bondCount, bondFilterCache,
                                           bondFilterRows, filteredBonds,
                                           [&](size_t i, std::string &text) {
                                               const auto &bond = inspected->data.bonds[i];
                                               char value[96];
                                               snprintf(value, sizeof(value), "%u %u %d %d %d 1",
                                                        bond.a, bond.b, bond.image[0],
                                                        bond.image[1], bond.image[2]);
                                               text += value;
                                           });
                    const bool filtering = bondFilter[0];
                    const size_t shown = filtering ? filteredBonds.size() : bondCount;
                    inspectorToolRow("##bond-filter", bondFilter, shown, bondCount, "bonds");
                    if (!bondCount) {
                        ImGui::TextDisabled("No bonds have been created at this output.");
                    } else if (ImGui::BeginTable("Bond rows", 4,
                                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                     ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, U(74));
                        ImGui::TableSetupColumn("Topology [A B]", ImGuiTableColumnFlags_WidthFixed, U(140));
                        ImGui::TableSetupColumn("Periodic Image [X Y Z]", ImGuiTableColumnFlags_WidthFixed, U(170));
                        ImGui::TableSetupColumn("Bond Order");
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip;
                        clip.Begin(int(shown));
                        while (clip.Step())
                            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                                const int j = filtering ? int(filteredBonds[size_t(row)]) : row;
                                const auto &bond = inspected->data.bonds[size_t(j)];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("%d", j);
                                ImGui::TableNextColumn();
                                ImGui::Text("%u %u", bond.a, bond.b);
                                ImGui::TableNextColumn();
                                ImGui::Text("%d %d %d", bond.image[0], bond.image[1], bond.image[2]);
                                ImGui::TableNextColumn();
                                colorSwatch(inspected->data.bondStyle.color);
                                ImGui::SameLine();
                                ImGui::Text("%u",unsigned(bond.order));
                            }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Types")) {
                    std::vector<size_t> counts(inspected->data.species.size(), 0);
                    for (const auto &a : inspected->data.atoms)
                        if (a.type < counts.size()) ++counts[a.type];
                    if (ImGui::BeginTable("Type rows", 3,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, U(150));
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, U(160));
                        ImGui::TableSetupColumn("Particles");
                        ImGui::TableHeadersRow();
                        for (size_t t = 0; t < inspected->data.species.size(); ++t) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            colorSwatch(typeColor(t));
                            ImGui::SameLine();
                            ImGui::Text("%zu", t);
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(inspected->data.species[t].c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", counts[t]);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Simulation Cell")) {
                    const auto &cellVectors = inspected->data.cell;
                    const auto &origin = inspected->data.origin;
                    const auto length = [&cellVectors](int row) {
                        return std::hypot(cellVectors[row * 3], cellVectors[row * 3 + 1],
                                          cellVectors[row * 3 + 2]);
                    };
                    ImGui::SeparatorText("Geometry");
                    ImGui::RadioButton("Cell vectors", &cellViewMode, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("Cell parameters", &cellViewMode, 1);
                    if (cellViewMode == 1) {
                        // Read-only reduced form: the vector lengths a/b/c and
                        // the inter-vector angles alpha (b^c), beta (a^c),
                        // gamma (a^b) in degrees.
                        const auto angle = [&](int r1, int r2) {
                            const double d = cellVectors[r1*3]*cellVectors[r2*3] +
                                             cellVectors[r1*3+1]*cellVectors[r2*3+1] +
                                             cellVectors[r1*3+2]*cellVectors[r2*3+2];
                            const double denominator = length(r1) * length(r2);
                            return denominator > 0
                                ? std::acos(std::clamp(d / denominator, -1., 1.)) * 180. / 3.141592653589793
                                : 0.;
                        };
                        ImGui::Text("a = %12.4f   b = %12.4f   c = %12.4f",
                                    length(0), length(1), length(2));
                        ImGui::Text("alpha = %9.4f   beta = %9.4f   gamma = %9.4f",
                                    angle(1, 2), angle(0, 2), angle(0, 1));
                    } else {
                        for (int row = 0; row < 3; row++)
                            ImGui::Text("%c  %12.4f   %12.4f   %12.4f", 'a' + row,
                                        cellVectors[row * 3], cellVectors[row * 3 + 1],
                                        cellVectors[row * 3 + 2]);
                    }
                    ImGui::Text("Cell origin  o  %12.4f   %12.4f   %12.4f",
                                origin.x, origin.y, origin.z);
                    ImGui::SeparatorText("Bounding box");
                    ImGui::Text("Width (X)   %12.4f", length(0));
                    ImGui::Text("Length (Y)  %12.4f", length(1));
                    ImGui::Text("Height (Z)  %12.4f", length(2));
                    ImGui::SeparatorText("Periodic boundary conditions");
                    for (int axis = 0; axis < 3; ++axis) {
                        if (axis) ImGui::SameLine();
                        const ImVec4 color = inspected->data.pbc[axis]
                            ? ImVec4{.16f, .62f, .26f, 1.f} : ImVec4{.55f, .55f, .55f, 1.f};
                        ImGui::TextColored(color, "%c: %s", 'X' + axis,
                                           inspected->data.pbc[axis] ? "yes" : "no");
                    }
                    ImGui::SeparatorText("Dimensionality");
                    const bool flat = length(2) == 0;
                    ImGui::TextUnformatted(flat ? "2D" : "3D");
                    if (ImGui::Button("Edit in pipeline...")) {
                        size_t found = mods.size();
                        for (size_t i = 0; i < mods.size(); ++i)
                            if (mods[i].op == Op::EditCell) { found = i; break; }
                        if (found == mods.size())
                            add(Op::EditCell);
                        else {
                            checkpoint();
                            modifierGraph.selected = int(found);
                            update();
                        }
                    }
                    ImGui::EndTabItem();
                }
                const bool globalAttributesOpen=ImGui::BeginTabItem("Global Attributes",nullptr,
                    globalAttributesTab ? ImGuiTabItemFlags_SetSelected : 0);
                recordUiTestItem("inspector.global-attributes-tab","Global Attributes");
                if (globalAttributesOpen) {
                    globalAttributesTab=false;
                    ImGui::TextWrapped("%s", inspected->data.comment.c_str());
                    std::vector<std::pair<std::string, std::string>> entries;
                    for (const auto &[name, value] : inspected->data.globalAttributes) {
                        char formatted[64];
                        snprintf(formatted, sizeof(formatted), "%.8g", value);
                        entries.push_back({name, formatted});
                    }
                    if (!path.empty()) entries.push_back({"SourceFile", utf8(path.wstring())});
                    entries.push_back({"SourceFrame", std::to_string(current)});
                    entries.push_back({"Time", std::to_string(current)});
                    std::sort(entries.begin(), entries.end());
                    refreshInspectorFilter(globalAttributeFilter, entries.size(),
                                           globalAttributeFilterCache,
                                           globalAttributeFilterRows, filteredAttributes,
                                           [&](size_t i, std::string &text) {
                                               text += entries[i].first;
                                               text += ' ';
                                               text += entries[i].second;
                                           });
                    const bool filtering = globalAttributeFilter[0];
                    const size_t shown = filtering ? filteredAttributes.size() : entries.size();
                    inspectorToolRow("##global-attribute-filter", globalAttributeFilter,
                                     shown, entries.size(), "attributes");
                    if (ImGui::BeginTable("Global attribute rows", 2,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthFixed, U(220));
                        ImGui::TableSetupColumn("Value");
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clip;
                        clip.Begin(int(shown));
                        while (clip.Step())
                            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                                const size_t i = filtering
                                    ? filteredAttributes[size_t(row)] : size_t(row);
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(entries[i].first.c_str());
                                recordUiTestItem(std::string("inspector.global-attribute.") +
                                                 entries[i].first);
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(entries[i].second.c_str());
                            }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                const bool dataTablesOpen=ImGui::BeginTabItem("Data Tables", nullptr,
                                        histogramPreviewTab ? ImGuiTabItemFlags_SetSelected : 0);
                if (dataTablesOpen) {
                    histogramPreviewTab = false;
                    if (inspected->data.tables.empty()) ImGui::TextDisabled("No analysis tables have been produced at this output.");
                    for (size_t tableIndex = 0; tableIndex < inspected->data.tables.size(); ++tableIndex) {
                        const auto &table = inspected->data.tables[tableIndex];
                        ImGui::PushID(int(tableIndex));
                        if (ImGui::CollapsingHeader(table.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            if (ImGui::SmallButton("Export CSV...")) {
                                const auto outputPath = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                                if (!outputPath.empty()) {
                                    try { writeDataTableCsv(outputPath, table); status = "Exported " + table.name; }
                                    catch (const std::exception &exception) { error = exception.what(); }
                                }
                            }
                            if (table.name.rfind("Histogram: ", 0) == 0 &&
                                table.columns.size() >= 2 && !table.rows.empty()) {
                                std::vector<float> bins;
                                bins.reserve(table.rows.size());
                                for (const auto &row : table.rows) {
                                    char *end = nullptr;
                                    const float value = row.size() > 1
                                        ? std::strtof(row[1].c_str(), &end) : 0.f;
                                    const float finiteValue = end && *end == '\0' && std::isfinite(value)
                                        ? std::max(0.f, value) : 0.f;
                                    bins.push_back(finiteValue);
                                }
                                const std::string viewKey =
                                    (inspectorNode < 0 ? std::string("final") : inspectorNodeId) +
                                    "|" + table.name + "|" + std::to_string(tableIndex);
                                auto &view = histogramPlotViews[viewKey];
                                if (view.lastBin < 0) view.lastBin = int(bins.size()) - 1;
                                view.firstBin = std::clamp(view.firstBin, 0, int(bins.size()) - 1);
                                view.lastBin = std::clamp(view.lastBin, view.firstBin, int(bins.size()) - 1);
                                int firstBin = view.firstBin + 1, lastBin = view.lastBin + 1;
                                ImGui::SetNextItemWidth(U(100));
                                if (ImGui::InputInt("First bin", &firstBin, 1, 8))
                                    view.firstBin = std::clamp(firstBin - 1, 0, int(bins.size()) - 1);
                                view.lastBin = std::max(view.lastBin, view.firstBin);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(U(100));
                                if (ImGui::InputInt("Last bin", &lastBin, 1, 8))
                                    view.lastBin = std::clamp(lastBin - 1, view.firstBin, int(bins.size()) - 1);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Reset plot view")) {
                                    view.firstBin = 0;
                                    view.lastBin = int(bins.size()) - 1;
                                    view.yMaximum = 0;
                                }
                                ImGui::SetNextItemWidth(U(170));
                                ImGui::InputFloat("Y axis maximum (0 = auto)", &view.yMaximum,
                                                  0, 0, "%.6g");
                                view.yMaximum = std::max(0.f, std::isfinite(view.yMaximum)
                                    ? view.yMaximum : 0.f);
                                std::vector<float> visibleBins(
                                    bins.begin() + view.firstBin, bins.begin() + view.lastBin + 1);
                                float visibleMaximum = 0;
                                for (float value : visibleBins) visibleMaximum = std::max(visibleMaximum, value);
                                const float scaleMaximum = view.yMaximum > 0
                                    ? view.yMaximum : visibleMaximum > 0 ? visibleMaximum : 1.f;
                                ImGui::TextDisabled("%s across bins %d-%d of %zu",
                                    table.columns[1].c_str(), view.firstBin + 1, view.lastBin + 1, bins.size());
                                ImGui::PlotHistogram("##histogram-result", visibleBins.data(),
                                    int(visibleBins.size()), 0, nullptr, 0, scaleMaximum, {-1, U(72)});
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("Bin centers are listed in the table below.");
                            }
                            if (table.name.rfind("Scatter plot: ",0)==0 &&
                                table.columns.size()>=2 && !table.rows.empty()) {
                                std::vector<std::pair<double,double>> points;
                                points.reserve(table.rows.size());
                                double minX=std::numeric_limits<double>::infinity();
                                double maxX=-minX,minY=minX,maxY=-minX;
                                for (const auto &row:table.rows) {
                                    if (row.size()<2) continue;
                                    const double x=std::stod(row[0]), y=std::stod(row[1]);
                                    points.emplace_back(x,y);
                                    minX=std::min(minX,x); maxX=std::max(maxX,x);
                                    minY=std::min(minY,y); maxY=std::max(maxY,y);
                                }
                                if (!points.empty()) {
                                    const ImVec2 chartSize{ImGui::GetContentRegionAvail().x,U(150)};
                                    ImGui::InvisibleButton("##scatter-plot",chartSize);
                                    recordUiTestItem("inspector.scatter-chart","##scatter-plot");
                                    const ImVec2 chartMin=ImGui::GetItemRectMin();
                                    const ImVec2 chartMax=ImGui::GetItemRectMax();
                                    auto *draw=ImGui::GetWindowDrawList();
                                    draw->AddRectFilled(chartMin,chartMax,ImGui::GetColorU32(ImGuiCol_FrameBg),U(3));
                                    draw->AddRect(chartMin,chartMax,ImGui::GetColorU32(ImGuiCol_Border),U(3));
                                    const float pad=U(12), innerW=std::max(1.f,chartMax.x-chartMin.x-pad*2),
                                                innerH=std::max(1.f,chartMax.y-chartMin.y-pad*2);
                                    const double scaleX=std::max(std::abs(minX),std::abs(maxX));
                                    const double scaleY=std::max(std::abs(minY),std::abs(maxY));
                                    const double loX=scaleX?minX/scaleX:0, hiX=scaleX?maxX/scaleX:1;
                                    const double loY=scaleY?minY/scaleY:0, hiY=scaleY?maxY/scaleY:1;
                                    const double spanX=hiX-loX, spanY=hiY-loY;
                                    const size_t step=std::max<size_t>(1,(points.size()+29999)/30000);
                                    const ImU32 pointColor=ImGui::GetColorU32(accent);
                                    for (size_t i=0;i<points.size();i+=step) {
                                        const double x=scaleX?points[i].first/scaleX:0;
                                        const double y=scaleY?points[i].second/scaleY:0;
                                        const float fx=float(spanX>0?(x-loX)/spanX:.5);
                                        const float fy=float(spanY>0?(y-loY)/spanY:.5);
                                        draw->AddCircleFilled({chartMin.x+pad+std::clamp(fx,0.f,1.f)*innerW,
                                                               chartMax.y-pad-std::clamp(fy,0.f,1.f)*innerH},
                                                              U(1.8f),pointColor);
                                    }
                                    draw->AddText({chartMin.x+pad,chartMin.y+pad},
                                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),table.columns[1].c_str());
                                    draw->AddText({chartMax.x-pad-U(80),chartMax.y-pad-U(14)},
                                                  ImGui::GetColorU32(ImGuiCol_TextDisabled),table.columns[0].c_str());
                                    if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("%zu plotted pairs%s",points.size(),
                                            table.name.find("deterministic preview")!=std::string::npos
                                                ? " (deterministic preview subset)" : "");
                                }
                            }
                            if (ImGui::BeginTable("table", int(table.columns.size()), ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, {-1, U(240)})) {
                                for (const auto &column : table.columns) ImGui::TableSetupColumn(column.c_str());
                                ImGui::TableHeadersRow();
                                ImGuiListClipper clip;
                                clip.Begin(int(table.rows.size()));
                                while (clip.Step()) for (int rowIndex = clip.DisplayStart; rowIndex < clip.DisplayEnd; ++rowIndex) {
                                    const auto &row = table.rows[size_t(rowIndex)];
                                    ImGui::TableNextRow();
                                    for (size_t column = 0; column < table.columns.size(); ++column) {
                                        ImGui::TableNextColumn();
                                        if (column < row.size()) ImGui::TextUnformatted(row[column].c_str());
                                    }
                                }
                                ImGui::EndTable();
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
        }
        horizontalSplitter("timeline", timelinePane, 56.f,
                           std::max(56.f, avail.y * .32f / uiScale), true,
                           "Drag to resize timeline; double-click to reset");
        timeline(timelineH);
        ImGui::End();
    }
    void verticalSplitter(const char *id, float x, float h, float &size,
                          float minimum, float maximum, float sign, float reset) {
        ImGui::SetNextWindowPos({x - U(7), topInset()});
        ImGui::SetNextWindowSize({U(14), h - topInset() - statusHeight()});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, {1, 1});
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoNav;
        ImGui::Begin(id, nullptr, flags);
        ImGui::InvisibleButton("##splitter", ImGui::GetContentRegionAvail());
        recordUiTestItem(std::string("layout.") + id + "-splitter");
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (hovered) ImGui::SetTooltip("Drag to resize panel; double-click to reset");
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            const float delta = ImGui::GetIO().MouseDelta.x * sign / uiScale;
            if (delta != 0.f) {
                size = std::clamp(size + delta, minimum, maximum);
                layoutDirty = true;
            }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            size = reset;
            layoutDirty = true;
        }
        const ImVec2 mn = ImGui::GetWindowPos(), mx = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddLine({x, mn.y}, {x, mn.y + mx.y},
            active || hovered ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_Separator));
        const float midY = mn.y + mx.y * .5f;
        ImGui::GetWindowDrawList()->AddRectFilled({x-U(3), midY-U(24)},
            {x+U(3), midY+U(24)}, ImGui::GetColorU32(active || hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), U(3));
        for (int i=-1;i<=1;++i)
            ImGui::GetWindowDrawList()->AddCircleFilled({x,midY+U(9*i)},U(1.4f),
                active || hovered ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    void layoutSplitters(float w, float h) {
        const float minCenter = U(320);
        if (showWorkspace)
            verticalSplitter("Workspace edge", workspaceWidth(), h, workspacePane, 150.f,
                std::max(150.f, (w - pipelineWidth() - rightWidth() - minCenter) / uiScale), 1.f, 220.f);
        verticalSplitter("Pipeline edge", leftWidth(), h, pipelinePane, 180.f,
            std::max(180.f, (w - workspaceWidth() - rightWidth() - minCenter) / uiScale), 1.f, 268.f);
        verticalSplitter("Properties edge", w - rightWidth(), h, propertiesPane, 200.f,
            std::max(200.f, (w - leftWidth() - minCenter) / uiScale), -1.f, 316.f);
    }
    void homeWorkspace(float w, float h) {
        fixed("Home workspace", 0, topInset(), w, h - topInset() - statusHeight());
        const float cardW = U(250);
        ImGui::SetCursorPos({std::max(U(24),w*.5f-cardW*1.55f),U(105)});
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("开始使用 AtomX");
        if (headingFont) ImGui::PopFont();
        ImGui::SetCursorPosX(std::max(U(24),w*.5f-cardW*1.55f));
        ImGui::TextDisabled("打开结构，或从空白结构开始创作");
        ImGui::SetCursorPosX(std::max(U(24),w*.5f-cardW*1.55f));
        if (ImGui::Button("打开结构文件", {cardW,U(100)})) open();
        ImGui::SameLine(0,U(16));
        if (ImGui::Button("新建空白结构", {cardW,U(100)})) newBlankCreation();
        ImGui::SameLine(0,U(16));
        if (ImGui::Button("从晶体开始", {cardW,U(100)})) openCrystalDialog = true;
        ImGui::End();
    }
    void creationDialogs() {
        if(openCreationBondLabels) { ImGui::OpenPopup("键标签");openCreationBondLabels=false; }
        const auto *bondLabelViewport=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(bondLabelViewport->GetWorkCenter(),ImGuiCond_Always,{.5f,.5f});
        ImGui::SetNextWindowSizeConstraints({0,0},{std::max(U(200),bondLabelViewport->WorkSize.x-U(24)),std::max(U(200),bondLabelViewport->WorkSize.y-U(24))});
        if(ImGui::BeginPopupModal("键标签",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool valid=creationMode && activeTab>=0 && activeTab<int(tabs.size()) &&
                tabs[size_t(activeTab)].id==creationBondLabelTabId && pipelineGeneration==creationBondLabelGeneration;
            ImGui::TextDisabled("对象：显式键 · 数值跟随当前坐标和键级");
            ImGui::Checkbox("整个体系",&creationBondLabelAll);recordUiTestItem("creation.bond-label-all");
            ImGui::TextDisabled(creationBondLabelRows.empty()?"局部：两端原子都在打开窗口时的选择中":"局部：打开窗口时直接选中的键");
            ImGui::Text("作用范围：%zu 条键",creationBondLabelAll?source.bonds.size():creationBondLabelCount);
            ImGui::SeparatorText("属性（按勾选顺序组合）");
            static constexpr const char *captions[]={"键长 Length (Å)","键级 BondOrder","端点编号","中点 MidPoint (Å)","周期像 Image"};
            for(int i=0;i<5;++i) {
                auto &fields=creationBondLabelRule.fields;const auto value=creation::BondField(i);
                const auto it=std::find(fields.begin(),fields.end(),value);bool checked=it!=fields.end();
                if(ImGui::Checkbox(captions[i],&checked)) {if(checked)fields.push_back(value);else fields.erase(it);}
                recordUiTestItem("creation.bond-label-field-"+std::to_string(i));
            }
            ImGui::SetNextItemWidth(U(280));ImGui::InputText("文字",creationBondLabelText,sizeof(creationBondLabelText));
            recordUiTestItem("creation.bond-label-text");
            creationBondLabelRule.text=creationBondLabelText;
            ImGui::SetNextItemWidth(U(150));ImGui::SliderInt("有效数字",&creationBondLabelRule.precision,1,9);
            ImGui::SliderFloat("字号",&creationBondLabelDraft.fontSize,10,32,"%.0f");
            ImGui::Checkbox("粗体",&creationBondLabelDraft.bold);
            ImGui::ColorEdit4("颜色",creationBondLabelDraft.color.data(),ImGuiColorEditFlags_NoInputs);
            ImGui::Checkbox("显示键标签",&creationBondLabelDraft.visible);
            ImGui::SetNextItemWidth(U(150));ImGui::InputInt("屏幕标签上限",&creationBondLabelDraft.budget);
            ImGui::TextDisabled("1–2000，默认 500；大体系抽样显示；隐藏键不标注");
            if(!valid)ImGui::TextColored({1,.65f,.2f,1},"体系已改变，请关闭后重新打开");
            if(!creationBondLabelMessage.empty())ImGui::TextWrapped("%s",creationBondLabelMessage.c_str());
            auto apply=[&](const creation::BondLabel &rule,bool all,const char *message,bool style) {
                try {
                    auto next=creationDisplay;
                    if(style) {next.bondLabels.fontSize=creationBondLabelDraft.fontSize;next.bondLabels.color=creationBondLabelDraft.color;
                        next.bondLabels.bold=creationBondLabelDraft.bold;next.bondLabels.visible=creationBondLabelDraft.visible;next.bondLabels.budget=creationBondLabelDraft.budget;}
                    if(creationBondLabelRows.empty())next.bondLabels.set(source,creationBondLabelSelection,rule,all);
                    else next.bondLabels.setBonds(source,creationBondLabelRows,rule,all);
                    editCreationDisplay(std::move(next),message);creationBondLabelMessage=message;
                } catch(const std::exception &e) {creationBondLabelMessage=e.what();}
            };
            ImGui::BeginDisabled(!valid || documentsBusy());
            ImGui::BeginDisabled((!creationBondLabelAll && !creationBondLabelCount) || creationBondLabelDraft.budget<1 || creationBondLabelDraft.budget>2000 || !creation::validBondLabel(creationBondLabelRule));
            if(ImGui::Button("应用",{U(110),U(30)}))apply(creationBondLabelRule,creationBondLabelAll,"编辑键标签",true);
            recordUiTestItem("creation.bond-label-apply");ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button("移除",{U(110),U(30)}))apply({},creationBondLabelAll,"移除键标签",false);
            recordUiTestItem("creation.bond-label-remove");
            if(ImGui::Button("移除全部",{U(110),U(30)}))apply({},true,"移除全部键标签",false);
            recordUiTestItem("creation.bond-label-remove-all");ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button("关闭",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
            recordUiTestItem("creation.bond-label-close");ImGui::EndPopup();
        }
        if (openCreationLabels) { ImGui::OpenPopup("原子标签"); openCreationLabels=false; }
        // Keep the expanded property editor and its actions inside the native window.
        const auto *labelViewport=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(labelViewport->GetWorkCenter(),ImGuiCond_Always,{0.5f,0.5f});
        ImGui::SetNextWindowSizeConstraints({0,0},
            {std::max(U(200),labelViewport->WorkSize.x-U(24)),std::max(U(200),labelViewport->WorkSize.y-U(24))});
        if (ImGui::BeginPopupModal("原子标签",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool valid=creationMode && activeTab>=0 && activeTab<int(tabs.size()) &&
                tabs[size_t(activeTab)].id==creationLabelTabId;
            ImGui::TextDisabled("对象：原子 · 默认显示元素和编号");
            ImGui::Checkbox("整个体系",&creationLabelAll);
            recordUiTestItem("creation.label-all");
            if (!creationLabelAll) ImGui::Text("选中 %zu 个原子",creationLabelSelection.size());
            ImGui::SetNextItemWidth(U(280));
            const char *kinds[]={"无标签","元素","原子编号","元素 + 编号","笛卡尔坐标","分数坐标","自定义文字","属性组合"};
            if(ImGui::BeginCombo("内容",kinds[creationLabelKind])) {
                for(int i=0;i<8;++i) {
                    if(ImGui::Selectable(kinds[i],creationLabelKind==i)) creationLabelKind=i;
                    recordUiTestItem("creation.label-kind-"+std::to_string(i));
                } ImGui::EndCombo();
            } recordUiTestItem("creation.label-kind");
            const bool properties=creationLabelKind==int(creation::LabelKind::Properties);
            if(properties) {
                ImGui::TextDisabled("可组合至多 16 项，按勾选顺序排列");
                ImGui::BeginChild("##label-properties",{U(370),U(155)},ImGuiChildFlags_Borders);
                auto field=[&](creation::LabelField value,const std::string &caption,const std::string &key) {
                    const auto it=std::find(creationLabelFields.begin(),creationLabelFields.end(),value);
                    bool checked=it!=creationLabelFields.end();
                    ImGui::BeginDisabled(!checked && creationLabelFields.size()>=creation::labelFieldLimit);
                    ImGui::PushID(key.c_str());
                    if(ImGui::Checkbox(caption.c_str(),&checked)) {
                        if(checked) creationLabelFields.push_back(std::move(value)); else creationLabelFields.erase(it);
                    } recordUiTestItem("creation.label-field-"+key); ImGui::PopID(); ImGui::EndDisabled();
                };
                const char *captions[]={"元素","原子编号","笛卡尔坐标","分数坐标","元素名称","原子序数","质量 Mass"};
                for(int i=0;i<7;++i) field({creation::LabelFieldKind(i),{}},captions[i],"builtin-"+std::to_string(i));
                for(const auto &[name,values]:source.scalarProperties) {
                    (void)values; if(name!="Mass") field({creation::LabelFieldKind::Scalar,name},name,"scalar-"+name);
                }
                for(const auto &[name,values]:source.vectorProperties) {
                    (void)values;
                    for(int i=int(creation::LabelFieldKind::Vector);i<=int(creation::LabelFieldKind::VectorMagnitude);++i) {
                        creation::LabelField value{creation::LabelFieldKind(i),name};
                        field(value,creation::fieldTitle(value),"vector-"+name+"-"+std::to_string(i));
                    }
                }
                ImGui::EndChild();
                if(ImGui::Button("清空属性")) creationLabelFields.clear(); recordUiTestItem("creation.label-clear-fields");
                ImGui::SetNextItemWidth(U(160)); ImGui::SliderInt("有效数字",&creationLabelPrecision,1,9);
                recordUiTestItem("creation.label-precision");
                ImGui::TextDisabled("Mass 列优先；没有该列时用元素质量。缺失值为 N/A。");
            }
            if (creationLabelKind==int(creation::LabelKind::Custom) || properties) {
                ImGui::SetNextItemWidth(U(280));
                ImGui::InputText("文字",creationLabelText,sizeof(creationLabelText));
            }
            creation::Label rule{creation::LabelKind(creationLabelKind),
                creationLabelKind==int(creation::LabelKind::Custom) || properties?creationLabelText:""};
            if(properties) { rule.fields=creationLabelFields; rule.precision=creationLabelPrecision;
                const int preview=creationLabelSelection.empty()?0:creationLabelSelection.back();
                ImGui::BeginChild("##label-preview",{U(370),U(75)},ImGuiChildFlags_Borders);
                const auto previewText=creation::labelText(source,preview,rule);
                ImGui::TextUnformatted(previewText.c_str()); ImGui::EndChild();
            }
            ImGui::SliderFloat("字号",&creationLabelDraft.fontSize,10,32,"%.0f");
            ImGui::Checkbox("粗体",&creationLabelDraft.bold);
            ImGui::ColorEdit4("颜色",creationLabelDraft.color.data(),ImGuiColorEditFlags_NoInputs);
            ImGui::Checkbox("显示标签",&creationLabelDraft.labelsVisible);
            ImGui::SetNextItemWidth(U(160));
            ImGui::InputInt("屏幕标签上限",&creationLabelDraft.labelBudget);
            ImGui::TextDisabled("大体系自动抽样，优先显示选中原子的标签");
            ImGui::TextDisabled("隐藏原子不显示标签；上限 1-2000，默认 500");
            const bool validText=(creationLabelKind!=int(creation::LabelKind::Custom) || creationLabelText[0]) && creation::validLabel(rule);
            const bool validBudget=creationLabelDraft.labelBudget>0 && creationLabelDraft.labelBudget<=2000;
            ImGui::BeginDisabled(!valid || documentsBusy() || (!creationLabelAll && creationLabelSelection.empty()));
            ImGui::BeginDisabled(!validText || !validBudget);
            if (ImGui::Button("应用",{U(110),U(30)})) {
                auto next=creationDisplay;
                next.fontSize=creationLabelDraft.fontSize; next.color=creationLabelDraft.color;
                next.bold=creationLabelDraft.bold; next.labelsVisible=creationLabelDraft.labelsVisible;
                next.labelBudget=creationLabelDraft.labelBudget;
                next.setLabels(source.atoms.size(),creationLabelSelection,rule,creationLabelAll);
                editCreationDisplay(std::move(next),"编辑原子标签");
            }
            recordUiTestItem("creation.label-apply");
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("移除",{U(110),U(30)})) {
                auto next=creationDisplay;
                next.setLabels(source.atoms.size(),creationLabelSelection,{},creationLabelAll);
                editCreationDisplay(std::move(next),"移除原子标签");
            }
            recordUiTestItem("creation.label-remove");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!valid || documentsBusy());
            if (ImGui::Button("移除全部",{U(110),U(30)})) {
                auto next=creationDisplay; next.setLabels(source.atoms.size(),{}, {},true);
                editCreationDisplay(std::move(next),"移除全部原子标签");
            }
            recordUiTestItem("creation.label-remove-all");
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("关闭",{U(110),U(30)}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            recordUiTestItem("creation.label-close");
            ImGui::EndPopup();
        }
        if (openCreationMovement) {
            ImGui::OpenPopup("精准移动 / 旋转"); openCreationMovement=false;
        }
        if (ImGui::BeginPopupModal("精准移动 / 旋转",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("操作 %zu 个原子",creationMovementSelection.size());
            if(!creationMovementBonds.empty()) {
                ImGui::TextDisabled("含 %zu 条选中键的端点，共用端点只移动一次",creationMovementBonds.size());
                if(creationMovementPeriodic)ImGui::TextWrapped("按主晶胞端点坐标操作；晶胞不变，跨周期键在旋转后可能改变长度。");
            }
            ImGui::TextDisabled(creationMovementMassCenter?"绕选中原子的质心旋转 · 不移动晶胞":"绕选中原子的几何中心旋转 · 不移动晶胞");
            ImGui::BeginDisabled(!creationMovementHasMass);
            ImGui::Checkbox("绕质心旋转（需 Mass 属性）",&creationMovementMassCenter);
            recordUiTestItem("creation.movement-mass-center"); ImGui::EndDisabled();
            if (ImGui::RadioButton("屏幕轴",creationMovementScreenAxes)) creationMovementScreenAxes=true;
            recordUiTestItem("creation.movement-screen-axes");
            ImGui::SameLine();
            if (ImGui::RadioButton("体系轴 XYZ",!creationMovementScreenAxes)) creationMovementScreenAxes=false;
            recordUiTestItem("creation.movement-world-axes");
            ImGui::SeparatorText("移动");
            if (ImGui::RadioButton("距离 Å",!creationMovementPercent)) creationMovementPercent=false;
            ImGui::SameLine();
            ImGui::BeginDisabled(creationMovementScreenSpan<=0);
            if (ImGui::RadioButton("视图比例 %",creationMovementPercent)) creationMovementPercent=true;
            recordUiTestItem("creation.movement-percent");
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(U(220));
            ImGui::InputFloat(creationMovementPercent?"步长 (%)":"步长 (Å)",&creationMovementDistance,0,0,"%.4f");
            if (creationMovementPercent)
                ImGui::TextDisabled("以打开窗口时视图的较短边为 100%%（选中中心所在深度）");
            ImGui::SetNextItemWidth(U(220));
            ImGui::InputFloat("旋转步长 (°)",&creationMovementAngle,0,0,"%.4f");
            const bool canTranslate=std::isfinite(creationMovementDistance)&&creationMovementDistance>0 &&
                (!creationMovementPercent || creationMovementScreenSpan>0);
            const bool canRotate=std::isfinite(creationMovementAngle)&&creationMovementAngle>0;
            ImGui::BeginDisabled(!validCreationMovement() || documentsBusy());
            auto action=[&](const char *label,const char *key,int axis,int sign,bool rotate) {
                if (ImGui::Button(label,{U(134),U(34)})) applyCreationMovement(axis,sign,rotate);
                recordUiTestItem(key);
            };
            ImGui::BeginDisabled(!canTranslate);
            action(creationMovementScreenAxes?"← 左":"-X","creation.movement-left",0,-1,false); ImGui::SameLine();
            action(creationMovementScreenAxes?"右 →":"+X","creation.movement-right",0,1,false); ImGui::SameLine();
            action(creationMovementScreenAxes?"↑ 上":"+Y","creation.movement-up",1,1,false);
            action(creationMovementScreenAxes?"↓ 下":"-Y","creation.movement-down",1,-1,false); ImGui::SameLine();
            // The right-handed view matrix has +Z toward the viewer.
            action(creationMovementScreenAxes?"向里":"+Z","creation.movement-in",2,creationMovementScreenAxes?-1:1,false); ImGui::SameLine();
            action(creationMovementScreenAxes?"向外":"-Z","creation.movement-out",2,creationMovementScreenAxes?1:-1,false);
            ImGui::EndDisabled();
            ImGui::SeparatorText("旋转（右手方向，反向使用负角度）");
            ImGui::BeginDisabled(!canRotate);
            action("X +角度","creation.movement-rotate-x",0,1,true); ImGui::SameLine();
            action("Y +角度","creation.movement-rotate-y",1,1,true); ImGui::SameLine();
            action("Z +角度","creation.movement-rotate-z",2,1,true);
            action("X -角度","creation.movement-rotate-neg-x",0,-1,true); ImGui::SameLine();
            action("Y -角度","creation.movement-rotate-neg-y",1,-1,true); ImGui::SameLine();
            action("Z -角度","creation.movement-rotate-neg-z",2,-1,true);
            ImGui::EndDisabled(); ImGui::EndDisabled();
            if (!creationMovementMessage.empty()) ImGui::TextWrapped("%s",creationMovementMessage.c_str());
            if (!validCreationMovement()) ImGui::TextUnformatted("原标签或体系已改变，请关闭并重新打开");
            ImGui::Separator();
            if (ImGui::Button("关闭",{U(100),0}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            recordUiTestItem("creation.movement-close");
            ImGui::EndPopup();
        }
        if (openCreationPosition) {
            ImGui::OpenPopup("编辑原子坐标"); openCreationPosition=false;
        }
        if (ImGui::BeginPopupModal("编辑原子坐标",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool validDocument=creationMode && activeTab>=0 && activeTab<int(tabs.size()) &&
                tabs[size_t(activeTab)].id==creationPositionTabId && creationPositionIndex>=0 &&
                size_t(creationPositionIndex)<source.atoms.size();
            ImGui::Text("Atom #%d",creationPositionIndex);
            double a=0,b=0,c=0;
            const bool hasCell=validDocument && authoring::fractional(source,{},a,b,c);
            ImGui::BeginDisabled(!hasCell);
            if (ImGui::Checkbox("分数坐标",&creationPositionFractional)) {
                const Vec3 draft{creationPositionDraft[0],creationPositionDraft[1],creationPositionDraft[2]};
                Vec3 converted;
                if (creationPositionFractional) {
                    authoring::fractional(source,draft,a,b,c);
                    converted={float(a),float(b),float(c)};
                } else converted=authoring::cartesian(source,draft.x,draft.y,draft.z);
                creationPositionDraft[0]=converted.x; creationPositionDraft[1]=converted.y;
                creationPositionDraft[2]=converted.z;
            }
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(U(340));
            ImGui::InputFloat3(creationPositionFractional?"a / b / c":"X / Y / Z (Å)",
                creationPositionDraft,"%.6f");
            recordUiTestItem("creation.position-values");
            ImGui::TextDisabled("应用后记录一步历史 · 撤销可恢复");
            const Vec3 position{creationPositionDraft[0],creationPositionDraft[1],creationPositionDraft[2]};
            const bool finite=std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z);
            if (!finite) ImGui::TextUnformatted("请输入有限数值");
            const Vec3 cart=creationPositionFractional?
                authoring::cartesian(source,position.x,position.y,position.z):position;
            const bool finiteCartesian=std::isfinite(cart.x)&&std::isfinite(cart.y)&&std::isfinite(cart.z);
            ImGui::BeginDisabled(!validDocument || !finite || !finiteCartesian || documentsBusy());
            if (ImGui::Button("应用",{U(100),0})) {
                const int index=creationPositionIndex;
                editStructure("编辑原子坐标",[&](Dataset &data) {
                    authoring::setAtomPosition(data,index,position,creationPositionFractional);
                });
                creationPick=index;
                ImGui::CloseCurrentPopup();
            }
            recordUiTestItem("creation.position-apply");
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("取消",{U(100),0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openCrystalDialog) {
            if (crystalBasis.empty()) configureCrystalPreset(crystalPreset);
            crystalReplaceCurrent=creationMode;
            ImGui::OpenPopup("建晶体##v2");
            openCrystalDialog=false;
        }
        ImGui::SetNextWindowSize({U(580),U(580)},ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("建晶体##v2",nullptr,ImGuiWindowFlags_NoSavedSettings)) {
            const char *presets="Cu · FCC\0NaCl · 岩盐\0Si · 金刚石\0Fe · BCC\0Mg · HCP\0自定义 P1\0按空间群构建\0";
            int preset=crystalPreset;
            if (ImGui::Combo("结构模板",&preset,presets)) configureCrystalPreset(preset);
            if (crystalPreset==6) {
                ImGui::InputInt("空间群编号 (1-230)",&crystalSpaceGroup);
                const auto setting=symmetry::defaultSetting(crystalSpaceGroup);
                if (setting) ImGui::TextDisabled("标准设置：%s · Hall #%d",
                    setting->symbol.c_str(),setting->hall);
                else ImGui::TextColored({1.f,.52f,.35f,1.f},"空间群编号须在 1 到 230 之间");
            }
            const bool cubic=crystalPreset<=3,hexagonal=crystalPreset==4;
            ImGui::TextDisabled("晶格参数（Å）");
            if (ImGui::InputFloat("a",&crystalA,.1f,1.f,"%.4f") && cubic)
                crystalB=crystalC=crystalA;
            if (cubic) ImGui::BeginDisabled();
            ImGui::InputFloat("b",&crystalB,.1f,1.f,"%.4f");
            if (cubic) ImGui::EndDisabled();
            if (cubic) ImGui::BeginDisabled();
            ImGui::InputFloat("c",&crystalC,.1f,1.f,"%.4f");
            if (cubic) ImGui::EndDisabled();
            if (cubic||hexagonal) ImGui::BeginDisabled();
            ImGui::InputFloat("α",&crystalAlpha,1.f,5.f,"%.2f");
            ImGui::InputFloat("β",&crystalBeta,1.f,5.f,"%.2f");
            if (cubic||hexagonal) ImGui::EndDisabled();
            if (cubic||hexagonal) ImGui::BeginDisabled();
            ImGui::InputFloat("γ",&crystalGamma,1.f,5.f,"%.2f");
            if (cubic||hexagonal) ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextUnformatted("基元原子（分数坐标）");
            int remove=-1;
            for (size_t index=0;index<crystalBasis.size();++index) {
                auto &atom=crystalBasis[index];
                ImGui::PushID(int(index));
                ImGui::SetNextItemWidth(U(70));
                ImGui::InputText("##element",atom.element,sizeof(atom.element));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(U(300));
                ImGui::InputFloat3("##fractional",atom.fractional,"%.4f");
                ImGui::SameLine();
                if (ImGui::SmallButton("×")) remove=int(index);
                ImGui::PopID();
            }
            if (remove>=0) crystalBasis.erase(crystalBasis.begin()+remove);
            if (ImGui::Button("+ 添加原子") && crystalBasis.size()<32)
                crystalBasis.push_back(CrystalBasis{});
            if (creationMode) ImGui::Checkbox("替换当前创作结构（否则新标签页）",&crystalReplaceCurrent);
            const bool valid=authoring::validCellParameters(crystalA,crystalB,crystalC,
                crystalAlpha,crystalBeta,crystalGamma)&&!crystalBasis.empty()&&
                (crystalPreset!=6 || (crystalSpaceGroup>=1&&crystalSpaceGroup<=230));
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("生成晶胞",{U(110),U(36)})) {
                try {
                    Dataset data=crystalFromDialog();
                    if (creationMode && crystalReplaceCurrent) adoptStructure(std::move(data),"已生成晶胞");
                    else {
                        newStructureTab(std::move(data),"晶体 · 创作");
                        creationMode=true;
                        chooseCreationTool(CreationTool::Select);
                        cameras[3].mode=7; active=3;
                        saveCreationSnapshot("晶体初始");
                    }
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception &e) { status=e.what(); }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("取消",{U(80),U(36)})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openSupercellDialog) { ImGui::OpenPopup("超胞##v2"); openSupercellDialog=false; }
        if (ImGui::BeginPopupModal("超胞##v2",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("沿晶胞 a / b / c 方向重复");
            ImGui::InputInt3("重复次数",supercellFactor);
            const uint64_t count=uint64_t(source.atoms.size())*
                uint64_t(std::max(supercellFactor[0],0))*uint64_t(std::max(supercellFactor[1],0))*
                uint64_t(std::max(supercellFactor[2],0));
            ImGui::Text("预览：%llu 原子",static_cast<unsigned long long>(count));
            const bool valid=creationMode && count<=60000 &&
                std::all_of(std::begin(supercellFactor),std::end(supercellFactor),[](int n){return n>=1&&n<=12;});
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("扩展",{U(95),U(35)})) {
                adoptStructure(authoring::replicate(source,supercellFactor[0],supercellFactor[1],supercellFactor[2]),
                    "已扩展超胞");
                fitCamera(3,false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("取消",{U(80),U(35)})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openSurfaceDialog) { ImGui::OpenPopup("切面·真空##v2"); openSurfaceDialog=false; }
        if (ImGui::BeginPopupModal("切面·真空##v2",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputInt3("Miller 指数 (h k l)",surfaceMiller);
            ImGui::InputInt("层数",&surfaceLayers);
            ImGui::InputFloat("真空厚度 (Å)",&surfaceVacuum,1.f,5.f,"%.2f");
            const uint64_t count=uint64_t(source.atoms.size())*uint64_t(std::max(surfaceLayers,0));
            ImGui::Text("预览：%llu 原子 · 真空 %.2f Å",static_cast<unsigned long long>(count),surfaceVacuum);
            ImGui::TextDisabled("在 (hkl) 面内重排晶胞；沿法向添加真空，关闭法向周期边界。");
            const bool valid=creationMode && surfaceLayers>=1&&surfaceLayers<=12&&
                std::isfinite(surfaceVacuum)&&surfaceVacuum>=0&&count<=60000&&
                (surfaceMiller[0]||surfaceMiller[1]||surfaceMiller[2])&&
                std::all_of(std::begin(surfaceMiller),std::end(surfaceMiller),
                    [](int n){return n>=-6&&n<=6;})&&
                std::all_of(source.pbc.begin(),source.pbc.end(),[](bool periodic){return periodic;});
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("生成切面",{U(110),U(35)})) {
                auto slab=authoring::millerSurface(source,surfaceMiller[0],surfaceMiller[1],
                    surfaceMiller[2],surfaceLayers,surfaceVacuum);
                if (slab) {
                    adoptStructure(std::move(*slab),"已生成 Miller 切面和真空层");
                    fitCamera(3,false);
                    ImGui::CloseCurrentPopup();
                } else {
                    status="无法生成该切面；请检查晶胞与 Miller 指数";
                }
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("取消",{U(80),U(35)})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void creationWorkspace(float w, float h) {
        const float leftW = U(240), rightW = U(275);
        const float y = topInset(), bodyH = h - y - statusHeight();
        std::vector<size_t> counts(source.species.size());
        for (const auto &atom : source.atoms)
            if (atom.type < counts.size()) ++counts[atom.type];
        size_t common = 0;
        for (size_t count : counts) if (count) common = std::gcd(common, count);
        std::string formula;
        for (size_t i = 0; i < counts.size(); ++i) if (counts[i]) {
            formula += source.species[i];
            const size_t unit = counts[i] / std::max<size_t>(common, 1);
            if (unit > 1) formula += std::to_string(unit);
        }
        const float footerH=U(32);
        const float propertyH=creationPropertiesOpen
            ? std::max(0.f,std::min(std::max(bodyH*.43f,U(220)),bodyH-U(170))) : 0.f;
        const float workH=std::max(U(80),bodyH-propertyH-footerH);
        fixed("Creation snapshots",0,y,leftW,workH);
        ImGui::TextDisabled("工作区");
        ImGui::SameLine(leftW-U(68));
        ImGui::PushStyleColor(ImGuiCol_Button,{.25f,.19f,.11f,1});
        ImGui::PushStyleColor(ImGuiCol_Text,{1.f,.69f,.23f,1});
        if (ImGui::Button("暂存",{U(50),U(26)})) saveCreationSnapshot();
        ImGui::PopStyleColor(2);
        ImGui::Separator();
        if(ImGui::CollapsingHeader("文档")) {
            if(ImGui::SmallButton("按名称排序")) {
                tabs[size_t(activeTab)]=captureTab();const auto selectedId=tabs[size_t(activeTab)].id;
                std::stable_sort(tabs.begin(),tabs.end(),[](const auto &a,const auto &b){return a.title<b.title;});
                for(size_t index=0;index<tabs.size();++index)if(tabs[index].id==selectedId)activeTab=int(index);
            }
            ImGui::SameLine();if(ImGui::SmallButton("打开…"))open();
            ImGui::SameLine();if(ImGui::SmallButton("刷新")){update();status="已刷新当前体系显示";}
            for(int index=0;index<int(tabs.size());++index) {ImGui::PushID(index);
                if(ImGui::Selectable(tabs[size_t(index)].title.c_str(),index==activeTab))switchTab(index);
                ImGui::PopID();}
            if(ImGui::SmallButton("保存工作区…"))projectDialog(true);
        }
        ImGui::Text("%s",creationBasedOn.empty()?"未命名结构":creationBasedOn.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu 快照",creationSnapshots.size());
        ImGui::Spacing();
        const auto current=ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled({current.x+U(7),current.y+U(9)},U(5),IM_COL32(65,227,127,255));
        ImGui::Dummy({U(18),U(18)}); ImGui::SameLine();
        ImGui::TextUnformatted("当前编辑");
        ImGui::Indent(U(22));
        ImGui::TextDisabled("%s · %s 原子 · 第 %zu 步",formula.empty()?"—":formula.c_str(),
            number(source.atoms.size()).c_str(),authorUndo.size()+1);
        ImGui::Unindent(U(22));
        ImGui::Spacing();
        ImGui::TextDisabled("暂存的结果  SNAPSHOTS");
        int restoreIndex=-1,removeIndex=-1;
        for (int index=0;index<int(creationSnapshots.size());++index) {
            ImGui::PushID(index);
            const auto &snapshot=creationSnapshots[size_t(index)];
            if (ImGui::Selectable(snapshot.name.c_str(),creationSnapshotSelected==index,0,{leftW-U(58),U(27)}))
                restoreIndex=index;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("载入 %s",snapshot.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeIndex=index;
            ImGui::TextDisabled("%s · %s 原子",snapshot.time.c_str(),
                snapshot.data?number(snapshot.data->atoms.size()).c_str():"0");
            ImGui::PopID();
        }
        ImGui::End();
        if (removeIndex>=0) {
            creationSnapshots.erase(creationSnapshots.begin()+removeIndex);
            if (creationSnapshotSelected==removeIndex) creationSnapshotSelected=-1;
            else if (creationSnapshotSelected>removeIndex) --creationSnapshotSelected;
        } else if (restoreIndex>=0) restoreCreationSnapshot(restoreIndex);

        if (creationPropertiesOpen) {
            fixed("Creation file properties",0,y+workH,leftW,propertyH);
            ImGui::TextDisabled("体系属性");
            creationPropertyPage=segmented("##creation-property-pages",creationPropertyPage,{"晶格","组成","选中","测量"},ImGui::GetContentRegionAvail().x);
            ImGui::Separator();
            auto propertyRow=[&](const char *name,const std::string &value) {
                ImGui::TextDisabled("%s",name);
                ImGui::SameLine();
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                    leftW-ImGui::CalcTextSize(value.c_str()).x-U(24)));
                ImGui::TextUnformatted(value.c_str());
                ImGui::Separator();
            };
            if (creationPropertyPage==0) {
                const auto lattice=authoring::latticeOf(source);
                if (!creationSymmetryChecked && symmetry::prepared(source)) {
                    creationSymmetry=symmetry::analyze(source);
                    creationSymmetryChecked=true;
                }
                if (ImGui::Button("编辑晶胞参数...",{-1,U(28)})) {
                    creationCellParameters={lattice.a>1e-5?lattice.a:10.0,
                        lattice.b>1e-5?lattice.b:10.0,lattice.c>1e-5?lattice.c:10.0,
                        lattice.alpha,lattice.beta,lattice.gamma};
                    creationCellOrigin[0]=source.origin.x;
                    creationCellOrigin[1]=source.origin.y;
                    creationCellOrigin[2]=source.origin.z;
                    creationCellPbc=source.pbc;
                    creationCellPreserveFractional=false;
                    ImGui::OpenPopup("编辑模拟晶胞");
                }
                char value[80];
                for (const auto [name,length]:{std::pair{"a",lattice.a},
                    {"b",lattice.b},{"c",lattice.c}}) {
                    snprintf(value,sizeof(value),"%.4f Å",length); propertyRow(name,value);
                }
                for (const auto [name,angle]:{std::pair{"α",lattice.alpha},
                    {"β",lattice.beta},{"γ",lattice.gamma}}) {
                    snprintf(value,sizeof(value),"%.2f°",angle); propertyRow(name,value);
                }
                snprintf(value,sizeof(value),"%.2f Å³",lattice.volume); propertyRow("体积 V",value);
                std::string group="未计算",bravais="未计算";
                if (creationSymmetry) {
                    group=creationSymmetry->symbol+" ("+
                        std::to_string(creationSymmetry->number)+")";
                    const int number=creationSymmetry->number;
                    const char *system=number<=2?"三斜":number<=15?"单斜":
                        number<=74?"正交":number<=142?"四方":number<=167?"三方":
                        number<=194?"六方":"立方";
                    const char code=creationSymmetry->symbol.empty()? '?':creationSymmetry->symbol[0];
                    bravais=std::string(system)+" "+code;
                } else if (source.atoms.size()>symmetry::interactiveAtomLimit)
                    group="原子过多；请先截取晶胞";
                else if (!source.pbc[0] || !source.pbc[1] || !source.pbc[2])
                    group="需三维周期晶胞";
                else if (creationSymmetryChecked)
                    group="识别失败";
                propertyRow("空间群",group);
                propertyRow("布拉维格子",bravais);
                if (creationSymmetry) {
                    propertyRow("对称操作",std::to_string(creationSymmetry->operations));
                    propertyRow("原胞原子数",std::to_string(creationSymmetry->primitiveAtoms));
                    if (creationSymmetry->primitiveAtoms>0 &&
                        size_t(creationSymmetry->primitiveAtoms)<source.atoms.size()) {
                        if (ImGui::Button("转换为原胞",{-1,U(28)})) {
                            auto primitive=symmetry::primitive(source);
                            if (primitive && primitive->atoms.size()<source.atoms.size()) {
                                adoptStructure(std::move(*primitive),"已转换为 primitive cell");
                                fitCamera(3,false);
                            } else status="原胞转换失败；检查晶胞与周期性";
                        }
                    }
                }
                propertyRow("原子数",std::to_string(source.atoms.size()));
                snprintf(value,sizeof(value),"%.4f Å⁻³",lattice.volume>0
                    ? double(source.atoms.size())/lattice.volume:0.0);
                propertyRow("密度",value);
            } else if (creationPropertyPage==1) {
                ImGui::TextDisabled("点击元素可选中同种原子");
                for (size_t type=0;type<source.species.size();++type) {
                    if (!counts[type]) continue;
                    ImGui::PushID(int(type));
                    const auto color=typeColor(type);
                    ImGui::ColorButton("##type",{color[0],color[1],color[2],1},0,{U(14),U(14)});
                    ImGui::SameLine();
                    const std::string row=source.species[type]+"    "+std::to_string(counts[type]);
                    if (ImGui::Selectable(row.c_str())) {
                        creationSelection.clear();creationBondSelection.clear();
                        for (size_t i=0;i<source.atoms.size();++i)
                            if (source.atoms[i].type==type && creationAtomVisible(int(i))) creationSelection.push_back(int(i));
                        creationPick=creationSelection.empty()?-1:creationSelection.back();
                    }
                    ImGui::PopID();
                }
            } else if(creationPropertyPage==2) {
                propertyRow("已选中",std::to_string(creationSelection.size()));
                if (creationPick>=0 && size_t(creationPick)<source.atoms.size()) {
                    const auto &atom=source.atoms[size_t(creationPick)];
                    propertyRow("元素",atom.type<source.species.size()?source.species[atom.type]:"?");
                    char position[80];
                    snprintf(position,sizeof(position),"%.3f, %.3f, %.3f",atom.x,atom.y,atom.z);
                    propertyRow("位置",position);
                }
            }
            if(creationPropertyPage==3) {
                ImGui::SetNextItemWidth(-1);ImGui::Combo("##measurement-filter",&creationMonitorFilter,"全部测量\0距离\0角度\0二面角\0");
                recordUiTestItem("creation.properties-measurement-filter");
                ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##property-filter","按元素或编号筛选…",creationPropertyFilter,sizeof(creationPropertyFilter));
                ImGui::TextDisabled("点击对象，在右侧修改几何或约束");
                for(size_t row=0;row<creationDisplay.monitors.size();++row) {
                    const auto &m=creationDisplay.monitors[row];if(creationMonitorFilter && m.count!=creationMonitorFilter+1)continue;
                    std::string name;
                    for(int k=0;k<m.count;++k){if(k)name+="–";const int index=m.atoms[size_t(k)];
                        if(index>=0 && size_t(index)<source.atoms.size()) {
                            const auto type=source.atoms[size_t(index)].type;name+=(type<source.species.size()?source.species[type]:"?")+std::string("#")+std::to_string(index);
                        }}
                    if(creationPropertyFilter[0] && name.find(creationPropertyFilter)==std::string::npos)continue;
                    const auto measured=geometry::value(result.data,m);char value[80];
                    if(measured)snprintf(value,sizeof(value),"%.5f %s",*measured,m.count==2?"Å":"°");else snprintf(value,sizeof(value),"未定义");
                    ImGui::PushID(int(row));
                    if(ImGui::Selectable(name.c_str(),creationDisplay.activeMonitor==int(row)))activateGeometry(int(row));
                    recordUiTestItem("creation.properties-measurement-"+std::to_string(row));
                    propertyRow(geometryName(m.count),value);propertyRow("约束",m.fixed?"固定":"自由");
                    ImGui::PopID();
                }
                if(creationDisplay.monitors.empty())ImGui::TextDisabled("用尺子工具点击原子创建测量。");
            }
            if (ImGui::BeginPopupModal("编辑模拟晶胞",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("晶格常数 (Å)");
            for (int axis=0;axis<3;++axis) {
                ImGui::PushID(axis);
                ImGui::SetNextItemWidth(U(120));
                ImGui::InputDouble(axis==0?"a":axis==1?"b":"c",
                    &creationCellParameters[size_t(axis)],0,0,"%.6f");
                ImGui::PopID();
            }
            ImGui::TextUnformatted("晶格角 (°)");
            for (int axis=0;axis<3;++axis) {
                ImGui::PushID(axis+3);
                ImGui::SetNextItemWidth(U(120));
                ImGui::InputDouble(axis==0?"α":axis==1?"β":"γ",
                    &creationCellParameters[size_t(axis+3)],0,0,"%.4f");
                ImGui::PopID();
            }
            ImGui::TextUnformatted("晶胞原点 (Å)");
            for (int axis=0;axis<3;++axis) {
                if (axis) ImGui::SameLine();
                ImGui::PushID(axis+6);
                ImGui::SetNextItemWidth(U(90));
                ImGui::InputDouble(axis==0?"X":axis==1?"Y":"Z",
                    &creationCellOrigin[axis],0,0,"%.4f");
                ImGui::PopID();
            }
            ImGui::TextUnformatted("周期性边界");
            for (int axis=0;axis<3;++axis) {
                if (axis) ImGui::SameLine();
                ImGui::PushID(axis+9);
                ImGui::Checkbox(axis==0?"X":axis==1?"Y":"Z",
                    &creationCellPbc[size_t(axis)]);
                ImGui::PopID();
            }
            ImGui::Checkbox("保持原子的分数坐标",&creationCellPreserveFractional);
            ImGui::TextDisabled("关闭时保持笛卡尔坐标；开启时原子随晶胞变形。");
            const auto &p=creationCellParameters;
            bool valid=authoring::validCellParameters(p[0],p[1],p[2],p[3],p[4],p[5]);
            for (double coordinate : creationCellOrigin)
                valid &= std::isfinite(coordinate) && std::abs(coordinate)<1e7;
            if (creationCellPreserveFractional && authoring::latticeOf(source).volume<1e-8)
                valid=false;
            if (!valid) ImGui::TextColored({1.f,.52f,.35f,1.f},"晶胞尺寸或角度无效；分数坐标需要有效的原晶胞。");
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("应用",{U(90),U(32)})) {
                const Vec3 origin{float(creationCellOrigin[0]),float(creationCellOrigin[1]),
                    float(creationCellOrigin[2])};
                const auto periodic=creationCellPbc;
                const bool preserve=creationCellPreserveFractional;
                editStructure("编辑模拟晶胞",[&](Dataset &data) {
                    authoring::setCellParameters(data,p[0],p[1],p[2],p[3],p[4],p[5],
                        origin,periodic,preserve);
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("取消",{U(90),U(32)})) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::End();
        }
        fixed("Creation properties toggle",0,y+workH+propertyH,leftW,footerH);
        if (ImGui::Button(creationPropertiesOpen?"属性面板    收起":"属性面板    展开",{-1,U(25)}))
            creationPropertiesOpen=!creationPropertiesOpen;
        recordUiTestItem("creation.properties-toggle","Creation file properties toggle");
        ImGui::End();

        fixed("Creation canvas", leftW, y, w - leftW - rightW, bodyH);
        ImGui::SetCursorPos({0,0});
        viewport(3, w - leftW - rightW, bodyH);
        ImGui::End();

        fixed("Creation selection", w - rightW, y, rightW, bodyH);
        geometryPanel();
        ImGui::Checkbox("自动更新氢",&creationAutoHydrogens);recordUiTestItem("creation.auto-hydrogen");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("提交绘制、换元素、改键级、接环或删除时调整相邻位点；开关本身不改体系。\n默认关闭；大体系可用 H+ 后台预览。");
        if(!creationAutoHydrogenMessage.empty())ImGui::TextWrapped("%s",creationAutoHydrogenMessage.c_str());
        if (creationTool==CreationTool::Fragment) {
            ImGui::SeparatorText("放置片段");
            if (const auto *t=currentFragment()) ImGui::TextWrapped("%s",t->name.c_str());
            if (ImGui::Button("片段浏览器...")) { requestFragmentLibrary(); showFragmentBrowser=true; }
            recordUiTestItem("creation.fragment-browser");
            ImGui::TextDisabled("空白处放置 · 末端原子接枝");
            ImGui::TextDisabled("按住拖动调整朝向 · Esc 取消");
            ImGui::TextDisabled("末端氢会被替换 · 其他原子保留");
            ImGui::BeginDisabled(documentsBusy());
            if (ImGui::Button("连接已有片段...")) {
                clearCreationFusion(); creationFusionPick=true; creationFragmentPreviewValid=false;
                status="点击移动片段的末端原子，再点击目标片段末端";
            }
            recordUiTestItem("creation.fragment-fuse");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Alt 点击也可指定移动片段连接点");
            if (creationFusionSeed || creationFusionPick) {
                ImGui::TextWrapped("%s",creationFusionSeed?"已指定移动片段；点击另一片段的末端":"请选择移动片段的末端原子");
                if (creationFusionSeed) ImGui::TextDisabled("移动 %zu 个原子",creationFusionSeed->indices.size());
                if (ImGui::Button("取消连接")) clearCreationFusion();
                recordUiTestItem("creation.fragment-fuse-cancel");
            }
            ImGui::Separator();
        }
        if (creationTool==CreationTool::Ring) {
            ImGui::SeparatorText("绘制碳环");
            ImGui::TextDisabled("环大小"); ImGui::SameLine();
            for (int size=4;size<=6;++size) {
                const auto caption=std::to_string(size)+" 元";
                if (ImGui::RadioButton(caption.c_str(),creationRingSize==size)) {
                    creationRingSize=size; creationRingPreviewValid=false;
                }
                recordUiTestItem("creation.ring-size-"+std::to_string(size));
                if (size<6) ImGui::SameLine();
            }
            ImGui::TextDisabled("空白处放置 · 原子或键上接环");
            ImGui::TextDisabled("按住拖动调整朝向 · 松开提交");
            ImGui::TextDisabled("Alt 点击芳香环 · Esc 取消");
            ImGui::TextDisabled(creationAutoHydrogens?"自动氢：开启":"自动氢：关闭");
            ImGui::Separator();
        }
        if (creationTool==CreationTool::Sketch) {
            ImGui::SeparatorText("绘制原子与键");
            ImGui::SetNextItemWidth(U(75));
            ImGui::InputText("元素##sketch",creationElement,sizeof(creationElement));
            recordUiTestItem("creation.sketch-element");
            ImGui::SameLine(); ImGui::SetNextItemWidth(U(75));
            int order=creationSketchOrder-1;
            if (ImGui::Combo("键级##sketch",&order,"单键\0双键\0三键\0")) creationSketchOrder=order+1;
            recordUiTestItem("creation.sketch-order");
            ImGui::Checkbox("连续成链",&creationSketchContinuous);
            recordUiTestItem("creation.sketch-continuous");
            if(ImGui::Checkbox("点击键中插入原子",&creationSketchInsert)) {
                creationSketchAnchor=creationSketchLastPlaced=-1;creationSketchPreviewValid=false;
                creationInsertHover=-1;creationInsertMouse={-1,-1};
            }
            recordUiTestItem("creation.sketch-insert");
            ImGui::TextDisabled("插入中点 · 两条新键采用当前键级");
            ImGui::TextDisabled("原端点固定 · 可用测量工具调整键长 / 角度");
            ImGui::TextDisabled("点击已有原子吸附连键");
            ImGui::TextDisabled("双击结束 · Esc 取消虚拟原子");
            ImGui::TextDisabled("Alt 点击替换元素 / 放置孤立原子");
            ImGui::TextDisabled("Alt 点击键插入 · Esc 退出插入模式");
            ImGui::TextDisabled("Alt 拖动向里 · Shift + Alt 自由键长");
            if (creationSketchAnchor>=0) {
                ImGui::Text("起点 #%d · 默认键长 %.2f Å",creationSketchAnchor,
                    authoring::sketchBondLength(source,creationSketchAnchor,creationElement));
                if (ImGui::Button("结束成链",{-1,U(28)})) {
                    creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
                }
                recordUiTestItem("creation.sketch-finish");
            }
            ImGui::Separator();
        }
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("选中");
        if (headingFont) ImGui::PopFont();
        if (creationSelection.empty() && creationBondSelection.empty()) {
            ImGui::TextDisabled("点击原子或键选择");
            ImGui::TextDisabled("Shift + 点击 多选 · 拖动空白处 框选");
            ImGui::TextDisabled("中键拖动 旋转 · 滚轮 缩放");
            ImGui::TextDisabled("右键 更多操作");
        }
        ImGui::Separator();
        if(!creationBondSelection.empty()) {
            if(ImGui::Button("选中键所在连接片段",{-1,U(28)}))selectCreationFragments();
            recordUiTestItem("creation.selected-bond-fragments");
        }
        if(!creationBondSelection.empty()) {
            ImGui::Text("已选中 %zu 条键",creationBondSelection.size());
            if(ImGui::Button("精准移动 / 旋转键端点...",{-1,U(28)}))requestCreationMovement();
            recordUiTestItem("creation.selected-bond-movement");
            const int row=creationBondSelection.back();
            if(row>=0 && size_t(row)<source.bonds.size()) {
                const auto &b=source.bonds[size_t(row)];const auto vector=bondVector(source,b);
                ImGui::Text("键 #%d · #%u — #%u",row,b.a,b.b);
                ImGui::Text("键长 %.4f Å",std::hypot(vector[0],vector[1],vector[2]));
                ImGui::Text("键级 %s",b.order==4?"1.5 (芳香)":std::to_string(b.order).c_str());
                if(b.image!=std::array<int32_t,3>{})ImGui::TextDisabled("周期像 (%d, %d, %d)",b.image[0],b.image[1],b.image[2]);
                int order=1;
                for(const char *caption:{"单键","双键","三键","芳香"}) {
                    if(ImGui::Button(caption,{U(56),U(28)}))editSelectedCreationBonds(order);
                    recordUiTestItem("creation.selected-bond-order-"+std::to_string(order));
                    if(++order<5)ImGui::SameLine();
                }
                if(ImGui::Button("断开选中键",{-1,U(28)}))editSelectedCreationBonds(0);
                recordUiTestItem("creation.selected-bond-break");
                // Bond may have been removed by the button above this frame.
                if(creationBondSelection.size()==1 && size_t(row)<source.bonds.size() && source.bonds[size_t(row)].image==std::array<int32_t,3>{}) {
                    if(ImGui::Button("测量 / 修改此键长...",{-1,U(28)})) {
                        const auto edge=source.bonds[size_t(row)];
                        addGeometryMonitor({2,{int32_t(edge.a),int32_t(edge.b),-1,-1}});
                        chooseCreationTool(CreationTool::Distance);
                    }
                    recordUiTestItem("creation.selected-bond-measure");
                }
            }
            ImGui::Separator();
        }
        if (creationSelection.size()==2 && creationBondSelection.empty()) {
            ImGui::TextDisabled("连接选中原子 / 修改键级");
            int order=0;
            for (const char *label:{"断键","单键","双键","三键","芳香"}) {
                ImGui::PushID(order);
                if (ImGui::Button(label,{U(58),U(28)}))
                    editCreationBond(creationSelection[0],creationSelection[1],order);
                recordUiTestItem("creation.bond-order-"+std::to_string(order));
                ImGui::PopID(); if (++order<4) ImGui::SameLine();
            }
        }
        if (creationSelection.size()>1) {
            ImGui::Text("已选中 %zu 个原子",creationSelection.size());
            ImGui::TextDisabled("Shift + 点击继续选择 · 拖动空白处框选");
            ImGui::Separator();
            ImGui::TextDisabled("替换元素");
            int elementIndex=0;
            for (const char *element : {"H","C","N","O","Si","Fe","Cu","Ni"}) {
                if (ImGui::Button(element,{U(42),U(30)})) {
                    snprintf(creationElement,sizeof(creationElement),"%s",element);
                    replacePickedElement();
                }
                if (++elementIndex%4) ImGui::SameLine();
            }
            if (ImGui::Button("删除 · 制造空位",{-1,U(34)})) deletePickedAtom();
        } else if (creationPick >= 0 && size_t(creationPick) < source.atoms.size()) {
            const Atom atom = source.atoms[size_t(creationPick)];
            const char *symbol = atom.type < source.species.size()
                ? source.species[atom.type].c_str() : "?";
            colorSwatch(typeColor(atom.type));
            ImGui::SameLine();
            ImGui::Text("%s  Atom #%d", symbol, creationPick);
            ImGui::Text("Position (Å)  %.4f  %.4f  %.4f",atom.x,atom.y,atom.z);
            double fa=0,fb=0,fc=0;
            if (authoring::fractional(source,{atom.x,atom.y,atom.z},fa,fb,fc))
                ImGui::TextDisabled("Fractional  %.4f  %.4f  %.4f",fa,fb,fc);
            if (ImGui::Button("编辑坐标...",{-1,U(28)})) requestCreationPosition();
            recordUiTestItem("creation.edit-position");
            ImGui::TextDisabled("Replace element");
            int elementIndex=0;
            for (const char *element : {"H", "C", "N", "O", "Si", "Fe", "Cu", "Ni"}) {
                if (ImGui::Button(element, {U(37), U(29)})) {
                    snprintf(creationElement, sizeof(creationElement), "%s", element);
                    replacePickedElement();
                }
                if (++elementIndex%4) ImGui::SameLine();
            }
            ImGui::Spacing();
            if (ImGui::Button("Delete / make vacancy", {-1, U(34)})) deletePickedAtom();
        }
        if (!creationSelection.empty()) {
            if(ImGui::Button("原子数值属性...",{-1,U(30)}))requestAtomProperties();
            recordUiTestItem("creation.edit-properties");
            if(ImGui::Button("原子模拟约束...",{-1,U(30)}))requestAtomConstraints();
            recordUiTestItem("creation.edit-constraints");
            if(ImGui::Button("原子化学设置...",{-1,U(30)}))requestCreationChemistry();
            recordUiTestItem("creation.edit-chemistry");
            if (ImGui::Button("精准移动 / 旋转...",{-1,U(32)})) requestCreationMovement();
            recordUiTestItem("creation.edit-movement");
        }
        if (!creationSelection.empty() || !creationBondSelection.empty()) {
            const bool selectedBonds=!creationBondSelection.empty();
            const float width=selectedBonds?-1:rightW*.43f;
            if (ImGui::Button("隐藏选中",{width,U(28)})) creationVisibility(0);
            recordUiTestItem("creation.hide-selected");if(!selectedBonds)ImGui::SameLine();
            if (ImGui::Button(selectedBonds?"仅显示键及端点":"仅显示选中",{width,U(28)})) creationVisibility(1);
            recordUiTestItem("creation.show-only");
        }
        if (creationDisplay.hiddenCount || creationDisplay.bondVisibility.hiddenCount) {
            ImGui::TextDisabled("已隐藏 %zu 原子 · %zu 键",creationDisplay.hiddenCount,creationDisplay.bondVisibility.hiddenCount);
            if (ImGui::Button("显示全部",{-1,U(28)})) creationVisibility(2);
            recordUiTestItem("creation.show-all");
        }
        if (ImGui::Button("原子标签...",{-1,U(28)})) requestCreationLabels();
        recordUiTestItem("creation.edit-labels");
        if(ImGui::Button("键标签...",{-1,U(28)}))requestCreationBondLabels();
        recordUiTestItem("creation.edit-bond-labels");
        if (ImGui::Button("显示样式...",{-1,U(28)})) requestCreationStyles();
        recordUiTestItem("creation.edit-styles");
        if (ImGui::Button("运动分组...",{-1,U(28)})) requestMotionGroups();
        recordUiTestItem("creation.edit-motion-groups");
        ImGui::Separator();
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("历史");
        if (headingFont) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("点击回到该步");
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled({row.x,row.y+U(2)},
            {row.x+U(3),row.y+U(23)},IM_COL32(32,163,241,255));
        ImGui::Indent(U(13));
        const std::string origin = "从 " + (creationBasedOn.empty() ? std::string("新结构") : creationBasedOn);
        if (ImGui::Selectable(origin.c_str(),authorUndo.empty(),0,{rightW-U(75),U(27)}))
            jumpCreationHistory(0);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu",source.atoms.size());
        ImGui::Unindent(U(13));
        const size_t historyCount=authorUndo.size()+authorRedo.size();
        for (size_t step=1;step<=historyCount;++step) {
            const auto &entry=step<=authorUndo.size()?authorUndo[step-1]:authorRedo[historyCount-step];
            const std::string label=std::to_string(step)+"  "+(entry.action.empty()?"编辑结构":entry.action);
            if (ImGui::Selectable(label.c_str(),step==authorUndo.size()))
                jumpCreationHistory(step);
        }
        if (!authorUndo.empty() || !authorRedo.empty()) {
            if (ImGui::Button("撤销", {U(80), U(30)})) history(false);
            ImGui::SameLine();
            if (ImGui::Button("重做", {U(80), U(30)})) history(true);
        }
        ImGui::End();
    }
    void right(float w, float h) {
        fixed("Properties", w - rightWidth(), topInset(), rightWidth(),
              h - topInset() - statusHeight());
        // Tighter than the global theme so checkboxes, combos and sliders hug
        // their labels. release() runs before End(): End() asserts
        // "Missing PopStyleVar()" (and blocks startup) if these are still pushed.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(5), U(2)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(4), U(3)});
        ImGuiStyleVarGuard panelStyle(2);
        // OVITO layout: the compact icon row IS the section switcher; the
        // former Pipeline/Render/Analysis/System text tab bar is gone. The
        // actions of the old icon buttons here are covered elsewhere (open and
        // snapshot live in the toolbar, particle visibility in Scene
        // visibility), so nothing is lost by retiring them.
        if (selectPipeline) {
            rightTab = 0;
            selectPipeline = false;
        }
        if (selectAnalysis) {
            rightTab = 2;
            selectAnalysis = false;
        }
        rightTab = std::clamp(rightTab, 0, 3);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(6), U(4)});
        if (iconButton("panel.tab.pipeline", 0xE71D, "Pipeline",
                       "Pipeline", rightTab == 0))
            rightTab = 0;
        ImGui::SameLine();
        if (iconButton("panel.tab.render", 0xE722, "Render",
                       "Render", rightTab == 1))
            rightTab = 1;
        ImGui::SameLine();
        if (iconButton("panel.tab.analysis", 0xE9D9, "Analysis",
                       "Analysis", rightTab == 2))
            rightTab = 2;
        ImGui::SameLine();
        if (iconButton("panel.tab.system", 0xE713, "System",
                       "System", rightTab == 3))
            rightTab = 3;
        ImGui::PopStyleVar();
        // P12 inspector header: category caption, mono badge chip, node name
        // and — for toggleable modifiers — the 30x16 toggle switch.
        if (rightTab == 0) {
            const bool modSel = modifierGraph.selected < mods.size();
            const char *cat = modSel ? mods[modifierGraph.selected].category.c_str()
                                     : panelSelection == -2 ? "Visual element"
                                     : panelSelection == -3 ? "Visual element"
                                                            : "Data source";
            const std::string badgeText =
                modSel ? nodeBadgeText(mods[modifierGraph.selected])
                       : std::string(panelSelection == -2 ? "PA"
                                     : panelSelection == -3 ? "SC" : "XYZ");
            const char *badge = badgeText.c_str();
            const std::string sourceName =
                path.empty() ? std::string("Generated crystal")
                             : utf8(path.filename().wstring());
            const char *name = modSel
                ? (mods[modifierGraph.selected].displayName.empty()
                       ? opName(mods[modifierGraph.selected].op)
                       : mods[modifierGraph.selected].displayName.c_str())
                : panelSelection == -2 ? "Particles"
                : panelSelection == -3 ? "Simulation cell"
                                       : sourceName.c_str();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(cat);
            ImGui::PopStyleColor();
            ImGui::Spacing();
            monoBadge(badge, modSel);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::TextUnformatted(name);
            ImGui::PopStyleColor();
            if (modSel) {
                ImGui::SameLine();
                const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - U(30);
                ImGui::SetCursorPosX(right);
                bool enabled = mods[modifierGraph.selected].enabled;
                ImGui::PushID(mods[modifierGraph.selected].id.c_str());
                if (toggleSwitch("##enable-node", &enabled)) {
                    checkpoint();
                    mods[modifierGraph.selected].enabled = enabled;
                    update(modifierGraph.selected);
                }
                ImGui::PopID();
                recordUiTestItem(std::string("pipeline.node.enabled.") +
                                 mods[modifierGraph.selected].id);
            }
            ImGui::Spacing();
            ImGui::Separator();
        }
            // P12 bespoke inspector panels for the pseudo-nodes selectable in
            // the left panel: Data source / Particles / Simulation cell.
            if (rightTab == 0 && modifierGraph.selected >= mods.size()) {
                if (panelSelection == -1) {
                    ImGui::Spacing();
                    auto gridLabel = [](const char *label) {
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
                        ImGui::TextUnformatted(label);
                        ImGui::PopStyleColor();
                    };
                    gridLabel("File");
                    ImGui::TextWrapped("%s", path.empty() ? "(generated)"
                                                          : utf8(path.wstring()).c_str());
                    gridLabel("Reader");
                    ImGui::TextUnformatted(readerName.c_str());
                    gridLabel("Frames");
                    ImGui::Text("%zu%s", std::max<size_t>(frames.size(), 1),
                                frames.size() > 1 ? " · indexed" : "");
                    gridLabel("Particles");
                    ImGui::Text("%s · stride %d", number(source.sourceCount).c_str(),
                                int(source.stride));
                    ImGui::Spacing();
                    sectionLabel("PARTICLE TYPES");
                    {
                        std::vector<size_t> counts(result.data.species.size(), 0);
                        for (const auto &a : result.data.atoms)
                            if (a.type < counts.size()) ++counts[a.type];
                        for (size_t t = 0; t < result.data.species.size(); ++t) {
                            colorSwatch(typeColor(t));
                            ImGui::SameLine();
                            ImGui::Text("%s", result.data.species[t].c_str());
                            ImGui::SameLine(ImGui::GetContentRegionAvail().x - U(110));
                            ImGui::TextDisabled("%s", number(counts[t]).c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("%.2f A",
                                                t < gpu.styles.size() ? gpu.styles[t].visual[3] : radius);
                        }
                    }
                    ImGui::Spacing();
                    sectionLabel("SIMULATION CELL");
                    {
                        const auto &cv = result.data.cell;
                        const bool lattice = cv[0] != 0 || cv[4] != 0 || cv[8] != 0;
                        for (int row = 0; row < 3; ++row) {
                            ImGui::TextDisabled("%c", 'a' + row);
                            for (int col = 0; col < 3; ++col) {
                                ImGui::SameLine();
                                ImGui::Text("%11.4f", cv[row * 3 + col]);
                            }
                        }
                        if (lattice) {
                            ImGui::TextDisabled("o");
                            for (int col = 0; col < 3; ++col) {
                                ImGui::SameLine();
                                ImGui::Text("%11.4f", col == 0 ? result.data.origin.x
                                                    : col == 1 ? result.data.origin.y
                                                               : result.data.origin.z);
                            }
                        }
                        ImGui::Spacing();
                        ImGui::TextDisabled("PBC:  %s  %s  %s", source.pbc[0] ? "X" : "-",
                                            source.pbc[1] ? "Y" : "-", source.pbc[2] ? "Z" : "-");
                    }
                } else if (panelSelection == -2) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
                    ImGui::TextUnformatted("Radius scale");
                    ImGui::PopStyleColor();
                    if (scrubBar("##Radius scale", &radius, .02f, 2.f, "%.2f", "x"))
                        status = "Particle radius scale updated";
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
                    ImGui::TextUnformatted("Shape");
                    ImGui::PopStyleColor();
                    const int shape = segmented("##Particle shape",
                                                particleShape == 2 ? 1 : 0, {"Sphere", "Cube"});
                    if (particleShape != (shape == 1 ? 2 : 0)) {
                        particleShape = shape == 1 ? 2 : 0;
                        status = "Particle shape updated";
                    }
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("Base radii from Cordero 2008 covalent radii. Per-type "
                                       "overrides take precedence; selection and color coding "
                                       "override type colors.");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
                    ImGui::TextUnformatted("Line width");
                    ImGui::PopStyleColor();
                    if (scrubBar("##Cell line width", &cellWidth, .5f, 4.f, "%.1f", "px"))
                        status = "Cell line width updated";
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(154, 161, 169, 255));
                    ImGui::TextUnformatted("Line color");
                    ImGui::PopStyleColor();
                    ImGui::ColorEdit3("##Cell line color", cellColor);
                }
            }
        if (rightTab == 0) {
            // Selected modifier parameter section (the stack itself lives in
            // the left PIPELINE panel).
            if (modifierGraph.selected < mods.size()) {
                    auto &m = mods[modifierGraph.selected];
                    ImGui::PushID(m.id.c_str());
                    heading(m.displayName.empty() ? opName(m.op) : m.displayName.c_str());
                    if (m.op == Op::Slice) {
                        float normal[3]{m.sliceNormal[0], m.sliceNormal[1], m.sliceNormal[2]};
                        double distance = m.sliceDistance, width = m.sliceWidth;
                        bool reverse = m.sliceInvert, visualize = m.sliceShowPlane;
                        bool createSelection = m.sliceCreateSelection;
                        bool applySelectionOnly = m.sliceApplySelectionOnly;
                        bool operateOnParticles = m.sliceOperateOnParticles;
                        bool changed = false, centerPlane = false;
                        int mode = 1;
                        ImGui::RadioButton("Cartesian", &mode, 1);
                        ImGui::SameLine();
                        ImGui::BeginDisabled();
                        ImGui::RadioButton("Miller indices", &mode, 2);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Miller index input requires OVITO Pro and is not implemented.");
                        // P12 design controls: the plane normal as an X/Y/Z
                        // segmented control, Distance and Slab width as scrub
                        // bars over the same state. The free-form normal
                        // fields remain below for arbitrary planes.
                        {
                            const int axis = std::abs(normal[0]) >= std::abs(normal[1])
                                ? (std::abs(normal[0]) >= std::abs(normal[2]) ? 0 : 2)
                                : (std::abs(normal[1]) >= std::abs(normal[2]) ? 1 : 2);
                            const int pickedAxis =
                                segmented("##Slice normal", axis, {"X", "Y", "Z"}, U(180));
                            if (pickedAxis != axis) {
                                normal[0] = normal[1] = normal[2] = 0;
                                normal[pickedAxis] = 1;
                                changed = true;
                            }
                            const auto &cv = result.data.cell;
                            const auto &og = result.data.origin;
                            double extent = 0;
                            for (int r = 0; r < 3; ++r)
                                extent = std::max(extent,
                                                  std::hypot(cv[r * 3], cv[r * 3 + 1], cv[r * 3 + 2]) +
                                                      std::abs(r == 0 ? og.x : r == 1 ? og.y : og.z));
                            changed |= scrubBar("##Slice distance", &distance, 0.0,
                                                extent > 0 ? extent : 10.0, "%.2f", "A");
                            changed |= scrubBar("##Slab width", &width, 0.0, 6.0, "%.2f", "A");
                        }
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (x)", &normal[0], .01f);
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (y)", &normal[1], .01f);
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (z)", &normal[2], .01f);
                        const double minimumWidth = 0;
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragScalar("Slab width", ImGuiDataType_Double, &width,
                                                     .02f, &minimumWidth, nullptr, "%.4g");                        changed |= ImGui::Checkbox("Reverse orientation", &reverse);
                        changed |= ImGui::Checkbox("Create selection (do not delete)", &createSelection);
                        changed |= ImGui::Checkbox("Apply to selection only", &applySelectionOnly);
                        changed |= ImGui::Checkbox("Visualize plane", &visualize);
                        if (ImGui::Button("Center in simulation cell"))
                            centerPlane = true;
                        const auto &sliceAttributes = result.data.globalAttributes;
                        auto sliceInput = sliceAttributes.find("Slice.input_particles");
                        auto sliceDeleted = sliceAttributes.find("Slice.particles_deleted");
                        auto sliceRemaining = sliceAttributes.find("Slice.particles_remaining");
                        if (sliceInput != sliceAttributes.end() &&
                            sliceDeleted != sliceAttributes.end() &&
                            sliceRemaining != sliceAttributes.end())
                            ImGui::TextDisabled("%.0f input particles / %.0f deleted / %.0f remaining",
                                                sliceInput->second, sliceDeleted->second,
                                                sliceRemaining->second);
                        heading("Operate on");
                        changed |= ImGui::Checkbox("Particles", &operateOnParticles);
                        ImGui::BeginDisabled();
                        bool absent = false;
                        for (const char *kind : {"Surfaces (not present)", "Voxel grids (not present)",
                                                 "Dislocations (not present)", "Lines (not present)",
                                                 "Vectors (not present)"}) {
                            ImGui::PushID(kind);
                            ImGui::Checkbox(kind, &absent);
                            ImGui::PopID();
                        }
                        ImGui::EndDisabled();
                        if (changed || centerPlane) {
                            checkpoint();
                            m.sliceNormal[0] = normal[0];
                            m.sliceNormal[1] = normal[1];
                            m.sliceNormal[2] = normal[2];
                            m.sliceDistance = distance;
                            m.sliceInvert = reverse;
                            m.sliceWidth = std::max(width, 0.0);
                            m.sliceShowPlane = visualize;
                            m.sliceCreateSelection = createSelection;
                            m.sliceApplySelectionOnly = applySelectionOnly;
                            m.sliceOperateOnParticles = operateOnParticles;
                            if (centerPlane) {
                                const double n[3]{m.sliceNormal[0], m.sliceNormal[1], m.sliceNormal[2]};
                                const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                                if (length > 0)
                                    m.sliceDistance =
                                        (n[0] * (double(result.data.origin.x) + (result.data.cell[0] +
                                             result.data.cell[3] + result.data.cell[6]) * .5) +
                                         n[1] * (double(result.data.origin.y) + (result.data.cell[1] +
                                             result.data.cell[4] + result.data.cell[7]) * .5) +
                                         n[2] * (double(result.data.origin.z) + (result.data.cell[2] +
                                             result.data.cell[5] + result.data.cell[8]) * .5)) / length;
                            }
                            update();
                        }
                    }
                    if (m.op == Op::Translate || m.op == Op::Scale || m.op == Op::Rotate || m.op == Op::SelectRange) {
                        float v = m.value;
                        ImGui::SetNextItemWidth(U(140));
                        if (ImGui::DragFloat("Value", &v, .1f)) {
                            checkpoint();
                            m.value = v;
                            update();
                        }
                        if (m.op != Op::Scale) {
                            int a = m.axis;
                            ImGui::SetNextItemWidth(U(140));
                            if (ImGui::Combo("Axis", &a, "X\0Y\0Z\0")) {
                                checkpoint();
                                m.axis = a;
                                update();
                            }
                        }
                    }
                    if (m.op == Op::SelectRange) {
                        float upper = m.upper;
                        if (ImGui::DragFloat("Upper bound", &upper, .1f)) {
                            checkpoint(); m.upper = upper; update();
                        }
                    }
                    if (m.op == Op::EditCell) {
                        auto editedCell = m.editedCell;
                        auto origin = m.editedOrigin;
                        auto pbc = m.editedPbc;
                        bool transform = m.transformCoordinatesWithCell;
                        bool changed = false;
                        double originValues[3]{origin.x, origin.y, origin.z};
                        ImGui::Text("Cell origin");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(100 + axis);
                            ImGui::SetNextItemWidth(U(88));
                            changed |= ImGui::InputDouble(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                         &originValues[axis], 0, 0, "%.6g");
                            ImGui::PopID();
                        }
                        origin = {float(originValues[0]), float(originValues[1]),
                                  float(originValues[2])};
                        heading("Cell vectors");
                        if (ImGui::BeginTable("Edited cell vectors", 4,
                                              ImGuiTableFlags_SizingStretchSame)) {
                            ImGui::TableSetupColumn("Vector");
                            ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y");
                            ImGui::TableSetupColumn("Z"); ImGui::TableHeadersRow();
                            for (int row = 0; row < 3; ++row) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%c", 'a' + row);
                                for (int column = 0; column < 3; ++column) {
                                    ImGui::TableSetColumnIndex(column + 1);
                                    ImGui::PushID(row * 3 + column);
                                    ImGui::SetNextItemWidth(-1);
                                    changed |= ImGui::InputDouble("##cell", &editedCell[row * 3 + column],
                                                                  0, 0, "%.8g");
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::Text("Periodic boundary flags");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(200 + axis);
                            changed |= ImGui::Checkbox(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                       &pbc[axis]);
                            ImGui::PopID();
                        }
                        changed |= ImGui::Checkbox("Transform particle coordinates with cell", &transform);
                        ImGui::TextWrapped("Off: particle positions stay fixed while the cell changes. On: preserve fractional coordinates in the edited cell.");
                        if (changed) {
                            checkpoint(); m.editedCell = editedCell; m.editedOrigin = origin;
                            m.editedPbc = pbc; m.transformCoordinatesWithCell = transform; update();
                        }
                    }
                    if (m.op == Op::AffineTransform) {
                        auto matrix = m.affineTransform;
                        bool reducedCoords = m.affineReducedCoords;
                        bool onlySelected = m.affineOnlySelected;
                        bool transformVectors = m.transformVectorProperties;
                        bool changed = false;
                        ImGui::Text("Transformation matrix:");
                        ImGui::TextDisabled("Translate/Scale/Shear:");
                        if (ImGui::BeginTable("Translate/Scale/Shear", 3,
                                              ImGuiTableFlags_SizingStretchSame)) {
                            for (const char *column : {"a", "b", "c"})
                                ImGui::TableSetupColumn(column);
                            ImGui::TableHeadersRow();
                            for (int row = 0; row < 3; ++row) {
                                ImGui::TableNextRow();
                                for (int column = 0; column < 3; ++column) {
                                    ImGui::TableSetColumnIndex(column);
                                    ImGui::PushID(row * 4 + column);
                                    ImGui::SetNextItemWidth(-1);
                                    changed |= ImGui::InputDouble("##affine-linear",
                                                                  &matrix[row * 4 + column],
                                                                  0, 0, "%.7g");
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::SameLine();
                        static int rotationAxis = 2;
                        static double rotationAngle = 90;
                        static double rotationCenter[3] = {0, 0, 0};
                        static bool rotationCenterSeeded = false;
                        if (ImGui::Button("Enter rotation")) {
                            const auto [seedCell, seedOrigin] =
                                pipelineCellBefore(modifierGraph.selected);
                            rotationCenter[0] = seedOrigin.x + (seedCell[0] + seedCell[3] + seedCell[6]) * .5;
                            rotationCenter[1] = seedOrigin.y + (seedCell[1] + seedCell[4] + seedCell[7]) * .5;
                            rotationCenter[2] = seedOrigin.z + (seedCell[2] + seedCell[5] + seedCell[8]) * .5;
                            rotationCenterSeeded = true;
                            ImGui::OpenPopup("Enter rotation");
                        }
                        if (ImGui::BeginPopup("Enter rotation")) {
                            if (!rotationCenterSeeded) rotationCenterSeeded = true;
                            ImGui::Combo("Rotation axis", &rotationAxis,
                                         "X axis\0Y axis\0Z axis\0Custom vector...\0");
                            float customAxis[3] = {0, 0, 1};
                            if (rotationAxis == 3)
                                ImGui::DragFloat3("Axis vector", customAxis, .01f, -1.f, 1.f, "%.3f");
                            ImGui::InputDouble("Rotation angle (degrees)", &rotationAngle, 0, 0, "%.6g");
                            for (int axis = 0; axis < 3; ++axis) {
                                if (axis) ImGui::SameLine();
                                ImGui::PushID(400 + axis);
                                ImGui::SetNextItemWidth(U(72));
                                ImGui::InputScalar(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                   ImGuiDataType_Double, &rotationCenter[axis],
                                                   nullptr, nullptr, "%.6g");
                                ImGui::PopID();
                            }
                            if (ImGui::Button("Enter")) {
                                std::array<double, 3> axis{rotationAxis == 0 ? 1.0 : 0.0,
                                                           rotationAxis == 1 ? 1.0 : 0.0,
                                                           rotationAxis == 2 ? 1.0 : 0.0};
                                if (rotationAxis == 3)
                                    axis = {customAxis[0], customAxis[1], customAxis[2]};
                                const double norm = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] +
                                                              axis[2] * axis[2]);
                                if (norm > 0 && std::isfinite(norm)) {
                                    for (auto &component : axis) component /= norm;
                                    const double radians =
                                        rotationAngle * (3.14159265358979323846 / 180.0);
                                    const double c = std::cos(radians), s = std::sin(radians),
                                                 ic = 1 - c;
                                    const double x = axis[0], y = axis[1], z = axis[2];
                                    const std::array<double, 9> rotation{
                                        x * x * ic + c, x * y * ic - z * s, x * z * ic + y * s,
                                        y * x * ic + z * s, y * y * ic + c, y * z * ic - x * s,
                                        z * x * ic - y * s, z * y * ic + x * s, z * z * ic + c};
                                    std::array<double, 12> next = m.affineTransform;
                                    for (int row = 0; row < 3; ++row) {
                                        for (int column = 0; column < 3; ++column)
                                            next[row * 4 + column] =
                                                rotation[row * 3] * m.affineTransform[column] +
                                                rotation[row * 3 + 1] * m.affineTransform[4 + column] +
                                                rotation[row * 3 + 2] * m.affineTransform[8 + column];
                                        next[row * 4 + 3] =
                                            rotation[row * 3] * m.affineTransform[3] +
                                            rotation[row * 3 + 1] * m.affineTransform[7] +
                                            rotation[row * 3 + 2] * m.affineTransform[11] +
                                            rotationCenter[row] -
                                            (rotation[row * 3] * rotationCenter[0] +
                                             rotation[row * 3 + 1] * rotationCenter[1] +
                                             rotation[row * 3 + 2] * rotationCenter[2]);
                                    }
                                    matrix = next;
                                    changed = true;
                                }
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::TextDisabled("Translation:");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(300 + axis);
                            ImGui::SetNextItemWidth(U(88));
                            changed |= ImGui::InputDouble(axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                                                          &matrix[axis * 4 + 3], 0, 0, "%.7g");
                            ImGui::PopID();
                        }
                        changed |= ImGui::Checkbox("In reduced cell coordinates", &reducedCoords);
                        if (reducedCoords)
                            ImGui::TextDisabled("The translation is given in fractional cell coordinates and resolved through the simulation cell.");
                        const auto [inputCell, inputOrigin] =
                            pipelineCellBefore(modifierGraph.selected);
                        ImGui::Text("Transform simulation cell:");
                        ImGui::TextDisabled("Transform cell vectors:");
                        if (ImGui::BeginTable("Transformed cell vectors", 4,
                                              ImGuiTableFlags_SizingStretchSame)) {
                            for (const char *column : {"Vector", "X", "Y", "Z"})
                                ImGui::TableSetupColumn(column);
                            ImGui::TableHeadersRow();
                            for (int row = 0; row < 3; ++row) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("%c", 'a' + row);
                                for (int component = 0; component < 3; ++component) {
                                    ImGui::TableSetColumnIndex(component + 1);
                                    ImGui::TextDisabled("%.5g",
                                        matrix[row * 4] * inputCell[component] +
                                        matrix[row * 4 + 1] * inputCell[3 + component] +
                                        matrix[row * 4 + 2] * inputCell[6 + component]);
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TextDisabled("Transform cell origin:");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::TextDisabled("%.5g", matrix[axis * 4] * inputOrigin.x +
                                                        matrix[axis * 4 + 1] * inputOrigin.y +
                                                        matrix[axis * 4 + 2] * inputOrigin.z +
                                                        matrix[axis * 4 + 3]);
                        }
                        heading("Operate on");
                        changed |= ImGui::Checkbox("Transform only selected particles/vertices",
                                                   &onlySelected);
                        ImGui::BeginDisabled();
                        bool all = true;
                        ImGui::Checkbox("Simulation cell <all>", &all);
                        ImGui::Checkbox("Particles <all>", &all);
                        ImGui::Checkbox("Surfaces <not present>", &all);
                        ImGui::Checkbox("Triangle meshes <not present>", &all);
                        ImGui::EndDisabled();
                        int vectorsOperate = transformVectors ? 0 : 1;
                        if (ImGui::Combo("Vector properties", &vectorsOperate, "<all>\0<none>\0"))
                            transformVectors = vectorsOperate == 0;
                        changed |= transformVectors != m.transformVectorProperties;
                        ImGui::TextDisabled("Cell vectors, the cell origin and vector properties take only the linear part; the translation never applies to them.");
                        if (changed) {
                            checkpoint();
                            m.affineTransform = matrix;
                            m.affineReducedCoords = reducedCoords;
                            m.affineOnlySelected = onlySelected;
                            m.transformVectorProperties = transformVectors;
                            update();
                        }
                    }
                    if (m.op == Op::ClusterAnalysis) {
                        bool useBonds=m.clusterByBonds;
                        if (ImGui::Checkbox("Group by existing bonds",&useBonds)) {
                            checkpoint(); m.clusterByBonds=useBonds; update();
                        }
                        bool onlySelected=m.clusterOnlySelected;
                        if (ImGui::Checkbox("Use only selected particles",&onlySelected)) {
                            checkpoint(); m.clusterOnlySelected=onlySelected; update();
                        }
                        bool sortBySize=m.clusterSortBySize;
                        if (ImGui::Checkbox("Sort clusters by size",&sortBySize)) {
                            checkpoint(); m.clusterSortBySize=sortBySize; update();
                        }
                        ImGui::TextDisabled("When enabled, the largest component receives Cluster ID 1.");
                        ImGui::TextDisabled(useBonds
                            ? "Connected components follow the input bond topology; selected-only mode assigns ID 0 to other particles."
                            : "Connected components follow the periodic minimum-image cutoff. Unselected particles get Cluster ID 0 when selected-only is enabled.");
                    }
                    if (m.op == Op::CommonNeighborAnalysis ||
                        m.op == Op::CoordinationAnalysis || m.op == Op::ClusterAnalysis ||
                        m.op == Op::RadialDistribution) {
                        if (!(m.op==Op::ClusterAnalysis && m.clusterByBonds)) {
                            float value = m.value;
                            if (ImGui::DragFloat("Cutoff distance", &value, .01f, .0001f, 100000.f, "%.5g")) {
                                checkpoint(); m.value = value; update();
                            }
                        }
                    }
                    if (m.op==Op::RadialDistribution) {
                        int bins=m.rdfBins;
                        const float binsLabelWidth=ImGui::CalcTextSize("RDF histogram bins").x+
                                                   ImGui::GetStyle().ItemInnerSpacing.x;
                        ImGui::SetNextItemWidth(std::max(U(50),ImGui::GetContentRegionAvail().x-binsLabelWidth));
                        if (ImGui::InputInt("RDF histogram bins",&bins)) {
                            checkpoint(); m.rdfBins=std::clamp(bins,1,4096); update();
                        }
                        recordUiTestItem(std::string("pipeline.rdf-bins.")+m.id,"RDF histogram bins");
                    }
                    if (m.op==Op::RadialDistribution || m.op==Op::CoordinationAnalysis) {
                        bool onlySelected=m.neighborOnlySelected;
                        if (ImGui::Checkbox("Use only selected particles",&onlySelected)) {
                            checkpoint(); m.neighborOnlySelected=onlySelected; update();
                        }
                        ImGui::TextDisabled("Unselected particles are excluded as both analysis centers and neighbors.");
                    }
                    if (m.op == Op::CentrosymmetryParameter) {
                        int neighbors = m.cspNeighbors;
                        if (ImGui::InputInt("Number of neighbors", &neighbors)) {
                            checkpoint();
                            m.cspNeighbors = std::clamp(neighbors, 2, 64);
                            if (m.cspNeighbors % 2) --m.cspNeighbors;
                            update();
                        }
                        recordUiTestItem(std::string("pipeline.csp-neighbors.") + m.id,
                                         "Number of neighbors");
                        int mode = m.cspMode;
                        bool changed = ImGui::RadioButton("Conventional CSP", &mode, 0);
                        changed |= ImGui::RadioButton("Minimum-weight matching CSP", &mode, 1);
                        bool onlySelected = m.cspOnlySelected;
                        changed |= ImGui::Checkbox("Only selected particles", &onlySelected);
                        if (changed) {
                            checkpoint();
                            m.cspMode = mode;
                            m.cspOnlySelected = onlySelected;
                            update();
                        }
                        ImGui::TextDisabled("Use the ideal coordination number of the lattice: 12 for FCC/HCP, 8 for BCC.");
                        if (onlySelected)
                            ImGui::TextDisabled("Unselected particles are skipped as centers and report a CSP of 0.");
                        if (auto cspValues = result.data.scalarProperties.find("Centrosymmetry");
                            cspValues != result.data.scalarProperties.end() && !cspValues->second.empty()) {
                            std::array<float, 64> bins{};
                            double lo = 0, hi = 0;
                            bool first = true;
                            for (double value : cspValues->second)
                                if (std::isfinite(value)) {
                                    if (first) { lo = hi = value; first = false; }
                                    else { lo = std::min(lo, value); hi = std::max(hi, value); }
                                }
                            if (!first) {
                                if (hi <= lo) hi = lo + 1e-12;
                                for (double value : cspValues->second)
                                    if (std::isfinite(value))
                                        bins[std::clamp(size_t((value - lo) / (hi - lo) * 63),
                                                        size_t(0), size_t(63))] += 1;
                                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, {1.f, .49f, .05f, 1.f});
                                ImGui::PlotHistogram("##csp-histogram", bins.data(), int(bins.size()),
                                                     0, "Centrosymmetry", 0.f, FLT_MAX, {-1, 75});
                                ImGui::PopStyleColor();
                                ImGui::TextDisabled("CSP range %.4g - %.4g", lo, hi);
                            }
                        }
                    }
                    if (m.op == Op::DisplacementVectors) {
                        ImGui::SeparatorText("Calculate displacements");
                        // Reference configuration source (v1: same pipeline only).
                        int sourceMode = 0;
                        ImGui::TextDisabled("Reference configuration source");
                        if (ImGui::RadioButton("Same pipeline", &sourceMode, 0)) {}
                        ImGui::BeginDisabled();
                        ImGui::RadioButton("External file", &sourceMode, 1);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Not implemented yet");
                        bool changed = false;
                        // Reference frame within the loaded trajectory.
                        ImGui::TextDisabled("Use animation frame ... as reference configuration");
                        int relativeMode = m.displacementRelative ? 1 : 0;
                        if (ImGui::RadioButton("Frame number", &relativeMode, 0) |
                            ImGui::RadioButton("Relative to current frame", &relativeMode, 1))
                            changed = true;
                        const bool relative = relativeMode != 0;
                        if (!relative) {
                            int frame = m.displacementFrame;
                            if (ImGui::InputInt("Frame number", &frame)) {
                                m.displacementFrame = std::max(0, frame);
                                changed = true;
                            }
                        } else {
                            int offset = m.displacementOffset;
                            if (ImGui::InputInt("Frame offset", &offset)) {
                                m.displacementOffset = offset;
                                changed = true;
                            }
                        }
                        // Mapping of the simulation cell.
                        ImGui::TextDisabled("Mapping of simulation cell");
                        int mapping = m.displacementCellMapping;
                        changed |= ImGui::RadioButton("Off", &mapping, 0);
                        changed |= ImGui::RadioButton("To reference", &mapping, 1);
                        changed |= ImGui::RadioButton("To current", &mapping, 2);
                        bool minimumImage = m.displacementMinimumImage;
                        changed |= ImGui::Checkbox("Use minimum image convention", &minimumImage);
                        recordUiTestItem(std::string("pipeline.displacement-relative.") + m.id,
                                         "Relative to current frame");
                        recordUiTestItem(std::string("pipeline.displacement-mic.") + m.id,
                                         "Use minimum image convention");
                        if (changed) {
                            checkpoint();
                            m.displacementRelative = relative;
                            m.displacementCellMapping = mapping;
                            m.displacementMinimumImage = minimumImage;
                            update();
                        }
                        ImGui::TextWrapped(
                            "Compares the current frame with the chosen raw input frame and "
                            "publishes the Displacement vector and Displacement Magnitude "
                            "particle properties. Non-periodic axes never use the minimum "
                            "image convention.");
                    }
                    if (m.op == Op::FreezeProperty) {
                        heading("Operate on");
                        ImGui::BeginDisabled();
                        bool operateParticles = true;
                        ImGui::Checkbox("Particles", &operateParticles);
                        ImGui::EndDisabled();
                        // Property to freeze: upstream scalar and vector
                        // particle properties. OVITO defaults to Particle
                        // Type, which is not numeric in AtomX, so v1 lists
                        // scalar/vector properties and defaults to the first.
                        const auto choices = pipelineFreezablePropertyChoices(
                            source, mods, std::min(modifierGraph.selected, mods.size()));
                        if (choices.empty()) {
                            ImGui::TextWrapped(
                                "No freezable particle property is available at this pipeline stage.");
                        } else {
                            if (std::find(choices.begin(), choices.end(), m.property) ==
                                choices.end()) {
                                checkpoint();
                                m.property = choices.front();
                                update();
                            }
                            if (ImGui::BeginCombo("Property to freeze", m.property.c_str())) {
                                for (const auto &name : choices) {
                                    const bool selected = m.property == name;
                                    if (ImGui::Selectable(name.c_str(), selected)) {
                                        checkpoint();
                                        m.property = name;
                                        update();
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            recordUiTestItem(std::string("pipeline.freeze-property.") + m.id,
                                             "Property to freeze");
                        }
                        int frame = m.freezeFrame;
                        if (ImGui::InputInt("Reference frame", &frame)) {
                            checkpoint();
                            m.freezeFrame = std::max(0, frame);
                            update();
                        }
                        recordUiTestItem(std::string("pipeline.freeze-frame.") + m.id,
                                         "Reference frame");
                        ImGui::TextWrapped(
                            "Snapshots the property at the reference frame and restores those "
                            "values on every other frame. Particles are matched by index or "
                            "Particle Identifier.");
                    }
                    if (m.op == Op::SelectOverlapping) {
                        bool useRadii = m.overlapUseRadii;
                        if (ImGui::Checkbox("Use per-particle radii", &useRadii)) {
                            checkpoint(); m.overlapUseRadii = useRadii; update();
                        }
                        if (m.overlapUseRadii) {
                            std::vector<std::string> radiusProperties;
                            for (const auto &[name, values] : result.data.scalarProperties)
                                if (values.size() == result.data.atoms.size())
                                    radiusProperties.push_back(name);
                            std::sort(radiusProperties.begin(), radiusProperties.end());
                            if (radiusProperties.empty()) {
                                ImGui::TextWrapped("No scalar particle-radius property is available. Compute or import a Radius property first.");
                            } else if (ImGui::BeginCombo("Radius property", m.property.c_str())) {
                                for (const auto &name : radiusProperties) {
                                    const bool selected = m.property == name;
                                    if (ImGui::Selectable(name.c_str(), selected)) {
                                        checkpoint(); m.property = name; update();
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::TextWrapped("Two particles overlap when their minimum-image distance is less than the sum of their radii.");
                        } else {
                            float value = m.value;
                            if (ImGui::DragFloat("Pair cutoff", &value, .01f, .0001f, 100000.f, "%.5g")) {
                                checkpoint(); m.value = value; update();
                            }
                            ImGui::TextWrapped("Selects both endpoints of every pair closer than this distance. Enable per-particle radii for sphere-overlap testing.");
                        }
                    }
                    if (m.op == Op::BondLengthDistribution || m.op == Op::BondAngleDistribution) {
                        int bins=m.type;
                        if (ImGui::InputInt("Bins", &bins)) {
                            checkpoint(); m.type=std::clamp(bins,1,4096); update();
                        }
                        int normalization=m.histogramNormalization;
                        if (ImGui::Combo("Normalization",&normalization,
                                         "Absolute counts\0Relative frequency\0Probability density\0")) {
                            checkpoint(); m.histogramNormalization=normalization; update();
                        }
                        if (normalization==2)
                            ImGui::TextDisabled(m.op==Op::BondLengthDistribution ?
                                "Density is normalized per length unit." : "Density is normalized per degree.");
                        ImGui::TextWrapped("Uses explicit pipeline bonds, including periodic image shifts. Add Create bonds upstream if the dataset has no bond topology.");
                    }
                    if (m.op==Op::ScatterPlot) {
                        auto edited=m;
                        bool changed=colorPropertyCombo("X property",edited.scatterXProperty,true);
                        recordUiTestItem(std::string("pipeline.scatter-x.")+m.id,"X property");
                        changed|=colorPropertyCombo("Y property",edited.scatterYProperty,true);
                        recordUiTestItem(std::string("pipeline.scatter-y.")+m.id,"Y property");
                        if (ImGui::Checkbox("Use only selected particles",&edited.scatterSelectedOnly))
                            changed=true;
                        recordUiTestItem(std::string("pipeline.scatter-selected.")+m.id,
                                         "Use only selected particles");
                        ImGui::TextDisabled("Non-finite pairs are skipped; large plots use a deterministic exported preview sample.");
                        if (changed) {
                            checkpoint();
                            m.scatterXProperty=std::move(edited.scatterXProperty);
                            m.scatterYProperty=std::move(edited.scatterYProperty);
                            m.scatterSelectedOnly=edited.scatterSelectedOnly;
                            update();
                        }
                    }
                    if (m.op == Op::Histogram || m.op == Op::ReduceProperty) {
                        auto edited = m;
                        bool changed = colorPropertyCombo("Input property", edited.property, true);
                        recordUiTestItem(std::string("pipeline.analysis-property.")+m.id,"Input property");
                        if (m.op == Op::Histogram) {
                            int bins = edited.type;
                            if (ImGui::InputInt("Bins", &bins)) { edited.type = std::clamp(bins, 1, 4096); changed = true; }
                            if (ImGui::Checkbox("Use only selected elements", &edited.histogramSelectedOnly))
                                changed = true;
                            int normalization = edited.histogramNormalization;
                            if (ImGui::Combo("Normalization", &normalization,
                                             "Absolute counts\0Relative frequency\0Probability density\0")) {
                                edited.histogramNormalization = normalization;
                                changed = true;
                            }
                            if (ImGui::Checkbox("Select value range", &edited.histogramSelectRange))
                                changed = true;
                            if (edited.histogramSelectRange) {
                                changed |= ImGui::InputDouble("Range start", &edited.histogramRangeStart,
                                                              0, 0, "%.8g");
                                changed |= ImGui::InputDouble("Range end", &edited.histogramRangeEnd,
                                                              0, 0, "%.8g");
                                ImGui::TextDisabled("Replaces the output selection with particles in the inclusive range.");
                            }
                            ImGui::TextDisabled("The table uses this frame; selected-only filters its input samples.");
                        } else {
                            int reduction = edited.reduceOperation;
                            if (ImGui::Combo("Reduction", &reduction, "Minimum\0Maximum\0Mean\0Sum\0")) {
                                edited.reduceOperation = reduction; changed = true;
                            }
                            recordUiTestItem(std::string("pipeline.reduce-operation.")+m.id,"Reduction");
                        }
                        if (changed) {
                            checkpoint();
                            m.property = edited.property;
                            m.type = edited.type;
                            m.reduceOperation = edited.reduceOperation;
                            m.histogramSelectedOnly = edited.histogramSelectedOnly;
                            m.histogramNormalization = edited.histogramNormalization;
                            m.histogramSelectRange = edited.histogramSelectRange;
                            m.histogramRangeStart = edited.histogramRangeStart;
                            m.histogramRangeEnd = edited.histogramRangeEnd;
                            update();
                        }
                    }
                    if (m.op == Op::Replicate) {
                        int counts[3]{m.replicateN[0], m.replicateN[1], m.replicateN[2]};
                        bool adjustBox = m.replicateAdjustBox;
                        bool changed = false;
                        ImGui::Text("Number of copies");
                        for (int axis = 0; axis < 3; ++axis) {
                            if (axis) ImGui::SameLine();
                            ImGui::PushID(axis);
                            ImGui::SetNextItemWidth(U(88));
                            changed |= ImGui::InputInt(axis == 0 ? "Na" : axis == 1 ? "Nb" : "Nc",
                                                       &counts[axis]);
                            ImGui::PopID();
                        }
                        changed |= ImGui::Checkbox("Adjust box size", &adjustBox);
                        if (changed) {
                            checkpoint();
                            for (int axis = 0; axis < 3; ++axis)
                                m.replicateN[axis] = std::clamp(counts[axis], 1, 32);
                            m.replicateAdjustBox = adjustBox;
                            update();
                        }
                    }
                    if (m.op == Op::SelectIndex) {
                        int index = m.type;
                        if (ImGui::InputInt("Atom index", &index)) {
                            checkpoint(); m.type = std::max(0,index); update();
                        }
                    }
                    if (m.op == Op::ManualSelection) {
                        ImGui::TextDisabled("%zu particles selected. Click a table row to replace; Ctrl-click to toggle.",
                                            m.manualSelection.size());
                    }
                    if (m.op == Op::EditType) {
                        int t = m.type;
                        if (ImGui::InputInt("Type index", &t)) {
                            checkpoint();
                            m.type = std::clamp(t, 0, std::max(0, int(source.species.size()) - 1));
                            update();
                        }
                    }
                    if (m.op == Op::SelectType) {
                        ImGui::TextWrapped("Check one or more particle types; the selection is replaced with all particles of the checked types.");
                        bool changed = false;
                        const bool legacySingle = m.selectedTypes.empty();
                        std::vector<uint32_t> types =
                            legacySingle ? std::vector<uint32_t>{m.type >= 0 ? uint32_t(m.type) : 0}
                                         : m.selectedTypes;
                        if (source.species.empty())
                            ImGui::TextDisabled("The dataset has no particle types.");
                        else if (ImGui::BeginTable("Type selection", 2,
                                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Id");
                            ImGui::TableHeadersRow();
                            for (size_t i = 0; i < source.species.size(); ++i) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::PushID(int(i));
                                bool checked = std::find(types.begin(), types.end(), uint32_t(i)) !=
                                               types.end();
                                if (ImGui::Checkbox(source.species[i].c_str(), &checked)) {
                                    if (checked)
                                        types.push_back(uint32_t(i));
                                    else
                                        types.erase(std::remove(types.begin(), types.end(),
                                                                uint32_t(i)),
                                                    types.end());
                                    changed = true;
                                }
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("%zu", i + 1);
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        if (changed) {
                            checkpoint();
                            m.selectedTypes = types;
                            m.type = types.empty() ? -1 : int(types.front());
                            update();
                        }
                    }
                    if (m.op == Op::ColorType)
                        ImGui::TextWrapped("Particles use their type colors from Particle appearance settings.");
                    if (m.op == Op::AssignColor) {
                        auto color=m.assignColor;
                        if (ImGui::ColorEdit3("Assigned color",color.data())) {
                            checkpoint(); m.assignColor=color; update();
                        }
                        ImGui::TextWrapped("Colors currently selected particles. If the selection is empty, colors all particles.");
                    }
                    if (m.op == Op::RemoveProperty) {
                        char property[256]{};
                        snprintf(property, sizeof(property), "%s", m.property.c_str());
                        if (ImGui::InputText("Property name", property, sizeof(property), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = property; update();
                        }
                        ImGui::TextWrapped("Enter the exact scalar or vector property name and press Enter. Position and particle types are retained.");
                    }
                    if (m.op == Op::ColorCoding) {
                        bool changed = false;
                        auto edited = m;
                        heading("Operate on");
                        ImGui::BeginDisabled();
                        bool operateParticles = true;
                        ImGui::Checkbox("Particles", &operateParticles);
                        ImGui::EndDisabled();
                        auto property = edited.property;
                        if (colorPropertyCombo("Property", property, true)) {
                            edited.property = std::move(property); edited.colorAutoRange = true;
                            edited.colorAllFramesRange = false; changed = true;
                        }
                        recordUiTestItem(std::string("pipeline.color-property.")+m.id, "Property");
                        int gradient = edited.colorGradient;
                        if (ImGui::Combo("Gradient", &gradient, "Rainbow\0Blue-White-Red\0Cyclic Rainbow\0Fast\0Grayscale\0Hot\0Jet\0Magma\0Viridis\0Plasma\0")) { edited.colorGradient = gradient; changed = true; }
                        recordUiTestItem(std::string("pipeline.color-gradient.")+m.id, "Gradient");
                        { // Gradient bar preview between the Start and End fields, like OVITO.
                            ImDrawList *drawList = ImGui::GetWindowDrawList();
                            const ImVec2 barTopLeft = ImGui::GetCursorScreenPos();
                            const float barWidth = ImGui::GetContentRegionAvail().x;
                            const float barHeight = U(44);
                            const int segments = 32;
                            for (int segment = 0; segment < segments; ++segment) {
                                const auto color = sampleColorGradient(
                                    edited.colorGradient,
                                    (float(segment) + .5f) / float(segments));
                                const float x0 = barTopLeft.x + barWidth * float(segment) / float(segments);
                                const float x1 = barTopLeft.x + barWidth * float(segment + 1) / float(segments);
                                drawList->AddRectFilled({x0, barTopLeft.y}, {x1, barTopLeft.y + barHeight},
                                                        IM_COL32(uint8_t(std::clamp(color[0], 0.f, 1.f) * 255.f),
                                                                 uint8_t(std::clamp(color[1], 0.f, 1.f) * 255.f),
                                                                 uint8_t(std::clamp(color[2], 0.f, 1.f) * 255.f), 255));
                            }
                            ImGui::Dummy({barWidth, barHeight});
                        }
                        // P12: START/END scrub bars over the same range state
                        // (design RANGE group). Scrubbing in automatic mode
                        // switches the node to a manual range.
                        {
                            double plo = 0, phi = 0;
                            const bool hasPR = propertyValueRange(result.data, edited.property, plo, phi);
                            double sMin = -1e9, sMax = 1e9;
                            if (hasPR) {
                                const double pad = (phi - plo) * .1 + 1e-6;
                                sMin = plo - pad;
                                sMax = phi + pad;
                            }
                            if (edited.colorAutoRange) {
                                ImGui::BeginDisabled(!hasPR);
                                double low = hasPR ? plo : 0, high = hasPR ? phi : 1;
                                if (scrubBar("##Color start", &low, sMin, sMax, "%.4g", nullptr,
                                             0, "START") |
                                    scrubBar("##Color end", &high, sMin, sMax, "%.4g", nullptr,
                                             0, "END")) {
                                    edited.colorMin = float(low);
                                    edited.colorMax = float(high);
                                    edited.colorAutoRange = false;
                                    edited.colorAllFramesRange = false;
                                    changed = true;
                                }
                                ImGui::EndDisabled();
                                if (!hasPR)
                                    ImGui::TextDisabled("The property has no finite values on this frame.");
                            } else {
                                double low = edited.colorMin, high = edited.colorMax;
                                if (scrubBar("##Color start", &low, sMin, sMax, "%.4g", nullptr,
                                             0, "START")) {
                                    edited.colorMin = float(low);
                                    edited.colorAllFramesRange = false;
                                    changed = true;
                                }
                                if (scrubBar("##Color end", &high, sMin, sMax, "%.4g", nullptr,
                                             0, "END")) {
                                    edited.colorMax = float(high);
                                    edited.colorAllFramesRange = false;
                                    changed = true;
                                }
                            }
                        }
                        bool automatic = edited.colorAutoRange;
                        if (ImGui::Checkbox("Automatic range", &automatic)) { edited.colorAutoRange = automatic; changed = true; }
                        bool symmetric = edited.colorSymmetricRange;
                        if (ImGui::Checkbox("Symmetric range", &symmetric)) { edited.colorSymmetricRange = symmetric; changed = true; }
                        bool discrete = edited.colorDiscrete;
                        if (ImGui::Checkbox("Discretize", &discrete)) { edited.colorDiscrete = discrete; changed = true; }
                        double rangeLo = 0, rangeHi = 0;
                        const bool hasCurrentRange = propertyValueRange(result.data, edited.property, rangeLo, rangeHi);
                        ImGui::BeginDisabled(!hasCurrentRange);
                        if (ImGui::Button("Adjust range")) {
                            checkpoint();
                            edited.colorMin = float(rangeLo);
                            edited.colorMax = float(rangeHi);
                            edited.colorAutoRange = false;
                            edited.colorAllFramesRange = false;
                            m.colorMin = edited.colorMin;
                            m.colorMax = edited.colorMax;
                            m.colorAutoRange = edited.colorAutoRange;
                            m.colorAllFramesRange = edited.colorAllFramesRange;
                            update();
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::BeginDisabled(colorRangeRunning || busy || indexing || pipelineBusy || frames.empty());
                        if (ImGui::Button("Adjust range (all frames)"))
                            computeColorRangeAllFrames(modifierGraph.selected);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button("Reverse range")) { edited.colorReverse = !edited.colorReverse; changed = true; }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Reverses the gradient mapping between the range endpoints.");
                        recordUiTestItem(std::string("pipeline.color-reverse.")+m.id, "Reverse range");
                        bool selectedOnly = edited.colorSelectedOnly;
                        if (ImGui::Checkbox("Color only selected elements", &selectedOnly)) { edited.colorSelectedOnly = selectedOnly; changed = true; }
                        bool keepSelection = edited.colorKeepSelection;
                        if (ImGui::Checkbox("Keep selection", &keepSelection)) { edited.colorKeepSelection = keepSelection; changed = true; }
                        bool showLegend = edited.colorLegend;
                        if (ImGui::Checkbox("Show color legend", &showLegend)) {
                            checkpoint(); m.colorLegend = showLegend; colorLegend = showLegend;
                        }
                        if (colorRangeRunning) {
                            ImGui::ProgressBar(colorRangeProgress.load(), ImVec2(-1, 0), "Scanning trajectory");
                            if (ImGui::Button("Cancel range calculation")) colorRangeCancel = true;
                        } else if (m.colorAllFramesRange) {
                            ImGui::TextDisabled("Range: %.6g to %.6g across %zu frames", m.colorMin, m.colorMax, frames.size());
                            if (ImGui::SmallButton("Use current-frame automatic range")) {
                                checkpoint(); m.colorAllFramesRange = false; m.colorAutoRange = true; update();
                            }
                        }
                        if (changed) {
                            checkpoint();
                            m.property = std::move(edited.property);
                            m.colorGradient = edited.colorGradient;
                            m.colorAutoRange = edited.colorAutoRange;
                            m.colorSymmetricRange = edited.colorSymmetricRange;
                            m.colorReverse = edited.colorReverse;
                            m.colorDiscrete = edited.colorDiscrete;
                            m.colorSelectedOnly = edited.colorSelectedOnly;
                            m.colorKeepSelection = edited.colorKeepSelection;
                            m.colorMin = edited.colorMin; m.colorMax = edited.colorMax;
                            m.colorAllFramesRange = edited.colorAllFramesRange;
                            update();
                        }
                    }
                    if (m.op == Op::CommonNeighborAnalysis || m.op == Op::CreateBonds) {
                        if (m.op == Op::CreateBonds) {
                            heading("Operate on");
                            ImGui::BeginDisabled();
                            bool operateParticles = true, operateBonds = true;
                            ImGui::Checkbox("Particles", &operateParticles);
                            ImGui::Checkbox("Bonds", &operateBonds);
                            ImGui::EndDisabled();
                            ImGui::SeparatorText("Creation mode");
                            ImGui::BeginDisabled();
                            int creationMode = 0;
                            ImGui::Combo("Creation mode", &creationMode, "by cutoff distance\0");
                            ImGui::EndDisabled();
                            float bondCutoff = m.value;
                            if (m.bondTypeCutoffsEnabled) {
                                ImGui::BeginDisabled();
                                ImGui::DragFloat("Cutoff radius", &bondCutoff, .01f, .0001f, 100000.f, "%.5g");
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("The type-pair cutoff table controls bond creation");
                            } else if (ImGui::DragFloat("Cutoff radius", &bondCutoff, .01f, .0001f, 100000.f, "%.5g")) {
                                checkpoint(); m.value = bondCutoff; update();
                            }
                            recordUiTestItem(std::string("pipeline.bonds-cutoff.")+m.id, "Cutoff radius");
                            if (result.data.atoms.size() > 10000 && m.value <= 0)
                                ImGui::TextWrapped("Large structure: choose a small cutoff to create bonds. Atom rendering remains GPU accelerated.");
                            ImGui::SeparatorText("Options");
                            bool discard = m.discardExistingBonds;
                            if (ImGui::Checkbox("Discard existing bonds", &discard)) {
                                checkpoint(); m.discardExistingBonds = discard; update();
                            }
                            ImGui::TextDisabled("Checked: input bonds are removed before new cutoff bonds are created.");
                            double lowerCutoff = m.bondLowerCutoff;
                            if (ImGui::InputDouble("Lower cutoff", &lowerCutoff, 0, 0, "%.5g")) {
                                checkpoint();
                                m.bondLowerCutoff = std::max(0.0, lowerCutoff);
                                update();
                            }
                            ImGui::TextDisabled("Pairs closer than the lower cutoff are not bonded.");
                            ImGui::TextDisabled("%zu bonds.", result.data.bonds.size());
                            ImGui::SeparatorText("New bond type");
                            ImGui::BeginDisabled();
                            int newBondType = 0;
                            ImGui::Combo("Bond type", &newBondType, "Default\0");
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip("Not implemented yet");
                            ImGui::SeparatorText("Bonds display");
                            const size_t typeCount=result.data.species.size();
                            if (typeCount>0 && typeCount<=32) {
                                bool usePairCutoffs=m.bondTypeCutoffsEnabled;
                                if (ImGui::Checkbox("Use type-pair cutoffs",&usePairCutoffs)) {
                                    checkpoint();
                                    m.bondTypeCutoffsEnabled=usePairCutoffs;
                                    if (usePairCutoffs)
                                        m.bondTypeCutoffs.assign(typeCount*typeCount,m.value);
                                    update();
                                }
                                if (m.bondTypeCutoffsEnabled) {
                                    if (m.bondTypeCutoffs.size()!=typeCount*typeCount) {
                                        ImGui::TextColored({1.f,.62f,.18f,1.f},"Type table changed; reset pair cutoffs to continue.");
                                        if (ImGui::Button("Reset type-pair cutoffs")) {
                                            checkpoint(); m.bondTypeCutoffs.assign(typeCount*typeCount,m.value); update();
                                        }
                                    } else if (ImGui::BeginTable("Bond type-pair cutoffs",int(typeCount+1),
                                               ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollX,
                                               {0,U(180)})) {
                                        ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,U(90));
                                        for (const auto &name:result.data.species)
                                            ImGui::TableSetupColumn(name.c_str(),ImGuiTableColumnFlags_WidthFixed,U(96));
                                        ImGui::TableHeadersRow();
                                        for (size_t a=0;a<typeCount;++a) {
                                            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                                            ImGui::TextUnformatted(result.data.species[a].c_str());
                                            for (size_t b=0;b<typeCount;++b) {
                                                ImGui::TableSetColumnIndex(int(b+1));
                                                if (b<a) { ImGui::TextDisabled("same"); continue; }
                                                float pair=m.bondTypeCutoffs[a*typeCount+b];
                                                ImGui::PushID(int(a*typeCount+b));
                                                ImGui::SetNextItemWidth(-1);
                                                if (ImGui::DragFloat("##cutoff",&pair,.01f,0.f,100000.f,"%.4g")) {
                                                    checkpoint();
                                                    m.bondTypeCutoffs[a*typeCount+b]=pair;
                                                    m.bondTypeCutoffs[b*typeCount+a]=pair;
                                                    update();
                                                }
                                                ImGui::PopID();
                                            }
                                        }
                                        ImGui::EndTable();
                                    }
                                    ImGui::TextDisabled("Symmetric pair matrix; zero disables that type pair.");
                                }
                            } else ImGui::TextDisabled(typeCount==0 ? "Load particle types to configure bonds." :
                                                       "Type-pair mode supports at most 32 particle types.");
                            bool visible = m.bondsVisible;
                            bool cylinders = m.bondCylinders;
                            bool colorByType = m.bondColorByType;
                            bool showPeriodicImages = m.bondShowPeriodicImages;
                            float width = m.bondWidth;
                            float bondRadius = m.bondRadius;
                            auto color = m.bondColor;
                            bool changed = ImGui::Checkbox("Show bonds", &visible);
                            if (result.data.bonds.size() > atomx::interactiveBondBudget)
                                ImGui::TextWrapped("Bond display paused: this dataset exceeds the interactive bond budget.");
                            else if (result.data.bonds.size() > atomx::cylinderBondBudget && cylinders)
                                ImGui::TextWrapped("Fast line preview is active for this bond count.");
                            const char *representations[] = {"Screen-space lines", "3D cylinders"};
                            int representation = cylinders ? 1 : 0;
                            if (ImGui::Combo("Bond representation", &representation, representations, 2)) {
                                cylinders = representation == 1;
                                changed = true;
                            }
                            if (cylinders)
                                changed |= ImGui::DragFloat("Cylinder radius", &bondRadius, .005f,
                                                            .001f, 100.f, "%.4g units");
                            else
                                changed |= ImGui::SliderFloat("Line width", &width, .5f, 12.f, "%.1f px");
                            changed |= ImGui::Checkbox("Color by particle type", &colorByType);
                            changed |= ImGui::Checkbox("Show periodic boundary bonds", &showPeriodicImages);
                            if (!colorByType)
                                changed |= ImGui::ColorEdit3("Bond color", color.data());
                            if (changed) {
                                checkpoint(); m.bondsVisible=visible; m.bondCylinders=cylinders;
                                m.bondWidth=width; m.bondRadius=bondRadius; m.bondColor=color;
                                m.bondColorByType=colorByType;
                                m.bondShowPeriodicImages=showPeriodicImages; update();
                            }
                        }
                        else {
                            size_t counts[5]{};
                            if (auto codes = result.data.scalarProperties.find("Structure Type");
                                codes != result.data.scalarProperties.end())
                                for (double value : codes->second)
                                    if (value >= 0 && value < 5) ++counts[size_t(value)];
                            ImGui::TextDisabled("FCC %zu | HCP %zu | BCC %zu | ICO %zu | Other %zu",
                                                counts[1], counts[2], counts[3], counts[4], counts[0]);
                        }
                    }
                    if (m.op == Op::ExpandSelection) {
                        int mode = m.expandMode;
                        bool changed = false;
                        ImGui::Text("Expansion mode");
                        changed |= ImGui::RadioButton("...within the range of: cutoff distance",
                                                      &mode, 0);
                        if (mode == 0) {
                            float cutoffValue = m.value;
                            ImGui::SetNextItemWidth(U(140));
                            if (ImGui::DragFloat("Cutoff distance", &cutoffValue, .01f, .0001f,
                                                 100000.f, "%.5g")) {
                                m.value = cutoffValue;
                                changed = true;
                            }
                        }
                        changed |= ImGui::RadioButton("...among the N nearest neighbors", &mode, 1);
                        if (mode == 1) {
                            int neighborCount = m.expandNeighbors;
                            if (ImGui::InputInt("N", &neighborCount)) {
                                m.expandNeighbors = std::clamp(neighborCount, 1, 100000);
                                changed = true;
                            }
                        }
                        changed |= ImGui::RadioButton("...bonded to a selected particle", &mode, 2);
                        changed |= ImGui::RadioButton("...of the same molecule", &mode, 3);
                        if ((mode == 2 || mode == 3) && result.data.bonds.empty())
                            ImGui::TextColored({1.f, .62f, .18f, 1.f},
                                               "Expand selection requires an existing bond topology; add Create bonds upstream.");
                        ImGui::Text("Iteration settings");
                        int iterations = m.type;
                        if (ImGui::InputInt("Number of iterations", &iterations)) {
                            m.type = std::clamp(iterations, 1, 64);
                            changed = true;
                        }
                        size_t selectedParticles =
                            std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        ImGui::TextDisabled("Selected: %zu / %zu", selectedParticles,
                                            result.data.atoms.size());
                        if (changed) {
                            checkpoint();
                            m.expandMode = mode;
                            update();
                        }
                    }
                    if (m.op == Op::SelectOverlapping) {
                        const size_t selectedParticles = std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        ImGui::TextDisabled("Overlapping particles: %zu", selectedParticles);
                    }
                    if (m.op == Op::ExpressionSelect) {
                        char expression[512]{};
                        snprintf(expression, sizeof(expression), "%s", m.property.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Expression", expression, sizeof(expression), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = expression; update();
                        }
                        ImGui::TextWrapped("Use x/y/z, Position.X/Y/Z, type, scalar properties, arithmetic, comparisons, &&, ||, !, abs(), sqrt(), isfinite(). Write `Potential Energy` to reference a property name containing spaces; double a backtick to escape it. Press Enter to evaluate.");
                        size_t selectedParticles = std::count(result.selected.begin(), result.selected.end(), uint8_t(1));
                        ImGui::TextDisabled("Selected: %zu / %zu", selectedParticles, result.data.atoms.size());
                    }
                    if (m.op == Op::ComputeProperty) {
                        char expression[512]{}, outputName[128]{};
                        snprintf(expression, sizeof(expression), "%s", m.property.c_str());
                        snprintf(outputName, sizeof(outputName), "%s", m.outputProperty.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Output property", outputName, sizeof(outputName), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.outputProperty = outputName; update();
                        }
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("Expression", expression, sizeof(expression), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            checkpoint(); m.property = expression; update();
                        }
                        ImGui::TextWrapped("Publishes one scalar value per particle. Use the safe expression language; press Enter to evaluate.");
                    }
                    ImGui::PopID();
                }
                heading("Scene visibility");
                ImGui::Checkbox("##particles-visible", &particles);
                ImGui::SameLine();
                if (ImGui::Button("Particles")) focusParticleAppearance = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit particle shape, radius and type colors");
                ImGui::SameLine();
                ImGui::Checkbox("Simulation cell", &cell);
                if (cell) {
                    ImGui::Combo("Cell dimensionality", &cellDimension, "2D\0" "3D\0");
                    ImGui::ColorEdit4("Cell line color", cellColor, ImGuiColorEditFlags_AlphaBar);
                    if (scrubBar("##Cell line width scene", &cellWidth, .5f, 4.f, "%.1f", "px")) {}
                    ImGui::SliderFloat("Glow", &cellGlow, 0.f, 1.f, "%.2f");
                    ImGui::Checkbox("Dashed secondary edges", &cellDashed); ImGui::Checkbox("Show cell labels", &cellLabels);
                    ImGui::TextDisabled("PBC: %s %s %s", source.pbc[0]?"X":"-", source.pbc[1]?"Y":"-", source.pbc[2]?"Z":"-");
                }
                heading("Particle appearance");
                if (focusParticleAppearance) { ImGui::SetScrollHereY(0.f); focusParticleAppearance = false; }
                if (scrubBar("##Particle radius", &radius, .02f, 2.f, "%.2f", "x")) {}
                const char *shapes = "Sphere / Ellipsoid\0Circle\0Cube / "
                                     "Box\0Cylinder\0Spherocylinder\0Square\0Mesh / User-defined\0";
                ImGui::Combo("Default shape", &particleShape, shapes);
                syncAppearance(result.data.species);
                if (!result.data.species.empty()) {
                    appearanceType =
                        std::clamp(appearanceType, 0, int(result.data.species.size()) - 1);
                    if (ImGui::BeginCombo("Particle type",
                                          result.data.species[appearanceType].c_str())) {
                        for (int i = 0; i < int(result.data.species.size()); ++i)
                            if (ImGui::Selectable(result.data.species[i].c_str(),
                                                  appearanceType == i))
                                appearanceType = i;
                        ImGui::EndCombo();
                    }
                    ImGui::PushID(result.data.species[appearanceType].c_str());
                    auto &style = gpu.styles[appearanceType];
                    bool visible = style.visual[2] > .5f;
                    if (ImGui::Checkbox("Show this type", &visible))
                        style.visual[2] = visible ? 1.f : 0.f;
                    ImGui::ColorEdit3("Type color", style.color.data());
                    if (ImGui::IsItemDeactivatedAfterEdit() && result.data.bondStyle.colorByType &&
                        !result.data.bonds.empty())
                        uploadCreationDisplay(result.data, result.selected, result.colorSelected);
                    bool inheritRadius = style.visual[0] == 0;
                    if (ImGui::Checkbox("Use default radius", &inheritRadius))
                        setDefaultRadius(result.data.species[appearanceType], style, inheritRadius);
                    if (!inheritRadius)
                        ImGui::SliderFloat("Type radius", &style.visual[0], .02f, 5.f, "%.3f");
                    else if (const auto *element =
                                 atomx::elements::find(result.data.species[appearanceType]))
                        ImGui::TextWrapped(
                            "Default radius: %.2f A (Cordero 2008 covalent radius of %s)",
                            style.visual[3], element->name);
                    int typeShape = int(style.visual[1]) + 1;
                    if (ImGui::Combo(
                            "Type shape", &typeShape,
                            "Use default\0Sphere / Ellipsoid\0Circle\0Cube / "
                            "Box\0Cylinder\0Spherocylinder\0Square\0Mesh / User-defined\0"))
                        style.visual[1] = float(typeShape - 1);
                    int effective = style.visual[1] < 0 ? particleShape : int(style.visual[1]);
                    if (effective == 0 || effective == 2 || effective == 6)
                        ImGui::SliderFloat3("Axis scales", style.axes.data(), .1f, 4.f, "%.2f");
                    else if (effective == 3 || effective == 4)
                        ImGui::SliderFloat("Half-length / radius", &style.axes[2], .1f, 6.f,
                                           "%.2f");
                    else {
                        ImGui::SliderFloat("Width scale", &style.axes[0], .1f, 4.f);
                        ImGui::SliderFloat("Height scale", &style.axes[1], .1f, 4.f);
                    }
                    ImGui::TextWrapped(
                        effective == 3 || effective == 4
                            ? "Cylinder axis: world Z. End caps and depth are rendered in 3D."
                            : "Type appearance overrides the particle defaults. Selection and "
                              "color coding may override type colors.");
                    if (effective == 6) {
                        ImGui::TextWrapped(
                            "Shared particle OBJ: triangulated, up to 256 triangles; centered and "
                            "normalized to unit radius. High mesh counts cost more GPU time.");
                        ImGui::Text("Loaded triangles: %zu", gpu.meshTriangleCount);
                        if (ImGui::Button("Load particle OBJ...")) {
                            auto p = dialog(window, false, L"Triangle mesh\0*.obj\0", L"obj");
                            if (!p.empty())
                                try {
                                    gpu.loadParticleMesh(p);
                                } catch (const std::exception &e) {
                                    error = e.what();
                                }
                        }
                    }
                    ImGui::PopID();
                }
                if (colorCoding) {
                    heading("ACTIVE COLOR CODING");
                    auto activeColor = std::find_if(mods.rbegin(), mods.rend(), [](const Modifier &m) {
                        return m.enabled && m.op == Op::ColorCoding;
                    });
                    if (activeColor != mods.rend()) {
                        ImGui::Text("Property: %s",activeColor->property.c_str());
                        const float shownMin=activeColor->colorAutoRange && !activeColor->colorAllFramesRange
                            ? colorMin : activeColor->colorMin;
                        const float shownMax=activeColor->colorAutoRange && !activeColor->colorAllFramesRange
                            ? colorMax : activeColor->colorMax;
                        ImGui::Text("Range: %.6g to %.6g%s",shownMin,shownMax,
                                    activeColor->colorAutoRange ? " (automatic)" : " (manual)");
                        ImGui::Text("Gradient: %s",activeColor->colorGradient>=0 && activeColor->colorGradient<10
                            ? std::array<const char *,10>{"Rainbow","Blue-White-Red","Cyclic Rainbow","Fast",
                                "Grayscale","Hot","Jet","Magma","Viridis","Plasma"}[activeColor->colorGradient]
                            : "Custom");
                        ImGui::Text("%s%s%s",activeColor->colorSelectedOnly ? "Selected only  " : "All particles  ",
                                    activeColor->colorDiscrete ? "Discrete  " : "Continuous  ",
                                    activeColor->colorReverse ? "Reversed" : "");
                        ImGui::TextDisabled("Edit coloring parameters on the selected Color coding node in Pipeline.");
                    }
                }
                heading("Position statistics");
                ImGui::Combo("Property", &propertyAxis, "Position X\0Position Y\0Position Z\0");
                auto s = cachedStats[propertyAxis];
                ImGui::PlotHistogram("##hist", s.histogram.data(), 64, 0, nullptr, 0, FLT_MAX,
                                     {-1, 80});
                ImGui::Text("Min %.3f   Max %.3f", s.min, s.max);
                ImGui::Text("Mean %.3f", s.mean);
                if (source.sampled())
                    ImGui::TextColored({.9f, .73f, .42f, 1}, "Statistics describe preview only.");
            }
            if (rightTab == 1) {
                heading("GPU RENDERER");
                ImGui::TextWrapped("%s", utf8(gpu.adapterName).c_str());
                ImGui::Combo("Renderer", &renderMode, "Standard GPU\0Wireframe GPU\0Flat particle preview\0Cinematic GPU preview\0");
                ImGui::TextDisabled(renderMode==0 ? "Direct3D 11 / shaded impostors" : renderMode==1 ? "Direct3D 11 / wireframe raster" : "Direct3D 11 / fast preview mode");
                ImGui::Text("Atom buffers: %.1f MiB", gpu.gpuBytes / 1048576.);
                heading("CAMERA");
                ImGui::Combo("View", &cameras[active].mode, views);
                heading("OUTPUT");
                ImGui::InputInt("Width", &exportW);
                ImGui::InputInt("Height", &exportH);
                exportW = std::clamp(exportW, 64, 8192);
                exportH = std::clamp(exportH, 64, 8192);
                ImGui::ColorEdit3("Background", bg);
                ImGui::TextWrapped("PNG exports particles from the active camera. UI cell overlay "
                                   "is not included.");
                if (ImGui::Button("Render active viewport", {-1, U(35)}))
                    exportImage();
                heading("TRAJECTORY");
                ImGui::SliderFloat("Frames/sec", &fps, 1, 60, "%.0f");
                heading("EXPORT DATA");
                ImGui::TextWrapped(source.sampled()
                                       ? "Exports the processed preview, not all source atoms."
                                       : "Exports the processed particle dataset.");
                ImGui::BeginDisabled(exporting || busy);
                if (ImGui::Button("Export data...", {-1, U(32)})) {
                    showDataExport = true;
                    exportFirst = current;
                    exportLast = std::max(0, int(frames.size()) - 1);
                }
                ImGui::EndDisabled();
                if (exporting) {
                    ImGui::ProgressBar(exportProgress);
                    if (ImGui::Button("Cancel export"))
                        exportCancel = true;
                }
            }
            if (rightTab == 2) {
                heading("DISLOCATION ANALYSIS (DXA)");
                ImGui::TextWrapped("Runs a coordination-based DXA prepass and reports candidate defect-core atoms. Full Burgers circuit tracing and a dislocation mesh are planned for the next DXA stage.");
                ImGui::InputFloat("DXA cutoff", &dxaCutoff, .05f, .1f, "%.3f");
                if (ImGui::Button("Run DXA prepass", {-1, U(32)}) && !dxaRunning) {
                    auto d=result.data; auto c=dxaCutoff; auto selected=result.selected;
                    dxaRunning=true; dxaJob=std::async(std::launch::async,[d=std::move(d),c,selected=std::move(selected)](){return dxaApproximate(d,c,false,&selected);});
                }
                if (dxaRunning) ImGui::TextDisabled("Analyzing neighbor coordination...");
                if (dxa) {
                    ImGui::Text("Analyzed: %llu", dxa->analyzed); ImGui::Text("Candidate core atoms: %llu", dxa->defectAtoms);
                    for (auto& [name,count] : dxa->structures) ImGui::Text("%s: %llu", name.c_str(), count);
                    ImGui::TextWrapped("%s", dxa->message.c_str());
                    if (ImGui::Button("Export DXA prepass CSV", {-1,U(30)})) { auto p=dialog(window,true,L"CSV file\0*.csv\0",L"csv"); if(!p.empty()){std::ofstream out(p);out<<"atom_index,coordination\n"; auto n=neighbors(result.data,dxaCutoff); for(auto i:dxa->coreAtoms)out<<i<<","<<n.coordination[i]<<"\n"; status="DXA prepass exported";} }
                }
                heading("NEIGHBOR ANALYSIS");
                ImGui::TextWrapped("Coordination number and connected clusters using spatial bins "
                                   "and minimum-image periodic distances.");
                ImGui::InputFloat("Cutoff", &cutoff, .05f, .1f, "%.3f");
                if (ImGui::Button("Compute neighbors", {-1, U(32)}) && !analyzing) {
                    analysisRevision = revision;
                    auto d = result.data;
                    auto c = cutoff;
                    analyzing = true;
                    analysisJob = std::async(std::launch::async, [d = std::move(d), c, this] {
                        return neighbors(d, c, &cancel);
                    });
                }
                if (analyzing)
                    ImGui::TextDisabled("Computing in background...");
                if (analysis) {
                    ImGui::Text("Clusters: %u", analysis->clusters);
                    ImGui::Text("Neighbor pairs: %llu", analysis->bonds);
                    ImGui::Text("Mean coordination: %.4f", analysis->meanCoordination);
                    ImGui::TextDisabled("Neighbor distances (0 to %.3f)", analysis->cutoff);
                    std::vector<float> histogram(analysis->pairHistogram.size());
                    std::transform(analysis->pairHistogram.begin(),analysis->pairHistogram.end(),histogram.begin(),
                                   [](uint64_t value){return float(value);});
                    ImGui::PlotHistogram("##distances", histogram.data(),int(histogram.size()),0,nullptr,0,FLT_MAX,{-1,75});
                    if (analysis->rdfValid) {
                        std::vector<float> rdf(analysis->rdf.size());
                        std::transform(analysis->rdf.begin(),analysis->rdf.end(),rdf.begin(),
                                       [](double value){return float(value);});
                        ImGui::TextUnformatted("Radial distribution g(r)");
                        ImGui::PlotLines("##rdf",rdf.data(),int(rdf.size()),0,nullptr,0,FLT_MAX,{-1,75});
                    } else ImGui::TextWrapped("Bulk RDF requires a valid fully periodic 3D cell.");
                    if (ImGui::Button("Export distributions CSV", {-1, U(30)})) {
                        auto p = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                        if (!p.empty()) {
                            std::ofstream out(p); out << "r_min,r_max,pair_count,g_r\n";
                            for (size_t i=0;i<analysis->pairHistogram.size();++i) {
                                out << analysis->cutoff*i/analysis->pairHistogram.size() << ','
                                    << analysis->cutoff*(i+1)/analysis->pairHistogram.size()
                                    << ',' << analysis->pairHistogram[i] << ',';
                                if (analysis->rdfValid) out << analysis->rdf[i];
                                out << '\n';
                            }
                            if (!out) throw std::runtime_error("Cannot write distributions CSV");
                        }
                    }
                    if (ImGui::Button("Export analysis CSV", {-1, U(32)})) {
                        auto p = dialog(window, true, L"CSV file\0*.csv\0", L"csv");
                        if (!p.empty()) {
                            std::ofstream out(p);
                            out << "index,coordination,cluster\n";
                            for (size_t i = 0; i < analysis->coordination.size(); i++)
                                out << i << "," << analysis->coordination[i] << ","
                                    << analysis->cluster[i] << "\n";
                            if (!out)
                                throw std::runtime_error("Cannot write analysis CSV");
                            status = "Analysis exported";
                        }
                    }
                }
                ImGui::TextWrapped("Requires unsampled data. Periodic neighbor search supports orthogonal and triclinic cells. "
                                   "Current analysis limit: 2 million atoms.");
            }
            if (rightTab == 3) {
                heading("AVAILABLE ADAPTERS");
                for (auto a : gpu.adapters) {
                    if (a.duplicate) continue;
                    ImGui::TextWrapped("[%u] %s%s", a.index, utf8(a.name).c_str(),
                                       a.active ? "  • IN USE" : "");
                    ImGui::TextDisabled("Dedicated memory: %.2f GiB%s", a.memory / 1073741824.,
                                        a.active ? "  |  Active renderer" : "");
                }
                ImGui::TextDisabled("DXGI aliases are grouped by adapter LUID; distinct devices keep their own index.");
                ImGui::TextWrapped("Select at launch: AtomX.exe --adapter N");
                heading("MEMORY BUDGET");
                ImGui::InputInt("Preview atoms", &budget, 100000, 1000000);
                budget = std::clamp(budget, 1000, 20000000);
                ImGui::TextWrapped(
                    "Files larger than this budget use deterministic stride sampling. All records "
                    "are scanned; only preview atoms are retained.");
                if (ImGui::Button("Reload with budget", {-1, U(32)}))
                    load(path, current);
                heading("ENGINE");
                ImGui::BulletText("16 bytes / displayed atom");
                ImGui::BulletText("1,048,576 atoms / GPU chunk");
                ImGui::BulletText("64-bit file offsets and counts");
                ImGui::BulletText("Background trajectory import");
                ImGui::TextWrapped(
                    "Hundreds of millions of full-resolution atoms and advanced OVITO analysis are "
                    "development targets, not validated capabilities.");
            }
        panelStyle.release();
        ImGui::End();
    }
    void rebuildFont() {
        if (!refreshFont) return;
        refreshFont = false;
        ImGui_ImplDX11_InvalidateDeviceObjects();
        auto &io = ImGui::GetIO();
        io.Fonts->Clear();
        const char *fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/consola.ttf"};
        ImFont *base = io.Fonts->AddFontFromFileTTF(fonts[preferences.font], float(preferences.size)*uiScale);
        if (!base) base = io.Fonts->AddFontDefault();
        // Merge the Windows Segoe MDL2 Assets icon set into the base font so a
        // single ImFont renders text and icons. Missing system font degrades to
        // the historical text buttons; the app stays fully usable either way.
        iconFont = nullptr;
        iconFontName = "text fallback";
        const float iconSize = U(15.5f);
        for (const char *iconFile : {"C:/Windows/Fonts/segmdl2.ttf",
                                     "C:/Windows/Fonts/SegoeMDL2.ttf",
                                     "C:/Windows/Fonts/segfluent.ttf"}) {
            ImFontConfig merge{};
            merge.MergeMode = true;
            merge.GlyphMinAdvanceX = iconSize;
            merge.GlyphOffset = {0, U(.5f)};
            if (io.Fonts->AddFontFromFileTTF(iconFile, iconSize, &merge, iconGlyphRanges)) {
                iconFont = base;
                iconFontName = "Segoe MDL2 Assets";
                break;
            }
        }
        // Merge Chinese button glyphs into the base font while it is still
        // the atlas's active destination. The heading font is added later.
        ImFontGlyphRangesBuilder chineseBuilder;
        chineseBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        chineseBuilder.AddText("测量模拟约束选择已有可同时修改多种类型全选清空测量选择已选未选择当前值随坐标更新约束随测量保存在中标准结构导出不保留测量移除测量或删除其端点会同时移除该约束体系或测量已改变请重新打开测量类型无效测量选择已改变测量约束选项无效");
        chineseBuilder.AddText("原子模拟约束整原子固定当前晶格方向保持各原子当前值解除固定笛卡尔同时部分轴及其旋转后的方向约束尚未支持分数分量固定全部解除全部晶格方向需要有效的三维周期晶胞这些是属性手工移动与旋转仍可使用不执行模拟求解保持当前值可保留多选中的不同约束保存两类保存晶格方向整原子笛卡尔固定需保存在中导出时须显式取消约束保留没有变化标志必须为零或一属性类型不正确层构建会重设晶格基矢请先解除来源的晶格方向约束片段库不保留来源晶格方向");
        chineseBuilder.AddText("创作模式文件编辑视图修改构建工具基于结构氯化钠岩盐组成点击选中同元素晶格体积空间群布拉维格子面心立方原子数密度历史回到该步从新撤销重做旋转缩放右键更多操作拖动空白处框选删除晶体超胞切面真空中键滚轮单位转换原胞对称操作识别失败三维周期晶面重排沿法向添加关闭边界编号须标准设置参数相容基元无效");
        chineseBuilder.AddText("精准移动旋转选中原子的几何中心不移动晶胞屏幕轴体系轴距离视图比例步长角度以打开窗口时视图的较短边为选中中心所在深度上下左右向里向外右手方向反向使用负角度坐标未改变已记录一步历史原标签已改变请关闭并重新打开位移与角度必须为有限数值旋转轴不能为零选中原子已改变请重新选择原子坐标无效位移超出坐标范围连接片段分数坐标切换添加约束←→↑↓−°Å");
        chineseBuilder.AddText("隐藏选中仅显示选中显示全部已隐藏原子标签对象默认显示元素和编号整个体系内容无标签笛卡尔坐标分数坐标自定义文字字号粗体颜色显示标签屏幕标签上限大体系自动抽样优先显示选中原子的标签隐藏原子不显示标签上限默认应用移除移除全部编辑原子标签撤销重做");
        chineseBuilder.AddText(creationDisplay.defaultLabel.text.c_str());
        chineseBuilder.AddText(creationDisplay.bondLabels.defaultLabel.text.c_str());
        chineseBuilder.AddText("键标签对象显式键数值跟随当前坐标键级局部两端原子都在打开窗口时的选择中作用范围条键属性按勾选顺序组合键长端点编号中点周期像显示大体系抽样隐藏不标注已改变请关闭后重新打开编辑移除全部规则无效超过缩小选择应用到整个体系");
        for(const auto &[key,label]:creationDisplay.bondLabels.labels) { (void)key;chineseBuilder.AddText(label.text.c_str()); }
        chineseBuilder.AddText("属性组合可组合至多项按勾选顺序排列元素名称原子序数质量列优先没有该列时用元素质量缺失值为清空属性有效数字");
        chineseBuilder.AddText("原子与半键着色来源颜色元素颜色自定义颜色属性渐变分类编号着色方式自定义色按当前范围调整正在后台读取范围已更新范围属性没有有限数值下限上限渐变反转颜色段显示全体系图例缺失值为灰色颜色随数据更新范围仅手动调整应用着色来源颜色保留文件配色元素颜色使用当前元素外观着色不修改科学属性或键拓扑非负整数编号保持固定颜色可选择运动分组或层编号其他值为灰色编辑原子着色");
        chineseBuilder.AddText(creationDisplay.defaultColor.property.c_str());
        for(const auto &field:creationDisplay.defaultLabel.fields) chineseBuilder.AddText(field.property.c_str());
        chineseBuilder.AddText("绘制原子与键键级单键双键三键连续成链点击已有原子吸附连键双击结束取消虚拟原子替换元素放置孤立原子拖动向里自由键长起点默认键长结束成链连接选中原子修改键级断键设置断开请选择有效元素无法连键重合坐标或键数已达上限");
        chineseBuilder.AddText("绘制碳环元芳香环大小空白处放置原子或键上接环按住拖动调整朝向松开提交取消环已存在环顶点重合请调整朝向环与隐藏原子重合请先显示原子碳环不自动加氢保存文档文档另存为");
        chineseBuilder.AddText("片段浏览器常用自定义搜索定义连接点末端接枝红圈双击更换拖动旋转右键平移滚轮缩放重置预览开始放置关闭名称库保存到正在读取本机文件已跳过无效上限氢会被替换其他保留甲基乙羟氨酰羧苯烃官能团卤素我的需要最多原子有效直接键不支持周期连通网络通过显式键未知元素长度匹配尚未加载状态改变超过无法暂存被隐藏请重新选择");
        chineseBuilder.AddText("显示样式原有外观线棒球棒整个体系半径比例范德华对应半键共享参数微小原子点便于选择大体系沿用显示预算超限自动降为省略");
        chineseBuilder.AddText("测量修改几何距离角度扭转角依次点击测量点标记空白处反向取消未定义已选顶点目标应用数值移除显示另一侧恢复按实际坐标不取周期最短距离片段约束连通路径无法独立移动最多重合须大于零超出范围 → Å ° − –");
        chineseBuilder.AddText("运动分组从选择创建按独立片段自动分组已属于取消名称改名选中整组整体移动旋转质心需属性保留原子显式键周期网络隐藏成员先显示不自动限制手工编辑正在查找连接分量暂无采样体系不能上限");
        for (const auto &group:motionGroupCache) chineseBuilder.AddText(group.name.c_str());
        for (const auto &entry:fragmentLibrary) { chineseBuilder.AddText(entry.name.c_str()); chineseBuilder.AddText(entry.category.c_str()); }
        for (const auto &[index,label]:creationDisplay.labels) { (void)index; chineseBuilder.AddText(label.text.c_str());
            for(const auto &field:label.fields) chineseBuilder.AddText(field.property.c_str()); }
        static const ImWchar extraRanges[] = {0x0370,0x03ff,0x2070,0x209f,0};
        chineseBuilder.AddRanges(extraRanges);
        ImVector<ImWchar> chineseRanges;
        chineseBuilder.BuildRanges(&chineseRanges);
        {
            ImFontConfig cjkMerge{};
            cjkMerge.MergeMode = true;
            for (const char *cjkFile : {"C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/Deng.ttf",
                                        "C:/Windows/Fonts/msyh.ttc"}) {
                if (io.Fonts->AddFontFromFileTTF(cjkFile, float(preferences.size) * uiScale,
                                                 &cjkMerge, chineseRanges.Data))
                    break;
            }
        }
        headingFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", float(preferences.size)*uiScale);
        {
            ImFontConfig headingMerge{};
            headingMerge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/simhei.ttf",
                float(preferences.size)*uiScale, &headingMerge, chineseRanges.Data);
        }
        ImGui_ImplDX11_CreateDeviceObjects();
    }
    void startDataExport(const std::filesystem::path &destination) {
        if (documentsBusy() || exporting) throw std::runtime_error("Wait for the current operation before saving");
        auto fmt = io::formats[exportFormat].id;
        auto options=exportOptions;
        if (fmt==io::Format::AtomX) {
            if (exportRange && frames.size()>1) throw std::runtime_error("AtomX documents save the current result; use a trajectory format for frame ranges");
            if (creationMode && !sameAtomCount()) throw std::runtime_error("Clear modifiers that change the atom count before saving a creation document");
            options.documentView=captureDocumentView();
        }
        bool range = exportRange && frames.size() > 1;
        int first = range ? exportFirst : current, last = range ? exportLast : current,
            step = range ? exportStep : 1;
        if (first < 0 || last < first || (range && last >= int(frames.size())) || step < 1)
            throw std::runtime_error("Invalid export frame range");
        bool sequence = range && (exportSequence || !io::info(fmt).trajectory);
        if (!(fmt==io::Format::AtomX && lowerExtension(path)==".atomx") &&
            !path.empty() && std::filesystem::exists(destination) &&
            std::filesystem::equivalent(destination, path))
            throw std::runtime_error("Choose a destination different from the input file");
        auto ext = lowerExtension(destination);
        bool valid = ext == std::string(".") + io::info(fmt).extension;
        if (fmt == io::Format::POSCAR)
            valid = isPOSCAR(destination);
        if (fmt == io::Format::XYZ)
            valid = ext == ".xyz" || ext == ".extxyz";
        if (fmt == io::Format::LammpsData)
            valid = ext == ".data" || ext == ".lmp";
        if (fmt == io::Format::LammpsDump)
            valid = ext == ".dump" || ext == ".lammpstrj";
        if (!valid)
            throw std::runtime_error(
                "Filename extension does not match the selected export format");
        exportCancel = false;
        exportProgress = 0;
        exportedDocumentPath = fmt==io::Format::AtomX?destination:std::filesystem::path{};
        exportedDocumentTab = tabs[size_t(activeTab)].id;
        exportJob = std::async(std::launch::async, [this, destination, fmt, range, sequence, first,
                                                    last, step, snapshot = result.data,
                                                    sourcePath = path, frameIndex = frames,
                                                    pipeline = modifierGraph, options = std::move(options),
                                                    atomBudget = budget,
                                                    allowPreview = exportPreview]() {
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> staged;
            auto nonce =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            auto cleanup = [&]() {
                for (const auto &[temp, target] : staged) {
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                }
            };
            try {
                int total = (last - first) / step + 1;
                for (int i = 0; i < (sequence ? total : 1); ++i) {
                    auto target = destination;
                    if (sequence) {
                        std::wostringstream n;
                        n << destination.stem().wstring() << L'_' << std::setw(6)
                          << std::setfill(L'0') << first + i * step
                          << destination.extension().wstring();
                        target = destination.parent_path() / n.str();
                        if (std::filesystem::exists(target))
                            throw std::runtime_error("Sequence target already exists: " +
                                                     utf8(target.wstring()));
                    }
                    auto temp = target;
                    temp += L".atomx-" + std::wstring(nonce.begin(), nonce.end()) + L".tmp";
                    staged.emplace_back(temp, target);
                }
                std::ofstream single;
                if (!sequence) {
                    single.open(staged[0].first, std::ios::binary);
                    if (!single)
                        throw std::runtime_error("Cannot create export file");
                }
                for (int i = 0; i < total; ++i) {
                    io::checkpoint(&exportCancel);
                    int frame = first + i * step;
                    Dataset data =
                        range ? evaluate(io::read(sourcePath, frameIndex.at(frame),
                                                  uint64_t(atomBudget), nullptr, &exportCancel),
                                         pipeline)
                                    .data
                              : snapshot;
                    if (data.sampled() && !allowPreview)
                        throw std::runtime_error(
                            "A frame exceeds the full-data budget; reload with a larger budget or "
                            "explicitly export a sampled preview");
                    if (sequence) {
                        std::ofstream file(staged[i].first, std::ios::binary);
                        if (!file)
                            throw std::runtime_error("Cannot create sequence file");
                        if (fmt==io::Format::AtomX) document::write(file,data,options.documentView,&exportCancel);
                        else io::writeFrame(file,fmt,data,options,frame);
                        file.flush();
                        if (!file)
                            throw std::runtime_error("Sequence write failed");
                    } else if (fmt==io::Format::AtomX) document::write(single,data,options.documentView,&exportCancel);
                    else io::writeFrame(single,fmt,data,options,frame);
                    exportProgress = float(i + 1) / total;
                }
                if (!sequence) {
                    single.flush();
                    if (!single)
                        throw std::runtime_error("Export write failed");
                    single.close();
                }
                io::checkpoint(&exportCancel);
                for (const auto &[temp, target] : staged)
                    if (!MoveFileExW(temp.c_str(), target.c_str(),
                                     sequence ? MOVEFILE_WRITE_THROUGH
                                              : MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error(
                            "Could not publish export file; check destination permissions");
                return std::string("Exported ") + std::to_string(total) + " frame(s) to " +
                       utf8(destination.wstring());
            } catch (...) {
                cleanup();
                throw;
            }
        });
        exporting = true;
    }
    void dataExportDialog() {
        if (showDataExport) {
            ImGui::OpenPopup("Data export settings");
            showDataExport = false;
        }
        ImGui::SetNextWindowSize({U(550), 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Data export settings", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (ImGui::BeginCombo("Format", io::formats[exportFormat].name)) {
            for (int i = 0; i < int(std::size(io::formats)); ++i)
                if (io::formats[i].writable &&
                    ImGui::Selectable(io::formats[i].name, exportFormat == i))
                    exportFormat = i;
            ImGui::EndCombo();
        }
        auto fmt = io::formats[exportFormat].id;
        ImGui::SeparatorText("Frame sequence");
        ImGui::BeginDisabled(frames.size() < 2);
        ImGui::Checkbox("Export frame range", &exportRange);
        ImGui::EndDisabled();
        if (exportRange && frames.size() > 1) {
            ImGui::InputInt("First frame", &exportFirst);
            ImGui::InputInt("Last frame", &exportLast);
            ImGui::InputInt("Every Nth frame", &exportStep);
            if (io::info(fmt).trajectory)
                ImGui::Checkbox("Separate file per frame", &exportSequence);
            else
                ImGui::TextDisabled("This format exports one file per frame.");
            ImGui::TextDisabled("Sequence naming: name_000000.ext");
        } else
            ImGui::Text("Current frame: %d", current);
        ImGui::SeparatorText("Format options");
        if (fmt==io::Format::AtomX)
            ImGui::TextWrapped("Saves the current result with cell/PBC, explicit bonds and orders, all particle properties, atom labels/visibility, particle styles and camera. Undo history, workspace snapshots and modifier steps are not stored. Open the .atomx file to continue editing.");
        else if (fmt != io::Format::GRO)
            ImGui::SliderInt("Numeric precision", &exportOptions.precision, 1, 17);
        else
            ImGui::TextWrapped("GRO uses nm, fixed-width 3 decimal coordinates, and at most 99999 "
                               "atoms. Residues are exported as MOL; topology is not retained.");
        if (fmt == io::Format::POSCAR) {
            ImGui::Checkbox("Fractional coordinates (Direct)", &exportOptions.fractionalPOSCAR);
            if (result.data.vectorProperties.count("MoveMask") || result.data.scalarProperties.count(constraints::cartesianProperty))
                ImGui::Checkbox("Preserve selective dynamics", &exportOptions.constraints);
            if(constraints::anyCartesian(result.data))ImGui::TextWrapped("Cartesian fixed positions cannot be preserved by POSCAR. Save .atomx or explicitly disable constraint preservation.");
            ImGui::TextWrapped("Particle positions are grouped by type. POSCAR does not preserve "
                               "arbitrary properties or non-periodic flags.");
        }
        if (fmt == io::Format::CIF)
            ImGui::TextWrapped("P1 structure; cell lengths/angles and fractional positions. The "
                               "reader uses the conventional cell orientation.");
        if (fmt == io::Format::LammpsData)
            ImGui::TextWrapped("Atomic style; sequential IDs; no masses or bonds. Boundary flags "
                               "are not stored by this format.");
        if (fmt == io::Format::LammpsDump)
            ImGui::TextWrapped("id, type, element, x, y, z; sequential IDs per frame. Restricted "
                               "triclinic cells and periodic flags are preserved.");
        if (fmt == io::Format::XYZ) {
            ImGui::Checkbox("Extended XYZ (cell / PBC / properties)", &exportOptions.extendedXYZ);
            if (exportOptions.extendedXYZ) {
                ImGui::TextDisabled("Species and XYZ coordinates are required.");
                auto property = [&](const std::string &name, std::vector<std::string> &selected) {
                    bool on = std::find(selected.begin(), selected.end(), name) != selected.end();
                    if (ImGui::Checkbox(name.c_str(), &on)) {
                        if (on)
                            selected.push_back(name);
                        else
                            selected.erase(std::remove(selected.begin(), selected.end(), name),
                                           selected.end());
                    }
                };
                if (!result.data.scalarProperties.empty() ||
                    !result.data.vectorProperties.empty()) {
                    ImGui::BeginChild("Export properties", {0, U(110)}, ImGuiChildFlags_Borders);
                    for (const auto &[name, v] : result.data.scalarProperties)
                        if (name != "type")
                            property(name, exportOptions.scalarProperties);
                    for (const auto &[name, v] : result.data.vectorProperties)
                        property(name, exportOptions.vectorProperties);
                    ImGui::EndChild();
                }
            } else
                ImGui::TextWrapped("Basic XYZ contains only species and positions; cell and "
                                   "additional properties are omitted.");
        }
        if (source.sampled()) {
            ImGui::Separator();
            ImGui::Checkbox("Export sampled preview only", &exportPreview);
            ImGui::TextWrapped("The loaded data is sampled. Increase the import budget and reload "
                               "to export full data.");
        }
        ImGui::BeginDisabled(source.sampled() && !exportPreview);
        if (ImGui::Button("Choose file and export")) {
            try {
                std::string e = io::info(fmt).extension;
                std::wstring ext(e.begin(), e.end());
                std::wstring exportFilter = L"Selected format";
                exportFilter.push_back(0);
                exportFilter += L"*." + ext;
                exportFilter.push_back(0);
                exportFilter.push_back(0);
                auto p = dialog(window, true, exportFilter.c_str(), ext.c_str());
                if (!p.empty()) {
                    startDataExport(p);
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception &e) {
                error = e.what();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void settings() {
        if (showSettings) { ImGui::OpenPopup("Settings"); showSettings = false; }
        ImGui::SetNextWindowSize({U(540),U(preferences.size >= 19 ? 480.f : 410.f)});
        if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize)) {
            heading("APPEARANCE");
            bool changed = ImGui::Combo("Theme", &preferences.theme,
                                        "View mode dark\0Classic light\0Midnight\0");
            bool fontChanged = ImGui::Combo("Font", &preferences.font, "Segoe UI\0Arial\0Consolas\0");
            fontChanged |= ImGui::SliderInt("Font size", &preferences.size, 14,20,"%d px");
            if (changed || fontChanged) {
                theme(preferences.theme);
                refreshFont |= fontChanged;
                preferences.save();
            }
            ImGui::TextWrapped("Appearance is applied immediately and saved for the next launch.");
            heading("WINDOW CONTROLS");
            ImGui::BulletText("Minimize: collect in the taskbar");
            ImGui::BulletText("X: keep running in the system tray");
            ImGui::BulletText("Power: exit the application completely");
            ImGui::TextWrapped("Double-click the tray logo to restore. Right-click for Restore / Exit.");
            if (ImGui::Button("Done",{U(100),U(30)})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void catalog() {
        if (showCatalog) {
            ImGui::OpenPopup("Add modification");
            showCatalog = false;
        }
        auto display = ImGui::GetIO().DisplaySize;
        float width = std::min(U(1360), display.x - U(24));
        ImGui::SetNextWindowPos({std::max(U(12),catalogAnchor.x-width),catalogAnchor.y}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize({width,std::min(U(790),display.y-catalogAnchor.y-U(36))}, ImGuiCond_Appearing);
        if (ImGui::BeginPopup("Add modification", ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##search", "Search modifications...", modifierSearch, sizeof(modifierSearch));
            auto matches = [&](const char *name) {
                std::string a=name, b=modifierSearch;
                for (auto &c:a) c=char(std::tolower((unsigned char)c));
                for (auto &c:b) c=char(std::tolower((unsigned char)c));
                return a.find(b)!=std::string::npos;
            };
            auto operation = [&](Op op, const char *hint) {
                if (!matches(opName(op))) return;
                if (ImGui::Selectable(opName(op))) { add(op); ImGui::CloseCurrentPopup(); }
                recordUiTestItem(std::string("catalog.")+opName(op));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",hint);
            };
            auto planned = [&](const char *name) {
                if (!matches(name)) return;
                ImGui::BeginDisabled();
                ImGui::Selectable(name);
                ImGui::EndDisabled();
                // P12: muted entries carry the mono PLANNED badge.
                const ImVec2 r = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddText({r.x - ImGui::CalcTextSize("PLANNED").x - U(8), r.y - ImGui::GetTextLineHeight() - U(1)},
                                                    IM_COL32(107, 114, 122, 255), "PLANNED");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Not implemented. This entry does not execute an algorithm.");
            };
            auto beginCard = [&](const char *title) {
                ImGui::BeginChild(title,{0,0},ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                // P12: amber group caption (design: amber uppercase headers).
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_NavCursor));
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                ImGui::Separator();
            };
            auto endCard = [&] { ImGui::EndChild(); };
            if (ImGui::BeginTable("Modifier categories",4,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableNextColumn();
                beginCard("Analysis");
                for (auto name : {"Atomic strain", "Bader charge integration", "Bond order"}) planned(name);
                operation(Op::BondAngleDistribution,"Build a 0–180 degree histogram from pairs of incident bonds, resolving periodic image shifts.");
                operation(Op::BondLengthDistribution,"Build a bond-length histogram from current explicit topology, resolving periodic image shifts.");
                operation(Op::ClusterAnalysis,"Build connected components by a periodic cutoff or existing bond topology; adds per-particle Cluster IDs and a cluster-size table."); planned("Difference between frames");
                operation(Op::CoordinationAnalysis,"Compute periodic neighbor counts and publish the Coordination particle property.");
                operation(Op::DisplacementVectors,"Match particles against a reference animation frame and publish the Displacement vector and Displacement Magnitude particle properties.");
                operation(Op::Histogram,"Build a finite-value histogram data table for a particle scalar or position component.");
                operation(Op::RadialDistribution,"Compute a periodic 3D radial distribution table from the current pipeline data.");
                operation(Op::ReduceProperty,"Reduce a scalar particle property to a global minimum, maximum, mean, or sum.");
                operation(Op::ScatterPlot,"Plot two upstream particle properties and export the finite data pairs as a table; large inputs are deterministically sampled.");
                for (auto name : {"Spatial binning", "Spatial correlation function", "Structure factor", "Time averaging", "Time series", "Voronoi analysis", "Wigner-Seitz defect analysis"}) planned(name);
                endCard();
                ImGui::TableNextColumn();
                beginCard("Modification");
                operation(Op::AffineTransform,"Apply an invertible 3 x 4 affine matrix to particle positions, simulation-cell vectors and origin."); planned("Combine datasets");
                operation(Op::ComputeProperty,"Evaluate a scalar expression for every particle and publish the named property.");
                operation(Op::Delete,"Remove selected particles.");
                operation(Op::EditCell,"Edit cell origin, vectors and periodic boundaries. Particle coordinates stay fixed unless fractional-coordinate remapping is explicitly enabled."); operation(Op::EditType,"Edit particle type assignments.");
                operation(Op::FreezeProperty,"Snapshots a particle property at a reference frame and keeps it constant across the trajectory.");
                for (auto name : {"Load trajectory", "Python script (deferred)"}) planned(name);
                operation(Op::RemoveProperty,"Remove a scalar or vector particle property by name.");
                operation(Op::Replicate,"Duplicate the structure along the three cell vectors and resize the simulation cell.");
                operation(Op::Rotate,"Rotate positions and cell vectors around an axis (degrees).");
                operation(Op::Scale,"Scale positions and simulation cell uniformly.");
                operation(Op::Slice,"Cut the structure at a plane: keep one side, or a slab of adjustable width.");
                planned("Smooth trajectory");
                operation(Op::Translate,"Translate particle positions along an axis.");
                operation(Op::UnwrapTrajectories,"Continuously unwraps particle positions across periodic boundaries using per-frame minimum-image steps.");
                operation(Op::Wrap,"Wrap positions into orthogonal or triclinic periodic cells, including partially periodic cells.");
                endCard();
                ImGui::TableNextColumn();
                beginCard("Structure identification");
                planned("Ackland-Jones analysis");
                operation(Op::CentrosymmetryParameter,"Computes the centrosymmetry parameter to identify atoms in defective crystal environments (stacking faults, surfaces, dislocations).");
                planned("Chill+");
                operation(Op::CommonNeighborAnalysis,"Fixed-cutoff Honeycutt–Andersen pair signatures; FCC and BCC are verified against periodic reference crystals. Unsupported or ambiguous motifs remain Other.");
                for (auto name : {"Identify diamond structure", "Polyhedral template matching", "VoroTop analysis"}) planned(name);
                endCard();
                beginCard("Visualization");
                for (auto name : {"Add text labels", "Construct surface mesh", "Coordination polyhedra"}) planned(name);
                operation(Op::CreateBonds,"Create and GPU-render cutoff neighbor bonds as screen-space lines or 3D cylinders with periodic image shifts.");
                for (auto name : {"Create isosurface", "Generate trajectory lines"}) planned(name);
                endCard();
                beginCard("Modifier templates");
                planned("Manage templates...");
                endCard();
                ImGui::TableNextColumn();
                beginCard("Selection");
                operation(Op::Clear,"Clear the current selection.");
                operation(Op::ExpandSelection,"Expand the current selection by cutoff range, nearest neighbors, bonds, or molecule.");
                operation(Op::ExpressionSelect,"Select particles using the safe native scalar expression language.");
                operation(Op::SelectOverlapping,"Select overlapping pairs using either a distance cutoff or the sum of a scalar radius property, with periodic minimum-image distances.");
                operation(Op::Invert,"Invert selected and unselected particles.");
                operation(Op::ManualSelection,"Select particles in the Particles table; Ctrl-click toggles rows.");
                operation(Op::SelectRange,"Select particles in a coordinate interval.");
                operation(Op::SelectType,"Select particles of one or more checked particle types.");
                endCard();
                beginCard("Python modifiers");
                for (auto name : {"Assign shared visual element", "Calculate local entropy", "Identify fcc planar faults", "Render LAMMPS regions", "Shrink-wrap simulation box"}) planned(name);
                endCard();
                beginCard("Coloring");
                planned("Ambient occlusion");
                operation(Op::AssignColor,"Assign a solid color to the selected particles; if there is no selection, apply it to all particles.");
                operation(Op::ColorType,"Use the particle-type color palette.");
                operation(Op::ColorCoding,"Map a particle property to a color gradient.");
                endCard();
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::TextWrapped("Muted items are in development. All available tools are unrestricted.");
            ImGui::EndPopup();
        }
    }
    // Case-insensitive substring filter over the command registry, shared
    // implementation with the modifier catalog search.
    std::vector<const Command *> paletteMatches() const {
        std::vector<const Command *> matches;
        for (const auto &command : commandRegistry()) {
            std::string text = std::string(command.category) + ": " + command.label;
            std::string a = text, b = commandSearch;
            for (auto &c : a) c = char(std::tolower((unsigned char)c));
            for (auto &c : b) c = char(std::tolower((unsigned char)c));
            if (b.empty() || a.find(b) != std::string::npos) matches.push_back(&command);
        }
        return matches;
    }
    void closePalette() {
        paletteActive = false;
        paletteIndex = 0;
        commandSearch[0] = 0;
    }
    void executePaletteEntry() {
        auto matches = paletteMatches();
        if (paletteIndex < 0 || size_t(paletteIndex) >= matches.size()) return;
        const Command &command = *matches[size_t(paletteIndex)];
        switch (command.action) {
        case CommandAction::Modifier: add(command.op); break;
        case CommandAction::OpenFile: open(); break;
        case CommandAction::ExportFile: showDataExport = true; break;
        case CommandAction::SaveSessionState: saveSessionState(false); break;
        case CommandAction::Snapshot:
        case CommandAction::Render: exportImage(); break;
        case CommandAction::Settings: showSettings = true; break;
        case CommandAction::LayerBuilder: requestLayerBuilder(); break;
        case CommandAction::ToggleQuad: quad = !quad; break;
        case CommandAction::FitAll:
            for (int i = 0; i < 4; ++i) fitCamera(i, false);
            break;
        case CommandAction::ToolZoom: viewportTool = 0; break;
        case CommandAction::ToolPan: viewportTool = 1; break;
        case CommandAction::ToolOrbit: viewportTool = 2; break;
        case CommandAction::ToolFov: viewportTool = 3; break;
        case CommandAction::ViewTop:
        case CommandAction::ViewBottom:
        case CommandAction::ViewFront:
        case CommandAction::ViewBack:
        case CommandAction::ViewLeft:
        case CommandAction::ViewRight:
        case CommandAction::ViewOrtho:
        case CommandAction::ViewPerspective:
            cameras[active].mode = int(command.action) - int(CommandAction::ViewTop);
            cameras[active].roll = 0;
            break;
        }
        closePalette();
    }
    // Floating command list under the toolbar search field. Drawn at the end
    // of the frame so later windows cannot cover it; keeps the search field
    // focused so typing continues to filter while the dropdown is open.
    void commandPalette() {
        if (!paletteActive) return;
        const auto matches = paletteMatches();
        if (paletteIndex >= int(matches.size())) paletteIndex = int(matches.size()) - 1;
        if (paletteIndex < 0) paletteIndex = 0;
        ImGui::SetNextWindowPos({paletteAnchor.x, paletteAnchor.y + U(30)});
        ImGui::SetNextWindowSize({U(430), 0});
        if (!ImGui::Begin("##command-palette", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && paletteIndex > 0) --paletteIndex;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && paletteIndex + 1 < int(matches.size()))
            ++paletteIndex;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) closePalette();
        ImGui::BeginChild("##palette-list", {0, std::min(U(320), U(28.f) * float(std::max(matches.size(), size_t(1))))});
        if (matches.empty())
            ImGui::TextDisabled("No matching command");
        int index = 0;
        for (const Command *command : matches) {
            const std::string label =
                std::string(command->category) + ": " + command->label;
            if (ImGui::Selectable(label.c_str(), index == paletteIndex)) executePaletteEntry();
            recordUiTestItem("palette.item." + label, label.c_str());
            ++index;
        }
        ImGui::EndChild();
        // Enter runs the highlighted entry; Up/Down move the highlight. The
        // search field keeps keyboard focus while the dropdown is open (no
        // SetItemDefaultFocus here: moving nav focus would deactivate the
        // text input and swallow typing).
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && paletteIndex < int(matches.size()))
            executePaletteEntry();
        // Clicking anywhere outside the search field and this list closes
        // the palette, matching the menus' click-away behavior.
        if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered() &&
            !ImGui::IsMouseHoveringRect(paletteAnchor,
                                        {paletteAnchor.x + U(260), paletteAnchor.y + U(28)}))
            closePalette();
        ImGui::End();
    }
    // P12 bottom status strip (design: mono status row #0f1113): the newest
    // status message plus particle / selection / tool / fps facts. The
    // viewport overlay still fades the newest message in the active view.
    void statusStrip(float w, float h) {
        fixed("##status-strip", 0, h - statusHeight(), w, statusHeight());
        if (homeMode) { ImGui::TextDisabled("AtomX %s · Ctrl+T 新标签页 · Ctrl+O 打开文件",atomxVersion); ImGui::End(); return; }
        if (creationMode) {
            ImGui::TextColored({0.29f,0.87f,0.51f,1},"创作");
            ImGui::SameLine();
            ImGui::TextDisabled("点击选择 · Shift 添加 · Ctrl 切换 · 双击片段 · 右键/中键旋转 · Alt+右键平移 · 滚轮缩放");
            ImGui::SameLine(std::max(U(900),w-U(370)));
            ImGui::TextDisabled("%zu 原子 · 选中 %zu 原子 / %zu 键 · Å",
                source.atoms.size(),creationSelection.size(),creationBondSelection.size());
            ImGui::End();
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(12), U(4)});
        ImGui::PushStyleColor(ImGuiCol_Text, playing ? IM_COL32(240, 164, 49, 255)
                                                     : IM_COL32(87, 171, 90, 255));
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(U(420));
        ImGui::TextDisabled("%s particles", number(result.data.atoms.size()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s selected", number(selectedCount).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("tool %s", viewportTool == 0 ? "zoom"
                                     : viewportTool == 1 ? "pan"
                                     : viewportTool == 2 ? "orbit" : "fov");
        ImGui::PopStyleVar();
        ImGui::End();
    }
    void ui() {
        poll();
        if (pendingTabId && !documentsBusy()) {
            const uint64_t target=pendingTabId;
            pendingTabId=0;
            for (int index=0;index<int(tabs.size());++index)
                if (tabs[size_t(index)].id==target) { switchTab(index); break; }
        }
        editCheckpoint.beginFrame(ImGui::IsMouseDown(ImGuiMouseButton_Left));
        // Frame-fresh cursor state: viewport children re-assert the tool
        // cursor while hovered; everywhere else (and for no active tool) the
        // normal arrow applies. g_cursorOverride is per-frame by contract.
        g_cursorOverride = nullptr;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        // The removed status bar lives on as a fading overlay in the active
        // viewport; track the newest message here so center() can draw it.
        if (status != overlayStatus) {
            overlayStatus = status;
            overlayStatusAt = ImGui::GetTime();
        }
        auto &io = ImGui::GetIO();
        // Modal inputs own their keyboard shortcuts. Never delete/move atoms,
        // switch documents, or create an undo step behind a modal editor.
        if (!ImGui::GetTopMostPopupModal()) {
        // Menu accelerators. Ctrl+O loads per OVITO; Ctrl+Z/Y stay on undo /
        // redo; Ctrl+P focuses the Quick command search.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T))
            newHomeTab();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab) && tabs.size()>1)
            switchTab((activeTab+(io.KeyShift?int(tabs.size())-1:1))%int(tabs.size()));
        if (io.KeyCtrl && !io.KeyAlt) {
            constexpr ImGuiKey numbers[]{ImGuiKey_1,ImGuiKey_2,ImGuiKey_3,ImGuiKey_4,
                ImGuiKey_5,ImGuiKey_6,ImGuiKey_7,ImGuiKey_8,ImGuiKey_9};
            for (int index=0;index<9 && index<int(tabs.size());++index)
                if (ImGui::IsKeyPressed(numbers[index])) switchTab(index);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O,false)) open();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I)) open();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W)) closeTab(activeTab);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E)) showDataExport = true;
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S))
            saveSessionState(true);
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveSessionState(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)) paletteRequested = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            openUrl("https://github.com/WhiteCrosstheRiver/AtomX#readme");
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            history(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
            history(true);
        if (creationMode && ImGui::IsKeyPressed(ImGuiKey_Delete) && !io.WantTextInput) {
            if(isGeometryTool(creationTool) && creationDisplay.activeMonitor>=0) removeGeometryMonitor();
            else deletePickedAtom();
        }
        if (creationMode && !io.WantTextInput) {
            if(io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C,false))copyCreationSelection();
            if(io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X,false))copyCreationSelection(true);
            if(io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_V,false))pasteCreationSelection();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) selectCreationAtom(-1,false);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
                creationBondSelection.clear();
                if(source.bonds.size()<=interactiveBondBudget)for(size_t row=0;row<source.bonds.size();++row)
                    if(creationBondVisible(int(row)))creationBondSelection.push_back(int(row));
                creationSelection.clear();
                for (size_t index=0;index<source.atoms.size();++index)
                    if (creationAtomVisible(int(index))) creationSelection.push_back(int(index));
                creationPick=creationSelection.empty()?-1:creationSelection.back();
            }
            if (!io.KeyCtrl && !io.KeyAlt) {
                if (ImGui::IsKeyPressed(ImGuiKey_V)) chooseCreationTool(CreationTool::Select);
                if (ImGui::IsKeyPressed(ImGuiKey_R)) chooseCreationTool(CreationTool::Rotate);
                if (ImGui::IsKeyPressed(ImGuiKey_T)) chooseCreationTool(CreationTool::Pan);
                if (ImGui::IsKeyPressed(ImGuiKey_G)) chooseCreationTool(CreationTool::Move);
                if (ImGui::IsKeyPressed(ImGuiKey_P)) chooseCreationTool(CreationTool::Sketch);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if(creationDrag==CreationDrag::None && geometryPending.empty() && creationSketchAnchor<0 && !creationFusionSeed)chooseCreationTool(CreationTool::Select);
                creationSketchAnchor=creationSketchLastPlaced=-1; creationSketchPreviewValid=false;
                selectCreationAtom(-1,false);
            }
        }
        }
        if (openNewCell) {
            ImGui::OpenPopup("New Orthogonal Cell");
            openNewCell = false;
        }
        if (openTriclinicCell) {
            ImGui::OpenPopup("New Triclinic Cell");
            openTriclinicCell = false;
        }
        if (ImGui::BeginPopupModal("New Orthogonal Cell", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Create an empty orthogonal cell with one atom at the center.");
            ImGui::InputFloat("Length a (A)", &newCellA);
            ImGui::InputFloat("Length b (A)", &newCellB);
            ImGui::InputFloat("Length c (A)", &newCellC);
            ImGui::InputText("Element symbol", creationElement, sizeof(creationElement));
            if (ImGui::Button("Create Cell", {U(140), 0})) {
                newCellA = std::clamp(newCellA, 1.f, 200.f);
                newCellB = std::clamp(newCellB, 1.f, 200.f);
                newCellC = std::clamp(newCellC, 1.f, 200.f);
                newStructureTab(authoring::orthogonalCell(newCellA, newCellB, newCellC, creationElement),
                                "Orthogonal cell");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("New Triclinic Cell", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Create a cell from lengths a, b, c and angles alpha, beta, gamma.");
            ImGui::InputFloat("Length a (angstrom)", &newCellA);
            ImGui::InputFloat("Length b (angstrom)", &newCellB);
            ImGui::InputFloat("Length c (angstrom)", &newCellC);
            ImGui::InputFloat("Angle alpha (degrees)", &newAlpha);
            ImGui::InputFloat("Angle beta (degrees)", &newBeta);
            ImGui::InputFloat("Angle gamma (degrees)", &newGamma);
            ImGui::InputText("Element symbol", creationElement, sizeof(creationElement));
            if (ImGui::Button("Create Triclinic Cell", {U(180), 0})) {
                newCellA = std::clamp(newCellA, 1.f, 200.f);
                newCellB = std::clamp(newCellB, 1.f, 200.f);
                newCellC = std::clamp(newCellC, 1.f, 200.f);
                newAlpha = std::clamp(newAlpha, 20.f, 160.f);
                newBeta = std::clamp(newBeta, 20.f, 160.f);
                newGamma = std::clamp(newGamma, 20.f, 160.f);
                newStructureTab(authoring::triclinicCell(newCellA, newCellB, newCellC, newAlpha, newBeta, newGamma,
                                                        creationElement),
                                "Triclinic cell");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (playing && !busy && frames.size() > 1 && ImGui::GetTime() - lastFrame > 1 / fps) {
            lastFrame = ImGui::GetTime();
            const int next = current + 1;
            if (next < int(frames.size()))
                load(path, next);
            else if (loopPlayback)
                load(path, 0);
            else
                playing = false;
        }
        float w = io.DisplaySize.x, h = io.DisplaySize.y;
        top(w);
        if (homeMode) {
            homeWorkspace(w, h);
        } else if (creationMode) {
            creationWorkspace(w, h);
        } else {
            if (showWorkspace) left(h);
            pipelinePanel(h);
            right(w, h);
            center(w, h);
            layoutSplitters(w, h);
        }
        if (layoutDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            preferences.workspacePane = int(std::lround(workspacePane));
            preferences.pipelinePane = int(std::lround(pipelinePane));
            preferences.propertiesPane = int(std::lround(propertiesPane));
            preferences.inspectorPane = int(std::lround(inspectorPane));
            preferences.timelinePane = int(std::lround(timelinePane));
            try { preferences.save(); layoutDirty = false; }
            catch (const std::exception &ex) { status = ex.what(); }
        }
        statusStrip(w, h);
        creationDialogs();
        fragmentBrowser();
        chemistryDialog();
        atomPropertiesDialog();
        atomConstraintsDialog();
        measurementConstraintsDialog();
        hydrogenDialog();
        motionGroupsDialog();
        creationStylesDialog();
        layerBuilderDialog();
        catalog();
        settings();
        commandPalette();
        dataExportDialog();
        // Help > System Information: the same adapter facts the System tab
        // shows, in a small modal.
        if (showSystemInfo) {
            ImGui::OpenPopup("System Information");
            showSystemInfo = false;
        }
        if (ImGui::BeginPopupModal("System Information", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("AtomX %s", atomxVersion);
            ImGui::Separator();
            ImGui::Text("GPU adapter: %s", utf8(gpu.adapterName).c_str());
            ImGui::Text("DXGI adapters visible: %zu", gpu.adapters.size());
            ImGui::Text("GPU memory in use: %s", number(uint64_t(gpu.gpuBytes)).c_str());
            ImGui::Text("Icon font: %s", iconFontName);
            if (ImGui::Button("Close", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (showAbout) {
            ImGui::OpenPopup("About AtomX");
            showAbout = false;
        }
        if (ImGui::BeginPopupModal("About AtomX", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            if (logo.Get()) {
                ImGui::Image((ImTextureID)(intptr_t)logo.Get(), {U(48), U(48)});
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::Text("AtomX %s", atomxVersion);
            ImGui::TextDisabled("Atomic visualization workspace");
            ImGui::EndGroup();
            ImGui::Separator();
            ImGui::TextWrapped(
                "AtomX 1.0 is a Direct3D 11 structure viewer with a modifier pipeline, "
                "browser-style structure tabs, and Creation Mode for adding, moving, "
                "replacing, and deleting atoms. Build tools cover cells, supercells, "
                "vacuum, layers, sheets, nanotubes, and nanoparticles.");
            if (ImGui::Button("Close", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (animationSettings) {
            ImGui::OpenPopup("Animation settings");
            animationSettings = false;
        }
        if (ImGui::BeginPopupModal("Animation settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("Trajectory playback");
            ImGui::SliderFloat("Frames/sec", &fps, 1.f, 60.f, "%.0f");
            ImGui::TextWrapped("Playback repeats the loaded trajectory. Use the timeline ruler or frame field to scrub.");
            if (ImGui::Button("Close", {U(110), 0})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (busy && indexing)
            ImGui::OpenPopup("Loading trajectory");
        if (ImGui::BeginPopupModal("Loading trajectory", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Indexing / reading XYZ records...");
            ImGui::ProgressBar(progress, {U(400), U(20)});
            if (ImGui::Button("Cancel"))
                cancel = true;
            if (!busy)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!error.empty())
            ImGui::OpenPopup("Operation failed");
        // TextWrapped inside an AlwaysAutoResize window collapses to the
        // minimum width when it is the only wide item, turning the dialog
        // into a sliver a user can easily miss — while the modal blocks every
        // other interaction. Pin a readable width range instead.
        ImGui::SetNextWindowSizeConstraints({U(430), U(110)}, {U(720), FLT_MAX});
        if (ImGui::BeginPopupModal("Operation failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(U(660));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Separator();
            const float okWidth = U(120);
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetContentRegionAvail().x - okWidth + ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::Button("OK", {okWidth, 0})) {
                error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
};
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        SetProcessDPIAware();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int argc;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int adapter = -1, smoke = 0, smokeSelectedAtom = -1;
        bool smokeCatalog = false, smokeSettings = false, smokeExport = false, desktopTest = false,
             smokeColorLegend = false, smokeBondPairs = false, smokeInspectorNode = false,
             smokeHistogram = false, smokeTypesPanel = false, smokePalette = false,
              smokeMenu = false, smokeCreation = false, smokeBondCloseup = false;
        std::filesystem::path input, shot;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--catalog") smokeCatalog = true;
            else if (a == L"--settings") smokeSettings = true;
            else if (a == L"--export-settings")
                smokeExport = true;
            else if (a == L"--smoke-color-legend") smokeColorLegend = true;
            else if (a == L"--smoke-bond-pairs") smokeBondPairs = true;
            else if (a == L"--smoke-bond-closeup") { smokeBondPairs = true; smokeBondCloseup = true; }
            else if (a == L"--smoke-inspector-node") smokeInspectorNode = true;
            else if (a == L"--smoke-histogram") smokeHistogram = true;
            else if (a == L"--smoke-types-panel") smokeTypesPanel = true;
            else if (a == L"--palette") smokePalette = true;
            else if (a == L"--menu") smokeMenu = true;
            else if (a == L"--smoke-creation") smokeCreation = true;
            else if (a == L"--smoke-selected-atom" && i + 1 < argc)
                smokeSelectedAtom = _wtoi(argv[++i]);
            else if (a == L"--desktop-test") desktopTest = true;
            else if (a == L"--adapter" && i + 1 < argc)
                adapter = _wtoi(argv[++i]);
            else if (a == L"--smoke" && i + 1 < argc)
                smoke = _wtoi(argv[++i]);
            else if (a == L"--screenshot" && i + 1 < argc)
                shot = argv[++i];
            else
                input = a;
        }
        LocalFree(argv);
        WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc,  0,
                       0,          instance,   nullptr,  LoadCursor(nullptr, IDC_ARROW),
                       nullptr,    nullptr,    L"AtomX", nullptr};
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(102));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"AtomX - Atomic visualization workspace",
                                    WS_OVERLAPPEDWINDOW, 80, 50, 1560, 1000, nullptr, nullptr,
                                    instance, nullptr);
        uiScale = std::max(1.f,GetDpiForWindow(window)/96.f);
        MONITORINFO workArea{sizeof(workArea)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &workArea);
        int initialW = std::min(int(U(1560)), int(workArea.rcWork.right-workArea.rcWork.left-40));
        int initialH = std::min(int(U(1000)), int(workArea.rcWork.bottom-workArea.rcWork.top-40));
        SetWindowPos(window,nullptr,workArea.rcWork.left+20,workArea.rcWork.top+20,
                     initialW,initialH,SWP_NOZORDER | SWP_FRAMECHANGED);
        // Generate the zoom-tool magnifier cursor once; if that fails the
        // viewport falls back to the stock 4-way cursor at use time.
        g_magnifierCursor = createMagnifierCursor(uiScale);
        Renderer gpu;
        renderer = &gpu;
        gpu.init(window, adapter);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17);
        uiScale = std::max(1.f, GetDpiForWindow(window)/96.f);
        theme(0); // P12: the redesigned "View mode dark" palette is the startup default.
        ImGui_ImplWin32_Init(window);
        ImGui_ImplDX11_Init(gpu.device.Get(), gpu.context.Get());
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(window, SW_SHOWDEFAULT);
        if (smoke) ShowWindow(window, SW_SHOWNORMAL);
        DragAcceptFiles(window, TRUE);
        {
            App app(window, gpu);
            app.showCatalog = smokeCatalog;
            app.showSettings = smokeSettings;
            app.showDataExport = smokeExport;
            if (smokeMenu) app.smokeMenuFile = true;
            if (smokePalette) {
                app.paletteRequested = true;
                strcpy_s(app.commandSearch, "slice");
            }
            if (smokeInspectorNode) {
                app.showTable = true;
                app.add(Op::Translate);
            }
            if (smokeHistogram) {
                app.showTable = true;
                app.histogramPreviewTab = true;
                app.add(Op::Histogram);
            }
            if (smokeColorLegend) app.add(Op::ColorCoding);
            if (smokeTypesPanel) app.focusParticleAppearance = true;
             if (smokeBondCloseup) { app.quad = false; app.active = 3; }
            if (desktopTest) {
                auto requireWindow = [](bool ok, const char *message) {
                    if (!ok) throw std::runtime_error(message);
                };
                ShowWindow(window, SW_MINIMIZE);
                requireWindow(IsIconic(window), "Minimize must use taskbar");
                ShowWindow(window, SW_RESTORE);
                ShowWindow(window, SW_MAXIMIZE);
                requireWindow(IsZoomed(window), "Maximize failed");
                SendMessageW(window, WM_CLOSE, 0, 0);
                requireWindow(desktop::inTray && !IsWindowVisible(window), "Close must hide to tray");
                desktop::restore(window);
                requireWindow(IsWindowVisible(window) && IsZoomed(window), "Tray restore must preserve maximized state");
                ShowWindow(window, SW_RESTORE);
                requireWindow(!IsIconic(window) && !IsZoomed(window), "Restore failed");
                RECT r{}; GetWindowRect(window, &r);
                requireWindow(desktop::hitTest(window, MAKELPARAM(r.left+100,r.top+20)) == HTCAPTION,
                              "Custom title must support native dragging");
                requireWindow(desktop::hitTest(window, MAKELPARAM(r.left+300,r.top+20)) == HTCLIENT,
                              "Browser tabs must receive pointer clicks");
                std::ofstream("build/desktop-test.txt") << "PASS: minimize, maximize, close-to-tray, restore, title drag hit test\n";
            }
            if (!input.empty()) {if(lowerExtension(input)==".atomx-project")app.loadProject(input);else app.load(input);}
            else {
                app.source={};
                app.result={};
                app.homeMode=true;
                app.update();
                app.tabs[0]=app.captureTab();
            }
            if (desktopTest && !input.empty()) {
                app.job.wait(); app.poll();
                if (app.frames.size() < 2) throw std::runtime_error("Timeline test needs at least two frames");
                app.seekFrame(1); app.poll();
                app.seekFrame(int(app.frames.size())+100); // Replace request while the prior frame loads.
                app.job.wait(); app.poll();
                if (app.busy) { app.job.wait(); app.poll(); }
                if (app.current != int(app.frames.size())-1 || !app.error.empty())
                    throw std::runtime_error("Queued trajectory seek failed");
                app.seekFrame(-10); app.poll(); app.job.wait(); app.poll();
                if (app.current != 0) throw std::runtime_error("First-frame seek failed");
                app.seekFrame(int(app.frames.size())-1); app.poll(); app.job.wait(); app.poll();
                std::ofstream("build/desktop-test.txt",std::ios::app)
                    << "PASS: trajectory seek, latest-request wins, first/last bounds\n";
            }
            bool done = false;
            int ticks = 0;
            bool inspectorSmokeStarted = false;
            bool bondSmokeStarted = false;
            bool creationSmokeStarted = false;
            while (!done) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    if (msg.message == WM_QUIT)
                        done = true;
                }
                if (done)
                    break;
                if (IsIconic(window) || !IsWindowVisible(window)) {
                    app.poll();
                    Sleep(20);
                    continue;
                }
                if (resized) {
                    gpu.resize();
                    resized = false;
                }
                app.rebuildFont();
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                try {
                    app.ui();
                } catch (const std::exception &e) {
                    app.error = e.what();
                }
                if (smokeCreation && !creationSmokeStarted && !app.documentsBusy() &&
                    !app.result.data.atoms.empty()) {
                    app.openCreationTab();
                    creationSmokeStarted = app.creationMode;
                    if (creationSmokeStarted && smokeSelectedAtom>=0 &&
                        size_t(smokeSelectedAtom)<app.source.atoms.size())
                        app.selectCreationAtom(smokeSelectedAtom,false);
                }
                if (smokeInspectorNode && !inspectorSmokeStarted && !app.busy &&
                    !app.indexing && !app.pipelineBusy) {
                    app.inspectPipelineNode(0);
                    inspectorSmokeStarted = true;
                }
                // Deferred so the data source is loaded first (an eager add
                // would be wiped by load()); the Bonds inspector page shows in
                // the smoke screenshot once the bond topology is published.
                if (smokeBondPairs && !bondSmokeStarted && !app.busy &&
                    !app.pipelineBusy && app.result.data.atoms.size()) {
                    app.showTable = true;
                    app.bondsTab = true;
                    app.add(Op::CreateBonds);
                     if (smokeBondCloseup) app.showTable = false;
                    auto &bondNode = app.mods.back();
                    const size_t typeCount = app.source.species.size();
                    bondNode.bondTypeCutoffsEnabled = true;
                    bondNode.bondTypeCutoffs.assign(typeCount * typeCount, bondNode.value);
                    app.update(app.mods.size() - 1);
                    bondSmokeStarted = true;
                }
                ImGui::Render();
                float clear[4] = {.086f, .094f, .106f, 1}; // #16181b design background
                gpu.context->OMSetRenderTargets(1, gpu.back.GetAddressOf(), nullptr);
                gpu.context->ClearRenderTargetView(gpu.back.Get(), clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                if (smoke && !app.busy && !app.pipelineBusy && !app.inspectorBusy &&
                    (!smokeInspectorNode || inspectorSmokeStarted) &&
                    (!smokeCreation || creationSmokeStarted) && ++ticks >= smoke) {
                    if (!shot.empty()) {
                        // Capture before the error abort below so a failing
                        // run still documents the dialog it died on.
                        Target t;
                        ComPtr<ID3D11Resource> res;
                        gpu.back->GetResource(&res);
                        check(res.As(&t.texture), "Screenshot texture");
                        D3D11_TEXTURE2D_DESC d;
                        t.texture->GetDesc(&d);
                        t.w = d.Width;
                        t.h = d.Height;
                        gpu.png(t, shot);
                    }
                    if (!app.error.empty())
                        throw std::runtime_error(app.error);
                    std::ofstream report("smoke-report.txt");
                    report << "adapter=" << utf8(gpu.adapterName)
                           << "\natoms=" << app.result.data.atoms.size()
                           << "\nsource_atoms=" << app.source.sourceCount
                           << "\nsample_stride=" << app.source.stride
                           << "\ntrajectory_frames=" << std::max<size_t>(app.frames.size(), 1)
                           << "\ngpu_bytes=" << gpu.gpuBytes << "\nframes=" << ticks
                           << "\nfps=" << io.Framerate
                           << "\nicon_font=" << iconFontName
                           << "\ncursor_zoom="
                           << (g_magnifierCursor ? "procedural-magnifier" : "sizeall-fallback")
                           << "\n";
                    done = true;
                }
                check(gpu.swap->Present(1, 0), "Present");
            }
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, instance);
        CoUninitialize();
        return 0;
    } catch (const std::exception &e) {
        std::ofstream("atomx-error.txt") << e.what();
        MessageBoxA(nullptr, e.what(), "AtomX startup error", MB_ICONERROR);
        return 1;
    }
}
