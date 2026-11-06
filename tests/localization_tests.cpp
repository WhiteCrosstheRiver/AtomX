// Translation data checks. These need no window, so a bad pack row fails early.
#include "../src/localization.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    // Headers throw their source text and main.cpp translates it where it is
    // shown, so every Chinese exception in a header needs a table row.
    size_t thrown = 0;
    for (const auto &file : std::filesystem::directory_iterator("src")) {
        if (file.path().extension() != ".hpp")
            continue;
        std::ifstream in(file.path(), std::ios::binary);
        const std::string code((std::istreambuf_iterator<char>(in)), {});
        for (size_t at = code.find("throw "); at != std::string::npos; at = code.find("throw ", at + 1)) {
            const size_t open = code.find('"', at), end = code.find(';', at);
            const size_t close = open < end ? code.find('"', open + 1) : std::string::npos;
            if (close == std::string::npos)
                continue;
            const std::string message = code.substr(open + 1, close - open - 1);
            if (!std::any_of(message.begin(), message.end(), [](char c) { return c & 0x80; }))
                continue;
            ++thrown;
            check(sources.count(message) == 1, "header message has no table row: " + message);
        }
    }
    check(thrown > 100, "header scan found the exception messages");

    const auto original = loc::current;
    loc::current = loc::Language::French;
    check(std::string(loc::text("请选择原子")) == "Sélectionnez des atomes", "header message is translated");
    check(std::string(loc::text("File")) == "Fichier", "pack text is used");
    check(std::string(loc::text("Open official source")) == "Ouvrir la source officielle", "panel text is translated");
    check(std::string(loc::text("Position X")) == "Position X", "property identifiers stay untranslated");
    check(std::string(loc::text("NaCl.xyz")) == "NaCl.xyz", "unknown text is left alone");
    check(loc::format("删除 %zu 个原子", size_t(3)) == "Supprimer 3 atomes", "runtime message uses the pack");
    check(loc::format("%s · 创作", "NaCl.xyz") == "NaCl.xyz · Création", "user text passes through a message");
    for (auto language : {loc::Language::Italian, loc::Language::Korean, loc::Language::TraditionalChinese,
                           loc::Language::Polish, loc::Language::Turkish, loc::Language::Ukrainian}) {
        loc::current = language;
        check(std::string(loc::text("Edit")) != "Edit", "new language pack is reached in enum order");
    }
    loc::current = loc::Language::French;
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
              << " kept in English), " << thrown
              << " header messages, placeholders, runtime messages, stable IDs\n";
}
