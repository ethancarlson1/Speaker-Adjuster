#pragma once

#include "roomeq/grading.h"

#include <juce_graphics/juce_graphics.h>

#include <cmath>

// Dark UI palette (steps from the project's data-viz reference palette).
namespace theme
{
inline const juce::Colour surface { 0xff1a1a19 };
inline const juce::Colour plane { 0xff0d0d0d };
inline const juce::Colour panel { 0xff242423 };
inline const juce::Colour ink { 0xffffffff };
inline const juce::Colour ink2 { 0xffc3c2b7 };
inline const juce::Colour muted { 0xff898781 };
inline const juce::Colour grid { 0xff2c2c2a };
inline const juce::Colour axis { 0xff383835 };
inline const juce::Colour blue { 0xff3987e5 };      // average
inline const juce::Colour orange { 0xffd95926 };    // selected position
inline const juce::Colour good { 0xff0ca30c };
inline const juce::Colour warning { 0xfffab219 };
inline const juce::Colour critical { 0xffd03b3b };

inline juce::Colour gradeColour (roomeq::Grade g)
{
    switch (g)
    {
        case roomeq::Grade::pass: return good;
        case roomeq::Grade::marginal: return warning;
        case roomeq::Grade::redo: return critical;
    }
    return muted;
}

// Status colours never carry meaning alone: always paired with a symbol and a word.
inline juce::String gradeText (roomeq::Grade g)
{
    switch (g)
    {
        case roomeq::Grade::pass: return juce::String::fromUTF8 ("\xE2\x9C\x93 PASS");
        case roomeq::Grade::marginal: return "! MARGINAL";
        case roomeq::Grade::redo: return juce::String::fromUTF8 ("\xE2\x9C\x97 REDO");
    }
    return {};
}

inline juce::String formatHz (double f)
{
    // Note juce::String (double, 0) means "full precision", so round explicitly.
    if (f >= 10000.0)
        return juce::String (juce::roundToInt (f / 1000.0)) + " kHz";
    if (f >= 1000.0)
        return juce::String (std::round (f / 100.0) / 10.0, 1) + " kHz";
    return juce::String (juce::roundToInt (f)) + " Hz";
}
} // namespace theme
