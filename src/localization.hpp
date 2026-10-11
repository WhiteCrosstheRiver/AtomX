#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstring>
#include <cwchar>

// UI-only localization. Scientific keys, expressions, user labels, filenames and
// serialized documents retain their original spelling. No network/dependencies.
namespace localization {
enum class Language { Chinese, English };
inline Language current = Language::Chinese;
inline const wchar_t *code(Language value) { return value==Language::English ? L"en" : L"zh-CN"; }
inline Language fromCode(const wchar_t *value) {
    return std::wcscmp(value,L"en")==0 ? Language::English : Language::Chinese;
}
struct Entry { const char *source; const char *chinese; const char *english; };
inline constexpr Entry entries[] = {
#include "localization_strings.inc"
};
struct Translation { std::string text[2], label[2]; };
inline const auto &catalog() {
    static const auto values=[] {
        std::unordered_map<std::string_view,Translation> map;
        for(const auto &entry:entries) {
            Translation value;
            value.text[0]=entry.chinese; value.text[1]=entry.english;
            // ### fixes ImGui IDs across live changes, including open popups.
            value.label[0]=value.text[0]+"###"+entry.source;
            value.label[1]=value.text[1]+"###"+entry.source;
            map.emplace(entry.source,std::move(value));
        }
        return map;
    }();
    return values;
}
inline const char *text(const char *source) {
    if(!source || !*source || source[0]=='#') return source;
    const auto &map=catalog();const auto it=map.find(source);
    return it==map.end()?source:it->second.text[int(current)].c_str();
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
        for(int lang=0;lang<2;++lang) value.label[lang]=base->second.text[lang]+"###"+source;
        found=variants.emplace(source,std::move(value)).first;
    }
    return found->second.label[int(current)].c_str();
}
template<size_t N> const char *items(const char (&source)[N]) {
    // Literal addresses are stable; cache both languages including NUL separators.
    static std::unordered_map<const char *,Translation> cache;
    auto found=cache.find(source);
    if(found==cache.end()) {
        Translation value;
        for(int lang=0;lang<2;++lang) {
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
