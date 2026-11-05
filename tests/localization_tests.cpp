// Translation data checks. These need no window, so a bad pack row fails early.
#include "../src/localization.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

namespace loc = localization;
static int failures = 0;
static void check(bool ok, const std::string &what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
static long percents(std::string_view text) { return long(std::count(text.begin(), text.end(), '%')); }
// Conversion specifications in order: a format argument must not change type or
// position. A lone % followed by no conversion is a literal in a plain label.
static std::vector<std::string> conversions(std::string_view text) {
    std::vector<std::string> found;
    for (size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '%')
            continue;
        size_t end = at + 1;
        while (end < text.size() && std::string_view("-+ #0123456789.lz").find(text[end]) != std::string_view::npos)
            ++end;
        if (end < text.size() && std::string_view("%dufgscx").find(text[end]) != std::string_view::npos) {
            found.emplace_back(text.substr(at, end - at + 1));
            at = end;
        }
    }
    return found;
}

int main() {
    for (int lang = 0; lang < loc::languageCount; ++lang) {
        const auto language = loc::Language(lang);
        check(loc::fromCode(loc::code(language)) == language, "language code round trip");
        check(loc::languages[lang].name[0] != 0, "language has a native name");
    }
    check(loc::fromCode(L"xx") == loc::Language::Chinese, "unknown code falls back to Chinese");

    std::set<std::string_view> sources, packed;
    for (const auto &entry : loc::entries) {
        sources.insert(entry.source);
        check(conversions(entry.chinese) == conversions(entry.source),
              "Chinese placeholders differ: " + std::string(entry.source));
        check(conversions(entry.english) == conversions(entry.source),
              "English placeholders differ: " + std::string(entry.source));
    }
    for (const auto &entry : loc::packEntries) {
        const std::string name = entry.source;
        check(sources.count(entry.source) == 1, "pack source is not in the string table: " + name);
        check(packed.insert(entry.source).second, "duplicate pack source: " + name);
        for (const char *text : entry.text) {
            check(text && *text, "empty translation: " + name);
            if (text) {
                check(percents(text) == percents(entry.source), "printf placeholders differ: " + name);
                check(conversions(text) == conversions(entry.source), "printf placeholder order differs: " + name);
            }
        }
    }

    const auto original = loc::current;
    loc::current = loc::Language::French;
    check(std::string(loc::text("File")) == "Fichier", "pack text is used");
    check(std::string(loc::text("Open official source")) == "Ouvrir la source officielle", "panel text is translated");
    check(std::string(loc::text("Position X")) == "Position X", "property identifiers stay untranslated");
    check(std::string(loc::text("NaCl.xyz")) == "NaCl.xyz", "unknown text is left alone");
    check(loc::format("删除 %zu 个原子", size_t(3)) == "Supprimer 3 atomes", "runtime message uses the pack");
    check(loc::format("%s · 创作", "NaCl.xyz") == "NaCl.xyz · Création", "user text passes through a message");
    const std::string french = loc::label("File");
    loc::current = loc::Language::Japanese;
    const std::string japanese = loc::label("File");
    check(french.substr(french.find("###")) == japanese.substr(japanese.find("###")),
          "widget IDs stay the same across pack languages");
    const char *items = loc::items("Top\0Bottom\0");
    check(std::strlen(items) > 0, "combo lists work in pack languages");
    loc::current = loc::Language::Chinese;
    check(loc::format("删除 %zu 个原子", size_t(3)) == "删除 3 个原子", "Chinese message keeps its source wording");
    loc::current = original;

    if (failures)
        return EXIT_FAILURE;
    std::cout << "PASS: " << loc::languageCount << " languages, " << std::size(loc::packEntries) << " of "
              << std::size(loc::entries) << " rows translated (" << sources.size() - packed.size()
              << " kept in English), placeholders, runtime messages, stable IDs\n";
}
