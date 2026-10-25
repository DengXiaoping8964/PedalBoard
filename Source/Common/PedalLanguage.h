#pragma once

#include <juce_core/juce_core.h>

/*==============================================================================
  Interface language: English and Simplified Chinese.

  Two decisions worth stating before the code:

  1. Translation happens where a string is *drawn*, never where it is stored.
     Effect names, parameter names and categories live once, in English, inside
     the effect classes and EffectFactory; the UI wraps them in tr() at the point
     of display. Storing a translated copy instead would mean the language could
     not change without rebuilding every object, and the settings file (which
     keys effects by index and parameter by position) would become language
     dependent.

  2. The Chinese table is compiled in, not loaded from a file next to the
     executable. A missing or mis-encoded translation file would otherwise leave
     a first-run user with an empty dialog, and the table is 8 kB of text that
     nobody edits at runtime.

  The lookup itself is juce::LocalisedStrings, so strings that come from inside
  JUCE - the file chooser's buttons, for instance - can be translated too. A key
  that has no entry falls back to the English text, which is why the checker
  script in _toolchain_test compares the tr() call sites against the table: a
  missing entry is silent.

  Keys must be single-line: LocalisedStrings::loadFromText splits its input on
  newlines, so any string containing \n has to be assembled from translated
  parts at the call site instead.
==============================================================================*/
namespace pedal
{

enum class Language : int { english = 0, chinese = 1 };

/** Number of languages the table can serve; the first-run prompt and the
    language menu are both built from this. */
inline constexpr int numLanguages = 2;

/** Installs the mappings and the typeface for a language. Safe to call before any
    component exists - that is exactly when it is called at start-up - and it does
    not repaint anything, because at start-up there is nothing to repaint. Callers
    changing the language on screen send a look-and-feel change and refresh the
    text themselves. */
void setLanguage (Language newLanguage);

Language currentLanguage() noexcept;

/** False until the user has actually answered the language question, which is
    what separates "English, by default" from "English, because they chose it".
    setLanguage() deliberately does not set this: start-up installs a language
    that nobody was asked about. */
bool languageWasChosen() noexcept;

/** Records that the answer has been given. Called by the prompt's handler and by
    start-up when the settings file already carries a language. */
void noteLanguageChosen() noexcept;

/** Each language's own name, so the menu never has to be translated to be
    readable: 中文 stays 中文 whichever language is showing. Returned as a String
    rather than a const char* because the literal is not ASCII. */
juce::String languageName (Language);

/** The typeface able to render the current language, or an empty string for
    "whatever JUCE would have picked". Probed once per change, because asking
    DirectWrite for a font that is not installed does not fail - it silently
    hands back a substitute with no CJK coverage. */
const juce::String& uiFontFamily() noexcept;

/** A UTF-8 literal from this project's own source, turned into a juce::String.
    Always use this for a string containing non-ASCII characters.

    juce::String's const char* constructor goes through CharPointer_ASCII, which
    cannot decode bytes above 127 and asserts; in a release build it copies them
    one byte per character, so "中文" arrives as four Latin-1 characters instead of
    two ideographs - the mojibake this helper exists to prevent. With /utf-8 the
    compiler has already put real UTF-8 bytes in the literal, so all that is
    missing is the declaration of that fact. */
inline juce::String u8 (const char* utf8) { return juce::String::fromUTF8 (utf8); }

/** Looks a single-line English literal up in the current mappings. */
juce::String tr (const char* english);

/** The same for a string that is already a juce::String - an effect's title, say.
    Overloaded rather than defaulted to a String parameter so that a const char*
    argument does not pay for a temporary String on the way in. */
juce::String tr (const juce::String& english);

/** tr() followed by %1..%16 substitution, in that order in the English text.
    Substitution happens after lookup, so a translation may reorder its
    placeholders: "%1 ms" can become "%1 毫秒". Replacement runs from the highest
    index down so %1 is never eaten out of a longer placeholder. */
template <typename... Args>
juce::String trf (const char* english, Args&&... args)
{
    static_assert (sizeof...(Args) > 0 && sizeof...(Args) <= 16,
                   "trf needs between one and sixteen values");

    const juce::String values[] { juce::String (std::forward<Args> (args))... };
    auto out = tr (english);

    for (int i = static_cast<int> (sizeof...(Args)); i >= 1; --i)
        out = out.replace ("%" + juce::String (i), values[static_cast<std::size_t> (i - 1)]);

    return out;
}

} // namespace pedal
