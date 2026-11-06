#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <cwchar>

// UI-only localization. Scientific keys, expressions, user labels, filenames and
// serialized documents retain their original spelling. No network/dependencies.
namespace localization {
// Values index every per-language array. Append new languages at the end.
enum class Language {
    Chinese, English, French, German, Spanish, Portuguese, Russian, Japanese,
    Italian, Korean, TraditionalChinese, Polish, Turkish, Ukrainian
};
inline constexpr int languageCount = 14;
// Languages after Chinese and English come from localization_packs.inc.
inline constexpr int packLanguageCount = languageCount - 2;
struct LanguageInfo { const wchar_t *code; const char *name; };
inline constexpr LanguageInfo languages[languageCount] = {
    {L"zh-CN", "简体中文"}, {L"en", "English"},   {L"fr", "Français"}, {L"de", "Deutsch"},
    {L"es", "Español"},     {L"pt", "Português"}, {L"ru", "Русский"},  {L"ja", "日本語"},
    {L"it", "Italiano"},    {L"ko", "한국어"},    {L"zh-TW", "繁體中文"}, {L"pl", "Polski"},
    {L"tr", "Türkçe"},        {L"uk", "Українська"},
};
inline Language current = Language::Chinese;
inline const wchar_t *code(Language value) { return languages[int(value)].code; }
inline Language fromCode(const wchar_t *value) {
    for(int lang=0;lang<languageCount;++lang)
        if(std::wcscmp(value,languages[lang].code)==0) return Language(lang);
    return Language::Chinese;
}
struct Entry { const char *source; const char *chinese; const char *english; };
inline constexpr Entry entries[] = {
#include "localization_strings.inc"
};
// Every language after Chinese and English, in enum order. A source
// missing here shows its English text, so a pack can grow one row at a time.
struct PackEntry { const char *source; const char *text[packLanguageCount]; };
inline constexpr PackEntry packEntries[] = {
#include "localization_packs.inc"
};
struct Translation { std::string text[languageCount], label[languageCount]; };
inline const auto &catalog() {
    static const auto values=[] {
        std::unordered_map<std::string_view,Translation> map;
        for(const auto &entry:entries) {
            Translation value;
            value.text[0]=entry.chinese;
            for(int lang=1;lang<languageCount;++lang) value.text[lang]=entry.english;
            map.emplace(entry.source,std::move(value));
        }
        for(const auto &entry:packEntries) {
            const auto found=map.find(entry.source);
            if(found==map.end()) continue; // localization_tests rejects unknown sources.
            for(int pack=0;pack<packLanguageCount;++pack) found->second.text[pack+2]=entry.text[pack];
        }
        for(auto &[source,value]:map)
            // ### fixes ImGui IDs across live changes, including open popups.
            for(int lang=0;lang<languageCount;++lang)
                value.label[lang]=value.text[lang]+"###"+std::string(source);
        return map;
    }();
    return values;
}
inline const char *text(const char *source) {
    if(!source || !*source || source[0]=='#') return source;
    const auto &map=catalog();const auto it=map.find(source);
    return it==map.end()?source:it->second.text[int(current)].c_str();
}
// Message built at runtime: `source` is a printf row of the table, so its
// translation is free to move the surrounding words but keeps the arguments.
template<class... Args> std::string format(const char *source,Args... args) {
    const char *pattern=text(source);
    const int size=std::snprintf(nullptr,0,pattern,args...);
    if(size<=0) return pattern;
    std::string out(size_t(size),'\0');
    std::snprintf(out.data(),out.size()+1,pattern,args...);
    return out;
}
inline const char *label(const char *source) {
    if(!source || !*source || source[0]=='#') return source;
    const auto &map=catalog();const auto it=map.find(source);
    if(it!=map.end()) return it->second.label[int(current)].c_str();
    // A visible label may have a per-widget hidden suffix (e.g. Element##draw).
    // Keep that suffix in the stable ID; never translate the suffix itself.
    const char *suffix=std::strstr(source,"##");
    if(!suffix) return source;
    const auto base=map.find(std::string_view(source,size_t(suffix-source)));
    if(base==map.end()) return source;
    static std::unordered_map<std::string,Translation> variants;
    auto found=variants.find(source);
    if(found==variants.end()) {
        Translation value;
        for(int lang=0;lang<languageCount;++lang) value.label[lang]=base->second.text[lang]+"###"+source;
        found=variants.emplace(source,std::move(value)).first;
    }
    return found->second.label[int(current)].c_str();
}
template<size_t N> const char *items(const char (&source)[N]) {
    // Literal addresses are stable; cache every language including NUL separators.
    static std::unordered_map<const char *,Translation> cache;
    auto found=cache.find(source);
    if(found==cache.end()) {
        Translation value;
        for(int lang=0;lang<languageCount;++lang) {
            const auto previous=current;current=Language(lang);
            for(size_t at=0;at<N;) {
                value.text[lang]+=text(source+at);value.text[lang]+='\0';
                at+=std::strlen(source+at)+1;
            }
            current=previous;
        }
        found=cache.emplace(source,std::move(value)).first;
    }
    return found->second.text[int(current)].c_str();
}
}
