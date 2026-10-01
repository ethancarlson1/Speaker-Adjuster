#include "roomeq/targetfile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <map>
#include <sstream>

namespace roomeq
{
namespace
{
// Numbers always with a '.', whatever the host's locale.
std::string number (double v, int decimals)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    std::string s (buf);
    std::replace (s.begin(), s.end(), ',', '.');
    return s;
}

std::string jsonString (const std::string& s)
{
    std::string out = "\"";
    for (const auto c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char> (c) >= 0x20)
            out += c;
    }
    return out + "\"";
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
}

// A number in C-locale form. Checked by hand (strict: nothing else on the
// cell), then read by a stream in the classic locale: correctly rounded, and
// unaffected by the host's locale (strtod isn't).
std::optional<double> parseNumber (std::string s)
{
    s = trim (s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = trim (s.substr (1, s.size() - 2));
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        ++i;
    auto digits = 0;
    for (; i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])); ++i)
        ++digits;
    if (i < s.size() && s[i] == '.')
        for (++i; i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])); ++i)
            ++digits;
    if (digits == 0)
        return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
    {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            ++i;
        auto expDigits = 0;
        for (; i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])); ++i)
            ++expDigits;
        if (expDigits == 0)
            return std::nullopt;
    }
    if (i != s.size())
        return std::nullopt;
    std::istringstream in (s[0] == '+' ? s.substr (1) : s);
    in.imbue (std::locale::classic());
    double value = 0.0;
    in >> value;
    if (in.fail())
        return std::nullopt;
    return value;
}
} // namespace

std::string targetToJson (const TargetCurve& t)
{
    std::string s = "{\n  \"format\": " + jsonString (targetFileFormat) + ",\n  \"version\": " + std::to_string (targetFileVersion)
                    + ",\n  \"name\": " + jsonString (t.name) + ",\n  \"points\": [";
    for (std::size_t i = 0; i < t.points.size(); ++i)
        s += std::string (i > 0 ? ", " : "") + "[" + number (t.points[i].first, 1) + ", " + number (t.points[i].second, 2) + "]";
    return s + "]\n}\n";
}

std::string targetToCsv (const TargetCurve& t)
{
    std::string s = "# Adaptive Room EQ target: " + std::string (targetFileFormat) + " version " + std::to_string (targetFileVersion) + "\n";
    s += "# name: " + t.name + "\n";
    s += "frequency_hz,gain_db\n";
    for (const auto& [f, g] : t.points)
        s += number (f, 1) + "," + number (g, 2) + "\n";
    return s;
}

std::optional<TargetCurve> targetFromCsv (const std::string& text, const std::string& fallbackName, std::string& error)
{
    TargetCurve t;
    std::map<double, double> points;
    std::istringstream in (text);
    std::string line;
    auto lineNumber = 0;
    std::string firstBad;
    while (std::getline (in, line))
    {
        ++lineNumber;
        line = trim (line);
        if (line.empty())
            continue;
        if (line[0] == '#' || line[0] == '*' || line.rfind ("//", 0) == 0)
        {
            const auto body = trim (line.substr (line[0] == '/' ? 2 : 1));
            if (body.rfind ("name:", 0) == 0 && t.name.empty())
                t.name = trim (body.substr (5));
            continue;
        }
        // Columns: semicolons; else tabs or spaces (with a comma, it's a decimal comma); else commas.
        std::vector<std::string> cells;
        const auto semicolons = line.find (';') != std::string::npos;
        const auto tabs = line.find ('\t') != std::string::npos;
        const auto commas = line.find (',') != std::string::npos;
        if (semicolons || ! commas || tabs)
        {
            if (semicolons)
            {
                std::size_t start = 0;
                for (auto at = line.find (';');; at = line.find (';', start))
                {
                    cells.push_back (line.substr (start, at == std::string::npos ? std::string::npos : at - start));
                    if (at == std::string::npos)
                        break;
                    start = at + 1;
                }
            }
            else
            {
                std::istringstream words (line);
                for (std::string w; words >> w;)
                    cells.push_back (w);
            }
            for (auto& c : cells)
                std::replace (c.begin(), c.end(), ',', '.');
        }
        else
        {
            std::size_t start = 0;
            for (auto at = line.find (',');; at = line.find (',', start))
            {
                cells.push_back (line.substr (start, at == std::string::npos ? std::string::npos : at - start));
                if (at == std::string::npos)
                    break;
                start = at + 1;
            }
        }
        const auto f = cells.size() >= 2 ? parseNumber (cells[0]) : std::nullopt;
        const auto g = cells.size() >= 2 ? parseNumber (cells[1]) : std::nullopt;
        if (! f || ! g)
        {
            // Headers come before the points; text among them is a problem.
            if (! points.empty() && firstBad.empty())
                firstBad = "line " + std::to_string (lineNumber) + ": \"" + line.substr (0, 40) + "\"";
            continue;
        }
        if (! (std::isfinite (*f) && std::isfinite (*g) && *f > 0.0))
            continue;
        points[*f] = *g;
    }
    if (! firstBad.empty())
    {
        error = "Couldn't read " + firstBad + " (expected frequency and gain)";
        return std::nullopt;
    }
    if (points.empty())
    {
        error = "No points found (expected lines of frequency and gain, e.g. \"1000,0\")";
        return std::nullopt;
    }
    for (const auto& p : points)
        t.points.push_back (p);
    if (t.name.empty())
        t.name = fallbackName;
    return t;
}
} // namespace roomeq
