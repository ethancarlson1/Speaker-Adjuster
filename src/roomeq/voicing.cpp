#include "roomeq/voicing.h"

namespace roomeq
{
bool VoicingBand::operator== (const VoicingBand& o) const
{
    return on == o.on && type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q;
}

bool hasGain (VoicingType type)
{
    return type == VoicingType::bell || type == VoicingType::lowShelf || type == VoicingType::highShelf;
}

bool hasQ (VoicingType type)
{
    return type != VoicingType::highPass24 && type != VoicingType::lowPass24;
}

Sections voicingSections (const VoicingBand& v)
{
    Sections s;
    if (! v.on)
        return s;
    // 4th-order Butterworth as two 2nd-order sections.
    constexpr double butterQ1 = 0.54119610014619698;
    constexpr double butterQ2 = 1.3065629648763766;
    const auto add = [&s] (BandKind kind, double freq, double gain, double q)
    {
        s.bands[static_cast<std::size_t> (s.count++)] = Band { kind, freq, gain, q, true };
    };
    switch (v.type)
    {
        case VoicingType::bell:       add (BandKind::bell, v.freq, v.gainDb, v.q); break;
        case VoicingType::lowShelf:   add (BandKind::lowShelf, v.freq, v.gainDb, v.q); break;
        case VoicingType::highShelf:  add (BandKind::highShelf, v.freq, v.gainDb, v.q); break;
        case VoicingType::highPass12: add (BandKind::highPass, v.freq, 0.0, v.q); break;
        case VoicingType::lowPass12:  add (BandKind::lowPass, v.freq, 0.0, v.q); break;
        case VoicingType::highPass24:
            add (BandKind::highPass, v.freq, 0.0, butterQ1);
            add (BandKind::highPass, v.freq, 0.0, butterQ2);
            break;
        case VoicingType::lowPass24:
            add (BandKind::lowPass, v.freq, 0.0, butterQ1);
            add (BandKind::lowPass, v.freq, 0.0, butterQ2);
            break;
    }
    return s;
}

VoicingBand defaultVoicingBand (int index)
{
    struct Default { VoicingType type; double freq, q; };
    static constexpr Default defaults[numVoicingBands] {
        { VoicingType::highPass24, 35.0, 0.71 }, { VoicingType::lowShelf, 100.0, 0.71 },
        { VoicingType::bell, 250.0, 1.0 },       { VoicingType::bell, 600.0, 1.0 },
        { VoicingType::bell, 1500.0, 1.0 },      { VoicingType::bell, 3500.0, 1.0 },
        { VoicingType::highShelf, 8000.0, 0.71 }, { VoicingType::lowPass12, 18000.0, 0.71 },
    };
    const auto& d = defaults[index < 0 ? 0 : (index >= numVoicingBands ? numVoicingBands - 1 : index)];
    return { false, d.type, d.freq, 0.0, d.q };
}
} // namespace roomeq
