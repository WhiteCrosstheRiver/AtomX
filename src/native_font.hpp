#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <numeric>
#include <vector>
#pragma comment(lib, "gdi32.lib")

namespace atomx::native_font {
// Rasterize only when the DPI/font settings change, then use the usual GPU
// atlas. Windows supplies grid fitting; grayscale masks work on either theme
// without ClearType RGB fringes. No fonts or additional runtime DLLs are shipped.
inline bool lastBuildNative = false;
inline ImFont* add(ImFontAtlas* atlas, const char* file, float pixels,
                   const ImFontConfig* source, const ImWchar* ranges = nullptr) {
    ImFontConfig config = source ? *source : ImFontConfig{};
    const char* family = nullptr;
    if (std::strstr(file, "segoeui")) family = "Segoe UI";
    else if (std::strstr(file, "arial")) family = "Arial";
    else if (std::strstr(file, "consola")) family = "Consolas";
    else if (std::strstr(file, "msyh")) family = "Microsoft YaHei";
    else if (std::strstr(file, "Deng")) family = "DengXian";
    else if (std::strstr(file, "simhei")) family = "SimHei";
    else if (std::strstr(file, "segmdl2") || std::strstr(file, "SegoeMDL2")) family = "Segoe MDL2 Assets";
    else if (std::strstr(file, "segfluent")) family = "Segoe Fluent Icons";
    if (family) ImFormatString(config.Name, IM_ARRAYSIZE(config.Name), "@win:%s", family);
    if (std::strstr(file, "segoeuib") || std::strstr(file, "msyhbd")) config.FontBuilderFlags = FW_BOLD;
    return atlas->AddFontFromFileTTF(file, pixels, &config, ranges);
}

struct Face {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ previous = nullptr;
    float baseline = 0;
    ~Face() { if (previous) SelectObject(dc, previous); if (font) DeleteObject(font); if (dc) DeleteDC(dc); }
    bool matchesSource(const ImFontConfig& config) const {
        // GetTextFaceW may return a localized family name. Verify actual font
        // data instead, so unavailable fonts cannot silently become substitutes.
        const auto* data = (const unsigned char*)config.FontData;
        const size_t size = size_t(config.FontDataSize);
        auto u16 = [&](size_t p) { return (unsigned(data[p]) << 8) | data[p+1]; };
        auto u32 = [&](size_t p) { return (unsigned(data[p]) << 24) | (unsigned(data[p+1]) << 16) | (unsigned(data[p+2]) << 8) | data[p+3]; };
        if (!data || size < 12) return false;
        size_t offset = 0;
        if (u32(0) == 0x74746366) {
            if (config.FontNo < 0 || unsigned(config.FontNo) >= u32(8) || size < 16 + size_t(config.FontNo)*4) return false;
            offset = u32(12 + size_t(config.FontNo)*4);
        }
        if (offset > size - 12) return false;
        const unsigned count = u16(offset + 4);
        if (count > (size - offset - 12)/16) return false;
        for (unsigned i = 0; i < count; ++i) {
            const size_t entry = offset + 12 + i*16;
            if (u32(entry) != 0x68656164) continue; // 'head'
            const size_t table = u32(entry + 8), length = u32(entry + 12);
            if (length < 54 || table > size || length > size - table) return false;
            unsigned char actual[54]{};
            return GetFontData(dc, 0x64616568, 0, actual, sizeof(actual)) == sizeof(actual) && !std::memcmp(data + table, actual, sizeof(actual));
        }
        return false;
    }
    bool open(const ImFontConfig& config) {
        if (std::strncmp(config.Name, "@win:", 5) || config.RasterizerDensity != 1.f) return false;
        wchar_t family[LF_FACESIZE]{};
        if (!MultiByteToWideChar(CP_UTF8, 0, config.Name + 5, -1, family, LF_FACESIZE)) return false;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        font = CreateFontW(-int(std::round(config.SizePixels)), 0, 0, 0,
                          config.FontBuilderFlags == FW_BOLD ? FW_BOLD : FW_NORMAL,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, family);
        if (!font) return false;
        previous = SelectObject(dc, font);
        if (!previous || previous == HGDI_ERROR) { previous = nullptr; return false; }
        if (!matchesSource(config)) return false;
        // Center the face's ordinary ink in the existing logical line box.
        // This preserves layout size instead of importing generous CJK leading.
        const MAT2 identity = {{0,1},{0,0},{0,0},{0,1}};
        const bool cjk = std::wcsstr(family,L"YaHei") || std::wcsstr(family,L"Deng") || std::wcsstr(family,L"SimHei");
        const bool icons = std::wcsstr(family,L"Assets") || std::wcsstr(family,L"Icons");
        const wchar_t sample = cjk ? L'\x4e2d' : icons ? L'\xe710' : L'H';
        WORD sampleIndex = 0xffff;
        if (GetGlyphIndicesW(dc, &sample, 1, &sampleIndex, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || sampleIndex == 0xffff) return false;
        GLYPHMETRICS metrics{};
        if (GetGlyphOutlineW(dc, sampleIndex, GGO_METRICS | GGO_GLYPH_INDEX, &metrics, 0, nullptr, &identity) == GDI_ERROR) return false;
        baseline = std::round((config.SizePixels - float(metrics.gmBlackBoxY)) * .5f + float(metrics.gmptGlyphOrigin.y));
        return true;
    }
};
struct Glyph {
    int source = 0;
    ImWchar codepoint = 0;
    GLYPHMETRICS metrics{};
    int x = 0, y = 0;
    std::vector<unsigned char> bitmap;
};

inline bool buildNative(ImFontAtlas* atlas) {
    if (atlas->Sources.empty()) return false;
    std::vector<std::unique_ptr<Face>> faces;
    faces.reserve(size_t(atlas->Sources.Size));
    for (const auto& source : atlas->Sources) {
        auto face = std::make_unique<Face>();
        if (!face->open(source)) return false;
        faces.push_back(std::move(face));
    }
    std::vector<std::vector<unsigned char>> seen(size_t(atlas->Fonts.Size), std::vector<unsigned char>(65536));
    std::vector<Glyph> glyphs;
    size_t bitmapBytes = 0, totalArea = 0;
    const MAT2 identity = {{0,1},{0,0},{0,0},{0,1}};
    const int padding = std::max(atlas->TexGlyphPadding, 1);
    for (int sourceIndex = 0; sourceIndex < atlas->Sources.Size; ++sourceIndex) {
        auto& source = atlas->Sources[sourceIndex];
        const auto& face = *faces[size_t(sourceIndex)];
        int destination = -1;
        for (int n = 0; n < atlas->Fonts.Size; ++n) if (atlas->Fonts[n] == source.DstFont) destination = n;
        if (destination < 0) return false;
        const ImWchar* ranges = source.GlyphRanges ? source.GlyphRanges : atlas->GetGlyphRangesDefault();
        for (const ImWchar* range = ranges; range[0]; range += 2) {
            if (range[1] < range[0] || range[1] > 0xffff) return false;
            for (unsigned codepoint = range[0]; codepoint <= range[1]; ++codepoint) {
                if (seen[size_t(destination)][codepoint]) continue;
                const wchar_t character = wchar_t(codepoint);
                WORD index = 0xffff;
                if (GetGlyphIndicesW(face.dc, &character, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) return false;
                if (index == 0xffff) continue; // Let later merged fonts provide missing glyphs.
                Glyph glyph; glyph.source = sourceIndex; glyph.codepoint = ImWchar(codepoint);
                const DWORD bytes = GetGlyphOutlineW(face.dc, index, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                                                    &glyph.metrics, 0, nullptr, &identity);
                if (bytes == GDI_ERROR) return false;
                if (bytes > 1024 * 1024 || bitmapBytes + bytes > 64 * 1024 * 1024 || glyphs.size() >= 30000) return false;
                glyph.bitmap.resize(bytes);
                if (bytes && GetGlyphOutlineW(face.dc, index, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                                              &glyph.metrics, bytes, glyph.bitmap.data(), &identity) == GDI_ERROR) return false;
                if (!bytes) glyph.metrics.gmBlackBoxX = glyph.metrics.gmBlackBoxY = 0; // Space can report a 1x1 box without ink.
                const size_t stride = (size_t(glyph.metrics.gmBlackBoxX) + 3) & ~size_t(3);
                if (stride * glyph.metrics.gmBlackBoxY > glyph.bitmap.size()) return false;
                bitmapBytes += bytes;
                totalArea += (glyph.metrics.gmBlackBoxX + padding) * (glyph.metrics.gmBlackBoxY + padding);
                seen[size_t(destination)][codepoint] = 1;
                glyphs.push_back(std::move(glyph));
            }
        }
    }
    if (glyphs.empty()) return false;
    ImFontAtlasBuildInit(atlas);
    for (const auto& rect : atlas->CustomRects) totalArea += size_t(rect.Width + padding) * (rect.Height + padding);
    const int width = atlas->TexDesiredWidth > 0 ? atlas->TexDesiredWidth : (totalArea > 8*1024*1024 ? 4096 : totalArea > 512*1024 ? 2048 : 1024);
    if (width > 8192) return false;
    int x = padding, y = padding, rowHeight = 0;
    auto pack = [&](int w, int h, int& px, int& py) {
        if (w + padding*2 > width || h > 8192) return false;
        if (x + w + padding > width) { x = padding; y += rowHeight + padding; rowHeight = 0; }
        if (y + h + padding > 8192) return false;
        px = x; py = y; x += w + padding; rowHeight = std::max(rowHeight, h);
        return true;
    };
    for (auto& rect : atlas->CustomRects) {
        int rx = 0, ry = 0;
        if (!pack(rect.Width, rect.Height, rx, ry)) return false;
        rect.X = (unsigned short)rx; rect.Y = (unsigned short)ry;
    }
    std::vector<size_t> order(glyphs.size()); std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return glyphs[a].metrics.gmBlackBoxY > glyphs[b].metrics.gmBlackBoxY; });
    for (size_t i : order) {
        auto& glyph = glyphs[i];
        if (!pack(int(glyph.metrics.gmBlackBoxX), int(glyph.metrics.gmBlackBoxY), glyph.x, glyph.y)) return false;
    }
    atlas->ClearTexData();
    atlas->TexID = 0;
    atlas->TexWidth = width;
    atlas->TexHeight = ImUpperPowerOfTwo(y + rowHeight + padding);
    atlas->TexUvScale = {1.f / float(width), 1.f / float(atlas->TexHeight)};
    atlas->TexPixelsAlpha8 = (unsigned char*)IM_ALLOC(size_t(width) * atlas->TexHeight);
    std::memset(atlas->TexPixelsAlpha8, 0, size_t(width) * atlas->TexHeight);
    atlas->TexPixelsUseColors = false;
    for (int n = 0; n < atlas->Sources.Size; ++n) {
        auto& source = atlas->Sources[n];
        const float baseline = faces[size_t(n)]->baseline;
        ImFontAtlasBuildSetupFont(atlas, source.DstFont, &source, baseline, baseline - source.SizePixels);
    }
    for (const auto& glyph : glyphs) {
        auto& source = atlas->Sources[glyph.source];
        const auto& m = glyph.metrics;
        const float x0 = float(m.gmptGlyphOrigin.x) + source.GlyphOffset.x;
        const float y0 = std::round(faces[size_t(glyph.source)]->baseline - float(m.gmptGlyphOrigin.y) + source.GlyphOffset.y
                                    + (source.DstFont->FontSize - source.SizePixels) * .5f);
        source.DstFont->AddGlyph(&source, glyph.codepoint, x0, y0, x0 + m.gmBlackBoxX, y0 + m.gmBlackBoxY,
                                glyph.x * atlas->TexUvScale.x, glyph.y * atlas->TexUvScale.y,
                                (glyph.x + m.gmBlackBoxX) * atlas->TexUvScale.x,
                                (glyph.y + m.gmBlackBoxY) * atlas->TexUvScale.y, float(m.gmCellIncX));
        const size_t stride = (size_t(m.gmBlackBoxX) + 3) & ~size_t(3);
        for (unsigned row = 0; row < m.gmBlackBoxY; ++row)
            for (unsigned column = 0; column < m.gmBlackBoxX; ++column)
                atlas->TexPixelsAlpha8[size_t(glyph.y + row) * width + glyph.x + column] =
                    (unsigned char)((std::min(64u, unsigned(glyph.bitmap[size_t(row) * stride + column])) * 255u + 32u) / 64u);
    }
    ImFontAtlasBuildFinish(atlas);
    return true;
}
inline bool build(ImFontAtlas* atlas) {
    lastBuildNative = buildNative(atlas);
    if (lastBuildNative) return true;
    return ImFontAtlasGetBuilderForStbTruetype()->FontBuilder_Build(atlas);
}
inline const ImFontBuilderIO* builder() {
    static const ImFontBuilderIO value{build};
    return &value;
}
}
