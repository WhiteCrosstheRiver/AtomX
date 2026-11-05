// Translation data checks. These need no window, so a bad pack row fails early.
#include "../src/localization.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>

namespace loc = localization;
static int failures = 0;
static void check(bool ok, const std::string &what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
static long percents(std::string_view text) { return long(std::count(text.begin(), text.end(), '%')); }

int main() {
    for (int lang = 0; lang < loc::languageCount; ++lang) {
        const auto language = loc::Language(lang);
        check(loc::fromCode(loc::code(language)) == language, "language code round trip");
        check(loc::languages[lang].name[0] != 0, "language has a native name");
    }
    check(loc::fromCode(L"xx") == loc::Language::Chinese, "unknown code falls back to Chinese");

    std::set<std::string_view> sources, packed;
    for (const auto &entry : loc::entries)
        sources.insert(entry.source);
    for (const auto &entry : loc::packEntries) {
        const std::string name = entry.source;
        check(sources.count(entry.source) == 1, "pack source is not in the string table: " + name);
        check(packed.insert(entry.source).second, "duplicate pack source: " + name);
        for (const char *text : entry.text) {
            check(text && *text, "empty translation: " + name);
            if (text)
                check(percents(text) == percents(entry.source), "printf placeholders differ: " + name);
        }
    }

    const auto original = loc::current;
    loc::current = loc::Language::French;
    check(std::string(loc::text("File")) == "Fichier", "pack text is used");
    check(std::string(loc::text("Open official source")) == "Open official source",
          "a source without a pack row shows English");
    check(std::string(loc::text("NaCl.xyz")) == "NaCl.xyz", "unknown text is left alone");
    const std::string french = loc::label("File");
    loc::current = loc::Language::Japanese;
    const std::string japanese = loc::label("File");
    check(french.substr(french.find("###")) == japanese.substr(japanese.find("###")),
          "widget IDs stay the same across pack languages");
    const char *items = loc::items("Top\0Bottom\0");
    check(std::strlen(items) > 0, "combo lists work in pack languages");
    loc::current = original;

    if (failures)
        return EXIT_FAILURE;
    std::cout << "PASS: " << loc::languageCount << " languages, " << std::size(loc::packEntries)
              << " pack rows match the string table, placeholders, English fallback, stable IDs\n";
}
