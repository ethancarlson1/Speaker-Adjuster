#include "roomeq/eqexport.h"

#include <cmath>
#include <cstdio>
#include <optional>

namespace roomeq
{
namespace
{
const char* filterDefinition =
    "RBJ Audio EQ Cookbook biquads. Bell: Q sets the bandwidth between the points at half its gain in dB "
    "(bandwidth_octaves is the same width). Shelf: frequency at half its gain, Q 0.707 is slope 1. "
    "High- and low-pass: frequency at -3 dB; 24 dB/octave is two Butterworth sections.";
const char* notIncluded = "Level compensation: it follows the show level as it changes.";

// Numbers always with a '.', whatever the host's locale.
std::string number (double v, int decimals)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    std::string s (buf);
    for (auto& c : s)
        if (c == ',')
            c = '.';
    if (s == "-0" || s.find_first_not_of ("-0.") == std::string::npos)
        s = s.front() == '-' ? s.substr (1) : s;   // no "-0.00"
    return s;
}

std::string jsonString (const std::string& s)
{
    std::string out = "\"";
    for (const auto c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char> (c) < 0x20)
                {
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", static_cast<unsigned> (static_cast<unsigned char> (c)));
                    out += buf;
                }
                else
                    out += c;
        }
    }
    return out + "\"";
}

// One filter, whichever stage it's from.
struct Filter
{
    std::string type;                     // bell, low_shelf, high_shelf, high_pass, low_pass
    double freq = 0.0;
    std::optional<double> gainDb, q, octaves;
    std::optional<int> slope;             // dB per octave (pass filters)
};

Filter fromBand (const Band& b)
{
    Filter f { bandKindName (b.kind), b.freq, {}, b.q, {}, {} };
    if (b.kind == BandKind::bell)
    {
        f.gainDb = b.gainDb;
        f.octaves = bandwidthForQ (b.q);
    }
    else if (b.kind == BandKind::lowShelf || b.kind == BandKind::highShelf)
        f.gainDb = b.gainDb;
    else
        f.slope = 12;
    return f;
}

Filter fromVoicing (const VoicingBand& v)
{
    switch (v.type)
    {
        case VoicingType::bell: return { "bell", v.freq, v.gainDb, v.q, bandwidthForQ (v.q), {} };
        case VoicingType::lowShelf: return { "low_shelf", v.freq, v.gainDb, v.q, {}, {} };
        case VoicingType::highShelf: return { "high_shelf", v.freq, v.gainDb, v.q, {}, {} };
        case VoicingType::highPass12: return { "high_pass", v.freq, {}, v.q, {}, 12 };
        case VoicingType::highPass24: return { "high_pass", v.freq, {}, {}, {}, 24 };
        case VoicingType::lowPass12: return { "low_pass", v.freq, {}, v.q, {}, 12 };
        case VoicingType::lowPass24: return { "low_pass", v.freq, {}, {}, {}, 24 };
    }
    return {};
}

std::vector<Filter> correctionFilters (const EqExport& e)
{
    std::vector<Filter> out;
    for (const auto& b : e.correction)
        out.push_back (fromBand (b));
    return out;
}

std::vector<Filter> voicingFilters (const EqExport& e)
{
    std::vector<Filter> out;
    for (const auto& v : e.voicing)
        out.push_back (fromVoicing (v));
    return out;
}

std::string filterJson (const Filter& f)
{
    std::string s = "{\"type\": " + jsonString (f.type) + ", \"frequency_hz\": " + number (f.freq, 2);
    if (f.gainDb)
        s += ", \"gain_db\": " + number (*f.gainDb, 2);
    if (f.q)
        s += ", \"q\": " + number (*f.q, 3);
    if (f.octaves)
        s += ", \"bandwidth_octaves\": " + number (*f.octaves, 3);
    if (f.slope)
        s += ", \"slope_db_per_octave\": " + std::to_string (*f.slope);
    return s + "}";
}

std::string filtersJson (const std::vector<Filter>& filters)
{
    if (filters.empty())
        return "[]";
    std::string s = "[\n";
    for (std::size_t i = 0; i < filters.size(); ++i)
        s += "      " + filterJson (filters[i]) + (i + 1 < filters.size() ? ",\n" : "\n");
    return s + "    ]";
}

// "37.4 Hz", "147 Hz", "2.93 kHz", "12.5 kHz": three significant figures, for typing in.
std::string hz (double f)
{
    if (f >= 1000.0)
        return number (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz";
    return number (f, f >= 100.0 ? 0 : 1) + " Hz";
}

std::string signedDb (double db)
{
    return (db > 0.0 ? "+" : "") + number (db, 1) + " dB";
}

std::string typeName (const Filter& f)
{
    if (f.type == "bell")
        return "Bell";
    if (f.type == "low_shelf")
        return "Low shelf";
    if (f.type == "high_shelf")
        return "High shelf";
    return f.type == "high_pass" ? "High-pass" : "Low-pass";
}

std::string pad (std::string s, std::size_t width)
{
    if (s.size() < width)
        s.append (width - s.size(), ' ');
    return s;
}

std::string filterLines (const std::vector<Filter>& filters)
{
    std::string s;
    for (std::size_t i = 0; i < filters.size(); ++i)
    {
        const auto& f = filters[i];
        auto line = "  " + pad (std::to_string (i + 1), 3) + pad (typeName (f), 12) + pad (hz (f.freq), 11);
        if (f.gainDb)
            line += pad (signedDb (*f.gainDb), 10);
        if (f.slope)
            line += pad (std::to_string (*f.slope) + " dB/oct", 10);
        if (f.q)
            line += "Q " + number (*f.q, 2);
        if (f.octaves)
            line += " (" + number (*f.octaves, 2) + " oct)";
        while (! line.empty() && line.back() == ' ')
            line.pop_back();
        s += line + "\n";
    }
    return s;
}

std::string correctionWords (const EqExport& e)
{
    if (e.correctionStatus == "applied")
        return "applied, Amount " + number (e.amount * 100.0, 0) + "%" + (e.correctionOn ? "" : ", switched off");
    if (e.correctionStatus == "proposed")
        return "proposed, not applied yet";
    return "none";
}

// "fitted to 6 bands from 9: within 1.7 dB of the correction as it plays (0.5 dB RMS)"
std::string fitWords (const EqExport::Fit& f, int decimals)
{
    return "fitted to " + std::to_string (f.maxBands) + " bands from " + std::to_string (f.fromBands) + ": within "
           + number (f.maxErrorDb, decimals) + " dB of the correction as it plays (" + number (f.rmsErrorDb, decimals) + " dB RMS)";
}
} // namespace

EqExport withFewerBands (EqExport e, const Refit& refit, int maxBands)
{
    if (! refit.refitted)
        return e;
    e.correction = refit.bands;
    e.fit = EqExport::Fit { maxBands, refit.originalBands, refit.maxErrorDb, refit.rmsErrorDb };
    return e;
}

std::string eqExportJson (const EqExport& e)
{
    std::string s = "{\n";
    s += "  \"format\": " + jsonString (eqExportFormat) + ",\n";
    s += "  \"version\": " + std::to_string (eqExportVersion) + ",\n";
    s += "  \"generator\": " + jsonString (e.generator) + ",\n";
    s += "  \"created\": " + jsonString (e.created) + ",\n";
    s += "  \"source\": " + jsonString (e.source) + ",\n";
    s += "  \"sample_rate_hz\": " + number (e.sampleRate, 0) + ",\n";
    s += "  \"filter_definition\": " + jsonString (filterDefinition) + ",\n";
    s += "  \"correction\": {\n";
    s += "    \"status\": " + jsonString (e.correctionStatus) + ",\n";
    s += "    \"enabled\": " + std::string (e.correctionOn ? "true" : "false") + ",\n";
    s += "    \"amount\": " + number (e.amount, 3) + ",\n";
    if (e.fit)
        s += "    \"fit\": {\"max_bands\": " + std::to_string (e.fit->maxBands) + ", \"from_bands\": " + std::to_string (e.fit->fromBands)
             + ", \"max_error_db\": " + number (e.fit->maxErrorDb, 2) + ", \"rms_error_db\": " + number (e.fit->rmsErrorDb, 2) + "},\n";
    s += "    \"filters\": " + filtersJson (correctionFilters (e)) + "\n  },\n";
    s += "  \"voicing\": {\n";
    s += "    \"enabled\": " + std::string (e.voicingOn ? "true" : "false") + ",\n";
    s += "    \"filters\": " + filtersJson (voicingFilters (e)) + "\n  },\n";
    s += "  \"output_gain_db\": " + number (e.outputGainDb, 2) + ",\n";
    s += "  \"delay_ms\": " + number (e.delayMs, 3) + ",\n";
    s += "  \"polarity_inverted\": " + std::string (e.polarityInverted ? "true" : "false") + ",\n";
    s += "  \"not_included\": " + jsonString (notIncluded) + "\n";
    return s + "}\n";
}

std::string eqExportCsv (const EqExport& e)
{
    std::string s;
    s += "# Adaptive Room EQ export: " + std::string (eqExportFormat) + " version " + std::to_string (eqExportVersion) + "\n";
    s += "# generator: " + e.generator + "\n";
    s += "# created: " + e.created + "\n";
    s += "# source: " + e.source + "\n";
    s += "# sample rate: " + number (e.sampleRate, 0) + " Hz\n";
    s += "# correction: " + correctionWords (e) + " (gains as they play)\n";
    if (e.fit)
        s += "# correction " + fitWords (*e.fit, 2) + "\n";
    s += "# voicing: " + std::string (e.voicingOn ? "on" : "switched off") + "\n";
    s += "# filters: " + std::string (filterDefinition) + "\n";
    s += "# not included: " + std::string (notIncluded) + "\n";
    s += "section,index,type,frequency_hz,gain_db,q,bandwidth_octaves,slope_db_per_octave,value\n";
    const auto rows = [&s] (const char* section, const std::vector<Filter>& filters)
    {
        for (std::size_t i = 0; i < filters.size(); ++i)
        {
            const auto& f = filters[i];
            s += std::string (section) + "," + std::to_string (i + 1) + "," + f.type + "," + number (f.freq, 2) + ","
                 + (f.gainDb ? number (*f.gainDb, 2) : "") + "," + (f.q ? number (*f.q, 3) : "") + ","
                 + (f.octaves ? number (*f.octaves, 3) : "") + "," + (f.slope ? std::to_string (*f.slope) : "") + ",\n";
        }
    };
    rows ("correction", correctionFilters (e));
    rows ("voicing", voicingFilters (e));
    s += "output,1,gain,," + number (e.outputGainDb, 2) + ",,,,\n";
    s += "zone,1,delay_ms,,,,,," + number (e.delayMs, 3) + "\n";
    s += "zone,2,polarity_inverted,,,,,," + std::string (e.polarityInverted ? "1" : "0") + "\n";
    return s;
}

std::string eqExportText (const EqExport& e)
{
    std::string s = "Adaptive Room EQ: " + e.source + "\n";
    s += "Created " + e.created + ", at " + number (e.sampleRate / 1000.0, 1) + " kHz\n\n";
    const auto correction = correctionFilters (e);
    s += "Correction (" + correctionWords (e) + ")" + (correction.empty() ? ": no filters\n" : ":\n" + filterLines (correction));
    if (e.fit)
        s += "  (" + fitWords (*e.fit, 1) + ")\n";
    const auto voicing = voicingFilters (e);
    s += std::string ("Voicing") + (e.voicingOn ? "" : " (switched off)")
         + (voicing.empty() ? ": no bands on\n" : ":\n" + filterLines (voicing));
    s += "Output gain: " + signedDb (e.outputGainDb) + "\n";
    s += "Delay: " + number (e.delayMs, 2) + " ms, polarity " + (e.polarityInverted ? "inverted" : "normal") + "\n\n";
    s += "Bell Q: the bandwidth between the points at half the gain in dB (RBJ cookbook); a console that measures "
         "it at -3 dB from the peak needs a different Q for small gains. Shelves: frequency at half the gain.\n";
    s += "Not included: level compensation (it follows the show level as it changes).\n";
    return s;
}
} // namespace roomeq
