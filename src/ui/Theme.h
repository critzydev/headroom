// Design tokens: colours, spacing and the embedded Inter / JetBrains Mono fonts.
#pragma once

#include "Juce.h"

#include <BinaryData.h>
#include <array>

namespace hr::theme
{
// ---- Colours ------------------------------------------------------------------------------------
inline const juce::Colour bg0        { 0xff0f1012 };    // window
inline const juce::Colour bg1        { 0xff16171a };    // strips, drawer, panels
inline const juce::Colour bg2        { 0xff1d1f23 };    // controls, fader track
inline const juce::Colour bg3        { 0xff25282d };    // hover / pressed
inline const juce::Colour line       { 0xff2a2d33 };    // hairlines
inline const juce::Colour lineStrong { 0xff3a3e45 };    // focused outlines, scale ticks
inline const juce::Colour text0      { 0xffeceae6 };    // primary text
inline const juce::Colour text1      { 0xffa7a9ae };    // secondary
inline const juce::Colour text2      { 0xff6b6e75 };    // tertiary / disabled
inline const juce::Colour accent     { 0xffe9a23b };    // selection, active tab, primary button outline
inline const juce::Colour ok         { 0xff4cc38a };    // meter body, "on" LEDs
inline const juce::Colour warn       { 0xffe9b949 };    // meter -12..-3 dB, warnings
inline const juce::Colour bad        { 0xffe5534b };    // clip, mute, errors

// Fader cap: the only place with a (very subtle) gradient.
inline const juce::Colour capTop     { 0xffd9d6d0 };
inline const juce::Colour capBottom  { 0xffc7c4be };

// Channel tag colours, in the order new channels pick them (matches getChannelColourPalette()).
inline const std::array<juce::Colour, 8> channelTags
{
    juce::Colour (0xffe9a23b), juce::Colour (0xff3fb8af), juce::Colour (0xff5b9be6), juce::Colour (0xff9d7be0),
    juce::Colour (0xffe0708f), juce::Colour (0xff9bc94b), juce::Colour (0xffc9a77c), juce::Colour (0xff8c95a3)
};

// The model stores colours as "RRGGBB"; falls back to text1 for garbage.
inline juce::Colour colourFromHex (const juce::String& rrggbb)
{
    const auto hex = rrggbb.trimCharactersAtStart ("#");
    return hex.length() == 6 ? juce::Colour::fromString ("ff" + hex) : text1;
}

// "over" blends `fg` at `alpha` on top of an opaque `base`: tinted fills (banners, muted button) without transparency.
inline juce::Colour tint (juce::Colour base, juce::Colour fg, float alpha)
{
    return base.interpolatedWith (fg, alpha);
}

// ---- Metrics (4 px grid) -----------------------------------------------------------------------
constexpr int kTopBarHeight = 48;
constexpr int kBannerHeight = 36;
constexpr int kMixHeaderHeight = 44;
constexpr int kStripWidth = 104;
constexpr int kMasterWidth = 120;
constexpr int kDrawerWidth = 400;
constexpr int kGap = 12;
constexpr int kSidePadding = 16;
constexpr float kControlRadius = 6.0f;
constexpr float kPanelRadius = 8.0f;
constexpr float kMeterRadius = 3.0f;
constexpr int kDefaultWidth = 1120, kDefaultHeight = 700;
constexpr int kMinWidth = 860, kMinHeight = 540;

// ---- Glyphs the fonts have but source files shouldn't carry as raw bytes ------------------------
namespace glyph
{
    inline juce::String utf8 (const char* bytes)    { return juce::String (juce::CharPointer_UTF8 (bytes)); }

    inline const juce::String minus    = utf8 ("\xe2\x88\x92");     // U+2212
    inline const juce::String infinity = utf8 ("\xe2\x88\x9e");     // U+221E
    inline const juce::String emDash   = utf8 ("\xe2\x80\x94");     // U+2014
    inline const juce::String dot      = utf8 ("\xc2\xb7");         // U+00B7
    inline const juce::String ellipsis = utf8 ("\xe2\x80\xa6");     // U+2026
    inline const juce::String quoteOpen  = utf8 ("\xe2\x80\x98");   // U+2018
    inline const juce::String quoteClose = utf8 ("\xe2\x80\x99");   // U+2019
    inline const juce::String angleRight = utf8 ("\xe2\x80\xba");   // U+203A
    inline const juce::String angleLeft  = utf8 ("\xe2\x80\xb9");   // U+2039
}

// ---- Fonts -------------------------------------------------------------------------------------
enum class Sans { Regular, Medium, SemiBold };
enum class Mono { Regular, Medium };

namespace detail
{
    struct Typefaces
    {
        juce::Typeface::Ptr sans[3], mono[2];
    };

    inline Typefaces& typefaces()
    {
        static Typefaces faces = []
        {
            Typefaces t;
            t.sans[0] = juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_ttf, (size_t) BinaryData::InterRegular_ttfSize);
            t.sans[1] = juce::Typeface::createSystemTypefaceFor (BinaryData::InterMedium_ttf, (size_t) BinaryData::InterMedium_ttfSize);
            t.sans[2] = juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, (size_t) BinaryData::InterSemiBold_ttfSize);
            t.mono[0] = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoRegular_ttf, (size_t) BinaryData::JetBrainsMonoRegular_ttfSize);
            t.mono[1] = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, (size_t) BinaryData::JetBrainsMonoMedium_ttfSize);
            return t;
        }();

        return faces;
    }
}

// Loaded once on first use. Call releaseFonts() during shutdown so the typefaces don't outlive JUCE.
inline juce::Typeface::Ptr sansTypeface (Sans weight = Sans::Regular)   { return detail::typefaces().sans[(int) weight]; }
inline juce::Typeface::Ptr monoTypeface (Mono weight = Mono::Regular)   { return detail::typefaces().mono[(int) weight]; }

inline void releaseFonts()
{
    detail::typefaces() = {};
}

// Sizes below are CSS-style pixel sizes (the em square), which is what the spec's "13 px" means. JUCE's plain
// withHeight() is ascent + descent, roughly 1.2 x larger than the em, so we use the point height.
inline juce::Font sans (float pixels, Sans weight = Sans::Regular)
{
    return juce::Font (juce::FontOptions (sansTypeface (weight)).withPointHeight (pixels));
}

inline juce::Font mono (float pixels = 11.0f, Mono weight = Mono::Regular)
{
    return juce::Font (juce::FontOptions (monoTypeface (weight)).withPointHeight (pixels));
}

// 11 px section label: caps text with +6 % tracking (draw the string upper-cased).
inline juce::Font caps (float pixels = 11.0f)
{
    return juce::Font (juce::FontOptions (sansTypeface (Sans::Medium)).withPointHeight (pixels).withKerningFactor (0.06f));
}

inline float textWidth (const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (font, text);
}
}
