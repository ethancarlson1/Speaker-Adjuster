#pragma once

// Small DSP helpers for the C++ tests (biquads, DTFT, noise).

#include "roomeq/fft.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

namespace testing
{
constexpr double pi = 3.14159265358979323846;

struct Biquad
{
    double b0, b1, b2, a1, a2;   // normalised, a0 = 1

    std::vector<double> process (const std::vector<double>& x) const
    {
        std::vector<double> y (x.size());
        double s1 = 0.0, s2 = 0.0;   // transposed direct form II
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const auto out = b0 * x[i] + s1;
            s1 = b1 * x[i] - a1 * out + s2;
            s2 = b2 * x[i] - a2 * out;
            y[i] = out;
        }
        return y;
    }

    double magnitudeDb (double f, double fs) const
    {
        const auto z = std::polar (1.0, -2.0 * pi * f / fs);
        const auto h = (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z);
        return 20.0 * std::log10 (std::abs (h));
    }
};

// RBJ cookbook designs.
inline Biquad highpass (double f0, double q, double fs)
{
    const auto w0 = 2.0 * pi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
    const auto a0 = 1.0 + alpha;
    return { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
}

inline Biquad lowpass (double f0, double q, double fs)
{
    const auto w0 = 2.0 * pi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
    const auto a0 = 1.0 + alpha;
    return { (1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
}

inline Biquad peaking (double f0, double gainDb, double q, double fs)
{
    const auto a = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * pi * f0 / fs;
    const auto alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0), a0 = 1.0 + alpha / a;
    return { (1.0 + alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha / a) / a0 };
}

// |DTFT| of x at frequency f, in dB (x may start anywhere: magnitude ignores delay).
inline double dtftDb (const std::vector<double>& x, double f, double fs)
{
    std::complex<double> acc {};
    const auto step = std::polar (1.0, -2.0 * pi * f / fs);
    std::complex<double> z { 1.0, 0.0 };
    for (auto v : x)
    {
        acc += v * z;
        z *= step;
    }
    return 20.0 * std::log10 (std::abs (acc));
}

inline std::vector<double> geomspace (double a, double b, int n)
{
    std::vector<double> out (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] = a * std::pow (b / a, static_cast<double> (i) / (n - 1));
    return out;
}

inline std::vector<double> whiteNoise (std::size_t n, double rms, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> dist (0.0, rms);
    std::vector<double> x (n);
    for (auto& v : x)
        v = dist (rng);
    return x;
}

// Linear convolution truncated to a.size() samples.
inline std::vector<double> fftConvolve (const std::vector<double>& a, const std::vector<double>& b)
{
    const auto n = roomeq::nextPow2 (a.size() + b.size());
    auto A = roomeq::rfft (a, n);
    const auto B = roomeq::rfft (b, n);
    for (std::size_t k = 0; k < A.size(); ++k)
        A[k] *= B[k];
    auto y = roomeq::irfft (A, n);
    y.resize (a.size());
    return y;
}

inline std::vector<double> delayed (const std::vector<double>& x, std::size_t delay)
{
    std::vector<double> y (x.size(), 0.0);
    for (std::size_t i = delay; i < x.size(); ++i)
        y[i] = x[i - delay];
    return y;
}
} // namespace testing
