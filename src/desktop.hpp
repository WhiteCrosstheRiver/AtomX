#pragma once
#include "renderer.hpp"
#include <shellapi.h>
#include <windowsx.h>

namespace desktop {
inline NOTIFYICONDATAW tray{};
inline bool inTray = false;
inline UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
inline constexpr UINT trayMessage = WM_APP + 17;
inline std::filesystem::path settingsPath() {
    wchar_t dir[32768]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, 32768))
        return std::filesystem::temp_directory_path() / "AtomX-settings.ini";
    auto path = std::filesystem::path(dir) / "AtomX";
    std::filesystem::create_directories(path);
    return path / "settings.ini";
}
struct Preferences {
    int theme = 1, font = 0, size = 18;
    // Most recently opened files, persisted across launches and shown by the
    // File > Recent Files submenu. Newest entry first, at most 8 kept.
    static constexpr int maxRecentFiles = 8;
    std::vector<std::wstring> recentFiles;
    void load() {
        auto p = settingsPath().wstring();
        theme = std::clamp(int(GetPrivateProfileIntW(L"Appearance", L"Theme", 1, p.c_str())), 0, 2);
        font = std::clamp(int(GetPrivateProfileIntW(L"Appearance", L"Font", 0, p.c_str())), 0, 2);
        size = std::clamp(int(GetPrivateProfileIntW(L"Appearance", L"Size", 18, p.c_str())), 14, 20);
        recentFiles.clear();
        for (int i = 0; i < maxRecentFiles; ++i) {
            wchar_t buffer[32768]{};
            const auto key = L"File" + std::to_wstring(i);
            GetPrivateProfileStringW(L"Recent", key.c_str(), L"", buffer, 32768, p.c_str());
            if (buffer[0]) recentFiles.push_back(buffer);
        }
    }
    void save() const {
        auto p = settingsPath().wstring();
        for (auto entry : {std::pair{L"Theme", theme}, {L"Font", font}, {L"Size", size}})
            if (!WritePrivateProfileStringW(L"Appearance", entry.first,
                                            std::to_wstring(entry.second).c_str(), p.c_str()))
                throw std::runtime_error("Unable to save appearance settings");
        // Writing an empty value removes the key, so a shorter list shrinks.
        for (int i = 0; i < maxRecentFiles; ++i) {
            const auto key = L"File" + std::to_wstring(i);
            const auto text = i < int(recentFiles.size()) ? recentFiles[size_t(i)] : std::wstring();
            if (!WritePrivateProfileStringW(L"Recent", key.c_str(), text.c_str(), p.c_str()))
                throw std::runtime_error("Unable to save recent files");
        }
    }
};
inline void restore(HWND window) {
    ShowWindow(window, IsZoomed(window) ? SW_SHOW : SW_RESTORE);
    SetForegroundWindow(window);
    Shell_NotifyIconW(NIM_DELETE, &tray);
    inTray = false;
}
inline void hide(HWND window) {
    if (inTray || Shell_NotifyIconW(NIM_ADD, &tray)) {
        inTray = true;
        ShowWindow(window, SW_HIDE);
    } else {
        // A missing notification area must never make the app inaccessible.
        ShowWindow(window, SW_MINIMIZE);
    }
}
inline void initTray(HWND window, HICON icon) {
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = trayMessage;
    tray.hIcon = icon;
    wcscpy_s(tray.szTip, L"AtomX - double-click to restore");
}
inline LRESULT hitTest(HWND window, LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(window, &p);
    RECT r{};
    GetClientRect(window, &r);
    if (!IsZoomed(window)) {
        bool l = p.x < 7, right = p.x >= r.right - 7;
        bool t = p.y < 7, b = p.y >= r.bottom - 7;
        if (t) return l ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (b) return l ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (l) return HTLEFT;
        if (right) return HTRIGHT;
    }
    float scale = std::max(1.f,GetDpiForWindow(window)/96.f);
    return p.y < 42*scale && p.x < r.right - 220*scale ? HTCAPTION : HTCLIENT;
}
inline ComPtr<ID3D11ShaderResourceView> loadLogo(ID3D11Device *device, HICON &icon) {
    auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!resource) throw std::runtime_error("Missing embedded AtomX logo");
    auto data = LockResource(LoadResource(nullptr, resource));
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)), "Logo decoder");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "Logo stream");
    check(stream->InitializeFromMemory(static_cast<BYTE *>(data), SizeofResource(nullptr, resource)), "Logo bytes");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "Logo PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "Logo frame");
    ComPtr<IWICBitmapScaler> scaled;
    check(factory->CreateBitmapScaler(&scaled), "Logo scaler");
    check(scaled->Initialize(frame.Get(), 64, 64, WICBitmapInterpolationModeFant), "Logo size");
    ComPtr<IWICFormatConverter> converted;
    check(factory->CreateFormatConverter(&converted), "Logo converter");
    check(converted->Initialize(scaled.Get(), GUID_WICPixelFormat32bppBGRA,
          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Logo format");
    std::array<BYTE, 64 * 64 * 4> pixels{};
    check(converted->CopyPixels(nullptr, 256, UINT(pixels.size()), pixels.data()), "Logo pixels");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), 256, 0};
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&desc, &initial, &texture), "Logo texture");
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Logo view");
    icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), IMAGE_ICON, 64, 64, 0);
    if (!icon) throw std::runtime_error("Missing embedded AtomX icon");
    return view;
}
}

