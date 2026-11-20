#pragma once

namespace nf {

enum class Language { English, Russian };

constexpr const wchar_t* translated(Language language, const wchar_t* english, const wchar_t* russian) {
    return language == Language::Russian ? russian : english;
}

}
