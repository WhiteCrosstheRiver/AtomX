#include "../src/native_font.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void checkAtlas(float dpi, int choice) {
    ImFontAtlas atlas;
    atlas.FontBuilderIO = atomx::native_font::builder();
    const char* latin[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/consola.ttf"};
    static const ImWchar icons[] = {0xe700, 0xe711, 0};
    static const ImWchar cjk[] = {0x4e2d, 0x4e2d, 0x5b57, 0x5b57, 0x6d4b, 0x6d4b, 0x91cf, 0x91cf, 0};
    for (float logical : {14.f, 11.f, 16.f, 30.f}) {
        const float pixels = std::round(logical * dpi);
        ImFontConfig config{}; config.PixelSnapH = true;
        require(atomx::native_font::add(&atlas, latin[choice], pixels, &config) != nullptr, "Latin source");
        config.MergeMode = true;
        require(atomx::native_font::add(&atlas, "C:/Windows/Fonts/msyh.ttc", pixels, &config, cjk) != nullptr, "CJK source");
        require(atomx::native_font::add(&atlas, "C:/Windows/Fonts/segmdl2.ttf", pixels, &config, icons) != nullptr, "Icon source");
    }
    require(atlas.Build(), "Atlas build");
    require(atomx::native_font::lastBuildNative, "Native rasterizer unexpectedly fell back");
    require(atlas.TexWidth <= 8192 && atlas.TexHeight <= 8192, "Atlas limit");
    for (auto* font : atlas.Fonts) {
        for (ImWchar codepoint : std::initializer_list<ImWchar>{'H', 'g', ' ', 0x4e2d, 0x6d4b, 0xe710})
            require(font->FindGlyphNoFallback(codepoint) != nullptr, "Missing Latin/CJK/icon glyph");
        const auto* chinese = font->FindGlyphNoFallback(0x4e2d);
        const auto* latinGlyph = font->FindGlyphNoFallback('H');
        const auto* plusIcon = font->FindGlyphNoFallback(0xe710);
        require(chinese->AdvanceX == font->FontSize, "CJK em must match requested physical size");
        require(chinese->Y0 >= -1 && chinese->Y1 <= font->FontSize + 1, "CJK baseline alignment");
        require(latinGlyph->Y0 >= -1 && latinGlyph->Y1 <= font->FontSize + 1, "Latin baseline alignment");
        require(plusIcon->Y0 >= -1 && plusIcon->Y1 <= font->FontSize + 1, "Icon baseline alignment");
        require(chinese->Y1 - chinese->Y0 >= font->FontSize * .75f, "CJK ink unexpectedly shrunk");
        for (const auto& glyph : font->Glyphs)
            require(glyph.U0 >= 0 && glyph.V0 >= 0 && glyph.U1 <= 1 && glyph.V1 <= 1, "Glyph outside atlas");
    }
    // Unknown sources keep the established STB path instead of breaking startup.
    atlas.Clear();
    atlas.AddFontDefault();
    require(atlas.Build() && !atomx::native_font::lastBuildNative, "Default-font fallback");
}
int main() {
    try {
        ImGui::CreateContext();
        checkAtlas(1, 0); // Warm up Windows font caches before resource accounting.
        const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (float dpi : {1.f, 1.25f, 1.5f, 2.f})
            for (int choice = 0; choice < 3; ++choice) checkAtlas(dpi, choice);
        require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before, "GDI font/DC handle leak");
        ImGui::DestroyContext();
        std::puts("PASS native fonts: DPI, compact CJK/Latin metrics, merged icons, atlas bounds, fallback, GDI resource release");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL native fonts: %s\n", e.what()); return 1; }
}
