#include "renderer.hpp"
#include "analysis.hpp"
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
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <memory>
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
// generated once at startup as a 32x32 32-bit ARGB color icon (semi-transparent
// white glass, black outline and handle, fed to CreateIconIndirect) and applied
// through a frame-local override that wndProc
// checks BEFORE the ImGui backend handler (which would otherwise overwrite it
// with the last ImGui cursor on every WM_SETCURSOR).
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
static HCURSOR createMagnifierCursor() {
    // 32-bit ARGB color cursor: semi-transparent white glass, black outline
    // and handle. The AND mask stays all zero so the color bitmap's alpha
    // channel alone drives transparency.
    constexpr int cx = 32, cy = 32;
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
    auto circleDistance = [](double x, double y) {
        return std::sqrt((x - 12.0) * (x - 12.0) + (y - 12.0) * (y - 12.0));
    };
    auto handleDistance = [](double x, double y) {
        // Distance to the segment (18,18)-(27,27); the projection parameter
        // clamps to the segment ends, giving the rounded handle tip.
        constexpr double ax = 18, ay = 18, bx = 27, by = 27;
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
            const double d = circleDistance(x + 0.5, y + 0.5);
            double alpha = 0, red = 0, green = 0, blue = 0;
            if (d < 9.5) {
                if (d < 7.5) { // interior: semi-transparent white glass
                    red = green = blue = 255;
                    alpha = 150;
                }
                // 2 px black outline with smoothstep anti-aliasing over the rim.
                const double outline = 1.0 - smoothstep(7.5, 9.5, d);
                alpha = std::max(alpha, 255.0 * outline);
                red *= 1.0 - outline;
                green *= 1.0 - outline;
                blue *= 1.0 - outline;
            }
            if (handleDistance(x + 0.5, y + 0.5) < 1.8) { // opaque black handle wins
                alpha = 255;
                red = green = blue = 0;
            }
            const DWORD a8 = DWORD(alpha + 0.5) & 0xFF;
            row[x] = (a8 << 24) | (DWORD(red + 0.5) << 16) | (DWORD(green + 0.5) << 8) | DWORD(blue + 0.5);
        }
    }
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = 12;
    info.yHotspot = 12;
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
    0xE70D,0xE70D, 0xE70E,0xE70E, 0xE713,0xE713, 0xE714,0xE714,
    0xE71D,0xE71D, 0xE722,0xE722, 0xE768,0xE768, 0xE769,0xE769,
    0xE76B,0xE76B, 0xE76C,0xE76C, 0xE792,0xE792, 0xE7A6,0xE7A7,
    0xE7B3,0xE7B3, 0xE7B8,0xE7B8, 0xE7C9,0xE7C9, 0xE81E,0xE81E,
    0xE823,0xE823, 0xE892,0xE892, 0xE893,0xE893, 0xE721,0xE721, 0xE8A3,0xE8A3,
    0xE8A7,0xE8A7, 0xE8A9,0xE8A9, 0xE8AA,0xE8AA, 0xE8B5,0xE8B5,
    0xE8C8,0xE8C8, 0xE8E5,0xE8E5, 0xE8F1,0xE8F1, 0xE91B,0xE91B,
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
static void theme(int choice = 1) {
    ImGui::GetStyle() = ImGuiStyle{};
    if (choice == 1) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {10,8}; s.FramePadding = {6,3}; s.ItemSpacing = {6,4};
    s.WindowRounding = 0; s.ChildRounding = 3; s.FrameRounding = 3; s.PopupRounding = 5;
    s.ScrollbarSize = 13; s.WindowBorderSize = 0; s.ChildBorderSize = 1;
    s.FrameBorderSize = 1; s.PopupBorderSize = 1; s.GrabRounding = 2;
    s.DisabledAlpha = .72f;
    auto *c = s.Colors;
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
static const char *atomxVersion = "0.1.0";
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
    void history(bool forward) {
        if (!applyModifierHistory(mods, undo, redo, forward)) return;
        modifierGraph.selected = mods.empty() ? 0 : std::min(modifierGraph.selected, mods.size() - 1);
        pipelineCheckpoint.reset();
        pipelineCheckpointNode=SIZE_MAX;
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
                         ImGuiWindowFlags_NoSavedSettings | (std::string(name) == "Title" ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0));
    }
    void recordUiTestItem(const std::string &name,const char *explicitLabel=nullptr) {
        if (!captureUiTestItems) return;
        const ImGuiID id=explicitLabel ? ImGui::GetID(explicitLabel) : ImGui::GetItemID();
        uiTestItems[name]={id,ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
                           ImGui::IsItemHovered(),ImGui::IsItemClicked()};
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
    float leftWidth() const { return showWorkspace ? U(220) : 0.f; }
    float rightWidth() const { return U(preferences.size >= 19 ? 400.f : 370.f); }
    float titleBarHeight() const { return U(42); }
    float toolBarHeight() const { return U(34); }
    float topInset() const { return titleBarHeight() + toolBarHeight(); }
    void top(float w) {
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
                if (enabledItem("Load File...", "menu.file.load-file", "Ctrl+I")) open();
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
            titleWindow->DC.LayoutType = savedLayout;
        }
        // OVITO parity: the "Pipelines: <source>" selector lives in the
        // window's top strip, right-aligned before the min/max/close cluster
        // (which occupies the right ~U(215) of the row).
        const std::string sourceLabel = path.empty()
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
        fixed("Workspace", 0, topInset(), leftWidth(), h - topInset());
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
    const char *views = "Top\0Bottom\0Front\0Back\0Left\0Right\0Ortho\0Perspective\0";
    void viewport(int i, float w, float h) {
        ImGui::PushID(i);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{U(4),U(4)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg,{0,0,0,1});
        ImGui::BeginChild("view", {w, h}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto &cam = cameras[i];
        ImGui::PushStyleColor(ImGuiCol_Text,{.95f,.97f,1,1});
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,{.73f,.78f,.85f,1});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,{.035f,.045f,.06f,1});
        ImGui::PushStyleColor(ImGuiCol_Button,{.10f,.13f,.17f,1});
        ImGui::PushStyleColor(ImGuiCol_PopupBg,{.055f,.07f,.095f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{.16f,.26f,.39f,1});
        ImGui::PushStyleColor(ImGuiCol_Header,{.12f,.21f,.34f,1});
        ImGui::SetNextItemWidth(U(140));
        ImGui::Combo("##camera", &cam.mode, views);
        ImGui::SameLine();
        ImGui::TextDisabled(i == active ? "ACTIVE" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            fitCamera(i,false);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedCount==0);
        if (ImGui::SmallButton("Fit selected")) fitCamera(i,true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && selectedCount==0)
            ImGui::SetTooltip("Select particles in the data table first");
        ImGui::PopStyleColor(7);
        auto p = ImGui::GetCursorScreenPos();
        auto avail = ImGui::GetContentRegionAvail();
        gpu.target(targets[i], int(avail.x), int(avail.y));
            gpu.draw(targets[i], result.data, cam, radius, particleShape, renderMode, colorAxis, colorGradient, colorReverse?colorMax:colorMin, colorReverse?colorMin:colorMax, colorCoding, colorDiscrete, colorSelectedOnly, bg, particles, cell);
        ImGui::Image((ImTextureID)(intptr_t)targets[i].srv.Get(), avail);
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::GetIO().MouseWheel)
                active = i;
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
            if (viewportTool == 0 && ImGui::GetIO().MouseWheel == 0 && ImGui::IsMouseDragging(0))
                cam.zoom = std::clamp(cam.zoom * powf(.985f, ImGui::GetIO().MouseDelta.y), .01f, 50.f);
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
                        auto c = ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]*cellGlow});
                        auto hi = ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],cellColor[3]});
                        draw->AddLine(corners[j], corners[j | k], c, cellWidth+4.f);
                        if (!cellDashed) draw->AddLine(corners[j], corners[j | k], hi, cellWidth);
                        else { auto a=corners[j], b=corners[j|k]; for(int q=0;q<8;q+=2){float t0=q/8.f,t1=(q+1)/8.f;draw->AddLine({a.x+(b.x-a.x)*t0,a.y+(b.y-a.y)*t0},{a.x+(b.x-a.x)*t1,a.y+(b.y-a.y)*t1},hi,cellWidth);} }
                    }
            if (cellLabels) { draw->AddText(corners[0], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "O"); draw->AddText(corners[1], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "A"); draw->AddText(corners[2], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "B"); draw->AddText(corners[4], ImGui::ColorConvertFloat4ToU32({cellColor[0],cellColor[1],cellColor[2],1}), "C"); }
            draw->PopClipRect();
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
        if (ImGui::IsItemHovered()) {
            const char* toolHint = viewportTool == 0 ? "Zoom" : viewportTool == 1 ? "Pan" : viewportTool == 2 ? "Orbit" : "FOV";
            draw->AddText({p.x + U(12), p.y + avail.y - U(25)}, IM_COL32(190, 202, 215, 255),
                          (std::string("Tool: ") + toolHint + "   Wheel zoom").c_str());
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
        if (i == active) {
            draw->AddRect(p, {p.x + avail.x, p.y + avail.y}, IM_COL32(95, 180, 255, 255));
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
    bool transport(const char *name, unsigned glyph, int kind, const char *tip) {
        // Compact transport icon button; keeps the legacy vector-drawn arrows
        // when the Segoe MDL2 icon font is unavailable so the cluster never
        // degrades to blank or text-only buttons.
        char label[8];
        const bool useGlyph = glyphAvailable(glyph);
        if (useGlyph)
            glyphUtf8(glyph, label);
        ImGui::PushID(name);
        const bool pressed = useGlyph ? ImGui::Button(label, {U(26), U(26)})
                                      : ImGui::Button("##transport", {U(26), U(26)});
        const auto p = ImGui::GetItemRectMin();
        ImGui::PopID();
        if (!useGlyph) {
            auto *d = ImGui::GetWindowDrawList();
            auto color = ImGui::GetColorU32(ImGuiCol_Text);
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
        ImGui::BeginChild("Trajectory timeline", {-1,U(128)}, ImGuiChildFlags_Borders,
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
            separatorWidth + 4*square + 3*gap + separatorWidth + 2*square + gap;
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
        if (transport("##play",playing ? 0xE769 : 0xE768,2,"Play / pause")) playing = !playing; ImGui::SameLine();
        if (transport("##next",0xE76C,3,"Next frame (Right)")) seekFrame(selected+1); ImGui::SameLine();
        if (transport("##last",0xE893,4,"Last frame (End)")) seekFrame(last);
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
        auto toolButton = [&](const char* name, unsigned glyph, const char* fallback,
                              int tool, const char* tip) {
            if (iconButton(name, glyph, fallback, tip, viewportTool == tool, square))
                viewportTool = tool;
        };
        toolButton("timeline.zoom", 0xE721, "Zoom", 0, "Zoom active viewport (drag or wheel)");
        ImGui::SameLine();
        toolButton("timeline.pan", 0xE7C9, "Pan", 1, "Pan active viewport");
        ImGui::SameLine();
        toolButton("timeline.orbit", 0xE7B8, "Orbit", 2, "Orbit active viewport");
        ImGui::SameLine();
        toolButton("timeline.fov", 0xE714, "FOV", 3, "Adjust perspective field of view");
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
        float width = ImGui::GetContentRegionAvail().x, height = U(65);
        ImGui::InvisibleButton("##frame ruler", {width,height}, ImGuiButtonFlags_EnableNav);
        bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        recordUiTestItem("timeline.ruler");
        auto *d = ImGui::GetWindowDrawList();
        // P11 OVITO-style ruler: a rounded track bar with the played portion
        // accent-tinted, short minor/major ticks and frame labels beneath it,
        // and a single circular knob on the track for the current frame. All
        // colors derive from the theme (text/accent), never hardcoded.
        const float left = p.x+U(15), right = p.x+width-U(18);
        const float trackTop = p.y+U(14), trackBottom = trackTop+U(9);
        const float trackWidth = std::max(1.f, right-left);
        const auto text = ImGui::GetColorU32(ImGuiCol_Text);
        const auto tick = ImGui::GetColorU32(ImGuiCol_Text, .45f);
        const auto trackRest = ImGui::GetColorU32(ImGuiCol_Text, .14f);
        ImVec4 playedTint = accent; playedTint.w *= .55f;
        const auto trackPlayed = ImGui::GetColorU32(playedTint);
        const int64_t major = timelineLabelStep(last, trackWidth);
        int64_t minor = major >= 5 ? major/5 : 1;
        while (last > 0 && minor < last && trackWidth*float(minor)/float(last) < U(3))
            minor *= 2;
        auto xFor = [&](int frame) { return left+trackWidth*(last ? float(frame)/last : 0.f); };
        d->AddRectFilled({left,trackTop},{right,trackBottom},trackRest,U(4));
        const float knobX = xFor(selected);
        if (knobX > left+1.f)
            d->AddRectFilled({left,trackTop},{knobX,trackBottom},trackPlayed,U(4),
                             ImDrawFlags_RoundCornersLeft);
        for (int64_t frame=0;frame<=last;frame+=minor) {
            const float x=xFor(int(frame));
            const bool isMajor = frame%major==0;
            d->AddLine({x,trackBottom},{x,trackBottom+U(isMajor?8.f:4.f)},tick,U(1));
            if (isMajor) {
                const auto label=std::to_string(frame); // 0-based, matching the spinner's "/ total".
                const auto size=ImGui::CalcTextSize(label.c_str());
                d->AddText({std::clamp(x-size.x*.5f,p.x+2.f,p.x+width-size.x-2.f),
                            trackBottom+U(10)},text,label.c_str());
            }
        }
        d->AddCircleFilled({knobX,(trackTop+trackBottom)*.5f},U(5),ImGui::GetColorU32(accent));
        d->AddCircle({knobX,(trackTop+trackBottom)*.5f},U(5),
                     ImGui::GetColorU32(ImGuiCol_Text,.6f),0,U(1.5f));
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
    void center(float w, float h) {
        fixed("Viewport workspace", leftWidth(), topInset(), w - leftWidth() - rightWidth(),
              h - topInset());
        auto avail = ImGui::GetContentRegionAvail();
        float dataH = showTable ? U(280) : 0;
        float gap = ImGui::GetStyle().ItemSpacing.y;
        // Exact stack: viewports, button row, optional inspector, timeline.
        float sceneH = std::max(U(150),
                                avail.y - dataH - U(128) - ImGui::GetFrameHeight() -
                                    gap * (showTable ? 3 : 2));
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
    void right(float w, float h) {
        fixed("Properties", w - rightWidth(), topInset(), rightWidth(), h - topInset());
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
        // The Pipelines data-source selector moved into the title-bar strip
        // (OVITO layout); the panel starts directly with Add modification.
        if (ImGui::Button("Add modification...", {-1, U(28)}))
            showCatalog = true;
        recordUiTestItem("pipeline.add-modification");
        {
            auto p = ImGui::GetItemRectMax();
            const float cy = p.y - U(14);
            ImGui::GetWindowDrawList()->AddTriangleFilled({p.x-U(18),cy-U(4)},{p.x-U(10),cy-U(4)},{p.x-U(14),cy+U(1)},ImGui::GetColorU32(ImGuiCol_Text));
        }
        catalogAnchor = {ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + 2};
        if (rightTab == 0) {
                ImGui::BeginChild("Stack", {-1, U(205)}, ImGuiChildFlags_Borders);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(4), U(2)});
                // Panel-side failure indicator (replaces the removed status
                // bar): the failing node carries the red "!" below and this
                // one-line hint repeats the message inside the panel.
                for (const auto &failed : mods)
                    if (!failed.error.empty()) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(4), U(2)});
                        ImGui::TextColored({1, .32f, .28f, 1}, "! %s", failed.error.c_str());
                        ImGui::PopStyleVar();
                        break;
                    }
                if (mods.empty())
                    ImGui::TextWrapped("Add a modification to start processing.");
                bool stackMutated = false;
                constexpr float stripGutter = 30.f; // Room for the right-side action strip.
                for (int i = int(mods.size()) - 1; i >= 0; i--) {
                    auto &m = mods[i];
                    ImGui::PushID(m.id.c_str());
                    const auto &style = ImGui::GetStyle();
                    const float labelWidth = std::max(0.f, ImGui::GetContentRegionAvail().x -
                        ImGui::GetFrameHeight() - style.ItemSpacing.x * 2 - U(stripGutter));
                    bool enabled = m.enabled;
                    bool enabledChanged=ImGui::Checkbox("##enabled", &enabled);
                    recordUiTestItem(std::string("pipeline.node.enabled.")+m.id);
                    if (enabledChanged) {
                        checkpoint();
                        m.enabled = enabled;
                        update(size_t(i));
                    }
                    ImGui::SameLine();
                    const char *name = m.displayName.empty() ? opName(m.op) : m.displayName.c_str();
                    if (ImGui::Selectable(name, modifierGraph.selected == size_t(i),
                                          ImGuiSelectableFlags_None,{labelWidth,0}))
                        modifierGraph.selected = size_t(i);
                    recordUiTestItem(std::string("pipeline.node.select.")+m.id);
                    if (!m.error.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored({1, .32f, .28f, 1}, "!");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.error.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::PopStyleVar();
                ImGui::EndChild();
                // Vertical action strip pinned to the stack's right edge. It
                // acts on the selected node; every button keeps its recorded
                // UI-test name keyed by that node id. Overlaid as a borderless
                // transparent child on top of the stack region.
                if (!mods.empty()) {
                    constexpr float stripButton = 24.f;
                    const ImVec2 stackMin = ImGui::GetItemRectMin(), stackMax = ImGui::GetItemRectMax();
                    const ImVec2 flowCursor = ImGui::GetCursorPos();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0,0,0,0});
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {U(1), U(2)});
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {U(2), U(2)});
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {U(4), U(2)});
                    ImGui::SetCursorScreenPos({stackMax.x - U(stripGutter), stackMin.y + U(3)});
                    if (ImGui::BeginChild("##node-actions", {U(stripGutter) - U(2), U(4*stripButton + 3*2 + 6)},
                                          ImGuiChildFlags_None,
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground)) {
                        if (modifierGraph.selected < mods.size()) {
                            const size_t selectedNode = modifierGraph.selected;
                            const std::string selectId = mods[selectedNode].id;
                            if (iconButton("pipeline.strip.delete", 0xE74D, "x",
                                           "Delete the selected modification", false,
                                           U(stripButton), ("pipeline.node.delete."+selectId).c_str())) {
                                checkpoint();
                                modifierGraph.erase(selectedNode);
                                update(selectedNode);
                                stackMutated = true;
                            }
                            ImGui::BeginDisabled(selectedNode + 1 >= mods.size());
                            if (iconButton("pipeline.strip.up", 0xE70E, "^",
                                           "Move the selected modification up (later in the pipeline)",
                                           false, U(stripButton),
                                           ("pipeline.node.up."+selectId).c_str())) {
                                checkpoint();
                                modifierGraph.move(selectedNode, selectedNode + 1);
                                update(selectedNode);
                                stackMutated = true;
                            }
                            ImGui::EndDisabled();
                            ImGui::BeginDisabled(selectedNode == 0);
                            if (iconButton("pipeline.strip.down", 0xE70D, "v",
                                           "Move the selected modification down (earlier in the pipeline)",
                                           false, U(stripButton),
                                           ("pipeline.node.down."+selectId).c_str())) {
                                checkpoint();
                                modifierGraph.move(selectedNode, selectedNode - 1);
                                update(selectedNode - 1);
                                stackMutated = true;
                            }
                            ImGui::EndDisabled();
                            if (iconButton("pipeline.strip.copy", 0xE8C8, "Copy",
                                           "Duplicate the selected modification", false,
                                           U(stripButton), ("pipeline.node.copy."+selectId).c_str())) {
                                checkpoint();
                                auto copy = mods[selectedNode];
                                copy.id = std::to_string(nextModifierId++);
                                copy.error.clear(); copy.outputs.clear(); copy.dirty = true;
                                modifierGraph.insert(std::move(copy), selectedNode + 1);
                                update();
                                stackMutated = true;
                            }
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleVar(3);
                    ImGui::PopStyleColor();
                    // The overlay child is an item in the Properties flow;
                    // restore the cursor so the parameter section follows the
                    // stack rather than the strip.
                    ImGui::SetCursorPos(flowCursor);
                }
                if (!stackMutated && !mods.empty() && modifierGraph.selected < mods.size()) {
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
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragScalar("Distance", ImGuiDataType_Double, &distance,
                                                     .02f, nullptr, nullptr, "%.4g");
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (x)", &normal[0], .01f);
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (y)", &normal[1], .01f);
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragFloat("Normal (z)", &normal[2], .01f);
                        const double minimumWidth = 0;
                        ImGui::SetNextItemWidth(U(140));
                        changed |= ImGui::DragScalar("Slab width", ImGuiDataType_Double, &width,
                                                     .02f, &minimumWidth, nullptr, "%.4g");
                        changed |= ImGui::Checkbox("Reverse orientation", &reverse);
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
                        if (edited.colorAutoRange) {
                            double lo = 0, hi = 0;
                            const bool hasRange = propertyValueRange(result.data, edited.property, lo, hi);
                            ImGui::BeginDisabled(!hasRange);
                            float low = hasRange ? float(lo) : 0.f, high = hasRange ? float(hi) : 0.f;
                            ImGui::DragFloat("Start value", &low, .01f);
                            ImGui::DragFloat("End value", &high, .01f);
                            ImGui::EndDisabled();
                            if (!hasRange)
                                ImGui::TextDisabled("The property has no finite values on this frame.");
                        } else {
                            float low = edited.colorMin, high = edited.colorMax;
                            if (ImGui::DragFloat("Start value", &low, .01f)) { edited.colorMin = low; edited.colorAllFramesRange = false; changed = true; }
                            if (ImGui::DragFloat("End value", &high, .01f)) { edited.colorMax = high; edited.colorAllFramesRange = false; changed = true; }
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
                    ImGui::SliderFloat("Line width", &cellWidth, .5f, 4.f, "%.1f px");
                    ImGui::SliderFloat("Glow", &cellGlow, 0.f, 1.f, "%.2f");
                    ImGui::Checkbox("Dashed secondary edges", &cellDashed); ImGui::Checkbox("Show cell labels", &cellLabels);
                    ImGui::TextDisabled("PBC: %s %s %s", source.pbc[0]?"X":"-", source.pbc[1]?"Y":"-", source.pbc[2]?"Z":"-");
                }
                heading("Data source");
                ImGui::TextWrapped("%s", path.empty() ? "Generated FCC crystal"
                                                      : utf8(path.wstring()).c_str());
                ImGui::TextDisabled("Reader: %s", readerName.c_str());
                ImGui::TextDisabled("Trajectory: %zu frame(s)", std::max<size_t>(frames.size(), 1));
                heading("Particle appearance");
                if (focusParticleAppearance) { ImGui::SetScrollHereY(0.f); focusParticleAppearance = false; }
                ImGui::SliderFloat("Radius", &radius, .02f, 2.f, "%.2f");
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
        headingFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", float(preferences.size)*uiScale);
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
            bool changed = ImGui::Combo("Theme", &preferences.theme, "Slate dark\0Classic light\0Midnight\0");
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
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Not implemented. This entry does not execute an algorithm.");
            };
            auto beginCard = [&](const char *title) {
                ImGui::BeginChild(title,{0,0},ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                heading(title);
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
        if (playing && !busy && frames.size() > 1 && ImGui::GetTime() - lastFrame > 1 / fps) {
            lastFrame = ImGui::GetTime();
            load(path, (current + 1) % int(frames.size()));
        }
        float w = io.DisplaySize.x, h = io.DisplaySize.y;
        top(w);
        if (showWorkspace) left(h);
        right(w, h);
        center(w, h);
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
                "A Direct3D 11 accelerated structure viewer and modifier pipeline, "
                "modeled on the OVITO workflow.");
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
        if (ImGui::BeginPopupModal("Operation failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", error.c_str());
            if (ImGui::Button("OK")) {
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
             smokeMenu = false;
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
        g_magnifierCursor = createMagnifierCursor();
        Renderer gpu;
        renderer = &gpu;
        gpu.init(window, adapter);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17);
        uiScale = std::max(1.f, GetDpiForWindow(window)/96.f);
        theme();
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
                float clear[4] = {.15f, .17f, .2f, 1};
                gpu.context->OMSetRenderTargets(1, gpu.back.GetAddressOf(), nullptr);
                gpu.context->ClearRenderTargetView(gpu.back.Get(), clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                if (smoke && !app.busy && !app.pipelineBusy && !app.inspectorBusy &&
                    (!smokeInspectorNode || inspectorSmokeStarted) && ++ticks >= smoke) {
                    if (!app.error.empty())
                        throw std::runtime_error(app.error);
                    if (!shot.empty()) {
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
