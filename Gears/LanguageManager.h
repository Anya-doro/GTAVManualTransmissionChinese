#pragma once
#include <string>
#include <unordered_map>

class LanguageManager {
public:
    enum class Lang { EN, ZH_CN };
    static LanguageManager& Instance();
    void SetLanguage(Lang lang);
    Lang GetLanguage() const;
    const std::string& Tr(const std::string& key) const;
private:
    LanguageManager();
    void Load();
    Lang currentLang;
    std::unordered_map<std::string, std::string> dictEN;
    std::unordered_map<std::string, std::string> dictZH;
};
