#include "roomeq/capture.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;

std::vector<double> powerOf (const std::vector<cplx>& X)
{
    std::vector<double> p (X.size());
    for (std::size_t k = 0; k < X.size(); ++k)
        p[k] = std::norm (X[k]);
    return p;
}
} // namespace

std::vector<double> inBand (const std::vector<double>& freqs, double f1, double f2)
{
    std::vector<double> w (freqs.size());
    for (std::size_t k = 0; k < freqs.size(); ++k)
        w[k] = freqs[k] >= f1 && freqs[k] <= f2 ? 1.0 : 0.0;
    return w;
}

WindowedResponse windowedResponse (const std::vector<double>& ir, double fs, const AnalysisConfig& cfg)
{
    const auto win = irWindow (cfg.window, fs);
    const auto nfft = responseNfft (cfg.window, fs);
    std::vector<double> padded (win.nPre, 0.0);
    padded.insert (padded.end(), ir.begin(), ir.end());
    padded.resize (padded.size() + win.w.size(), 0.0);

    std::size_t pk = 0;
    for (std::size_t i = 1; i < padded.size(); ++i)
        if (std::abs (padded[i]) > std::abs (padded[pk]))
            pk = i;

    std::vector<double> seg (win.w.size());
    for (std::size_t i = 0; i < seg.size(); ++i)
        seg[i] = padded[pk - win.nPre + i] * win.w[i];
    return { rfftFreqs (nfft, fs), powerOf (rfft (seg, nfft)), pk - win.nPre };
}

Capture analyzeSweepCapture (const std::string& name, const std::vector<std::vector<double>>& recordings,
                             const SweepConfig& sweepCfg, const AnalysisConfig& cfg)
{
    const auto fs = sweepCfg.fs;
    const auto sweep = generateSweep (sweepCfg);
    const auto win = irWindow (cfg.window, fs);
    const auto nfft = responseNfft (cfg.window, fs);
    const auto freqs = rfftFreqs (nfft, fs);
    const auto margin = static_cast<std::size_t> (std::llround (cfg.noiseMarginSeconds * fs));
    const auto nw = win.w.size();

    std::vector<std::vector<double>> segments;
    std::vector<std::vector<double>> noiseSpectra;
    Capture c;
    c.name = name;
    c.kind = "sweep";
    c.fs = fs;
    c.freqs = freqs;

    for (const auto& rec : recordings)
    {
        const auto dec = deconvolve (rec, sweep, fs, sweepCfg.f1, sweepCfg.f2);
        const auto arrival = findArrival (dec, sweepCfg.nPreroll(), cfg.maxDelaySeconds);
        const auto delayMs = 1000.0 * static_cast<double> (arrival - dec.zero - sweepCfg.nPreroll()) / fs;
        const auto start = arrival - win.nPre;

        std::vector<double> seg (nw);
        for (std::size_t i = 0; i < nw; ++i)
            seg[i] = dec.h[start + i] * win.w[i];
        segments.push_back (seg);

        // Noise: same window, as late as the tail allows. IR time t (from sweep
        // start) is valid at every frequency only up to t = tail.
        const auto noiseEnd = dec.zero + sweepCfg.nPreroll() + sweepCfg.nTail() - margin;
        if (noiseEnd < nw || noiseEnd - nw < start + nw)
            throw std::runtime_error (name + ": tail too short for a " + std::to_string (static_cast<int> (std::lround (delayMs)))
                                      + " ms loop delay; lengthen the recording tail");
        const auto noiseStart = noiseEnd - nw;
        std::vector<double> nseg (nw);
        for (std::size_t i = 0; i < nw; ++i)
            nseg[i] = dec.h[noiseStart + i] * win.w[i];
        noiseSpectra.push_back (powerOf (rfft (nseg, nfft)));
        c.delaysMs.push_back (delayMs);
    }

    // Line repeats up to sub-sample accuracy (integer part is already done by
    // peak alignment), then average coherently: SNR improves ~3 dB per doubling.
    const auto bins = freqs.size();
    std::vector<cplx> H (bins, cplx {});
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        auto S = rfft (segments[i], nfft);
        if (i > 0)
        {
            const auto delta = fractionalPeakOffset (segments[0], segments[i], 8);
            for (std::size_t k = 0; k < bins; ++k)
                S[k] *= std::polar (1.0, 2.0 * pi * freqs[k] * delta / fs);
        }
        c.repeatPowers.push_back (powerOf (S));
        for (std::size_t k = 0; k < bins; ++k)
            H[k] += S[k];
    }
    const auto n = static_cast<double> (segments.size());
    for (auto& v : H)
        v /= n;
    c.power = powerOf (H);
    c.noisePower.assign (bins, 0.0);
    for (const auto& ns : noiseSpectra)
        for (std::size_t k = 0; k < bins; ++k)
            c.noisePower[k] += ns[k];
    for (auto& v : c.noisePower)
        v /= n * n;

    std::vector<double> signal (bins);
    for (std::size_t k = 0; k < bins; ++k)
        signal[k] = c.power[k] - c.noisePower[k];
    c.grade = gradeCapture (freqs, signal, c.noisePower, segments.size() > 1 ? &c.repeatPowers : nullptr,
                            cfg.grading, std::min (sweepCfg.f2, fs / 2.0));
    c.weight = inBand (freqs, sweepCfg.f1, sweepCfg.f2);
    c.ir = irfft (H, nfft);
    c.ir.resize (nw);
    return c;
}

Capture analyzeProgramCapture (const std::string& name, const std::vector<double>& reference,
                               const std::vector<double>& mic, double fs, const AnalysisConfig& cfg,
                               const DualFFTConfig& dualCfg)
{
    // The dual-FFT frame is longer than the sweep window (to hold LF reverb);
    // results are rebinned onto the sweep grid so the two average bin for bin.
    // Coherence drives the grade but does not weight the magnitude. Clock drift
    // between the output and the mic is measured and corrected first; the notes say so.
    const auto est = transferFunction (reference, mic, fs, dualCfg);
    Capture c;
    c.name = name;
    c.kind = "program";
    c.fs = fs;
    c.freqs = rfftFreqs (responseNfft (cfg.window, fs), fs);

    std::vector<double> h2 (est.H.size());
    for (std::size_t k = 0; k < h2.size(); ++k)
        h2[k] = std::norm (est.H[k]);
    c.power = rebinPower (est.freqs, h2, c.freqs);
    c.noisePower = rebinPower (est.freqs, noisePowerHDomain (est), c.freqs);
    const auto gate = rebinPower (est.freqs, excitationGate (est), c.freqs);
    const auto band = inBand (c.freqs, 20.0, 20000.0);
    c.weight.resize (c.freqs.size());
    for (std::size_t k = 0; k < c.weight.size(); ++k)
        c.weight[k] = gate[k] > 0.5 ? band[k] : 0.0;

    c.grade = gradeCapture (c.freqs, c.power, c.noisePower, nullptr, cfg.grading, std::min (20000.0, fs / 2.0));
    for (auto& note : driftNotes (est.drift))
        c.grade.notes.push_back (std::move (note));
    c.delaysMs.push_back (1000.0 * static_cast<double> (est.delay) / fs);
    c.driftPpm = est.drift.ppm;
    return c;
}

void regradeCapture (Capture& c, const GradingConfig& grading)
{
    auto signal = c.power;
    if (c.kind == "sweep")
        for (std::size_t k = 0; k < signal.size() && k < c.noisePower.size(); ++k)
            signal[k] -= c.noisePower[k];
    c.grade = regrade (c.grade, c.freqs, signal, grading);
}
} // namespace roomeq
