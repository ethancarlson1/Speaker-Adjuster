#include "plugin/LoudnessController.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace
{
namespace ids
{
const juce::Identifier version { "version" }, calibrated { "calibrated" }, outputDbfs { "outputDbfs" }, spl { "spl" };
const juce::Identifier hasMic { "hasMic" }, micDbfs { "micDbfs" }, calibratedAt { "calibratedAt" }, bands { "bands" };
const juce::Identifier hasMicOffset { "hasMicOffset" }, micOffsetDb { "micOffsetDb" }, calibratorSpl { "calibratorSpl" };
const juce::Identifier micCalibratedAt { "micCalibratedAt" }, recheckedAt { "recheckedAt" };
const juce::Identifier recheckChangeDb { "recheckChangeDb" };
} // namespace ids

constexpr double silentDbfs = -90.0;   // C-weighted level below which nothing was heard

bool same (double a, double b) noexcept { return std::equal_to<double>() (a, b); }

juce::String db (double v)
{
    const auto r = std::round (v * 10.0) / 10.0;   // no "-0.0"
    if (std::abs (r) < 0.05)
        return "0.0";
    return (r > 0.0 ? "+" : "") + juce::String (r, 1);
}

juce::MemoryBlock pack (const std::vector<double>& values)
{
    juce::MemoryBlock block (values.size() * sizeof (float));
    auto* out = static_cast<float*> (block.getData());
    for (std::size_t i = 0; i < values.size(); ++i)
        out[i] = static_cast<float> (values[i]);
    return block;
}

std::vector<double> unpack (const juce::var& v)
{
    std::vector<double> values;
    if (const auto* block = v.getBinaryData())
    {
        const auto* in = static_cast<const float*> (block->getData());
        values.assign (in, in + block->getSize() / sizeof (float));
    }
    return values;
}

double finiteOr (const juce::var& v, double fallback, double lo, double hi)
{
    const auto x = static_cast<double> (v);
    return std::isfinite (x) && x >= lo && x <= hi ? x : fallback;
}

int finiteCount (const std::vector<double>& v)
{
    return static_cast<int> (std::count_if (v.begin(), v.end(), [] (double x) { return std::isfinite (x); }));
}
} // namespace

LoudnessController::LoudnessController (LoudnessStage& stageToUse, MeasurementEngine& engineToUse)
    : stage (stageToUse), engine (engineToUse)
{
    startTimerHz (20);
}

LoudnessController::~LoudnessController()
{
    stopTimer();
    pool.removeAllJobs (true, 10000);
}

LoudnessController::Environment LoudnessController::environment() const
{
    return environmentSource ? environmentSource() : Environment {};
}

juce::Result LoudnessController::startTap (Step newStep, double skipSeconds, double recordSeconds, bool withMic)
{
    const auto fs = environment().fs;
    auto tap = std::make_unique<TapRequest>();
    const auto n = static_cast<std::size_t> (recordSeconds * fs);
    tap->output.assign (n, 0.0f);
    if (withMic)
        tap->mic.assign (n, 0.0f);
    tap->skip = static_cast<std::size_t> (skipSeconds * fs);
    if (! stage.startTap (std::move (tap)))
        return juce::Result::fail ("The loudness stage is still recording");
    tapFs = fs;
    step = newStep;
    analysing = false;
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result LoudnessController::startCalibration()
{
    const auto env = environment();
    if (! env.available)
        return juce::Result::fail ("Loudness compensation runs in the plugin inside your DAW, so calibrate it there.");
    if (env.fs <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (step != Step::idle && step != Step::awaitingSpl)
        return juce::Result::fail ("Wait for the current step to finish, or stop it");
    if (engine.getActivity() == MeasurementEngine::Activity::measuring)
        return juce::Result::fail ("A measurement is running");
    if (stage.isTapBusy())
        return juce::Result::fail ("The loudness stage is still recording");
    if (auto r = engine.startCalibration (env.fs, calibrationNoiseSeconds, env.signalLevelDbfs); r.failed())
        return r;
    startTap (Step::calibrating, calibrationSkipSeconds, calibrationRecordSeconds, env.micConnected && env.micSignal);
    status = "Calibrating: read your SPL meter (C-weighted, slow) at the mix position while the noise plays";
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result LoudnessController::setMeasuredSpl (double spl)
{
    if (step != Step::awaitingSpl)
        return juce::Result::fail ("Play the calibration noise first");
    if (! std::isfinite (spl) || spl < 40.0 || spl > 130.0)
        return juce::Result::fail ("Enter the level your meter read, in dB(C), between 40 and 130");
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        calibration = { pending.outputDbfs, spl, pending.micHeard, pending.micHeard ? pending.micDbfs : 0.0 };
        calibrationBands = pending.micHeard ? pending.bands : std::vector<double> {};
        calibrated = true;
        calibratedAt = juce::Time::currentTimeMillis();
        recheckedAt = 0;
        recheckChangeDb = 0.0;
    }
    step = Step::idle;
    status = "Calibrated: " + juce::String (spl, 1) + " dB(C) at " + juce::String (pending.outputDbfs, 1)
             + " dBFS (C) at the plugin output";
    publishModel();
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result LoudnessController::startMicCalibration (double splOfCalibrator)
{
    const auto env = environment();
    if (! env.available)
        return juce::Result::fail ("Loudness compensation runs in the plugin inside your DAW, so calibrate it there.");
    if (env.fs <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (! env.micConnected || ! env.micSignal)
        return juce::Result::fail ("There's no signal on the mic input. Route the measurement mic to the plugin's sidechain "
                                   "input, and check its gain and phantom power, then try again.");
    if (step != Step::idle && step != Step::awaitingSpl)
        return juce::Result::fail ("Wait for the current step to finish, or stop it");
    if (! std::isfinite (splOfCalibrator) || splOfCalibrator < 80.0 || splOfCalibrator > 130.0)
        return juce::Result::fail ("The calibrator level should be 94 or 114 dB");
    if (engine.getActivity() == MeasurementEngine::Activity::measuring)
        return juce::Result::fail ("Wait for the measurement or calibration noise to finish");
    requestedCalibratorSpl = splOfCalibrator;
    resumeAwaiting = step == Step::awaitingSpl;
    if (auto r = startTap (Step::micCalibrating, 0.5, micCalibrationSeconds, true); r.failed())
        return r;
    status = "Calibrating the mic: keep the calibrator running";
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result LoudnessController::startRecheck()
{
    const auto env = environment();
    if (! env.available)
        return juce::Result::fail ("Loudness compensation runs in the plugin inside your DAW");
    if (! getInfo().canRecheck)
        return juce::Result::fail ("Calibrate with the mic connected first: the re-check compares what the mic hears "
                                   "now with what it heard then");
    if (! env.micConnected || ! env.micSignal)
        return juce::Result::fail ("There's no signal on the mic input. Route the measurement mic to the plugin's sidechain "
                                   "input, and check its gain and phantom power, then try again.");
    if (env.fs <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (step != Step::idle)
        return juce::Result::fail ("Wait for the current step to finish, or stop it");
    if (engine.getActivity() == MeasurementEngine::Activity::measuring)
        return juce::Result::fail ("A measurement is running");
    if (auto r = startTap (Step::rechecking, 0.0, recheckSeconds, true); r.failed())
        return r;
    status = "Re-checking the level from the music...";
    sendChangeMessage();
    return juce::Result::ok();
}

void LoudnessController::cancel()
{
    switch (step)
    {
        case Step::idle:
            return;
        case Step::awaitingSpl:
            step = Step::idle;
            status = "Calibration cancelled; the previous calibration is kept";
            break;
        case Step::calibrating:
        case Step::micCalibrating:
        case Step::rechecking:
            if (step == Step::calibrating && engine.getActivity() == MeasurementEngine::Activity::measuring)
                engine.cancel();
            stopReason = {};
            if (stage.isTapBusy())
            {
                stage.cancelTap();   // the step ends when the stopped tap comes back
            }
            else
            {
                // Analysing: its result will be ignored.
                step = resumeAwaiting && step == Step::micCalibrating ? Step::awaitingSpl : Step::idle;
                analysing = false;
                status = "Stopped";
            }
            break;
    }
    sendChangeMessage();
}

float LoudnessController::getProgress() const
{
    return stage.isTapBusy() ? stage.getTapProgress() : 0.0f;
}

LoudnessController::Info LoudnessController::getInfo() const
{
    Info i;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        i.calibrated = calibrated;
        i.calibration = calibration;
        i.calibratedAt = calibratedAt;
        i.canRecheck = calibrated && calibration.hasMic && finiteCount (calibrationBands) >= 6;
        i.hasMicOffset = hasMicOffset;
        i.micOffsetDb = micOffsetDb;
        i.calibratorSpl = calibratorSpl;
        i.micCalibratedAt = micCalibratedAt;
        i.recheckedAt = recheckedAt;
        i.recheckChangeDb = recheckChangeDb;
    }
    if (step == Step::awaitingSpl || (step == Step::micCalibrating && resumeAwaiting))
    {
        i.measuredOutputDbfs = pending.outputDbfs;
        if (pending.micHeard && i.hasMicOffset)
            i.suggestedSpl = pending.micDbfs + i.micOffsetDb;
    }
    return i;
}

void LoudnessController::publishModel()
{
    modelDirty = false;
    LoudnessModel m;
    m.plan = plan;
    m.hasPlan = planned;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        m.calibration = calibration;
        m.calibrated = calibrated;
    }
    stage.setModel (m);
}

void LoudnessController::update()
{
    const auto env = environment();
    auto changed = false;

    if (env.fs > 0.0 && (! planned || ! same (env.fs, planFs) || ! same (env.referenceSpl, planRef)))
    {
        plan = roomeq::planShelves (env.referenceSpl, env.fs);   // a few ms
        planned = true;
        planFs = env.fs;
        planRef = env.referenceSpl;
        modelDirty = true;
    }

    auto base = 40.0;
    if (const auto d = engine.getDisplay(); d != nullptr && d->proposal)
        base = juce::jlimit (20.0, 200.0, d->proposal->fitRange.first);
    hpBase.store (base, std::memory_order_relaxed);

    // Stop recording if what's being recorded changed under it.
    if (stage.isTapBusy())
    {
        const auto measuring = engine.getActivity() == MeasurementEngine::Activity::measuring;
        if (step == Step::calibrating && ! measuring)
        {
            stopReason = "Calibration stopped: the noise stopped early";
            stage.cancelTap();
        }
        else if (step == Step::rechecking && measuring)
        {
            stopReason = "Re-check stopped: a measurement started";
            stage.cancelTap();
        }
        else if (step == Step::micCalibrating && measuring)
        {
            stopReason = "Mic calibration stopped: a measurement started";
            stage.cancelTap();
        }
    }
    if (auto tap = stage.collectTap())
    {
        tapFinished (std::move (tap));
        changed = true;
    }

    std::vector<Analysis> results;
    {
        const std::lock_guard<std::mutex> guard (mailbox->lock);
        results.swap (mailbox->results);
    }
    for (const auto& a : results)
    {
        handle (a);
        changed = true;
    }

    if (modelDirty.load())
        publishModel();
    if (changed)
        sendChangeMessage();
}

void LoudnessController::tapFinished (std::unique_ptr<TapRequest> tap)
{
    if (tap->cancelled)
    {
        if (stopReason.isNotEmpty())
            status = stopReason;
        else if (step == Step::calibrating)
            status = "Calibration stopped; the previous calibration is kept";
        else if (step == Step::micCalibrating)
            status = "Mic calibration stopped";
        else
            status = "Re-check stopped";
        stopReason = {};
        step = step == Step::micCalibrating && resumeAwaiting ? Step::awaitingSpl : Step::idle;
        return;
    }

    if (step == Step::micCalibrating)
    {
        const std::vector<double> mic (tap->mic.begin(), tap->mic.end());
        const auto level = roomeq::cWeightedLevelDbfs (mic, tapFs);
        const auto peak = std::accumulate (tap->mic.begin(), tap->mic.end(), 0.0f,
                                           [] (float m, float v) { return std::max (m, std::abs (v)); });
        if (! (level > silentDbfs))
        {
            status = "The mic heard nothing: check the calibrator is on it and the mic input is routed to the plugin";
        }
        else if (peak >= 0.999f)
        {
            status = "The mic input is clipping: turn its gain down, then calibrate it again";
        }
        else
        {
            const std::lock_guard<std::mutex> guard (stateLock);
            hasMicOffset = true;
            calibratorSpl = requestedCalibratorSpl;
            micOffsetDb = calibratorSpl - level;
            micCalibratedAt = juce::Time::currentTimeMillis();
            status = "Mic calibrated: " + juce::String (calibratorSpl, 0) + " dB = " + juce::String (level, 1) + " dBFS (C)";
        }
        step = resumeAwaiting ? Step::awaitingSpl : Step::idle;
        return;
    }

    // Calibration and re-check: analyse in the background.
    analysing = true;
    status = step == Step::calibrating ? "Analysing the calibration..." : "Analysing the re-check...";
    std::shared_ptr<TapRequest> t = std::move (tap);
    pool.addJob ([mb = mailbox, t, fs = tapFs, s = step]
    {
        Analysis a;
        a.step = s;
        const std::vector<double> out (t->output.begin(), t->output.end());
        a.outputDbfs = roomeq::cWeightedLevelDbfs (out, fs);
        if (! t->mic.empty())
        {
            const std::vector<double> mic (t->mic.begin(), t->mic.end());
            a.micDbfs = roomeq::cWeightedLevelDbfs (mic, fs);
            a.micHeard = a.micDbfs > silentDbfs;
            if (a.micHeard)
                a.bands = roomeq::transferBandsDb (out, mic, fs);
        }
        const std::lock_guard<std::mutex> guard (mb->lock);
        mb->results.push_back (std::move (a));
    });
}

void LoudnessController::handle (const Analysis& a)
{
    if (a.step != step || ! analysing)
        return;   // stopped meanwhile
    analysing = false;

    if (a.step == Step::calibrating)
    {
        if (! (a.outputDbfs > silentDbfs))
        {
            step = Step::idle;
            status = "The calibration noise didn't reach the plugin output; the previous calibration is kept";
            return;
        }
        pending = a;
        step = Step::awaitingSpl;
        status = "Enter the level your meter read";
        if (! a.micHeard && environment().micConnected)
            status += " (the mic heard nothing, so mic tracking and the re-check won't be available)";
        return;
    }

    // Re-check.
    step = Step::idle;
    std::vector<double> bands;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        bands = calibrationBands;
    }
    const auto change = a.micHeard ? roomeq::recheckGainChange (bands, a.bands) : std::nullopt;
    if (! a.micHeard)
    {
        status = "Re-check: the mic heard nothing; the calibration is unchanged";
    }
    else if (! change)
    {
        status = "Re-check: not enough of the music reached the mic clearly (it needs ~10 s of full-range music, "
                 "loud enough over the crowd); the calibration is unchanged";
    }
    else if (std::abs (*change) < minRecheckChangeDb)
    {
        status = "Re-check: the level after the plugin hasn't changed (" + db (*change) + " dB); calibration kept";
    }
    else
    {
        {
            const std::lock_guard<std::mutex> guard (stateLock);
            calibration = roomeq::recalibrated (calibration, *change);
            for (auto& b : calibrationBands)
                b += *change;
            recheckedAt = juce::Time::currentTimeMillis();
            recheckChangeDb = *change;
        }
        status = "Re-check: the system is " + juce::String (std::abs (*change), 1) + " dB "
                 + (*change > 0.0 ? "louder" : "quieter") + " after the plugin than at calibration; calibration updated";
        modelDirty = true;
    }
}

juce::ValueTree LoudnessController::toValueTree() const
{
    juce::ValueTree t (treeType);
    const std::lock_guard<std::mutex> guard (stateLock);
    t.setProperty (ids::version, 1, nullptr);
    t.setProperty (ids::calibrated, calibrated, nullptr);
    t.setProperty (ids::outputDbfs, calibration.outputDbfs, nullptr);
    t.setProperty (ids::spl, calibration.spl, nullptr);
    t.setProperty (ids::hasMic, calibration.hasMic, nullptr);
    t.setProperty (ids::micDbfs, calibration.micDbfs, nullptr);
    t.setProperty (ids::calibratedAt, calibratedAt, nullptr);
    t.setProperty (ids::bands, pack (calibrationBands), nullptr);
    t.setProperty (ids::hasMicOffset, hasMicOffset, nullptr);
    t.setProperty (ids::micOffsetDb, micOffsetDb, nullptr);
    t.setProperty (ids::calibratorSpl, calibratorSpl, nullptr);
    t.setProperty (ids::micCalibratedAt, micCalibratedAt, nullptr);
    t.setProperty (ids::recheckedAt, recheckedAt, nullptr);
    t.setProperty (ids::recheckChangeDb, recheckChangeDb, nullptr);
    return t;
}

void LoudnessController::fromValueTree (const juce::ValueTree& t)
{
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        // A session without a calibration (or from before Phase 3) starts uncalibrated.
        calibration = {};
        calibrated = false;
        calibrationBands.clear();
        calibratedAt = micCalibratedAt = recheckedAt = 0;
        hasMicOffset = false;
        micOffsetDb = recheckChangeDb = 0.0;
        calibratorSpl = 94.0;
        if (t.hasType (treeType))
        {
            calibration.outputDbfs = finiteOr (t[ids::outputDbfs], 0.0, -200.0, 50.0);
            calibration.spl = finiteOr (t[ids::spl], 0.0, 40.0, 130.0);
            calibrated = static_cast<bool> (t[ids::calibrated]) && calibration.spl >= 40.0;
            calibration.micDbfs = finiteOr (t[ids::micDbfs], 0.0, -200.0, 50.0);
            calibration.hasMic = calibrated && static_cast<bool> (t[ids::hasMic]);
            calibratedAt = static_cast<juce::int64> (t[ids::calibratedAt]);
            calibrationBands = unpack (t[ids::bands]);
            if (calibrationBands.size() != roomeq::recheckBands().size())
                calibrationBands.clear();
            micOffsetDb = finiteOr (t[ids::micOffsetDb], 0.0, -100.0, 300.0);
            hasMicOffset = static_cast<bool> (t[ids::hasMicOffset]);
            calibratorSpl = finiteOr (t[ids::calibratorSpl], 94.0, 80.0, 130.0);
            micCalibratedAt = static_cast<juce::int64> (t[ids::micCalibratedAt]);
            recheckedAt = static_cast<juce::int64> (t[ids::recheckedAt]);
            recheckChangeDb = finiteOr (t[ids::recheckChangeDb], 0.0, -60.0, 60.0);
        }
    }
    modelDirty = true;   // published on the message thread
}
