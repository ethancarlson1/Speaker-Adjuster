#include "roomeq/iso226.h"

#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>

namespace roomeq
{
namespace
{
// ISO 226:2003 Table 1.
const std::vector<double> freqsTable { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                       1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
const double alphaF[] { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330, 0.315, 0.301, 0.288, 0.276, 0.267,
                        0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242, 0.242, 0.245, 0.254, 0.271, 0.301 };
const double lU[] { -31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5, -3.1, -2.0, -1.1, -0.4, 0.0,
                    0.3, 0.5, 0.0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
const double tF[] { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4,
                    3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };
} // namespace

const std::vector<double>& iso226Freqs()
{
    return freqsTable;
}

std::vector<double> iso226ContourAtTable (double phon)
{
    std::vector<double> lp (freqsTable.size());
    for (std::size_t i = 0; i < lp.size(); ++i)
    {
        const auto af = 4.47e-3 * (std::pow (10.0, 0.025 * phon) - 1.15)
                        + std::pow (0.4 * std::pow (10.0, (tF[i] + lU[i]) / 10.0 - 9.0), alphaF[i]);
        lp[i] = 10.0 / alphaF[i] * std::log10 (af) - lU[i] + 94.0;
    }
    return lp;
}

std::vector<double> iso226Contour (const std::vector<double>& freqs, double phon)
{
    const auto table = iso226ContourAtTable (phon);
    std::vector<double> logF (freqsTable.size());
    for (std::size_t i = 0; i < logF.size(); ++i)
        logF[i] = std::log2 (freqsTable[i]);
    std::vector<double> out (freqs.size());
    for (std::size_t i = 0; i < freqs.size(); ++i)
        out[i] = interp (std::log2 (std::clamp (freqs[i], freqsTable.front(), freqsTable.back())), logF, table);
    return out;
}

std::vector<double> iso226RelativeContour (const std::vector<double>& freqs, double phon)
{
    auto out = iso226Contour (freqs, phon);
    const auto at1k = iso226Contour ({ 1000.0 }, phon).front();
    for (auto& v : out)
        v -= at1k;
    return out;
}
} // namespace roomeq
