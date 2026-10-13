#include "roomeq/filters.h"
#include "roomeq/livedelta.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;

Band band (BandKind kind, double freq, double gain, double q = shelfQ)
{
    Band b;
    b.kind = kind;
    b.freq = freq;
    b.gainDb = gain;
    b.q = q;
    return b;
}

const std::vector<Band> room { band (BandKind::lowShelf, 120, 5.0), band (BandKind::bell, 1000, -6.0, 2.0),
                               band (BandKind::highShelf, 6000, -4.0) };

// Pink-ish noise, loud and quiet in turns like music.
std::vector<double> pinkish (double seconds, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> normal;
    const auto n = static_cast<std::size_t> (seconds * fs);
    const double b[] { 0.049922035, -0.095993537, 0.050612699, -0.004408786 }, a[] { 1.0, -2.494956002, 2.017265875, -0.522189400 };
    std::vector<double> out (n);
    double xs[4] {}, ys[4] {};
    for (std::size_t i = 0; i < n; ++i)
    {
        for (int k = 3; k > 0; --k)
        {
            xs[k] = xs[k - 1];
            ys[k] = ys[k - 1];
        }
        xs[0] = normal (rng);
        ys[0] = b[0] * xs[0] + b[1] * xs[1] + b[2] * xs[2] + b[3] * xs[3] - a[1] * ys[1] - a[2] * ys[2] - a[3] * ys[3];
        out[i] = ys[0];
    }
    double sum = 0.0;
    for (auto v : out)
        sum += v * v;
    const auto scale = 0.05 / std::sqrt (sum / static_cast<double> (n));   // 0.05 RMS (-26 dBFS), as in the prototype's test
    for (std::size_t i = 0; i < n; ++i)
        out[i] *= scale * (0.6 + 0.4 * std::sin (2.0 * 3.14159265358979 * 0.7 * static_cast<double> (i) / fs));
    return out;
}

std::vector<double> throughRoom (const std::vector<double>& x, double delayMs = 37.0, unsigned seed = 9)
{
    auto y = x;
    for (const auto& b : room)
    {
        const auto c = designBiquad (b, fs);
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y)
        {
            const auto out = c.b0 * v + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = out;
            v = out;
        }
    }
    const auto d = static_cast<std::size_t> (std::lround (delayMs * 1e-3 * fs));
    std::vector<double> out (y.size(), 0.0);
    for (std::size_t i = d; i < y.size(); ++i)
        out[i] = y[i - d];
    std::mt19937 rng (seed);
    std::normal_distribution<double> noise (0.0, 3e-4);   // -40 dB under the signal
    for (auto& v : out)
        v += noise (rng);
    return out;
}

LiveDeltaTracker run (const std::vector<double>& x, const std::vector<double>& y, std::size_t block = 4800,
                      std::pair<double, double> refBand = { 250.0, 4000.0 })
{
    LiveDeltaTracker t (fs, refBand);
    for (std::size_t i = 0; i < x.size(); i += block)
        t.push (x.data() + i, y.data() + i, std::min (block, x.size() - i));
    return t;
}

std::vector<double> expected (const std::vector<double>& grid, std::pair<double, double> refBand = { 250.0, 4000.0 })
{
    auto db = responseDb (room, grid, fs);
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < grid.size(); ++i)
        if (grid[i] >= refBand.first && grid[i] <= refBand.second)
        {
            sum += db[i];
            ++count;
        }
    for (auto& v : db)
        v -= sum / count;
    return db;
}
} // namespace

TEST_CASE ("live delta: finds the delay and what the room does")
{
    const auto x = pinkish (30.0, 1);
    const auto r = run (x, throughRoom (x)).result();
    REQUIRE (r.delayMs.has_value());
    CHECK (*r.delayMs == doctest::Approx (37.0).epsilon (0.002));
    CHECK (r.frames > 30);
    const auto want = expected (r.grid);
    double worst = 0.0, bandSum = 0.0;
    int checked = 0, reliable = 0, bandCount = 0;
    for (std::size_t i = 0; i < r.grid.size(); ++i)
    {
        if (r.grid[i] < 40.0 || r.grid[i] > 16000.0)
            continue;
        ++checked;
        if (! r.valid[i])
            continue;
        ++reliable;
        worst = std::max (worst, std::abs (r.deltaDb[i] - want[i]));
        if (r.grid[i] >= 250.0 && r.grid[i] <= 4000.0)
        {
            bandSum += r.deltaDb[i];
            ++bandCount;
        }
    }
    CHECK (reliable > checked * 95 / 100);
    CHECK (worst < 0.75);
    CHECK (std::abs (bandSum / bandCount) < 1e-9);
}

TEST_CASE ("live delta: any block size gives the same result")
{
    const auto x = pinkish (14.0, 2);
    const auto y = throughRoom (x);
    const auto a = run (x, y, 4800).result(), b = run (x, y, 1237).result();
    CHECK (a.delayMs == b.delayMs);
    CHECK (a.frames == b.frames);
    CHECK (a.valid == b.valid);
    for (std::size_t i = 0; i < a.grid.size(); ++i)
        if (std::isfinite (a.deltaDb[i]))
            CHECK (a.deltaDb[i] == doctest::Approx (b.deltaDb[i]).epsilon (1e-9));
}

TEST_CASE ("live delta: nothing until there is music, and pauses leave the average")
{
    LiveDeltaTracker t (fs);
    std::vector<float> silence (static_cast<std::size_t> (8 * fs), 0.0f), hiss (silence.size());
    std::mt19937 rng (3);
    std::normal_distribution<float> noise (0.0f, 1e-3f);
    for (auto& v : hiss)
        v = noise (rng);
    t.push (silence.data(), hiss.data(), silence.size());
    auto r = t.result();
    CHECK_FALSE (r.delayMs.has_value());
    CHECK (r.frames == 0);
    CHECK (std::none_of (r.valid.begin(), r.valid.end(), [] (bool v) { return v; }));

    const auto x = pinkish (16.0, 4);
    const auto y = throughRoom (x);
    for (std::size_t i = 0; i < x.size(); i += 4800)
        t.push (x.data() + i, y.data() + i, std::min<std::size_t> (4800, x.size() - i));
    REQUIRE (t.result().delayMs.has_value());
    CHECK (*t.result().delayMs == doctest::Approx (37.0).epsilon (0.002));

    t.push (silence.data(), hiss.data(), static_cast<std::size_t> (2 * fs));   // frames reaching back into the music count
    const auto before = t.result();
    t.push (silence.data(), hiss.data(), silence.size());                       // silence doesn't
    const auto after = t.result();
    CHECK (after.frames == before.frames);
    for (std::size_t i = 0; i < after.grid.size(); ++i)
        if (std::isfinite (before.deltaDb[i]))
            CHECK (after.deltaDb[i] == doctest::Approx (before.deltaDb[i]));
}

TEST_CASE ("live delta: the reference band sets 0 dB")
{
    const auto x = pinkish (20.0, 11);
    const auto r = run (x, throughRoom (x), 4800, { 40.0, 100.0 }).result();
    const auto want = expected (r.grid, { 40.0, 100.0 });
    double worst = 0.0;
    for (std::size_t i = 0; i < r.grid.size(); ++i)
        if (r.valid[i] && r.grid[i] >= 40.0 && r.grid[i] <= 16000.0)
            worst = std::max (worst, std::abs (r.deltaDb[i] - want[i]));
    CHECK (worst < 0.75);
}
