// Headless end-to-end check of the plugin, plus UI screenshots.
//
// Instantiates the processor, runs processBlock against a simulated room (the
// mic hears the speaker output through filters, reflections, a small reverb
// and noise), measures several positions, a noisy one, a music capture and a
// pink-noise capture, checks grades and delays; then fits, applies and
// verifies a correction (the speakers must get exactly the predicted EQ, and
// re-measuring through it must land near the target), checks undo / compare;
// then loudness compensation: the mic calibrator, the level calibration in the
// room, tracking the level of music, the shelves the speakers get, the
// deadband, and the re-check after an amp gain change; then the state round
// trip; zones (a sub on a mono track, delay and polarity); pink noise and
// music across two clocks (a mic 20 ppm fast); the standalone app; and renders
// each editor tab to PNG.
//
//   AdaptiveRoomEQ_Harness [--out snapshot.png]
//   (also writes snapshot-correct.png, -voicing.png, -loudness.png, -drift.png, -standalone.png)
//
// Exits non-zero if any check fails.

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "roomeq/levelmatch.h"
#include "roomeq/noise.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <complex>
#include <iostream>
#include <random>

namespace
{
constexpr double fs = 48000.0;
constexpr int blockSize = 512;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (! ok)
        ++failures;
}

struct Biquad
{
    double b0, b1, b2, a1, a2, s1 = 0.0, s2 = 0.0;

    static Biquad peaking (double f0, double gainDb, double q)
    {
        const auto a = std::pow (10.0, gainDb / 40.0), w0 = juce::MathConstants<double>::twoPi * f0 / fs;
        const auto alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0), a0 = 1.0 + alpha / a;
        return { (1.0 + alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha / a) / a0 };
    }

    static Biquad lowpass (double f0, double q)
    {
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
        const auto a0 = 1.0 + alpha;
        return { (1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
    }

    static Biquad highpass (double f0, double q)
    {
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0);
        const auto a0 = 1.0 + alpha;
        return { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0 };
    }

    double process (double x)
    {
        const auto y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
};

// Random numbers that are the same on every platform: std::mt19937 is fully
// specified, but the standard distributions are not (libc++, libstdc++ and
// MSVC give different sequences), which would give each CI platform a
// different simulated room.
struct PortableRandom
{
    explicit PortableRandom (unsigned seed) : engine (seed) {}

    double uniform() { return (static_cast<double> (engine()) + 0.5) / 4294967296.0; }   // (0, 1)
    double uniform (double lo, double hi) { return lo + (hi - lo) * uniform(); }
    int uniformInt (int lo, int hi) { return lo + static_cast<int> (engine() % static_cast<std::uint32_t> (hi - lo + 1)); }

    double normal()   // Box-Muller
    {
        if (hasSpare)
        {
            hasSpare = false;
            return spare;
        }
        const auto r = std::sqrt (-2.0 * std::log (uniform()));
        const auto theta = juce::MathConstants<double>::twoPi * uniform();
        spare = r * std::sin (theta);
        hasSpare = true;
        return r * std::cos (theta);
    }

    std::mt19937 engine;
    double spare = 0.0;
    bool hasSpare = false;
};

// A streaming "room" for one mic position: PA filters, direct sound plus a
// few reflections, a Schroeder reverb tail, noise, and a fixed latency.
class SimulatedRoom
{
public:
    SimulatedRoom (double distance, unsigned seed, double noiseLevel, double rumbleLevel)
        : rng (seed), noise (noiseLevel), rumble (rumbleLevel)
    {
        direct = 1.0 / distance;
        delay = static_cast<int> (distance / 343.0 * fs) + 288;   // flight time + 6 ms latency
        for (int i = 0; i < 6; ++i)
        {
            const auto tap = rng.uniformInt (200, 2400);
            reflections.push_back ({ delay + tap, rng.uniform (-0.5, 0.5) / distance });
        }
        line.assign (static_cast<std::size_t> (delay + 2600), 0.0);
        const int combLengths[] = { 1557, 1617, 1491, 1422 };
        for (auto len : combLengths)
            combs.push_back ({ std::vector<double> (static_cast<std::size_t> (len + static_cast<int> (seed % 50)), 0.0), 0 });
        allpasses = { { std::vector<double> (225, 0.0), 0 }, { std::vector<double> (556, 0.0), 0 } };
    }

    float process (float speaker)
    {
        auto x = hp.process (speaker);
        x = dip.process (bump.process (x));
        line[writePos] = x;
        const auto at = [&] (int d) { return line[(writePos + line.size() - static_cast<std::size_t> (d)) % line.size()]; };
        auto y = direct * at (delay);
        for (const auto& [d, g] : reflections)
            y += g * at (d);

        // Diffuse tail (RT60 ~ 0.7 s), fed by the direct sound.
        auto wet = 0.0;
        for (auto& c : combs)
        {
            auto& v = c.buffer[c.pos];
            wet += v;
            v = at (delay) * 0.12 + v * 0.84;
            c.pos = (c.pos + 1) % c.buffer.size();
        }
        for (auto& a : allpasses)
        {
            const auto delayed = a.buffer[a.pos];
            const auto input = wet + 0.5 * delayed;
            a.buffer[a.pos] = input;
            wet = delayed - 0.5 * input;
            a.pos = (a.pos + 1) % a.buffer.size();
        }
        y += wet * 0.35;
        writePos = (writePos + 1) % line.size();

        const auto low = rumbleFilter2.process (rumbleFilter1.process (rng.normal()));   // below ~70 Hz
        return static_cast<float> (y + noise * rng.normal() + rumble * low);
    }

private:
    struct Delay
    {
        std::vector<double> buffer;
        std::size_t pos;
    };

    PortableRandom rng;
    double noise, rumble;
    Biquad rumbleFilter1 = Biquad::lowpass (70.0, 0.7071), rumbleFilter2 = Biquad::lowpass (70.0, 0.7071);
    double direct = 1.0;
    int delay = 0;
    std::vector<std::pair<int, double>> reflections;
    std::vector<double> line;
    std::size_t writePos = 0;
    std::vector<Delay> combs, allpasses;
    Biquad hp = Biquad::highpass (55.0, 0.7071), bump = Biquad::peaking (150.0, 3.0, 0.9),
           dip = Biquad::peaking (2800.0, -4.0, 1.8);
};

void pump (AdaptiveRoomEQProcessor& p)
{
    p.getEngine().update();
    p.getLoudness().update();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

// Runs audio blocks until the current measurement is recorded, then waits for
// its analysis without playing on (so the audio that follows is the same
// however long the background analysis takes).
template <typename Room>
void runMeasurement (AdaptiveRoomEQProcessor& p, Room& room, const std::vector<float>* program = nullptr)
{
    juce::AudioBuffer<float> buffer (3, blockSize);   // main L/R in-place + mic sidechain
    juce::MidiBuffer midi;
    std::vector<float> speaker (blockSize, 0.0f);
    std::size_t programPos = 0;
    const auto channel = p.getSweepSettings().channel;

    for (int block = 0; block < 100000 && p.getEngine().getActivity() == MeasurementEngine::Activity::measuring; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto in = program != nullptr && programPos < program->size() ? (*program)[programPos++] : 0.0f;
            buffer.setSample (0, i, in);
            buffer.setSample (1, i, in);
            buffer.setSample (2, i, room.process (speaker[static_cast<std::size_t> (i)]));   // hears last block
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            speaker[static_cast<std::size_t> (i)] = buffer.getSample (program != nullptr ? 0 : channel, i);
        if (block % 64 == 0)
            pump (p);
    }
    for (int i = 0; i < 12000 && p.getEngine().getActivity() != MeasurementEngine::Activity::idle; ++i)   // up to a minute (sanitizer builds are slow)
        pump (p);
    for (int i = 0; i < 20; ++i)   // let the averaged display catch up
        pump (p);
}

// Runs half a second of audio with `micLevel` of noise at the mic (0: a dead
// input) and no program, as a host does before anyone presses Measure.
void idleWithMic (AdaptiveRoomEQProcessor& p, float micLevel)
{
    const auto micChannel = p.isStandalone() ? 0 : 2;
    juce::AudioBuffer<float> buffer (p.isStandalone() ? 2 : 3, blockSize);
    juce::MidiBuffer midi;
    PortableRandom rng (99);
    for (int b = 0; b < static_cast<int> (fs) / 2 / blockSize; ++b)
    {
        buffer.clear();
        for (int i = 0; i < blockSize; ++i)
            buffer.setSample (micChannel, i, static_cast<float> (micLevel * rng.normal()));
        p.processBlock (buffer, midi);
    }
}

void setParam (AdaptiveRoomEQProcessor& p, const juce::String& id, float value)
{
    auto* param = p.getParameters().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

// The main path's response (program in, speakers out) to an impulse, after
// letting any EQ glide settle (the loudness stage glides with a 0.25 s time
// constant). `aSecondBefore` runs 1 s before the impulse.
std::vector<double> impulseThrough (AdaptiveRoomEQProcessor& p, int length = 1 << 15,
                                    const std::function<void()>& aSecondBefore = {})
{
    juce::AudioBuffer<float> buffer (3, blockSize);
    juce::MidiBuffer midi;
    PortableRandom rng (98);
    const auto micNoise = [&]   // a real mic always hears something; only the main path matters here
    {
        for (int i = 0; i < blockSize; ++i)
            buffer.setSample (2, i, static_cast<float> (1e-4 * rng.normal()));
    };
    constexpr int preroll = 300, oneSecond = static_cast<int> (fs) / blockSize;
    for (int b = 0; b < preroll; ++b)
    {
        if (b == preroll - oneSecond && aSecondBefore)
            aSecondBefore();
        buffer.clear();
        micNoise();
        p.processBlock (buffer, midi);
    }
    std::vector<double> out;
    for (int pos = 0; pos < length; pos += blockSize)
    {
        buffer.clear();
        micNoise();
        if (pos == 0)
        {
            buffer.setSample (0, 0, 1.0f);
            buffer.setSample (1, 0, 1.0f);
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    return out;
}

double dtftDb (const std::vector<double>& x, double f)
{
    std::complex<double> acc {}, z { 1.0, 0.0 };
    const auto step = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / fs);
    for (auto v : x)
    {
        acc += v * z;
        z *= step;
    }
    return 20.0 * std::log10 (std::abs (acc));
}

// `offsetDb`: a broadband gain on top of the bands (the output level match).
double worstMismatchDb (const std::vector<double>& ir, const std::vector<roomeq::Band>& expected, double offsetDb = 0.0)
{
    double worst = 0.0;
    for (double f : { 30.0, 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 })
        worst = std::max (worst, std::abs (dtftDb (ir, f) - roomeq::responseDb (expected, { f }, fs).front() - offsetDb));
    return worst;
}

// The make-up gain the output level match should apply for these correction + voicing bands.
double makeupFor (const std::vector<roomeq::Band>& bands)
{
    roomeq::LevelMatch lm;
    lm.prepare (fs);
    return lm.makeupDb (bands);
}

// K-weighted level (dB) of pink noise going into the plugin's main path and coming out.
std::pair<double, double> pinkLevelsInOut (AdaptiveRoomEQProcessor& p)
{
    roomeq::NoiseConfig cfg;
    cfg.fs = fs;
    cfg.duration = 10.0;
    cfg.levelDbfs = -12.0;
    const auto x = roomeq::generatePinkNoise (cfg);
    juce::AudioBuffer<float> buffer (3, blockSize);
    juce::MidiBuffer midi;
    std::vector<double> in, out;
    PortableRandom rng (97);
    for (std::size_t pos = 0; pos + blockSize <= x.size(); pos += blockSize)
    {
        buffer.clear();
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, static_cast<float> (x[pos + static_cast<std::size_t> (i)]));
            buffer.setSample (1, i, static_cast<float> (x[pos + static_cast<std::size_t> (i)]));
            buffer.setSample (2, i, static_cast<float> (1e-4 * rng.normal()));   // the mic's own noise floor
            in.push_back (buffer.getSample (0, i));
        }
        p.processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    const auto kLevel = [] (const std::vector<double>& v)
    {
        auto y = v;
        for (const auto& band : roomeq::kWeightingBands())
        {
            const auto c = roomeq::designBiquad (band, fs);
            double s1 = 0.0, s2 = 0.0;
            for (auto& t : y)
            {
                const auto o = c.b0 * t + s1;
                s1 = c.b1 * t - c.a1 * o + s2;
                s2 = c.b2 * t - c.a2 * o;
                t = o;
            }
        }
        double sum = 0.0;
        const auto start = static_cast<std::size_t> (fs);   // after any glide and the filters' start-up
        for (std::size_t i = start; i < y.size(); ++i)
            sum += y[i] * y[i];
        return 10.0 * std::log10 (sum / static_cast<double> (y.size() - start));
    };
    return { kLevel (in), kLevel (out) };
}

// RMS of (curve - target) over the fit range, outside nulls, on the display grid.
double rmsFromTarget (const MeasurementEngine::Display& d, const std::vector<double>& curve)
{
    const auto& r = *d.proposal;
    const auto& g = d.summary.grid;
    double sumSq = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < g.size(); ++i)
    {
        if (g[i] < r.fitRange.first || g[i] > r.fitRange.second || ! std::isfinite (curve[i]))
            continue;
        std::size_t k = 0;
        for (std::size_t j = 1; j < r.grid.size(); ++j)
            if (std::abs (std::log (r.grid[j] / g[i])) < std::abs (std::log (r.grid[k] / g[i])))
                k = j;
        if (r.nullMask[k])
            continue;
        sumSq += (curve[i] - d.targetDb[i]) * (curve[i] - d.targetDb[i]);
        ++n;
    }
    return n > 0 ? std::sqrt (sumSq / n) : 1e9;
}

std::shared_ptr<const MeasurementEngine::Display> waitForDisplay (AdaptiveRoomEQProcessor& p,
                                                                  const std::function<bool (const MeasurementEngine::Display&)>& ready)
{
    for (int i = 0; i < 12000; ++i)   // up to a minute; returns as soon as it is ready
    {
        pump (p);
        if (auto d = p.getEngine().getDisplay(); d != nullptr && p.getEngine().isDisplayCurrent() && ready (*d))
            return d;
    }
    return p.getEngine().getDisplay();
}

juce::String signedText (double v)
{
    return (v >= 0.0 ? "+" : "") + juce::String (v, 2);
}

// Endless music-like program (a beat on tilted noise) at an adjustable gain.
struct Music
{
    PortableRandom rng { 77 };
    Biquad tilt = Biquad::peaking (200.0, 8.0, 0.5);
    std::size_t n = 0;
    double gain = 0.05;

    float next()
    {
        const auto beat = std::fmod (static_cast<double> (n++) / fs, 0.5);
        const auto env = 0.4 + 0.6 * std::exp (-beat / 0.15);
        return static_cast<float> (gain * env * tilt.process (rng.normal()));
    }
};

// Loudness runs: plays `music` (or silence) on both inputs for up to `seconds`,
// the mic hearing channel 0 through the room and an amp gain after the plugin,
// or `micSignal` instead of the room. Stops early once `until` is true.
struct Show
{
    AdaptiveRoomEQProcessor& p;
    SimulatedRoom& room;
    double ampGainDb = 0.0;
    std::function<float (std::size_t)> micSignal;
    std::function<double (double)> roomChange;   // e.g. the low mids building up as the room fills
    std::vector<double> lastOutput;   // channel 0, the last `keepSeconds`
    double rightEnergy = 0.0, channelDiff = 0.0;
    double keepSeconds = 6.0;
    std::vector<float>* playedIn = nullptr;    // when set: channel 0 in, and `playedChannel` out
    std::vector<float>* playedOut = nullptr;
    int playedChannel = 0;
    std::vector<float> speaker = std::vector<float> (blockSize, 0.0f);
    std::size_t t = 0;

    void play (Music* music, double seconds, const std::function<bool()>& until = {})
    {
        juce::AudioBuffer<float> buffer (3, blockSize);
        juce::MidiBuffer midi;
        const auto amp = static_cast<float> (std::pow (10.0, ampGainDb / 20.0));
        const auto keep = static_cast<std::size_t> (keepSeconds * fs);
        const auto blocks = static_cast<int> (seconds * fs / blockSize);
        for (int block = 0; block < blocks; ++block)
        {
            for (int i = 0; i < blockSize; ++i, ++t)
            {
                const auto in = music != nullptr ? music->next() : 0.0f;
                buffer.setSample (0, i, in);
                buffer.setSample (1, i, in);
                auto heard = room.process (amp * speaker[static_cast<std::size_t> (i)]);   // hears last block
                if (roomChange)
                    heard = static_cast<float> (roomChange (heard));
                buffer.setSample (2, i, micSignal ? micSignal (t) : heard);
                if (playedIn != nullptr)
                    playedIn->push_back (in);
            }
            p.processBlock (buffer, midi);
            for (int i = 0; i < blockSize; ++i)
            {
                speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);
                lastOutput.push_back (buffer.getSample (0, i));
                if (playedOut != nullptr)
                    playedOut->push_back (buffer.getSample (playedChannel, i));
                rightEnergy += static_cast<double> (buffer.getSample (1, i)) * buffer.getSample (1, i);
                channelDiff = std::max (channelDiff, static_cast<double> (std::abs (buffer.getSample (1, i) - buffer.getSample (0, i))));
            }
            if (lastOutput.size() > 2 * keep)
                lastOutput.erase (lastOutput.begin(), lastOutput.end() - static_cast<long> (keep));
            if (block % 16 == 0)
            {
                pump (p);
                if (until && until())
                    return;
            }
        }
    }

    double outputLevelDbfs() const   // C-weighted, over the last keepSeconds
    {
        const auto keep = static_cast<std::size_t> (keepSeconds * fs);
        const std::vector<double> tail (lastOutput.end() - static_cast<long> (std::min (keep, lastOutput.size())), lastOutput.end());
        return roomeq::cWeightedLevelDbfs (tail, fs);
    }
};

std::vector<float> musicLikeProgram (double seconds)
{
    PortableRandom rng (42);
    std::vector<float> x (static_cast<std::size_t> (seconds * fs));
    Biquad tilt = Biquad::peaking (200.0, 8.0, 0.5);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto beat = std::fmod (static_cast<double> (i) / fs, 0.5);
        const auto env = 0.4 + 0.6 * std::exp (-beat / 0.15);
        x[i] = static_cast<float> (0.05 * env * tilt.process (rng.normal()));
    }
    return x;
}
} // namespace

void writeSnapshot (juce::Component& c, const juce::String& path)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.5f);
    juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
    file.deleteFile();
    juce::FileOutputStream stream (file);
    check (stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream), "wrote " + file.getFullPathName());
}

// The standalone app: mono mic in, stereo out, and the mic shares channel 0
// with the first output (as JUCE's AudioProcessorPlayer lays the buffer out).
// The room heard through a mic whose sample clock runs `fastPpm` fast against
// the output's (an aggregate device): mic sample n is the room at n / (1 + fastPpm),
// band-limited, so the delay grows. Fast clocks only, so it never needs a
// sample the room hasn't made yet.
struct DriftingMic
{
    SimulatedRoom& room;
    double fastPpm;
    std::vector<double> heard {};
    std::size_t n = 0;

    float process (float speaker)
    {
        heard.push_back (room.process (speaker));
        const auto at = static_cast<double> (n++) / (1.0 + fastPpm * 1e-6) - 40.0;   // 40 samples back: room for the taps
        return static_cast<float> (roomeq::interpolateAt (heard, at));
    }
};

// Pink noise and music measured across two clocks: the drift is measured and
// corrected, the captures pass, and they say so.
void clockDriftChecks (const juce::String& snapshotPath)
{
    std::cout << "Two clocks (an aggregate device)\n";
    AdaptiveRoomEQProcessor proc;
    proc.enableAllBuses();
    proc.setRateAndBufferSizeDetails (fs, blockSize);
    proc.prepareToPlay (fs, blockSize);
    auto& engine = proc.getEngine();
    constexpr double ppm = 20.0;

    setParam (proc, "measureSignal", 1.0f);   // pink noise, 20 s
    idleWithMic (proc, 3e-4f);
    {
        SimulatedRoom room (7.2, 11, 3e-4, 0.0);
        DriftingMic mic { room, ppm };
        check (proc.startMeasurement().wasOk(), "start pink noise, mic clock 20 ppm fast");
        runMeasurement (proc, mic);
    }
    {
        SimulatedRoom room (6.0, 9, 3e-4, 0.0);
        DriftingMic mic { room, ppm };
        const auto program = musicLikeProgram (MeasurementEngine::programSettleSeconds + AdaptiveRoomEQProcessor::programSeconds + 1.0);
        check (proc.startProgram().wasOk(), "start music capture, mic clock 20 ppm fast");
        runMeasurement (proc, mic, &program);
    }
    const auto entries = engine.getEntries();
    check (entries.size() == 2, "two captures");
    for (const auto& e : entries)
    {
        const auto& c = *e.capture;
        const auto note = std::find_if (c.grade.notes.begin(), c.grade.notes.end(),
                                        [] (const std::string& t) { return t.rfind ("output and mic clocks differ by", 0) == 0; });
        std::cout << "    " << c.name << " [" << c.kind << "] " << roomeq::gradeLabel (c.grade.overall) << "  drift " << c.driftPpm << " ppm\n";
        check (c.grade.overall == roomeq::Grade::pass, juce::String (c.name) + " graded pass across two clocks");
        check (std::abs (c.driftPpm - ppm) < 0.2 && note != c.grade.notes.end(),
               juce::String (c.name) + " measured the drift (" + juce::String (c.driftPpm, 2) + " ppm) and says so");
    }
    for (int i = 0; i < 40; ++i)
        pump (proc);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (1200, 820);
    for (int i = 0; i < 20; ++i)
        pump (proc);
    writeSnapshot (*editor, snapshotPath);
}

void standaloneChecks (const juce::String& snapshotPath)
{
    std::cout << "Standalone app mode\n";
    // Same steps as JUCE's createPluginFilterOfType for the standalone wrapper.
    juce::PluginHostType::jucePlugInClientCurrentWrapperType = juce::AudioProcessor::wrapperType_Standalone;
    juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);
    auto proc = std::make_unique<AdaptiveRoomEQProcessor>();
    juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);
    juce::PluginHostType::jucePlugInClientCurrentWrapperType = juce::AudioProcessor::wrapperType_Undefined;
    check (proc->isStandalone(), "processor knows it's the standalone app");
    check (proc->getTotalNumInputChannels() == 1 && proc->getTotalNumOutputChannels() == 2, "mono mic input, stereo output");
    proc->setRateAndBufferSizeDetails (fs, blockSize);
    proc->prepareToPlay (fs, blockSize);
    check (proc->startProgram().failed(), "music capture refused (no program passes through the app)");
    check (proc->getLoudness().startCalibration().failed(), "loudness calibration refused (plugin only)");

    const double distance = 7.0;
    SimulatedRoom room (distance, 11, 3e-4, 0.0);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> speaker (blockSize, 0.0f);
    idleWithMic (*proc, 0.0f);
    check (proc->startSweep().failed() && proc->checkMicSignal().getErrorMessage().contains ("Mic input"),
           "standalone: a dead mic input refuses the sweep and says where to pick the mic");
    idleWithMic (*proc, 3e-4f);
    check (proc->startSweep().wasOk(), "start sweep");
    for (int block = 0; block < 100000 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, room.process (speaker[static_cast<std::size_t> (i)]));   // mic, in place
            buffer.setSample (1, i, 0.0f);
        }
        proc->processBlock (buffer, midi);
        for (int i = 0; i < blockSize; ++i)
            speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);   // output 1 feeds the speaker
        if (block % 64 == 0)
            pump (*proc);
    }
    for (int i = 0; i < 400 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++i)
        pump (*proc);
    const auto entries = proc->getEngine().getEntries();
    check (entries.size() == 1 && entries[0].capture->grade.overall == roomeq::Grade::pass, "sweep capture graded pass");
    if (! entries.empty())
    {
        const auto expected = 1000.0 * (distance / 343.0 * fs + 288 + blockSize) / fs;
        check (std::abs (entries[0].capture->delaysMs.front() - expected) < 0.5,
               "loop delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
    }

    // Verify through a (quick-mode) correction: the app plays the sweep through the EQ.
    for (int i = 0; i < 400 && ! (proc->getEngine().getDisplay() && proc->getEngine().getDisplay()->proposal
                                 && proc->getEngine().isDisplayCurrent()); ++i)
        pump (*proc);
    proc->getEngine().applyProposal();
    check (! proc->getEngine().getApplied().empty(), "standalone: quick-mode correction applied");
    {
        SimulatedRoom again (distance, 12, 3e-4, 0.0);
        std::fill (speaker.begin(), speaker.end(), 0.0f);
        check (proc->startVerify().wasOk(), "standalone: start verify");
        for (int block = 0; block < 100000 && proc->getEngine().getActivity() != MeasurementEngine::Activity::idle; ++block)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                buffer.setSample (0, i, again.process (speaker[static_cast<std::size_t> (i)]));
                buffer.setSample (1, i, 0.0f);
            }
            proc->processBlock (buffer, midi);
            for (int i = 0; i < blockSize; ++i)
                speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);
            if (block % 64 == 0)
                pump (*proc);
        }
        for (int i = 0; i < 400 && ! proc->getEngine().isDisplayCurrent(); ++i)
            pump (*proc);
        const auto all = proc->getEngine().getEntries();
        check (all.size() == 2 && all.back().verify && all.back().capture->grade.overall == roomeq::Grade::pass,
               "standalone: verify capture graded pass");
        const auto d = proc->getEngine().getDisplay();
        check (d != nullptr && d->verifiedCount == 1, "standalone: verified curve shown");
    }

    // Idle: a loud mic must never reach the speakers.
    for (int i = 0; i < blockSize; ++i)
        buffer.setSample (0, i, 0.5f * std::sin (0.05f * static_cast<float> (i)));
    proc->processBlock (buffer, midi);
    check (buffer.getMagnitude (0, blockSize) <= 0.0f, "mic is not passed through to the outputs");

    // Render this editor with pink noise selected so both layouts get a snapshot.
    auto* signal = proc->getParameters().getParameter ("measureSignal");
    signal->setValueNotifyingHost (signal->convertTo0to1 (1.0f));
    for (int i = 0; i < 20; ++i)
        pump (*proc);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setSize (1160, 760);
    for (int i = 0; i < 20; ++i)
        pump (*proc);
    writeSnapshot (*editor, snapshotPath);
}

// A sub on a mono track: the zone band, its defaults, the fit, and re-grading
// when the zone changes. Then the zone's delay and polarity on a stereo
// instance: on the output, bypassed while measuring, included in Verify, saved.
void zoneChecks (const juce::String& snapshotStem)
{
    std::cout << "Zones: a sub on a mono track\n";
    {
        AdaptiveRoomEQProcessor proc;
        auto layout = proc.getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        auto surround = layout;
        surround.inputBuses.getReference (0) = surround.outputBuses.getReference (0) = juce::AudioChannelSet::create5point1();
        check (! proc.checkBusesLayoutSupported (surround), "5.1 refused");
        check (proc.setBusesLayout (layout) && proc.isMono() && proc.getTotalNumOutputChannels() == 1
                   && proc.getTotalNumInputChannels() == 2 && proc.isMicConnected(),
               "mono track: one channel in and out, plus the mic");
        proc.setRateAndBufferSizeDetails (fs, blockSize);
        proc.prepareToPlay (fs, blockSize);
        const auto rangeLo = [&] { return proc.getParameters().getRawParameterValue ("rangeLo")->load(); };
        const auto rangeHi = [&] { return proc.getParameters().getRawParameterValue ("rangeHi")->load(); };

        setParam (proc, "zone", 1.0f);   // subs
        for (int i = 0; i < 5; ++i)
            pump (proc);
        check (proc.getZone() == AdaptiveRoomEQProcessor::Zone::subs && proc.getReferenceBand() == std::pair { 40.0, 100.0 }
                   && std::abs (rangeLo() - 20.0f) < 0.5f && std::abs (rangeHi() - 150.0f) < 1.0f,
               "Subs: judged on 40-100 Hz, correcting 20-150 Hz to start");
        setParam (proc, "sweepSpeaker", 1.0f);   // right: meaningless on one channel
        check (proc.getSweepSettings().channel == 0, "mono: the sweep plays on the one channel");
        setParam (proc, "sweepLength", 0.0f);

        // A sub: 4th-order low-pass at 100 Hz and a 55 Hz room mode, into the room (its own 55 Hz high-pass).
        struct Sub
        {
            SimulatedRoom room;
            Biquad lp1 = Biquad::lowpass (100.0, 0.5412), lp2 = Biquad::lowpass (100.0, 1.3066),
                   mode = Biquad::peaking (62.0, 6.0, 3.0);
            float process (float x) { return room.process (static_cast<float> (mode.process (lp2.process (lp1.process (x))))); }
        };
        juce::AudioBuffer<float> buffer (2, blockSize);   // main (in place) + mic
        juce::MidiBuffer midi;
        const auto measure = [&] (Sub& sub)
        {
            std::vector<float> speaker (blockSize, 0.0f);
            for (int block = 0; block < 100000 && proc.getEngine().getActivity() == MeasurementEngine::Activity::measuring; ++block)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    buffer.setSample (0, i, 0.0f);
                    buffer.setSample (1, i, sub.process (speaker[static_cast<std::size_t> (i)]));
                }
                proc.processBlock (buffer, midi);
                for (int i = 0; i < blockSize; ++i)
                    speaker[static_cast<std::size_t> (i)] = buffer.getSample (0, i);
                if (block % 64 == 0)
                    pump (proc);
            }
            for (int i = 0; i < 12000 && proc.getEngine().getActivity() != MeasurementEngine::Activity::idle; ++i)
                pump (proc);
        };
        // The mic check wants recent signal on the mic channel (channel 1 here).
        const auto micAlive = [&]
        {
            PortableRandom rng (5);
            for (int b = 0; b < 20; ++b)
            {
                buffer.clear();
                for (int i = 0; i < blockSize; ++i)
                    buffer.setSample (1, i, static_cast<float> (3e-4 * rng.normal()));
                proc.processBlock (buffer, midi);
            }
        };
        for (int pos = 0; pos < 3; ++pos)
        {
            Sub sub { SimulatedRoom (4.0 + 2.5 * pos, static_cast<unsigned> (31 + pos), 2e-4, 0.0) };
            micAlive();
            check (proc.startSweep().wasOk(), "sub position " + juce::String (pos + 1) + ": start sweep");
            measure (sub);
        }
        auto& engine = proc.getEngine();
        const auto entries = engine.getEntries();
        const auto oneK = [] (const roomeq::Capture& c)
        { return std::find_if (c.grade.bands.begin(), c.grade.bands.end(), [] (const auto& b) { return b.name == "1k"; }); };
        auto graded = entries.size() == 3;
        for (const auto& e : entries)
        {
            std::cout << "    " << e.capture->name << " " << roomeq::gradeLabel (e.capture->grade.overall) << "\n";
            graded = graded && e.capture->grade.overall != roomeq::Grade::redo && oneK (*e.capture)->outOfRange;
        }
        check (graded, "sub captures graded where a sub plays (1 kHz out of its range, not a redo)");
        const auto d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
        if (d != nullptr && d->proposal)
        {
            const auto& fit = *d->proposal;
            auto below = ! fit.bands.empty();
            for (const auto& b : fit.bands)
                below = below && b.freq <= 150.0;
            std::cout << "    usable " << d->summary.usable.first << "-" << d->summary.usable.second << " Hz, fit "
                      << fit.fitRange.first << "-" << fit.fitRange.second << " Hz, " << fit.bands.size() << " bands\n";
            check (d->summary.usable.second < 200.0 && d->summary.usable.first < 45.0, "the sub's usable range is its own");
            check (below && fit.fitRange.second <= 150.0, "the fit stays at 150 Hz and below");
            check (roomeq::responseDb (fit.bands, { 62.0 }, fs)[0] < -2.0, "and cuts the room mode");
        }
        else
        {
            check (false, "a fit for the sub");
        }

        // Change the zone: every capture is graded again, exactly, and its defaults are set.
        std::vector<roomeq::CaptureGrade> asSub;
        for (const auto& e : entries)
            asSub.push_back (e.capture->grade);
        setParam (proc, "zone", 0.0f);   // mains
        for (int i = 0; i < 5; ++i)
            pump (proc);
        const auto asMains = engine.getEntries();
        check (asMains.size() == 3 && std::abs (asMains[0].capture->grade.bands[1].levelDb - asSub[0].bands[1].levelDb) > 10.0
                   && std::abs (rangeHi() - 20000.0f) < 1.0f,
               "Mains: the captures are re-graded against 250 Hz-4 kHz, and the range reset");
        setParam (proc, "zone", 1.0f);
        for (int i = 0; i < 5; ++i)
            pump (proc);
        auto same = true;
        const auto back = engine.getEntries();
        for (std::size_t i = 0; i < back.size() && i < asSub.size(); ++i)
        {
            const auto& g = back[i].capture->grade;
            same = same && g.overall == asSub[i].overall && g.reasons == asSub[i].reasons && g.notes == asSub[i].notes;
            for (std::size_t b = 0; b < g.bands.size(); ++b)
                same = same && g.bands[b].outOfRange == asSub[i].bands[b].outOfRange
                       && std::abs (g.bands[b].levelDb - asSub[i].bands[b].levelDb) < 1e-9;
        }
        check (same, "back to Subs: the same grades as measured");

        // A saved session keeps its own range (restoring its zone doesn't reset it).
        setParam (proc, "rangeHi", 180.0f);
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        AdaptiveRoomEQProcessor restored;
        restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        for (int i = 0; i < 5; ++i)
            pump (restored);
        check (restored.getZone() == AdaptiveRoomEQProcessor::Zone::subs
                   && std::abs (restored.getParameters().getRawParameterValue ("rangeHi")->load() - 180.0f) < 1.0f,
               "the zone is saved, and restoring it keeps the session's own range");

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ours = dynamic_cast<AdaptiveRoomEQEditor*> (editor.get());
        editor->setSize (1200, 820);
        for (int i = 0; i < 5; ++i)
            pump (proc);
        writeSnapshot (*editor, snapshotStem + "-sub-measure.png");
        ours->showTab (AdaptiveRoomEQEditor::Tab::zone);
        writeSnapshot (*editor, snapshotStem + "-sub-zone.png");
    }

    std::cout << "Zones: delay and polarity\n";
    {
        AdaptiveRoomEQProcessor proc;
        proc.enableAllBuses();
        proc.setRateAndBufferSizeDetails (fs, blockSize);
        proc.prepareToPlay (fs, blockSize);
        const auto peakOf = [] (const std::vector<double>& ir)
        {
            std::size_t at = 0;
            for (std::size_t i = 1; i < ir.size(); ++i)
                if (std::abs (ir[i]) > std::abs (ir[at]))
                    at = i;
            return at;
        };
        setParam (proc, "zoneDelay", 12.5f);
        setParam (proc, "polarityInvert", 1.0f);
        auto ir = impulseThrough (proc);
        check (peakOf (ir) == 600 && std::abs (ir[600] + 1.0) < 1e-6, "12.50 ms and inverted: the impulse comes out 600 samples later, flipped");
        auto* delayParam = proc.getParameters().getParameter ("zoneDelay");
        check (delayParam->getText (delayParam->getValue(), 64) == "12.50 ms (4.29 m / 14.1 ft)"
                   && std::abs (delayParam->getValueForText ("34.3 m") - delayParam->convertTo0to1 (100.0f)) < 1e-6f,
               "the delay reads as a distance, and a distance can be typed");
        setParam (proc, "zoneDelay", 12.51f);
        ir = impulseThrough (proc);
        check (peakOf (ir) == 600 && std::abs (dtftDb (ir, 1000.0)) < 0.01 && std::abs (dtftDb (ir, 10000.0)) < 0.01,
               "12.51 ms: a fractional delay, flat to 10 kHz (" + juce::String (dtftDb (ir, 10000.0), 3) + " dB)");

        // Measurements bypass them; Verify includes them.
        idleWithMic (proc, 3e-4f);
        setParam (proc, "sweepLength", 0.0f);
        setParam (proc, "zoneDelay", 100.0f);
        {
            SimulatedRoom room (7.0, 21, 3e-4, 0.0);
            check (proc.startSweep().wasOk(), "sweep with a 100 ms delay set");
            runMeasurement (proc, room);
        }
        const auto flight = 1000.0 * (7.0 / 343.0 * fs + 288 + blockSize) / fs;
        auto entries = proc.getEngine().getEntries();
        check (entries.size() == 1 && std::abs (entries[0].capture->delaysMs.front() - flight) < 0.5,
               "the measurement bypassed the delay (" + juce::String (entries.empty() ? 0.0 : entries[0].capture->delaysMs.front(), 2)
                   + " ms loop)");
        waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
        proc.getEngine().applyProposal();
        idleWithMic (proc, 3e-4f);
        {
            SimulatedRoom room (7.0, 22, 3e-4, 0.0);
            check (proc.startVerify().wasOk(), "verify with the delay");
            runMeasurement (proc, room);
        }
        entries = proc.getEngine().getEntries();
        check (entries.size() == 2 && entries[1].verify && std::abs (entries[1].capture->delaysMs.front() - flight - 100.0) < 0.5,
               "Verify measured through it (" + juce::String (entries.size() < 2 ? 0.0 : entries[1].capture->delaysMs.front(), 2)
                   + " ms loop)");

        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        AdaptiveRoomEQProcessor restored;
        restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        check (restored.getZoneSettings() == ZoneSettings { 100.0, true }, "delay and polarity are saved");

        setParam (proc, "zone", 3.0f);   // delay speakers
        setParam (proc, "zoneDelay", 12.5f);
        for (int i = 0; i < 5; ++i)
            pump (proc);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ours = dynamic_cast<AdaptiveRoomEQEditor*> (editor.get());
        editor->setSize (1200, 820);
        ours->showTab (AdaptiveRoomEQEditor::Tab::zone);
        for (int i = 0; i < 5; ++i)
            pump (proc);
        writeSnapshot (*editor, snapshotStem + "-zone.png");
        editor->setSize (1060, 740);
        writeSnapshot (*editor, snapshotStem + "-zone-small.png");
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::String outPath = "plugin_snapshot.png";
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--out")
            outPath = argv[i + 1];

    AdaptiveRoomEQProcessor proc;
    proc.enableAllBuses();
    proc.setRateAndBufferSizeDetails (fs, blockSize);
    proc.prepareToPlay (fs, blockSize);
    check (proc.isMicConnected(), "mic sidechain bus is enabled");

    std::cout << "Mic check\n";
    idleWithMic (proc, 0.0f);
    check (! proc.hasMicSignal() && proc.checkMicSignal().getErrorMessage().contains ("no signal on the mic input"),
           "a dead mic input is noticed");
    check (proc.startSweep().failed() && proc.startNoise().failed() && proc.startProgram().failed()
               && proc.getLoudness().startRecheck().failed() && proc.getLoudness().startMicCalibration (94.0).failed(),
           "sweeps, noise, music, re-check and mic calibration refuse to start");
    {
        juce::AudioBuffer<float> buffer (3, blockSize);
        juce::MidiBuffer midi;
        buffer.clear();
        proc.processBlock (buffer, midi);
        check (proc.getEngine().getActivity() == MeasurementEngine::Activity::idle && buffer.getMagnitude (0, 0, blockSize) <= 0.0f,
               "and nothing plays");
    }
    idleWithMic (proc, 3e-4f);   // the room's noise floor at the mic (-70 dBFS)
    check (proc.hasMicSignal() && proc.checkMicSignal().wasOk(), "a live mic input is ready");

    std::cout << "Measuring positions\n";
    const struct { double distance; unsigned seed; double noise; double rumble; } positions[] = {
        { 5.3, 1, 3e-4, 0.0 }, { 8.3, 2, 3e-4, 0.0 }, { 11.8, 3, 3e-4, 0.0 }, { 14.1, 4, 3e-4, 0.0 },
        { 9.0, 5, 3e-4, 2.0 },   // truck idling outside: rumble below 70 Hz at about -19 dBFS
    };
    for (const auto& pos : positions)
    {
        SimulatedRoom room (pos.distance, pos.seed, pos.noise, pos.rumble);
        check (proc.startSweep().wasOk(), "start sweep at " + juce::String (pos.distance, 1) + " m");
        runMeasurement (proc, room);
    }

    std::cout << "Measuring from program material\n";
    {
        SimulatedRoom room (6.0, 9, 3e-4, 0.0);
        const auto program = musicLikeProgram (MeasurementEngine::programSettleSeconds + AdaptiveRoomEQProcessor::programSeconds + 1.0);
        check (proc.startProgram().wasOk(), "start program capture");
        runMeasurement (proc, room, &program);
    }

    std::cout << "Measuring with pink noise\n";
    const double noiseDistance = 7.2;
    {
        auto* signal = proc.getParameters().getParameter ("measureSignal");
        signal->setValueNotifyingHost (signal->convertTo0to1 (1.0f));   // "Pink noise"
        check (proc.isNoiseSelected() && proc.getNoiseSeconds() > 19.0, "pink noise selected, 20 s");
        SimulatedRoom room (noiseDistance, 11, 3e-4, 0.0);
        check (proc.startMeasurement().wasOk(), "start pink noise capture");
        runMeasurement (proc, room);
        signal->setValueNotifyingHost (signal->convertTo0to1 (0.0f));
    }

    auto entries = proc.getEngine().getEntries();
    check (entries.size() == 7, "seven captures (" + juce::String (static_cast<int> (entries.size())) + ")");
    for (const auto& e : entries)
    {
        const auto& c = *e.capture;
        std::cout << "    " << c.name << " [" << c.kind << "] " << roomeq::gradeLabel (c.grade.overall)
                  << "  delay " << (c.delaysMs.empty() ? 0.0 : c.delaysMs.front()) << " ms";
        for (const auto& r : c.grade.reasons)
            std::cout << "  | " << r;
        std::cout << "\n";
    }
    if (entries.size() == 7)
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto expected = 1000.0 * (positions[i].distance / 343.0 * fs + 288 + blockSize) / fs;
            check (entries[static_cast<std::size_t> (i)].capture->grade.overall == roomeq::Grade::pass,
                   juce::String (entries[static_cast<std::size_t> (i)].capture->name) + " graded pass");
            check (std::abs (entries[static_cast<std::size_t> (i)].capture->delaysMs.front() - expected) < 0.5,
                   "loop delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
        }
        check (entries[4].capture->grade.overall == roomeq::Grade::redo, "rumble capture graded redo");
        check (entries[5].capture->kind == "program", "music capture analysed with the dual-FFT");
        check (entries[6].capture->kind == "noise" && entries[6].capture->grade.overall == roomeq::Grade::pass,
               "pink noise capture graded pass");
        const auto expected = 1000.0 * (noiseDistance / 343.0 * fs + 288 + blockSize) / fs;
        check (std::abs (entries[6].capture->delaysMs.front() - expected) < 0.5,
               "pink noise delay within 0.5 ms of the simulated " + juce::String (expected, 2) + " ms");
        proc.getEngine().setExcluded (entries[4].id, true);
        for (int i = 0; i < 40; ++i)
            pump (proc);
    }

    const auto display = proc.getEngine().getDisplay();
    check (display != nullptr, "averaged display available");
    if (display != nullptr)
        check (display->summary.nGood >= 4 && ! display->summary.policy.maxCorrectionDb.has_value(),
               "enough good positions for full strength (" + juce::String (display->summary.nGood) + ")");

    std::cout << "Correction\n";
    auto& engine = proc.getEngine();
    auto d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
    check (d != nullptr && d->proposal.has_value(), "proposed correction fitted in the background");
    std::vector<roomeq::Band> proposed;
    if (d != nullptr && d->proposal)
    {
        const auto& r = *d->proposal;
        proposed = r.bands;
        const auto [lo, hi] = std::minmax_element (r.correctionDb.begin(), r.correctionDb.end());
        check (! r.bands.empty() && r.bands.size() <= 10, juce::String (static_cast<int> (r.bands.size())) + " bands");
        check (*hi <= 3.1 && *lo >= -12.1, "within +3 / -12 dB (" + juce::String (*lo, 1) + " to " + juce::String (*hi, 1) + ")");
        check (r.rmsErrorDb < 1.5, "predicted " + juce::String (r.rmsErrorDb, 2) + " dB RMS from the target");
        check (juce::exactlyEqual (d->fitFs, 48000.0), "fitted at the plugin's sample rate");
        std::cout << "    fit range " << r.fitRange.first << " - " << r.fitRange.second << " Hz\n";
        for (const auto& b : r.bands)
            std::cout << "    " << roomeq::bandKindName (b.kind) << " " << b.freq << " Hz " << b.gainDb << " dB Q " << b.q << "\n";
    }

    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "audio passes unchanged before Apply");
    check (engine.canApply(), "Apply available");
    engine.applyProposal();
    check (! engine.canApply() && engine.getApplied() == proposed && engine.getAppliedId() > 0, "proposal applied");

    // Band 3 as a +4 dB bell at 1 kHz; the speakers must get correction + voicing exactly as the graph predicts.
    setParam (proc, "v3Type", 0.0f);
    setParam (proc, "v3Freq", 1000.0f);
    setParam (proc, "v3Gain", 4.0f);
    setParam (proc, "v3Q", 1.4f);
    setParam (proc, "v3On", 1.0f);
    auto expected = engine.getApplied();
    for (const auto& b : proc.getVoicingSections())
        expected.push_back (b);
    auto makeup = makeupFor (expected);
    auto worst = worstMismatchDb (impulseThrough (proc), expected, makeup);
    check (worst < 0.02, "speakers get correction + voicing + make-up as predicted (worst " + juce::String (worst, 3) + " dB)");
    check (std::abs (proc.getMakeupDb() - makeup) < 1e-4,   // worked out on the audio thread
           "output level match: " + juce::String (proc.getMakeupDb(), 2) + " dB make-up, as the curves give");
    {
        const auto [in, out] = pinkLevelsInOut (proc);
        check (std::abs (out - in) < 0.3, "pink noise comes out as loud as it goes in (" + juce::String (out - in, 2)
                                               + " dB, K-weighted)");
        setParam (proc, "levelMatch", 0.0f);
        check (worstMismatchDb (impulseThrough (proc), expected) < 0.02 && std::abs (proc.getMakeupDb()) < 1e-6,
               "match output level off: the EQ alone");
        const auto [in2, out2] = pinkLevelsInOut (proc);
        check (std::abs ((out2 - in2) + makeup) < 0.3, "and the level moves by the make-up (" + juce::String (out2 - in2, 2) + " dB)");
        setParam (proc, "levelMatch", 1.0f);
    }
    setParam (proc, "correctionAmount", 50.0f);
    expected.clear();
    for (const auto& b : engine.getApplied())
        expected.push_back (b.scaled (0.5));
    for (const auto& b : proc.getVoicingSections())
        expected.push_back (b);
    worst = worstMismatchDb (impulseThrough (proc), expected, makeupFor (expected));
    check (worst < 0.02, "50% amount halves the correction, make-up follows (worst " + juce::String (worst, 3) + " dB)");
    setParam (proc, "correctionAmount", 100.0f);
    setParam (proc, "correctionOn", 0.0f);
    setParam (proc, "voicingOn", 0.0f);
    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "both stages bypassed: unchanged");
    setParam (proc, "correctionOn", 1.0f);
    setParam (proc, "voicingOn", 1.0f);
    setParam (proc, "v3On", 0.0f);

    std::cout << "Verify\n";
    for (int i = 0; i < 4; ++i)   // the same four spots (same simulated rooms) the sweeps measured
    {
        SimulatedRoom room (positions[i].distance, positions[i].seed, positions[i].noise, 0.0);
        check (proc.startVerify().wasOk(), "start verify at " + juce::String (positions[i].distance, 1) + " m");
        runMeasurement (proc, room);
    }
    entries = engine.getEntries();
    const auto verifyCount = std::count_if (entries.begin(), entries.end(), [] (const auto& e) { return e.verify; });
    check (verifyCount == 4 && entries.back().capture->name == "V4", "four verify captures, V1-V4");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 4; });
    check (d != nullptr && d->proposal && d->verifiedCount == 4, "verified average from the four verify captures");
    if (d != nullptr && d->proposal && ! d->verifiedDb.empty())
    {
        // Phase 2 "done when": re-measuring after applying the correction lands
        // within a few dB of the target across the corrected range.
        const auto before = rmsFromTarget (*d, d->summary.averageDb);
        const auto after = rmsFromTarget (*d, d->verifiedDb);
        check (after < 1.0 && after < 0.75 * before, "re-measured through the correction: " + juce::String (before, 2) + " -> "
                                                         + juce::String (after, 2) + " dB RMS from target");
        check (d->proposal->bands == engine.getApplied(), "verify captures don't change the proposal");
        auto curvesOk = d->verifyIds.size() == 4 && d->verifyDb.size() == 4;
        for (const auto& c : d->verifyDb)
            curvesOk = curvesOk && c.size() == d->summary.grid.size() && std::isfinite (c[c.size() / 2]);
        check (curvesOk, "each verify capture has its own curve, for highlighting");
    }

    std::cout << "Compare and undo\n";
    engine.setComparingPrevious (true);
    check (engine.isComparingPrevious() && engine.getPlaying().empty(), "hear previous plays the previous (none)");
    check (worstMismatchDb (impulseThrough (proc), {}) < 0.001, "and the speakers get it");
    engine.setComparingPrevious (false);
    check (engine.getPlaying() == proposed, "back to the applied correction");
    engine.undoApply();
    check (engine.getApplied().empty() && engine.getPrevious() == proposed, "undo swaps back to the previous");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 0; });
    check (d != nullptr && d->verifiedDb.empty(), "verify captures belong to the correction they measured");
    engine.undoApply();
    check (engine.getApplied() == proposed, "undo again restores it");
    d = waitForDisplay (proc, [] (const MeasurementEngine::Display& x) { return x.verifiedCount == 4; });
    check (d != nullptr && d->verifiedCount == 4, "and its verify captures count again");

    std::cout << "Targets\n";
    proc.setCustomTarget ({ "Tilt", { { 100.0, 2.0 }, { 10000.0, -4.0 } } });
    setParam (proc, "target", static_cast<float> (AdaptiveRoomEQProcessor::targetCustom));
    d = waitForDisplay (proc, [&] (const MeasurementEngine::Display& x) { return x.proposal && x.proposal->bands != proposed; });
    check (d != nullptr && d->proposal && d->proposal->bands != proposed, "custom target refits the proposal");
    check (engine.canApply(), "and it can be applied");
    setParam (proc, "target", 0.0f);
    d = waitForDisplay (proc, [&] (const MeasurementEngine::Display& x) { return x.proposal && x.proposal->bands == proposed; });
    check (d != nullptr && d->proposal && d->proposal->bands == proposed, "back to flat: the same proposal (deterministic)");

    // Leave a little voicing on for the screenshots.
    setParam (proc, "v2On", 1.0f);
    setParam (proc, "v2Gain", 2.0f);
    setParam (proc, "v6On", 1.0f);
    setParam (proc, "v6Gain", -1.5f);
    for (int i = 0; i < 20; ++i)
        pump (proc);
    entries = engine.getEntries();

    std::cout << "Loudness compensation\n";
    auto& loud = proc.getLoudness();
    using Step = LoudnessController::Step;
    const auto& status = proc.getLoudnessStatus();
    const auto correctionAndVoicing = [&]
    {
        auto bands = engine.getApplied();
        for (const auto& b : proc.getVoicingSections())
            bands.push_back (b);
        return bands;
    };
    // The deadband's held level keeps drifting slowly (30 s) towards the tracked one
    // through the silence around the impulse, and the EQ follows it 0.25 s behind,
    // so the response must lie between the EQ the stage showed a second before and
    // just after it.
    const auto loudnessPathError = [&]
    {
        std::vector<roomeq::Band> before;
        const auto ir = impulseThrough (proc, 1 << 15, [&] { before = loudnessBands (status); });
        auto cvBefore = correctionAndVoicing(), cvAfter = correctionAndVoicing();
        const auto cvMakeup = makeupFor (cvBefore);
        for (const auto& b : before)
            cvBefore.push_back (b);
        for (const auto& b : loudnessBands (status))
            cvAfter.push_back (b);
        double worstError = 0.0;
        for (double f : { 30.0, 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 })
        {
            const auto x = dtftDb (ir, f);
            const auto a = roomeq::responseDb (cvBefore, { f }, fs).front() + cvMakeup;
            const auto c = roomeq::responseDb (cvAfter, { f }, fs).front() + cvMakeup;
            worstError = std::max ({ worstError, std::min (a, c) - x, x - std::max (a, c) });
        }
        return worstError;
    };
    // Plays until the step's recording is collected, then waits for its analysis
    // without playing on, so how long the background analysis takes never shifts
    // the audio that follows (the run is the same every time).
    const auto waitForStep = [&] (Show& show, Music* music, double seconds, Step until)
    {
        show.play (music, seconds, [&] { return loud.isAnalysing() || loud.getStep() == until; });
        for (int i = 0; i < 12000 && loud.isAnalysing(); ++i)
            pump (proc);
    };
    pump (proc);
    check (loud.hasPlan() && ! loud.getInfo().calibrated, "shelves planned; not calibrated yet");
    check (worstMismatchDb (impulseThrough (proc), correctionAndVoicing(), makeupFor (correctionAndVoicing())) < 0.02,
           "uncalibrated: loudness adds nothing");
    {
        SimulatedRoom room (7.0, 21, 3e-4, 0.0);
        Show show { proc, room, 0.0, {}, {}, {} };

        // Mic calibrator: a 1 kHz tone at 0.05 peak (-29.0 dBFS RMS) stands for 94 dB.
        show.micSignal = [] (std::size_t n) { return static_cast<float> (0.05 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * static_cast<double> (n) / fs)); };
        show.play (nullptr, 0.5);   // the calibrator goes on the mic first
        check (loud.startMicCalibration (94.0).wasOk(), "start the mic calibrator (94 dB)");
        waitForStep (show, nullptr, 6.0, Step::idle);
        auto info = loud.getInfo();
        const auto toneDbfs = 20.0 * std::log10 (0.05 / std::sqrt (2.0));
        check (info.hasMicOffset && std::abs ((94.0 - info.micOffsetDb) - toneDbfs) < 0.1,
               "mic calibrated: 94 dB = " + juce::String (94.0 - info.micOffsetDb, 2) + " dBFS (C)");

        // The show view's SPL meter reads the calibrator too (A and C are both 0 dB at 1 kHz).
        auto& spl = proc.getSpl();
        spl.reset();
        show.play (nullptr, 3.0);
        spl.collect();
        const auto fastSpl = spl.fastDb() + info.micOffsetDb, laeq = spl.laeqDb() + info.micOffsetDb,
                   lceq = spl.lceqDb() + info.micOffsetDb;
        check (std::abs (fastSpl - 94.0) < 0.1 && std::abs (laeq - 94.0) < 0.1 && std::abs (lceq - 94.0) < 0.1
                   && spl.secondsHeard() >= 2 && std::abs (spl.maxDb() + info.micOffsetDb - 94.0) < 0.1,
               "the SPL meter reads the calibrator: " + juce::String (fastSpl, 2) + " dB(A) fast, LAeq " + juce::String (laeq, 2)
                   + ", LCeq " + juce::String (lceq, 2));
        show.micSignal = nullptr;

        // Level calibration: noise on both speakers through the EQ; the mic hears the room.
        check (loud.startCalibration().wasOk(), "start the level calibration");
        check (engine.getActivity() == MeasurementEngine::Activity::measuring, "calibration noise playing");
        check (loud.startRecheck().failed() && proc.startMeasurement().failed(), "nothing else starts meanwhile");
        show.rightEnergy = show.channelDiff = 0.0;
        show.play (nullptr, 3.0);
        check (show.rightEnergy > 0.0 && show.channelDiff < 1e-6, "noise on both speakers, the same");
        waitForStep (show, nullptr, 15.0, Step::awaitingSpl);
        info = loud.getInfo();
        check (loud.getStep() == Step::awaitingSpl, "noise measured; waiting for the meter reading");
        check (info.measuredOutputDbfs > -40.0 && info.measuredOutputDbfs < -10.0,
               "output level " + juce::String (info.measuredOutputDbfs, 1) + " dBFS (C)");
        check (info.suggestedSpl.has_value(), "the calibrated mic suggests " + juce::String (info.suggestedSpl.value_or (0.0), 1) + " dB(C)");
        check (loud.setMeasuredSpl (20.0).failed(), "an implausible reading is refused");
        check (loud.setMeasuredSpl (95.0).wasOk(), "meter reading entered (95 dB(C))");
        info = loud.getInfo();
        check (info.calibrated && info.calibration.hasMic && info.canRecheck, "calibrated, with the mic: re-check available");
        const auto calOutput = info.calibration.outputDbfs;
        show.play (nullptr, 3.0, [&] { return engine.getActivity() == MeasurementEngine::Activity::idle; });
        check (engine.getEntries().size() == entries.size(), "calibration isn't filed as a capture");

        // Music about 12 dB below the reference.
        Music music;
        {
            Music probe;
            probe.gain = 1.0;
            std::vector<double> x (static_cast<std::size_t> (8 * fs));
            for (auto& v : x)
                v = probe.next();
            music.gain = std::pow (10.0, (calOutput - 12.0 - roomeq::cWeightedLevelDbfs (x, fs)) / 20.0);
        }
        // The stage's wiring is checked at the fastest Speed (5 s); the default (30 s) comes after.
        // Off: the output is the stage's input, so the level can be checked. (Long enough for the
        // level to come all the way down from the loud pink noise earlier.)
        setParam (proc, "loudSpeed", 5.0f);
        setParam (proc, "loudOn", 0.0f);
        show.play (&music, 28.0);
        const auto expectedSpl = 95.0 + show.outputLevelDbfs() - calOutput;
        check (std::abs (status.splNow.load() - expectedSpl) < 0.5,
               "tracked level " + juce::String (status.splNow.load(), 2) + " dB(C) vs " + juce::String (expectedSpl, 2) + " from the output");
        check (status.lowGainDb.load() <= 0.0f && status.highGainDb.load() <= 0.0f, "off: no boost");
        setParam (proc, "loudOn", 1.0f);
        show.play (&music, 2.0);
        const auto plan = roomeq::planShelves (95.0, fs);
        const auto want = roomeq::shelfGains (plan, status.splUsed.load(), proc.getLoudnessSettings().config);
        check (status.lowGainDb.load() > 4.0 && std::abs (status.lowGainDb.load() - want.first) < 0.01
                   && std::abs (status.highGainDb.load() - want.second) < 0.01,
               "about 12 dB below the reference: low " + juce::String (status.lowGainDb.load(), 2) + " dB, high "
                   + juce::String (status.highGainDb.load(), 2) + " dB (as planned)");
        auto worstEq = loudnessPathError();
        check (worstEq < 0.01, "speakers get correction + voicing + loudness shelves (worst " + juce::String (worstEq, 3) + " dB)");

        // Deadband: +1 dB barely moves the EQ; -5 dB moves it. (The impulse above
        // counted as a quiet moment of music, so let the level settle again first.)
        show.play (&music, 6.0);
        const auto used0 = status.splUsed.load(), now0 = status.splNow.load();
        music.gain *= std::pow (10.0, 1.0 / 20.0);
        show.play (&music, 6.0);
        check (std::abs (status.splNow.load() - (now0 + 1.0f)) < 0.5f && std::abs (status.splUsed.load() - used0) < 0.4f,
               "1 dB louder: level " + juce::String (status.splNow.load() - now0, 2) + " dB, EQ moved "
                   + juce::String (status.splUsed.load() - used0, 2) + " dB");
        music.gain *= std::pow (10.0, -6.0 / 20.0);
        show.play (&music, 20.0);
        check (status.splUsed.load() < used0 - 1.5f && status.splUsed.load() - status.splNow.load() <= 2.01f,
               "5 dB quieter: EQ follows " + juce::String (status.splUsed.load() - used0, 2) + " dB, within 2 dB of the level");
        music.gain *= std::pow (10.0, 5.0 / 20.0);

        // High-pass follows the boost.
        setParam (proc, "loudHighPass", 1.0f);
        show.play (&music, 12.0);
        const auto base = loud.getHighpassBase();
        const auto hp = static_cast<double> (status.hpFreq.load());
        check (hp > base * 1.05 && hp <= base * std::sqrt (2.0) + 0.01,
               "protective high-pass at " + juce::String (hp, 1) + " Hz (roll-off " + juce::String (base, 1) + " Hz)");
        worstEq = loudnessPathError();
        check (worstEq < 0.01, "and the speakers get it (worst " + juce::String (worstEq, 3) + " dB)");
        setParam (proc, "loudHighPass", 0.0f);

        // Measurements step it aside, and the correction and voicing EQ too.
        // A sweep plays exactly as generated from its first sample: the shelves
        // and high-pass go flat at once, under the silence it starts with.
        setParam (proc, "loudHighPass", 1.0f);
        show.play (&music, 3.0);
        check (status.lowGainDb.load() > 0.0f && status.hpFreq.load() > 0.0f, "shelves and high-pass on before measuring");
        const auto sweepSettings = proc.getSweepSettings();
        const auto asGenerated = makeSweepRequest (fs, sweepSettings.seconds, 1, sweepSettings.channel, sweepSettings.levelDbfs)->excitation;
        std::vector<float> played, heardIn;
        show.playedOut = &played;
        show.playedChannel = sweepSettings.channel;
        check (proc.startSweep().wasOk(), "start a sweep");
        show.play (&music, 1.5);
        show.playedOut = nullptr;
        double worstSweep = 0.0;
        for (std::size_t i = 0; i < played.size() && i < asGenerated.size(); ++i)
            worstSweep = std::max (worstSweep, static_cast<double> (std::abs (played[i] - asGenerated[i])));
        check (status.lowGainDb.load() <= 0.0f && status.highGainDb.load() <= 0.0f && worstSweep < 1e-9,
               "loudness is flat while measuring, and the sweep plays exactly as generated from its first sample");
        engine.cancel();
        show.play (&music, 2.0, [&] { return engine.getActivity() == MeasurementEngine::Activity::idle; });

        // Music captures: the correction, voicing EQ and loudness glide out while
        // the music plays on, and recording starts once they have.
        show.play (&music, 3.0);
        played.clear();
        show.playedIn = &heardIn;
        show.playedOut = &played;
        show.playedChannel = 0;
        check (proc.startProgram().wasOk(), "start a music capture");
        show.play (&music, MeasurementEngine::programSettleSeconds + 2.0);
        show.playedIn = show.playedOut = nullptr;
        const auto settled = static_cast<std::size_t> (MeasurementEngine::programSettleSeconds * fs);
        double residual = 0.0, peak = 0.0;
        for (std::size_t i = settled; i < played.size(); ++i)
        {
            residual = std::max (residual, static_cast<double> (std::abs (played[i] - heardIn[i])));
            peak = std::max (peak, static_cast<double> (std::abs (heardIn[i])));
        }
        const auto residualDb = 20.0 * std::log10 (std::max (residual, 1e-12) / peak);
        check (residualDb < -40.0 && proc.getEngine().getProgress() > 0.0f,
               "during a music capture the speakers get the music uncorrected once it has settled (residual "
                   + juce::String (residualDb, 1) + " dB), and recording has started");
        engine.cancel();
        show.play (&music, 2.0, [&] { return engine.getActivity() == MeasurementEngine::Activity::idle; });
        setParam (proc, "loudHighPass", 0.0f);
        show.play (&music, 3.0);
        const auto back = loudnessPathError();
        check (back < 0.01, "afterwards the correction, voicing and loudness are all back (worst " + juce::String (back, 3) + " dB)");

        // The amp gets 4 dB louder after the plugin: re-check finds it from the music.
        show.ampGainDb = 4.0;
        show.play (&music, 8.0);
        const auto before = status.splNow.load();
        check (loud.startRecheck().wasOk(), "start the re-check (music playing)");
        waitForStep (show, &music, 20.0, Step::idle);
        info = loud.getInfo();
        check (std::abs (info.recheckChangeDb - 4.0) < 0.6 && std::abs (info.calibration.outputDbfs - (calOutput - info.recheckChangeDb)) < 1e-9,
               "re-check found the amp change (" + signedText (info.recheckChangeDb) + " dB) and moved the calibration");
        show.play (&music, 8.0);
        check (std::abs (status.splNow.load() - before - static_cast<float> (info.recheckChangeDb)) < 0.5f,
               "tracked level includes it (" + juce::String (status.splNow.load() - before, 2) + " dB)");
        const auto moved = info.calibration.outputDbfs;
        check (loud.startRecheck().wasOk(), "re-check again");
        waitForStep (show, &music, 20.0, Step::idle);
        check (juce::exactlyEqual (loud.getInfo().calibration.outputDbfs, moved) && loud.getStatus().contains ("hasn't changed"),
               "nothing changed: calibration kept (" + loud.getStatus() + ")");

        setParam (proc, "loudOn", 0.0f);
        check (worstMismatchDb (impulseThrough (proc), correctionAndVoicing(), makeupFor (correctionAndVoicing())) < 0.02,
               "loudness off: flat");
        setParam (proc, "loudOn", 1.0f);

        // The default Speed, 30 s: a song's dynamics barely move the EQ, but a loud
        // song after a quiet one is followed within seconds.
        auto* speed = proc.getParameters().getParameter ("loudSpeed");
        check (std::abs (speed->convertFrom0to1 (speed->getDefaultValue()) - 30.0f) < 0.01f
                   && std::abs (speed->convertFrom0to1 (0.0f) - 5.0f) < 0.01f && std::abs (speed->convertFrom0to1 (1.0f) - 60.0f) < 0.01f,
               "Level speed defaults to 30 s, from 5 s to 1 min");
        setParam (proc, "loudSpeed", 30.0f);
        const auto songGain = music.gain;
        float lowest = 1e9f, highest = -1e9f, trackedLow = 1e9f, trackedHigh = -1e9f;
        for (int k = 0; k < 32; ++k)   // 4 s verses and choruses, 6 dB apart; the second minute is measured
        {
            music.gain = songGain * std::pow (10.0, (k % 2 == 0 ? 3.0 : -3.0) / 20.0);
            show.play (&music, 4.0);
            if (k < 16)
                continue;
            lowest = std::min (lowest, status.splUsed.load());
            highest = std::max (highest, status.splUsed.load());
            trackedLow = std::min (trackedLow, status.splNow.load());
            trackedHigh = std::max (trackedHigh, status.splNow.load());
        }
        check (highest - lowest < 0.5f && trackedHigh - trackedLow < 1.5f,
               "a song with 6 dB dynamics: the tracked level moves " + juce::String (trackedHigh - trackedLow, 2) + " dB, the EQ "
                   + juce::String (highest - lowest, 2) + " dB");
        music.gain = songGain * std::pow (10.0, -12.0 / 20.0);   // a ballad...
        show.play (&music, 90.0);
        const auto ballad = status.splNow.load();
        music.gain = songGain;                                   // ...then the band comes back in
        show.play (&music, 6.0);
        check (status.splNow.load() > ballad + 9.0f,
               "a loud song after a ballad is followed within seconds (+" + juce::String (status.splNow.load() - ballad, 1)
                   + " dB of 12 in 6 s)");
        show.play (&music, 3.0);   // boosting again, for the screenshot
    }

    std::cout << "State round trip\n";
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        AdaptiveRoomEQProcessor restored;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        waitForDisplay (restored, [] (const MeasurementEngine::Display& x) { return x.proposal.has_value(); });
        std::cout << "    summary and fit after restore: " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)
                  << " ms\n";
        const auto again = restored.getEngine().getEntries();
        check (again.size() == entries.size(), "captures restored");
        auto same = again.size() == entries.size();
        for (std::size_t i = 0; same && i < again.size(); ++i)
            same = again[i].capture->name == entries[i].capture->name
                   && again[i].capture->grade.reasons == entries[i].capture->grade.reasons
                   && again[i].capture->excluded == (i == 4);
        check (same, "names, reasons and excluded flags restored");
        check (restored.getEngine().getDisplay() != nullptr, "average recomputed after restore");
        auto verifySame = true;
        for (std::size_t i = 0; verifySame && i < again.size() && i < entries.size(); ++i)
            verifySame = again[i].verify == entries[i].verify && again[i].correctionId == entries[i].correctionId;
        check (verifySame, "verify captures restored");
        check (restored.getEngine().getApplied() == engine.getApplied()
                   && restored.getEngine().getAppliedId() == engine.getAppliedId() && restored.getEngine().hasPrevious(),
               "applied and previous corrections restored");
        check (restored.getCustomTarget() == proc.getCustomTarget(), "custom target restored");
        const auto was = proc.getLoudness().getInfo(), now = restored.getLoudness().getInfo();
        check (now.calibrated && juce::exactlyEqual (now.calibration.outputDbfs, was.calibration.outputDbfs)
                   && juce::exactlyEqual (now.calibration.spl, was.calibration.spl) && now.canRecheck && now.hasMicOffset
                   && juce::exactlyEqual (now.micOffsetDb, was.micOffsetDb),
               "loudness calibration restored");
        restored.setRateAndBufferSizeDetails (fs, blockSize);
        restored.prepareToPlay (fs, blockSize);
        auto restoredEq = restored.getEngine().getApplied();
        for (const auto& b : restored.getVoicingSections())
            restoredEq.push_back (b);
        check (restoredEq.size() > engine.getApplied().size()
                   && worstMismatchDb (impulseThrough (restored), restoredEq, makeupFor (restoredEq)) < 0.02,
               "restored plugin plays the same correction and voicing");
        std::cout << "    state size " << state.getSize() / 1024 << " KB\n";
    }

    std::cout << "Rendering the editor\n";
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ours = dynamic_cast<AdaptiveRoomEQEditor*> (editor.get());
        editor->setSize (1200, 820);
        for (int i = 0; i < 20; ++i)
            pump (proc);
        const auto stem = outPath.upToLastOccurrenceOf (".", false, false);
        writeSnapshot (*editor, outPath);
        ours->showTab (AdaptiveRoomEQEditor::Tab::correct);
        writeSnapshot (*editor, stem + "-correct.png");
        ours->selectVoicingBand (1);
        ours->showTab (AdaptiveRoomEQEditor::Tab::voicing);
        writeSnapshot (*editor, stem + "-voicing.png");
        ours->showTab (AdaptiveRoomEQEditor::Tab::loudness);
        writeSnapshot (*editor, stem + "-loudness.png");
        ours->setShowView (true);
        for (int i = 0; i < 5; ++i)
            pump (proc);
        check (ours->isShowView() && proc.isShowView(), "show view");
        {
            // The mic's spectrogram: music through a room, then a 1 kHz tone at the mic.
            ResponseGraph* showGraph = nullptr;
            for (auto* c : editor->getChildren())
                if (auto* gr = dynamic_cast<ResponseGraph*> (c))
                    showGraph = gr;
            auto& spectrogram = showGraph->getSpectrogram();
            SimulatedRoom room (7.0, 41, 1e-3, 0.0);
            Show show { proc, room, 0.0, {}, {}, {} };
            Music music;
            music.gain = 0.03;
            show.micSignal = [] (std::size_t n) { return static_cast<float> (0.05 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * static_cast<double> (n) / fs)); };
            show.play (nullptr, 1.0);
            spectrogram.update();
            check (std::abs (spectrogram.getNewestPeakHz() / 1000.0 - 1.0) < 0.05,
                   "a 1 kHz tone at the mic shows at " + juce::String (spectrogram.getNewestPeakHz(), 0) + " Hz");
            show.micSignal = {};
            const auto before = spectrogram.getFramesAnalysed();
            for (int s = 0; s < 44; ++s)   // the UI's timer takes what's new 30 times a second; every 0.5 s here
            {
                show.play (&music, 0.5);
                spectrogram.update();
            }
            const auto frames = spectrogram.getFramesAnalysed() - before;
            check (frames > 20 * 20, "22 s of music: " + juce::String (frames) + " spectrogram frames");
            proc.getSpl().collect();
        }
        {
            // The spectrogram/EQ panel: its hover readout, dragging over it, and its switch.
            using Panel = AdaptiveRoomEQProcessor::ShowPanel;
            ResponseGraph* showGraph = nullptr;
            for (auto* c : editor->getChildren())
                if (auto* gr = dynamic_cast<ResponseGraph*> (c))
                    showGraph = gr;
            check (proc.getShowPanel() == Panel::both, "the spectrogram with the EQ over it by default");
            const auto panelHover = showGraph->showHoverText (showGraph->getShowPanelPoint (1000.0));
            check (panelHover.contains ("correction") && panelHover.contains ("voicing"), "hovering the panel: " + panelHover);

            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto event = [&] (juce::Point<float> at, juce::Point<float> down, bool dragged)
            {
                const auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent (source, at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, showGraph, showGraph, now, down, now, 1, dragged);
            };
            const auto dragBand = [&] (int band, juce::Point<float> by)
            {
                const auto from = showGraph->getVoicingHandlePosition (band), to = from + by;
                showGraph->mouseDown (event (from, from, false));
                showGraph->mouseDrag (event ((from + to) / 2.0f, from, true));
                showGraph->mouseDrag (event (to, from, true));
                showGraph->mouseUp (event (to, from, true));
            };
            const auto freqBefore = proc.getEqSettings().voicing[5].freq, gainBefore = proc.getEqSettings().voicing[5].gainDb;
            dragBand (5, { -40.0f, 20.0f });
            const auto moved = proc.getEqSettings().voicing[5];
            check (moved.freq < freqBefore * 0.95 && moved.gainDb < gainBefore - 0.5,
                   "a voicing handle drags over the spectrogram (band 6 to " + juce::String (moved.freq, 0) + " Hz, "
                       + juce::String (moved.gainDb, 1) + " dB)");
            proc.setShowPanel (Panel::spectrogram);
            showGraph->showPanelChanged();
            dragBand (5, { -40.0f, 20.0f });
            check (proc.getEqSettings().voicing[5] == moved && ! showGraph->showHoverText (showGraph->getShowPanelPoint (1000.0)).contains ("correction"),
                   "spectrogram only: the handles step aside");
            writeSnapshot (*editor, stem + "-show-spectrogram.png");
            proc.setShowPanel (Panel::eq);
            showGraph->showPanelChanged();
            writeSnapshot (*editor, stem + "-show-eq.png");
            juce::MemoryBlock saved;
            proc.getStateInformation (saved);
            AdaptiveRoomEQProcessor restored;
            restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
            check (restored.getShowPanel() == Panel::eq, "the choice is saved with the session");
            setParam (proc, "v6Freq", static_cast<float> (freqBefore));
            setParam (proc, "v6Gain", static_cast<float> (gainBefore));
            proc.setShowPanel (Panel::both);
            showGraph->showPanelChanged();
            for (int i = 0; i < 3; ++i)
                pump (proc);
            const auto over200 = showGraph->getShowPanelPoint (200.0);
            showGraph->mouseMove (event (over200, over200, false));   // the snapshot shows the readout
            writeSnapshot (*editor, stem + "-show.png");
            showGraph->mouseExit (event (over200, over200, false));
        }
        editor->setSize (1060, 740);
        writeSnapshot (*editor, stem + "-show-small.png");
        editor->setSize (1200, 820);
        ours->setShowView (false);
        editor->setSize (1060, 740);   // the smallest size: everything still fits
        writeSnapshot (*editor, stem + "-loudness-small.png");
        ours->showTab (AdaptiveRoomEQEditor::Tab::correct);
        writeSnapshot (*editor, stem + "-correct-small.png");
        ours->showTab (AdaptiveRoomEQEditor::Tab::measure);
        writeSnapshot (*editor, stem + "-measure-small.png");
        editor->setSize (1200, 820);

        std::cout << "Editing on the graph\n";
        ResponseGraph* graph = nullptr;
        for (auto* c : editor->getChildren())
            if (auto* gr = dynamic_cast<ResponseGraph*> (c))
                graph = gr;
        check (graph != nullptr, "graph found");
        if (graph != nullptr)
        {
            // Selecting a capture highlights its curve: a sweep position, then a verify capture.
            const auto all = engine.getEntries();
            const auto firstVerify = std::find_if (all.begin(), all.end(), [] (const auto& e) { return e.verify; });
            ours->showTab (AdaptiveRoomEQEditor::Tab::measure);
            graph->setData (engine.getDisplay(), all[1].id);
            check (graph->isHighlighting(), "selecting a position highlights its curve");
            writeSnapshot (*editor, stem + "-selected.png");
            graph->setData (engine.getDisplay(), firstVerify != all.end() ? firstVerify->id : -1);
            check (graph->isHighlighting(), "and a verify capture's too");
            graph->setData (engine.getDisplay(), -1);
            check (! graph->isHighlighting(), "no selection, no highlight");

            auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto event = [&] (juce::Point<float> at, juce::Point<float> down, int clicks, bool dragged)
            {
                const auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent (source, at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, graph, graph, now, down, now, clicks, dragged);
            };
            const auto drag = [&] (juce::Point<float> from, juce::Point<float> to)
            {
                graph->mouseDown (event (from, from, 1, false));
                graph->mouseDrag (event ((from + to) / 2.0f, from, 1, true));
                graph->mouseDrag (event (to, from, 1, true));
                graph->mouseUp (event (to, from, 1, true));
            };

            // Band 5 (a bell at 1.5 kHz): drag it right and up.
            setParam (proc, "v5On", 1.0f);
            graph->refresh();
            const auto h = graph->getVoicingHandlePosition (4);
            drag (h, h + juce::Point<float> (60.0f, -25.0f));
            const auto v = proc.getEqSettings().voicing[4];
            check (v.freq > 1600.0 && v.gainDb > 1.0, "dragging a voicing handle sets frequency (" + juce::String (v.freq, 0)
                                                           + " Hz) and gain (" + juce::String (v.gainDb, 1) + " dB)");
            graph->mouseDoubleClick (event (graph->getVoicingHandlePosition (4), graph->getVoicingHandlePosition (4), 2, false));
            check (! proc.getEqSettings().voicing[4].on, "double-click switches the band off");

            // Custom target: drag its second point down 3 dB-ish; double-click adds a point.
            proc.setCustomTarget ({ "Custom", { { 50.0, 3.0 }, { 1000.0, 0.0 }, { 10000.0, -2.0 } } });
            setParam (proc, "target", static_cast<float> (AdaptiveRoomEQProcessor::targetCustom));
            waitForDisplay (proc, [] (const MeasurementEngine::Display&) { return true; });
            graph->setData (proc.getEngine().getDisplay(), -1);
            const auto p1 = graph->getTargetPointPosition (1);
            drag (p1, p1 + juce::Point<float> (0.0f, 30.0f));
            const auto moved = proc.getCustomTarget();
            check (moved.points.size() == 3 && moved.points[1].second < -1.0 && std::abs (moved.points[1].first - 1000.0) < 50.0,
                   "dragging a target point moves it (" + juce::String (moved.points[1].second, 1) + " dB)");
            const auto emptySpot = graph->getTargetPointPosition (1) + juce::Point<float> (120.0f, 0.0f);
            graph->mouseDoubleClick (event (emptySpot, emptySpot, 2, false));
            check (proc.getCustomTarget().points.size() == 4, "double-click adds a target point");

            // Save it, load it back.
            check (proc.saveCustomTarget ("Harness test target").wasOk(), "custom target saved");
            const auto file = AdaptiveRoomEQProcessor::getTargetsFolder().getChildFile ("Harness test target.json");
            const auto saved = proc.getCustomTarget();
            check (std::abs (saved.points[1].second - std::round (saved.points[1].second * 100.0) / 100.0) < 1e-12,
                   "saved at 0.01 dB");
            proc.setCustomTarget ({ "Custom", { { 1000.0, 0.0 } } });
            check (proc.loadTarget (file).wasOk(), "target file loads");
            check (proc.getCustomTarget().points == saved.points && proc.getCustomTarget().name == "Harness test target",
                   "and gives back exactly the saved points");
            file.deleteFile();
            setParam (proc, "target", 0.0f);
        }
    }

    std::cout << "Clear all\n";
    {
        idleWithMic (proc, 3e-4f);
        check (! engine.getEntries().empty() && ! engine.getApplied().empty() && proc.getLoudness().getInfo().calibrated,
               "before: measurements, a correction and a level calibration");
        check (proc.startSweep().wasOk(), "start a sweep");
        check (proc.clearRoomData().failed(), "clearing waits while a measurement runs");
        engine.cancel();
        for (int i = 0; i < 20 && engine.getActivity() != MeasurementEngine::Activity::idle; ++i)
        {
            idleWithMic (proc, 3e-4f);
            pump (proc);
        }
        const auto micKept = proc.getLoudness().getInfo().hasMicOffset;
        const auto voicing = proc.getVoicingSections();
        check (proc.clearRoomData().wasOk(), "clear all");
        for (int i = 0; i < 12000 && ! engine.isDisplayCurrent(); ++i)   // the display catches up
            pump (proc);
        const auto shown = engine.getDisplay();
        const auto li = proc.getLoudness().getInfo();
        check (engine.getEntries().empty() && engine.getApplied().empty() && ! engine.hasPrevious() && ! li.calibrated
                   && li.hasMicOffset == micKept && (shown == nullptr || ! shown->proposal),
               "no measurements, corrections or level calibration; the mic calibration stays");
        check (proc.getVoicingSections() == voicing && worstMismatchDb (impulseThrough (proc), voicing, makeupFor (voicing)) < 0.02,
               "the speakers get just the voicing EQ: no correction, loudness flat");
        juce::MemoryBlock cleared;
        proc.getStateInformation (cleared);
        AdaptiveRoomEQProcessor restored;
        restored.setStateInformation (cleared.getData(), static_cast<int> (cleared.getSize()));
        check (restored.getEngine().getEntries().empty() && restored.getEngine().getApplied().empty()
                   && ! restored.getLoudness().getInfo().calibrated,
               "and the saved session stays cleared");
    }

    zoneChecks (outPath.upToLastOccurrenceOf (".", false, false));
    clockDriftChecks (outPath.upToLastOccurrenceOf (".", false, false) + "-drift.png");
    standaloneChecks (outPath.upToLastOccurrenceOf (".", false, false) + "-standalone.png");

    std::cout << (failures == 0 ? "All checks passed\n" : juce::String (failures) + " check(s) failed\n");
    return failures == 0 ? 0 : 1;
}
