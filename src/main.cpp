#include "renderer.hpp"
#include "analysis.hpp"
#include "authoring.hpp"
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
static std::filesystem::path dropped;
static int droppedExtra = 0; // Additional files in a multi-file drop (first wins).
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
        // Keep it simple: the first dropped file is opened; any further files
        // are ignored and reported through the status bar.
        droppedExtra = std::max(0, int(DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0)) - 1);
        DragQueryFileW(hDrop, 0, p, 32768);
        dropped = p;
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
    return (save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of)) ? std::filesystem::path(path)
                                                                  : std::filesystem::path();
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
    ToggleQuad, FitAll, ToolZoom, ToolPan, ToolOrbit, ToolFov,
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
static const char *atomxVersion = "1.1.0";
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
        for (size_t i = 0; i < appearanceNames.size() && i < gpu.styles.size(); ++i)
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
    struct StructureTab {
        std::string title = "Structure";
        Dataset source;
        PipelineGraph graph;
        std::vector<std::vector<ModifierNode>> undo, redo;
        std::filesystem::path path;
        std::vector<Frame> frames;
        int current = 0;
        std::string readerName = "Generated crystal";
        Camera cameras[4]{};
        bool creationMode = false;
        bool creationSketch = false;
        int picked = -1;
        int measure = -1;
        int angleAtom = -1;
        int dihedralAtom = -1;
        std::vector<Dataset> authorUndo, authorRedo;
        char element[16] = "O";
    };
    std::vector<StructureTab> tabs;
    int activeTab = 0;
    int creationReturnTab = -1;
    std::string creationBasedOn;
    bool creationMode = false;
    bool creationSketch = false;
    int creationPick = -1;
    int creationMeasure = -1;
    int creationAngle = -1;
    int creationDihedral = -1;
    std::vector<Dataset> authorUndo, authorRedo;
    char creationElement[16] = "O";
    bool showLatticePanel = false;
    bool openNewCell = false;
    bool openTriclinicCell = false;
    float newCellA = 5.f, newCellB = 5.f, newCellC = 5.f;
    float newAlpha = 90.f, newBeta = 90.f, newGamma = 90.f;
    App(HWND w, Renderer &r) : window(w), gpu(r) {
        preferences.load();
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
        tabs.push_back(captureTab());
        activeTab = 0;
    }
    ~App() {
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
            gpu.upload(next.data, next.selected, next.colorSelected);
            syncAppearance(next.data.species);
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
        if (op == Op::CreateBonds) m.value = 3.2f; // OVITO default cutoff radius
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
        status = std::string("Added ") + opName(op);
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
        if (creationMode && activeTab >= 0 && activeTab < int(tabs.size()))
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
        tab.title = tabTitle();
        tab.source = source;
        tab.graph = modifierGraph;
        tab.undo = undo;
        tab.redo = redo;
        tab.path = path;
        tab.frames = frames;
        tab.current = current;
        tab.readerName = readerName;
        for (int i = 0; i < 4; ++i) tab.cameras[i] = cameras[i];
        tab.creationMode = creationMode;
        tab.creationSketch = creationSketch;
        tab.picked = creationPick;
        tab.measure = creationMeasure;
        tab.angleAtom = creationAngle;
        tab.dihedralAtom = creationDihedral;
        tab.authorUndo = authorUndo;
        tab.authorRedo = authorRedo;
        snprintf(tab.element, sizeof(tab.element), "%s", creationElement);
        return tab;
    }
    void restoreTab(const StructureTab &tab) {
        source = tab.source;
        modifierGraph = tab.graph;
        undo = tab.undo;
        redo = tab.redo;
        path = tab.path;
        frames = tab.frames;
        current = tab.current;
        readerName = tab.readerName;
        for (int i = 0; i < 4; ++i) cameras[i] = tab.cameras[i];
        creationMode = tab.creationMode;
        creationSketch = tab.creationSketch;
        creationPick = tab.picked;
        creationMeasure = tab.measure;
        creationAngle = tab.angleAtom;
        creationDihedral = tab.dihedralAtom;
        authorUndo = tab.authorUndo;
        authorRedo = tab.authorRedo;
        snprintf(creationElement, sizeof(creationElement), "%s", tab.element);
        pipelineCheckpoint.reset();
        pipelineCheckpointNode = SIZE_MAX;
        unwrapAccumulators.clear();
        structureEditIsLatest = !authorUndo.empty();
        syncAppearance(source.species);
        update();
    }
    bool documentsBusy() const { return busy || pipelineBusy || indexing; }
    void switchTab(int index) {
        if (index < 0 || index >= int(tabs.size()) || index == activeTab) return;
        if (documentsBusy()) {
            status = "Wait for the current load or pipeline to finish before switching tabs";
            return;
        }
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
        if (!tabs.empty()) tabs[activeTab] = captureTab();
        StructureTab tab;
        tab.source = std::move(data);
        tab.title = title;
        tab.readerName = "Authored structure";
        tab.graph = {};
        snprintf(tab.element, sizeof(tab.element), "C");
        tabs.push_back(std::move(tab));
        activeTab = int(tabs.size()) - 1;
        restoreTab(tabs[activeTab]);
        status = "New tab: " + title;
    }
    void openCreationTab() {
        if (documentsBusy()) {
            status = "Wait for the current operation before opening Creation Mode";
            return;
        }
        const int origin = activeTab;
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
                    editable.bondStyle.radius = .045f;
                    editable.bondStyle.color = {.25f,.75f,.21f,1.f};
                } catch (const std::exception &) {
                    editable.bonds.clear();
                }
            }
        }
        creationBasedOn = path.empty() ? tabTitle() : utf8(path.filename().wstring());
        const std::string title = tabTitle() + " · 创作";
        newStructureTab(std::move(editable), title);
        if (activeTab == origin) return;
        creationReturnTab = origin;
        cameras[3] = viewCamera;
        cameras[3].mode = 7;
        active = 3;
        creationMode = true;
        creationSketch = false;
        status = "Creation Mode: " + title;
    }
    void leaveCreationTab() {
        if (creationReturnTab >= 0 && creationReturnTab < int(tabs.size()) &&
            creationReturnTab != activeTab)
            switchTab(creationReturnTab);
        else
            creationMode = false;
    }
    void closeTab(int index) {
        if (index < 0 || index >= int(tabs.size())) return;
        if (documentsBusy()) {
            status = "Wait for the current load or pipeline to finish before closing a tab";
            return;
        }
        if (tabs.size() == 1) {
            newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
            tabs.erase(tabs.begin());
            activeTab = 0;
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
    void rememberStructure() {
        if (authorUndo.size() >= 64) authorUndo.erase(authorUndo.begin());
        authorUndo.push_back(source);
        authorRedo.clear();
        structureEditIsLatest = true;
    }
    void adoptStructure(Dataset data, const std::string &message) {
        rememberStructure();
        source = std::move(data);
        frames.clear();
        current = 0;
        pendingFrame = -1;
        mods.clear();
        modifierGraph.selected = 0;
        creationPick = creationMeasure = creationAngle = creationDihedral = -1;
        syncAppearance(source.species);
        update();
        status = message;
    }
    void editStructure(const std::string &message, const std::function<void(Dataset &)> &edit) {
        if (!sameAtomCount() && !mods.empty()) {
            status = "Clear modifiers first: the pipeline changed the atom count";
            return;
        }
        rememberStructure();
        edit(source);
        source.sourceCount = source.atoms.size();
        source.bounds();
        if (!mods.empty()) {
            mods.clear();
            modifierGraph.selected = 0;
        }
        syncAppearance(source.species);
        update();
        status = message;
    }
    void deletePickedAtom() {
        if (creationPick < 0 || size_t(creationPick) >= source.atoms.size()) {
            status = "Pick an atom in Creation Mode first";
            return;
        }
        int index = creationPick;
        editStructure("Deleted atom " + std::to_string(index + 1), [&](Dataset &data) {
            data.atoms.erase(data.atoms.begin() + index);
            data.bonds.clear();
        });
        creationPick = creationMeasure = creationAngle = creationDihedral = -1;
    }
    void replacePickedElement() {
        if (creationPick < 0 || size_t(creationPick) >= source.atoms.size()) {
            status = "Pick an atom in Creation Mode first";
            return;
        }
        std::string symbol = creationElement[0] ? creationElement : "C";
        int index = creationPick;
        editStructure("Replaced atom " + std::to_string(index + 1) + " with " + symbol, [&](Dataset &data) {
            data.atoms[index].type = authoring::speciesIndex(data, symbol);
        });
    }
    void addAtomAt(Vec3 position) {
        std::string symbol = creationElement[0] ? creationElement : "C";
        editStructure("Added " + symbol, [&](Dataset &data) {
            uint32_t type = authoring::speciesIndex(data, symbol);
            data.atoms.push_back({position.x, position.y, position.z, type});
            creationPick = int(data.atoms.size() - 1);
        });
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
        }
        cameras[0].mode = 0;
        cameras[1].mode = 2;
        cameras[2].mode = 4;
        cameras[3].mode = 7;
        for (int i = 0; i < 4; ++i) fitCamera(i, false);
        status = "Reset the camera on every viewport";
    }
    void addHydrogensCommand() {
        if (source.atoms.size() > 2500) {
            status = "Add Hydrogens is limited to 2500 atoms";
            return;
        }
        int added = 0;
        editStructure("Added hydrogens", [&](Dataset &data) { added = authoring::addHydrogens(data); });
        if (added >= 0) status = "Added " + std::to_string(added) + " hydrogen atoms";
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
        if (structureEditIsLatest) {
            if (!forward && !authorUndo.empty()) {
                authorRedo.push_back(source);
                source = authorUndo.back();
                authorUndo.pop_back();
                if (authorUndo.empty()) structureEditIsLatest = false;
                creationPick = creationMeasure = creationAngle = creationDihedral = -1;
                syncAppearance(source.species);
                update();
                status = "Undid structure edit";
                return;
            }
            if (forward && !authorRedo.empty()) {
                authorUndo.push_back(source);
                source = authorRedo.back();
                authorRedo.pop_back();
                creationPick = creationMeasure = creationAngle = creationDihedral = -1;
                syncAppearance(source.species);
                update();
                status = "Redid structure edit";
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
    void load(const std::filesystem::path &p, int frame = 0) {
        if (busy || p.empty())
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
                auto d = io::read(p, known[frame], b, &progress, &cancel);
                known[frame].count = d.sourceCount;
                return Loaded{std::move(d), std::move(known), p, frame};
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
        if (!dropped.empty()) {
            auto droppedPath = std::move(dropped);
            dropped.clear();
            const int extraFiles = droppedExtra;
            droppedExtra = 0;
            // Same loading path as the Open button and the CLI file argument;
            // unsupported formats surface through the existing error popup.
            dropNotice = extraFiles > 0
                ? " (" + std::to_string(extraFiles) + " more dropped file(s) ignored)"
                : "";
            load(droppedPath);
        }
    }
    void open() {
        load(dialog(window, false,
                    L"Atom "
                    L"structures\0*.xyz;*.extxyz;*.vasp;*.poscar;*.contcar;POSCAR;CONTCAR;*.cif;*."
                    L"data;*.lmp;*.dump;*.lammpstrj;*.pdb;*.ent;*.gro\0All files\0*.*\0",
                    L"xyz"));
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
    // Quick-save / Save As for the session data. The quick path derives a
    // sibling "-session.xyz" destination from the input; without a loaded
    // file (or with Save As) the standard export dialog opens instead.
    void saveSessionState(bool askLocation) {
        if (askLocation || path.empty()) {
            showDataExport = true;
            return;
        }
        try {
            exportFormat = 0; // Extended XYZ quick save
            startDataExport(path.parent_path() / (path.stem().wstring() + L"-session.xyz"));
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
            gpu.draw(t, result.data, cameras[active], radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell);
        ColorLegendOptions legend{colorCoding && colorLegend, colorRangeProperty, colorGradient,
                                  colorMin, colorMax, colorReverse, colorDiscrete};
        gpu.png(t, p, legend);
        status = "Rendered " + utf8(p.filename().wstring());
    }
    void fixed(const char *name, float x, float y, float w, float h) {
        ImGui::SetNextWindowPos({x, y});
        ImGui::SetNextWindowSize({std::max(w, 1.f), std::max(h, 1.f)});
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ((std::string(name) == "Title" ||
                         (creationMode && std::string_view(name).starts_with("Creation"))) ?
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
    float pipelineWidth() const { return U(268); }   // P12 left panel column
    float workspaceWidth() const { return showWorkspace ? U(220) : 0.f; }
    float leftWidth() const { return workspaceWidth() + pipelineWidth(); }
    float rightWidth() const { return U(316); }      // P12 inspector column
    float statusHeight() const { return U(24); }     // P12 bottom status strip
    float titleBarHeight() const { return U(42); }
    float toolBarHeight() const { return U(34); }
    float topInset() const { return titleBarHeight() + (creationMode ? U(94) : toolBarHeight()); }
    void creationTop(float w) {
        const float titleH = titleBarHeight();
        fixed("Creation tabs", 0, 0, w, titleH);
        ImGui::SetCursorPosY(U(3));
        if (logo.Get()) ImGui::Image((ImTextureID)(intptr_t)logo.Get(), {U(30), U(30)});
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("AtomX  %s", atomxVersion);
        ImGui::SameLine(0, U(16));
        for (int index = 0; index < int(tabs.size()); ++index) {
            ImGui::PushID(index);
            const bool selected = index == activeTab;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, {0.102f, 0.114f, 0.125f, 1.f});
            const std::string name = selected ? tabTitle() : tabs[size_t(index)].title;
            const std::string label = name;
            if (ImGui::Button(label.c_str(), {U(212), U(32)})) switchTab(index);
            if (selected) ImGui::PopStyleColor();
            ImGui::SameLine(0, U(2));
            if (ImGui::SmallButton("x")) closeTab(index);
            ImGui::SameLine(0, U(5));
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+"))
            newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
        ImGui::SetCursorPosX(w - U(210));
        ImGui::SetCursorPosY(U(5));
        control("##minimize", 0, "Minimize to taskbar"); ImGui::SameLine();
        control("##maximize", 1, "Maximize / restore"); ImGui::SameLine();
        control("##tray", 2, "Close to system tray"); ImGui::SameLine();
        control("##exit", 3, "Exit AtomX");
        ImGui::End();

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
            if (ImGui::MenuItem("打开结构...", "Ctrl+O")) open();
            if (ImGui::MenuItem("导出结构...", "Ctrl+E")) showDataExport = true;
            if (ImGui::MenuItem("关闭创作标签")) leaveCreationTab();
        });
        menu("编辑", "##create-edit", [&] {
            if (ImGui::MenuItem("撤销", "Ctrl+Z")) history(false);
            if (ImGui::MenuItem("重做", "Ctrl+Y")) history(true);
            if (ImGui::MenuItem("删除选中", "Del")) deletePickedAtom();
        });
        menu("视图", "##create-view", [&] {
            if (ImGui::MenuItem("晶胞边框", nullptr, cell)) cell = !cell;
            if (ImGui::MenuItem("化学键", nullptr, result.data.bondStyle.visible)) {
                source.bondStyle.visible = !source.bondStyle.visible; update();
            }
            if (ImGui::MenuItem("球棍")) setDisplayStyle(0);
            if (ImGui::MenuItem("填充")) setDisplayStyle(1);
            if (ImGui::MenuItem("适应视窗")) fitCamera(3, false);
        });
        menu("修改", "##create-modify", [&] {
            if (ImGui::MenuItem("替换元素")) replacePickedElement();
            if (ImGui::MenuItem("删除 / 空位")) deletePickedAtom();
            if (ImGui::MenuItem("自动加氢")) addHydrogensCommand();
            if (ImGui::MenuItem("几何优化")) cleanGeometryCommand();
        });
        menu("构建", "##create-build", [&] {
            if (ImGui::MenuItem("新建晶体...")) openNewCell = true;
            if (ImGui::MenuItem("三斜晶胞...")) openTriclinicCell = true;
            if (ImGui::MenuItem("超胞 2 x 2 x 2"))
                adoptStructure(authoring::replicate(source, 2, 2, 2), "Built supercell");
        });
        menu("工具", "##create-tools", [&] {
            if (ImGui::MenuItem("重置视角")) resetView();
            if (ImGui::MenuItem("适应视窗")) fitCamera(3, false);
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
            ImGui::PushID(id);
            const ImVec4 bg = selected ? tone : ImVec4{tone.x * .14f, tone.y * .14f, tone.z * .14f, 1.f};
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {tone.x * .35f, tone.y * .35f, tone.z * .35f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, tone);
            ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4{.04f,.06f,.07f,1.f} : tone);
            ImGui::PushStyleColor(ImGuiCol_Border, {tone.x * .4f, tone.y * .4f, tone.z * .4f, 1.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, U(1));
            char glyph[8];
            if (glyphAvailable(codepoint)) glyphUtf8(codepoint, glyph);
            else snprintf(glyph, sizeof(glyph), "%s", fallback);
            const bool pressed = ImGui::Button(glyph, {U(35), U(35)});
            if (pressed) action();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            ImGui::PopID();
            ImGui::SameLine(0, U(4));
        };
        auto divider = [&] { toolbarSeparator(U(35)); };
        const ImVec4 gray{.62f,.67f,.73f,1}, cyan{.11f,.65f,.95f,1}, green{.20f,.86f,.49f,1},
            orange{.95f,.57f,.18f,1}, red{.95f,.25f,.30f,1}, gold{.95f,.67f,.14f,1},
            violet{.72f,.53f,1.f,1}, blue{.58f,.68f,1.f,1};
        icon("undo",0xE7A7,"<","Undo",gray,false,[&]{history(false);});
        icon("redo",0xE7A6,">","Redo",gray,false,[&]{history(true);}); divider();
        icon("select",0xE7C9,"S","Select atoms",cyan,!creationSketch,[&]{creationSketch=false;});
        icon("rotate",0xE7AD,"R","Rotate view",cyan,false,[&]{viewportTool=2;});
        icon("pan",0xE72A,"P","Pan view",green,false,[&]{viewportTool=1;});
        icon("move",0xE8AB,"M","Move picked atom",orange,false,[&]{nudgePicked(0,.2f);});
        icon("draw",0xE70F,"+","Draw atoms",green,creationSketch,[&]{creationSketch=true;}); divider();
        icon("delete",0xE74D,"X","Delete selected atom",red,false,[&]{deletePickedAtom();});
        icon("hydrogen",0xE8FA,"H+","Add hydrogens",violet,false,[&]{addHydrogensCommand();});
        icon("clean",0xE734,"*","Clean geometry",violet,false,[&]{cleanGeometryCommand();}); divider();
        icon("distance",0xE8A0,"D","Measure distance",gold,false,[&]{status="Shift-click a second atom to measure distance";});
        icon("angle",0xE8B1,"A","Measure angle",gold,false,[&]{status="Click, Shift-click, Ctrl-click to measure angle";}); divider();
        icon("ball",0xE80F,"B","Ball and stick",blue,radius<.6f,[&]{setDisplayStyle(0);});
        icon("stick",0xE8A4,"I","Stick",blue,false,[&]{setDisplayStyle(2);});
        icon("fill",0xE8B7,"O","Space filling",blue,radius>=.6f,[&]{setDisplayStyle(1);}); divider();
        icon("cell",0xE8A7,"C","Show cell",cyan,cell,[&]{cell=!cell;});
        icon("bond",0xE8D7,"-","Show bonds",cyan,source.bondStyle.visible,[&]{source.bondStyle.visible=!source.bondStyle.visible;update();}); divider();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.57f,0.39f,0.91f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.67f,0.50f,1.f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text, {0.08f,0.05f,0.13f,1.f});
        if (ImGui::Button("建晶体", {U(78), U(35)})) openNewCell=true;
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0,U(4));
        ImGui::PushStyleColor(ImGuiCol_Button, {0.13f,0.09f,0.22f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text, violet);
        if (ImGui::Button("超胞", {U(65), U(35)}))
            adoptStructure(authoring::replicate(source,2,2,2),"Built supercell");
        ImGui::SameLine(0,U(4));
        if (ImGui::Button("切面·真空", {U(100), U(35)}))
            adoptStructure(authoring::addVacuum(source,2,15),"Added vacuum");
        ImGui::PopStyleColor(2);
        divider();
        icon("home",0xE80F,"H","Reset camera",gray,false,[&]{resetView();});
        icon("fit",0xE8AA,"F","Fit structure",gray,false,[&]{fitCamera(3,false);});
        ImGui::End();
    }
    void top(float w) {
        if (creationMode) { creationTop(w); return; }
        const float titleH = titleBarHeight(), barH = toolBarHeight();
        fixed("Title", 0, 0, w, titleH);
        ImGui::SetCursorPosY(U(4));
        ImGui::Image((ImTextureID)(intptr_t)logo.Get(), {U(32),U(32)});
        ImGui::SameLine(); ImGui::SetCursorPosY(U(9));
        ImGui::TextUnformatted("AtomX");
        ImGui::SameLine(); ImGui::TextDisabled(" / Atomic visualization");
        // OVITO parity: File / Edit / Help drop-downs live in the title strip
        // next to the logo. Plain BeginMenu calls give the standard behavior
        // (click to open, hover to switch while open, click-away/Esc to
        // close) without BeginMainMenuBar, which cannot coexist with the
        // custom title bar. Disabled entries are honest about being planned.
        {
            ImGui::SameLine(); ImGui::SetCursorPosY(U(10));
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
                if (enabledItem("New Tab", "menu.file.new-tab", "Ctrl+T"))
                    newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
                if (enabledItem("Load File...", "menu.file.load-file", "Ctrl+I")) open();
                if (enabledItem("Load File in New Tab...", "menu.file.load-new-tab")) {
                    newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
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
                            if (enabledItem(utf8(file).c_str(), record.c_str())) load(file);
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
                if (enabledItem("Add Hydrogens", "menu.modify.add-hydrogens")) addHydrogensCommand();
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
                if (enabledItem("Stack a Second Layer", "menu.build.layer"))
                    adoptStructure(authoring::stackLayers(source, 15),
                                   "Stacked a second layer with a 15 A gap");
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
            const float y = U(13);
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
            const float cy = U(20);
            dl->AddRectFilled({chipX, cy - U(3)}, {chipX + U(6), cy + U(3)},
                              ImGui::GetColorU32(ImGuiCol_NavCursor), U(1));
            dl->AddText({chipX + U(12), U(13)}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
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
        ImGui::SameLine(w-U(210)); ImGui::SetCursorPosY(U(5));
        control("##minimize",0,"Minimize to taskbar"); ImGui::SameLine();
        control("##maximize",1,"Maximize / restore"); ImGui::SameLine();
        control("##tray",2,"Close to system tray (keep running)"); ImGui::SameLine();
        control("##exit",3,"Power off: exit AtomX completely");
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
            tool("Hydrogens", "Add hydrogens", [&] { addHydrogensCommand(); });
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
        fixed("Top", 0, titleH, w, barH);
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
        if (ImGui::Button("Load demo crystal", {-1, U(30)}) && !busy) {
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
    void viewport(int i, float w, float h) {
        ImGui::PushID(i);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,creationMode ? ImVec2{0,0} : ImVec2{U(4),U(4)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,{0,0,0,1});
        ImGui::BeginChild("view", {w, h}, creationMode ? ImGuiChildFlags_None : ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto &cam = cameras[i];
        auto p = ImGui::GetCursorScreenPos();
        auto avail = ImGui::GetContentRegionAvail();
        std::vector<float> originalRadii;
        if (creationMode) {
            originalRadii.reserve(gpu.styles.size());
            for (auto &style : gpu.styles) {
                originalRadii.push_back(style.visual[0]);
                const float baseRadius = style.visual[0] > 0 ? style.visual[0] :
                    style.visual[3] > 0 ? style.visual[3] : radius;
                style.visual[0] = baseRadius * .43f;
            }
        }
        gpu.target(targets[i], int(avail.x), int(avail.y));
            gpu.draw(targets[i], result.data, cam, radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell);
        for (size_t type = 0; type < originalRadii.size(); ++type)
            gpu.styles[type].visual[0] = originalRadii[type];
        ImGui::Image((ImTextureID)(intptr_t)targets[i].srv.Get(), avail);
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::GetIO().MouseWheel)
                active = i;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("viewport-structure-menu");
            if (creationMode && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                std::abs(ImGui::GetMouseDragDelta(0).x) < 4.f &&
                std::abs(ImGui::GetMouseDragDelta(0).y) < 4.f) {
                int hit = -1;
                float best = U(16) * U(16);
                auto mvp = gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f), true, radius);
                const auto &atoms = result.data.atoms;
                const size_t step = atoms.size() > 60000 ? atoms.size() / 60000 : 1;
                for (size_t index = 0; index < atoms.size(); index += step) {
                    DirectX::XMFLOAT4 q;
                    DirectX::XMStoreFloat4(
                        &q, DirectX::XMVector4Transform(
                                DirectX::XMVectorSet(atoms[index].x, atoms[index].y, atoms[index].z, 1), mvp));
                    if (q.w <= 0 || q.z <= 0) continue;
                    float sx = p.x + (q.x / q.w + 1) * avail.x * .5f;
                    float sy = p.y + (1 - q.y / q.w) * avail.y * .5f;
                    float dx = sx - ImGui::GetIO().MousePos.x, dy = sy - ImGui::GetIO().MousePos.y;
                    float dist = dx * dx + dy * dy;
                    if (dist < best) {
                        best = dist;
                        hit = int(index);
                    }
                }
                if (hit >= 0 && ImGui::GetIO().KeyAlt) creationDihedral = hit;
                else if (hit >= 0 && ImGui::GetIO().KeyCtrl) creationAngle = hit;
                else if (hit >= 0 && ImGui::GetIO().KeyShift) creationMeasure = hit;
                else if (hit >= 0) {
                    creationPick = hit;
                    creationMeasure = creationAngle = creationDihedral = -1;
                } else if (creationSketch && !ImGui::GetIO().KeyAlt &&
                           !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                    using namespace DirectX;
                    const float nx = 2.f * (ImGui::GetIO().MousePos.x - p.x) / std::max(avail.x, 1.f) - 1.f;
                    const float ny = 1.f - 2.f * (ImGui::GetIO().MousePos.y - p.y) / std::max(avail.y, 1.f);
                    const XMMATRIX inverse = XMMatrixInverse(nullptr, mvp);
                    auto worldPoint = [&](float depth) {
                        XMFLOAT4 point;
                        XMStoreFloat4(&point, XMVector4Transform(XMVectorSet(nx, ny, depth, 1.f), inverse));
                        const float reciprocal = std::abs(point.w) > 1e-6f ? 1.f / point.w : 1.f;
                        return Vec3{point.x * reciprocal, point.y * reciprocal, point.z * reciprocal};
                    };
                    const Vec3 nearPoint = worldPoint(0.f), farPoint = worldPoint(1.f);
                    const Vec3 center = authoring::cartesian(source, .5f, .5f, .5f);
                    const float dz = farPoint.z - nearPoint.z;
                    const float t = std::abs(dz) > 1e-6f ? (center.z - nearPoint.z) / dz : .5f;
                    const Vec3 placed{nearPoint.x + (farPoint.x - nearPoint.x) * t,
                                      nearPoint.y + (farPoint.y - nearPoint.y) * t,
                                      center.z};
                    addAtomAt(placed);
                }
                if (hit >= 0) active = i;
            }
            if (ImGui::IsMouseDragging(0) && viewportTool == 2) {
                cam.yaw -= ImGui::GetIO().MouseDelta.x * .008f;
                cam.pitch =
                    std::clamp(cam.pitch + ImGui::GetIO().MouseDelta.y * .008f, -1.55f, 1.55f);
                if (cam.mode < 6)
                    cam.mode = 6;
            }
            if ((ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) || (ImGui::IsMouseDragging(0) && viewportTool == 1)) {
                cam.panX += ImGui::GetIO().MouseDelta.x / std::max(avail.x, 1.f) * cam.zoom;
                cam.panY -= ImGui::GetIO().MouseDelta.y / std::max(avail.y, 1.f) * cam.zoom;
            }
            // Dragging up (negative delta) zooms in, matching the wheel
            // (wheel up -> zoom in) and OVITO's zoom tool.
            if (viewportTool == 0 && ImGui::GetIO().MouseWheel == 0 && ImGui::IsMouseDragging(0))
                cam.zoom = std::clamp(cam.zoom * powf(.985f, -ImGui::GetIO().MouseDelta.y), .01f, 50.f);
            cam.zoom = std::clamp(cam.zoom * powf(.85f, ImGui::GetIO().MouseWheel), .01f, 50.f);
        }
        auto *draw = ImGui::GetWindowDrawList();
        if (cell) {
            using namespace DirectX;
            auto m = gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f), true, radius);
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
            const ImVec2 o{p.x+U(34),p.y+avail.y-U(39)};
            draw->AddLine(o,{o.x+U(22),o.y-U(5)},IM_COL32(231,58,62,255),U(2));
            draw->AddLine(o,{o.x+U(17),o.y+U(10)},IM_COL32(37,209,89,255),U(2));
            draw->AddLine(o,{o.x,o.y-U(26)},IM_COL32(34,172,237,255),U(2));
            draw->AddText({o.x+U(24),o.y-U(12)},IM_COL32(231,58,62,255),"x");
            draw->AddText({o.x+U(18),o.y+U(7)},IM_COL32(37,209,89,255),"y");
            draw->AddText({o.x-U(4),o.y-U(38)},IM_COL32(34,172,237,255),"z");
        }
        if (colorCoding && colorLegend && avail.x >= U(220) && avail.y >= U(110)) {
            const float sx = U(174), sy = U(64), pad = U(8);
            ImVec2 a{p.x + avail.x - sx - U(12), p.y + U(12)};
            ImVec2 b{a.x + sx, a.y + sy};
            draw->AddRectFilled(a,b,IM_COL32(15,19,25,238),U(4));
            draw->AddRect(a,b,IM_COL32(104,119,138,255),U(4));
            std::string title = colorRangeProperty;
            if (title.size() > 24) title = title.substr(0,21) + "...";
            draw->AddText({a.x+pad,a.y+U(5)},IM_COL32(238,243,250,255),title.c_str());
            const float barX=a.x+pad, barY=a.y+U(25), barW=sx-pad*2, barH=U(12);
            const int segments=colorDiscrete?12:96;
            for (int segment=0; segment<segments; ++segment) {
                float u=segments>1?float(segment)/float(segments-1):0;
                if (colorReverse) u=1-u;
                if (colorDiscrete) u=std::min(std::floor(u*12),11.f)/11.f;
                auto c=sampleColorGradient(colorGradient,u);
                auto packed=ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],1});
                const float x0=barX+barW*segment/segments;
                const float x1=barX+barW*(segment+1)/segments+.5f;
                draw->AddRectFilled({x0,barY},{x1,barY+barH},packed);
            }
            char low[48]{}, high[48]{};
            snprintf(low,sizeof(low),"%.5g",colorMin);
            snprintf(high,sizeof(high),"%.5g",colorMax);
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
        if (ImGui::IsWindowHovered()) {
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
        if (creationPick >= 0 && size_t(creationPick) < result.data.atoms.size()) {
            const auto &atom = result.data.atoms[creationPick];
            auto mvp = gpu.matrix(result.data, cam, avail.x / std::max(avail.y, 1.f), true, radius);
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(
                &q, DirectX::XMVector4Transform(DirectX::XMVectorSet(atom.x, atom.y, atom.z, 1), mvp));
            if (q.w > 0 && q.z > 0) {
                ImVec2 at{p.x + (q.x / q.w + 1) * avail.x * .5f, p.y + (1 - q.y / q.w) * avail.y * .5f};
                draw->AddCircle(at, U(10), IM_COL32(255, 196, 64, 255), 20, U(2));
            }
        }
        if (ImGui::BeginPopup("viewport-structure-menu")) {
            if (ImGui::MenuItem("Zoom to extents")) fitCamera(i, false);
            if (ImGui::MenuItem("Maximize / restore viewport")) {
                active = i;
                quad = !quad;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(creationMode ? "Exit Creation Mode" : "Enter Creation Mode"))
                creationMode ? leaveCreationTab() : openCreationTab();
            if (ImGui::MenuItem("Add Atom at Cell Center"))
                addAtomAt(authoring::cartesian(source, 0.5, 0.5, 0.5));
            if (ImGui::MenuItem("Delete Picked Atom", nullptr, false, creationPick >= 0)) deletePickedAtom();
            if (ImGui::MenuItem("Replace Picked Atom", nullptr, false, creationPick >= 0)) replacePickedElement();
            if (ImGui::MenuItem("Add Hydrogens")) addHydrogensCommand();
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
    void timeline() {
        ImGui::BeginChild("Trajectory timeline", {-1,U(94)}, ImGuiChildFlags_Borders,
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
        auto p = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x, height = U(47);
        ImGui::InvisibleButton("##frame ruler", {width,height}, ImGuiButtonFlags_EnableNav);
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
        d->AddLine({knobX,p.y+U(12)},{knobX,p.y+height-U(2)},IM_COL32(240,164,49,255),U(1));
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
               "Attach hydrogens to under-coordinated C, N, O, and similar atoms", [&] { addHydrogensCommand(); });
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
                   if (creationPick >= 0 && creationMeasure >= 0)
                       status = "Distance " + std::to_string(authoring::distance(source, creationPick, creationMeasure)) + " angstrom";
                   else status = "Click the first atom, then Shift-click the second atom";
               });
        action("model.measure-angle", "Measure Bond Angle",
               "Report the angle at the Shift-clicked atom", [&] {
                   if (creationPick >= 0 && creationMeasure >= 0 && creationAngle >= 0)
                       status = "Bond angle " + std::to_string(authoring::bondAngle(source, creationPick, creationMeasure, creationAngle)) + " degrees";
                   else status = "Click, Shift-click the vertex, then Ctrl-click the third atom";
               });
        action("model.measure-dihedral", "Measure Dihedral Angle",
               "Report the dihedral of four picked atoms", [&] {
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
    void center(float w, float h) {
        fixed("Viewport workspace", leftWidth(), topInset(), w - leftWidth() - rightWidth(),
              h - topInset() - statusHeight());
        // The reference workspace opens directly onto the viewport. Keep
        // document tabs visible only when there is another tab to switch to.
        if (tabs.size() > 1) documentTabBar();
        if (creationMode) modelingToolbar();
        if (showLatticePanel) latticePanel();
        auto avail = ImGui::GetContentRegionAvail();
        float dataH = showTable ? U(280) : 0;
        float gap = ImGui::GetStyle().ItemSpacing.y;
        // Exact stack: viewports, button row, optional inspector, timeline.
        float sceneH = std::max(U(120),
                                avail.y - dataH - U(94) - ImGui::GetFrameHeight() -
                                    gap * (showTable ? 4 : 3) - U(18));
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
                        ImGui::TableSetupColumn("Bond Type");
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
                                ImGui::TextUnformatted("1");
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
        timeline();
        ImGui::End();
    }
    void creationWorkspace(float w, float h) {
        const float leftW = U(240), rightW = U(275);
        const float y = topInset(), bodyH = h - y - statusHeight();
        fixed("Creation structure", 0, y, leftW, bodyH);
        ImGui::Spacing();
        ImGui::TextDisabled("结构");
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
        const bool isRockSalt = counts.size() == 2 && counts[0] == counts[1] &&
            ((source.species[0] == "Na" && source.species[1] == "Cl") ||
             (source.species[0] == "Cl" && source.species[1] == "Na"));
        const std::string displayName = isRockSalt ? "氯化钠 岩盐" :
            creationBasedOn.empty() ? "未命名结构" : creationBasedOn;
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextWrapped("%s", displayName.c_str());
        if (headingFont) ImGui::PopFont();
        ImGui::Text("%s", formula.empty() ? "—" : formula.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s 原子", number(source.atoms.size()).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("组成");
        if (headingFont) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("点击选中同元素");
        for (size_t type = 0; type < source.species.size(); ++type) {
            const size_t count = counts[type];
            if (!count) continue;
            ImGui::PushID(int(type));
            const auto p = ImGui::GetCursorScreenPos();
            const auto color = typeColor(type);
            ImGui::GetWindowDrawList()->AddCircleFilled({p.x+U(7),p.y+U(9)},U(6),
                ImGui::GetColorU32({color[0],color[1],color[2],1.f}));
            ImGui::Dummy({U(17),U(19)});
            ImGui::SameLine();
            std::string elementName = source.species[type] == "Na" ? "钠" :
                source.species[type] == "Cl" ? "氯" :
                (elements::find(source.species[type]) ? elements::find(source.species[type])->name : "");
            std::string row = source.species[type] + "     " + elementName +
                "                         " + std::to_string(count);
            if (ImGui::Selectable(row.c_str(), false, 0, {0,U(24)})) {
                for (size_t i = 0; i < source.atoms.size(); ++i)
                    if (source.atoms[i].type == type) { creationPick = int(i); break; }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("晶格");
        if (headingFont) ImGui::PopFont();
        const auto lattice = authoring::latticeOf(source);
        auto propertyRow = [&](const char *name, const std::string &value) {
            ImGui::TextDisabled("%s", name);
            ImGui::SameLine();
            const float x = std::max(ImGui::GetCursorPosX(), leftW - ImGui::CalcTextSize(value.c_str()).x - U(24));
            ImGui::SetCursorPosX(x);
            ImGui::TextUnformatted(value.c_str());
            ImGui::Separator();
        };
        char value[80];
        for (const auto [name, length] : {std::pair{"a",lattice.a}, {"b",lattice.b}, {"c",lattice.c}}) {
            snprintf(value,sizeof(value),"%.4f Å",length); propertyRow(name,value);
        }
        for (const auto [name, angle] : {std::pair{"α",lattice.alpha}, {"β",lattice.beta}, {"γ",lattice.gamma}}) {
            snprintf(value,sizeof(value),"%.2f°",angle); propertyRow(name,value);
        }
        snprintf(value,sizeof(value),"%.2f Å³",lattice.volume); propertyRow("体积 V",value);
        propertyRow("空间群",isRockSalt?"Fm-3m (225)":"—");
        propertyRow("布拉维格子",isRockSalt?"面心立方 cF":"—");
        propertyRow("原子数",std::to_string(source.atoms.size()));
        snprintf(value,sizeof(value),"%.4f Å⁻³",
                 lattice.volume > 0 ? double(source.atoms.size()) / lattice.volume : 0.0);
        propertyRow("密度",value);
        ImGui::End();

        fixed("Creation canvas", leftW, y, w - leftW - rightW, bodyH);
        ImGui::SetCursorPos({0,0});
        viewport(3, w - leftW - rightW, bodyH);
        ImGui::End();

        fixed("Creation selection", w - rightW, y, rightW, bodyH);
        if (headingFont) ImGui::PushFont(headingFont);
        ImGui::TextUnformatted("选中");
        if (headingFont) ImGui::PopFont();
        if (creationPick < 0) {
            ImGui::TextDisabled("点击原子选择");
            ImGui::TextDisabled("Shift + 点击 多选 · 拖动空白处 框选");
            ImGui::TextDisabled("中键拖动 旋转 · 滚轮 缩放");
            ImGui::TextDisabled("右键 更多操作");
        }
        ImGui::Separator();
        if (creationPick >= 0 && size_t(creationPick) < source.atoms.size()) {
            const Atom atom = source.atoms[size_t(creationPick)];
            const char *symbol = atom.type < source.species.size()
                ? source.species[atom.type].c_str() : "?";
            colorSwatch(typeColor(atom.type));
            ImGui::SameLine();
            ImGui::Text("%s  Atom #%d", symbol, creationPick);
            float position[3] = {atom.x, atom.y, atom.z};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputFloat3("Position (A)", position, "%.4f")) {
                const int index = creationPick;
                editStructure("Moved atom " + std::to_string(index), [&](Dataset &data) {
                    data.atoms[size_t(index)].x = position[0];
                    data.atoms[size_t(index)].y = position[1];
                    data.atoms[size_t(index)].z = position[2];
                });
                creationPick = index;
            }
            ImGui::TextDisabled("Replace element");
            for (const char *element : {"H", "C", "N", "O", "Si", "Fe", "Cu", "Ni"}) {
                if (ImGui::Button(element, {U(37), U(29)})) {
                    snprintf(creationElement, sizeof(creationElement), "%s", element);
                    replacePickedElement();
                }
                if (std::string_view(element) != "Ni") ImGui::SameLine();
            }
            ImGui::Spacing();
            if (ImGui::Button("Delete / make vacancy", {-1, U(34)})) deletePickedAtom();
        }
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
        ImGui::TextUnformatted(origin.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu",source.atoms.size());
        ImGui::Unindent(U(13));
        for (size_t step = 0; step < authorUndo.size(); ++step)
            ImGui::TextDisabled("%zu  编辑结构",step+1);
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
                            float width = m.bondWidth;
                            float bondRadius = m.bondRadius;
                            auto color = m.bondColor;
                            bool changed = ImGui::Checkbox("Show bonds", &visible);
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
                            changed |= ImGui::ColorEdit3("Bond color", color.data());
                            if (changed) {
                                checkpoint(); m.bondsVisible=visible; m.bondCylinders=cylinders;
                                m.bondWidth=width; m.bondRadius=bondRadius; m.bondColor=color; update();
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
        chineseBuilder.AddText("创作模式文件编辑视图修改构建工具基于结构氯化钠岩盐组成点击选中同元素晶格体积空间群布拉维格子面心立方原子数密度历史回到该步从新撤销重做旋转缩放右键更多操作拖动空白处框选删除晶体超胞切面真空中键滚轮单位");
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
        auto fmt = io::formats[exportFormat].id;
        bool range = exportRange && frames.size() > 1;
        int first = range ? exportFirst : current, last = range ? exportLast : current,
            step = range ? exportStep : 1;
        if (first < 0 || last < first || (range && last >= int(frames.size())) || step < 1)
            throw std::runtime_error("Invalid export frame range");
        bool sequence = range && (exportSequence || !io::info(fmt).trajectory);
        if (!path.empty() && std::filesystem::exists(destination) &&
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
        exportJob = std::async(std::launch::async, [this, destination, fmt, range, sequence, first,
                                                    last, step, snapshot = result.data,
                                                    sourcePath = path, frameIndex = frames,
                                                    pipeline = modifierGraph, options = exportOptions,
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
                        io::writeFrame(file, fmt, data, options, frame);
                        file.flush();
                        if (!file)
                            throw std::runtime_error("Sequence write failed");
                    } else
                        io::writeFrame(single, fmt, data, options, frame);
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
        if (fmt != io::Format::GRO)
            ImGui::SliderInt("Numeric precision", &exportOptions.precision, 1, 17);
        else
            ImGui::TextWrapped("GRO uses nm, fixed-width 3 decimal coordinates, and at most 99999 "
                               "atoms. Residues are exported as MOL; topology is not retained.");
        if (fmt == io::Format::POSCAR) {
            ImGui::Checkbox("Fractional coordinates (Direct)", &exportOptions.fractionalPOSCAR);
            if (result.data.vectorProperties.count("MoveMask"))
                ImGui::Checkbox("Preserve selective dynamics", &exportOptions.constraints);
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
        if (creationMode) {
            ImGui::TextColored({0.29f,0.87f,0.51f,1},"创作");
            ImGui::SameLine();
            ImGui::TextDisabled("点击选择 · Shift 多选 · 拖动框选 · 中键旋转 · 滚轮缩放 · Del 删除");
            ImGui::SameLine(std::max(U(900),w-U(370)));
            ImGui::TextDisabled("%zu 原子 · 选中 %d  单位 Å (1 Å = 0.1 nm)",
                source.atoms.size(),creationPick >= 0 ? 1 : 0);
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
        // Menu accelerators. Ctrl+O loads per OVITO; Ctrl+Z/Y stay on undo /
        // redo; Ctrl+P focuses the Quick command search.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T))
            newStructureTab(authoring::orthogonalCell(8, 8, 8, "C"), "Untitled structure");
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) open();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I)) open();
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
        if (creationMode && ImGui::IsKeyPressed(ImGuiKey_Delete) && !io.WantTextInput)
            deletePickedAtom();
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
        if (creationMode) {
            creationWorkspace(w, h);
        } else {
            if (showWorkspace) left(h);
            pipelinePanel(h);
            right(w, h);
            center(w, h);
        }
        statusStrip(w, h);
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
        int adapter = -1, smoke = 0;
        bool smokeCatalog = false, smokeSettings = false, smokeExport = false, desktopTest = false,
             smokeColorLegend = false, smokeBondPairs = false, smokeInspectorNode = false,
             smokeHistogram = false, smokeTypesPanel = false, smokePalette = false,
             smokeMenu = false, smokeCreation = false;
        std::filesystem::path input, shot;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--catalog") smokeCatalog = true;
            else if (a == L"--settings") smokeSettings = true;
            else if (a == L"--export-settings")
                smokeExport = true;
            else if (a == L"--smoke-color-legend") smokeColorLegend = true;
            else if (a == L"--smoke-bond-pairs") smokeBondPairs = true;
            else if (a == L"--smoke-inspector-node") smokeInspectorNode = true;
            else if (a == L"--smoke-histogram") smokeHistogram = true;
            else if (a == L"--smoke-types-panel") smokeTypesPanel = true;
            else if (a == L"--palette") smokePalette = true;
            else if (a == L"--menu") smokeMenu = true;
            else if (a == L"--smoke-creation") smokeCreation = true;
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
                std::ofstream("build/desktop-test.txt") << "PASS: minimize, maximize, close-to-tray, restore, title drag hit test\n";
            }
            if (!input.empty())
                app.load(input);
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
