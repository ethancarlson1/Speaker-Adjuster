#include "plugin/MeasurementEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
namespace ids
{
const juce::Identifier capture { "Capture" }, band { "Band" };
const juce::Identifier version { "version" }, smoothing { "smoothing" }, nextNumber { "nextNumber" };
const juce::Identifier id { "id" }, name { "name" }, kind { "kind" }, fs { "fs" }, excluded { "excluded" };
const juce::Identifier overall { "overall" }, reasons { "reasons" }, notes { "notes" }, delays { "delaysMs" };
const juce::Identifier power { "power" }, noise { "noise" }, weight { "weight" };
const juce::Identifier center { "center" }, lo { "lo" }, hi { "hi" }, level { "level" }, snr { "snr" };
const juce::Identifier spread { "spread" }, excess { "excess" }, outOfRange { "outOfRange" };
const juce::Identifier snrGrade { "snrGrade" }, consistencyGrade { "consistencyGrade" };
const juce::Identifier verify { "verify" }, correctionId { "correctionId" }, nextVerifyNumber { "nextVerifyNumber" };
const juce::Identifier applied { "Applied" }, previous { "Previous" }, appliedId { "appliedId" };
const juce::Identifier nextCorrectionId { "nextCorrectionId" }, correctionBand { "CorrectionBand" };
const juce::Identifier freq { "freq" }, gain { "gain" }, q { "q" };
const juce::Identifier arrivalMs { "arrivalMs" }, arrivalConfidence { "arrivalConfidence" }, arrivalReasons { "arrivalReasons" };
const juce::Identifier systemLatencyMs { "systemLatencyMs" };
} // namespace ids

// Spectra are stored as float32: plenty for dB-domain data, half the size.
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

juce::String joinLines (const std::vector<std::string>& lines)
{
    juce::StringArray a;
    for (const auto& l : lines)
        a.add (juce::String::fromUTF8 (l.c_str()));
    return a.joinIntoString ("\n");
}

std::vector<std::string> splitLines (const juce::String& s)
{
    std::vector<std::string> out;
    for (const auto& line : juce::StringArray::fromLines (s))
        if (line.isNotEmpty())
            out.push_back (line.toStdString());
    return out;
}

juce::String captureNumberName (int n, bool verify)
{
    return (verify ? "V" : "P") + juce::String (n);
}

bool same (double a, double b) { return std::equal_to<double>() (a, b); }

bool sameBand (const roomeq::CorrectionConfig& x, const roomeq::CorrectionConfig& y)
{
    return same (x.refBandLoHz, y.refBandLoHz) && same (x.refBandHiHz, y.refBandHiHz);
}

bool sameSettings (const MeasurementEngine::CorrectionSettings& a, const MeasurementEngine::CorrectionSettings& b)
{
    const auto& x = a.config;
    const auto& y = b.config;
    return a.target == b.target && same (a.fs, b.fs) && same (x.maxCutDb, y.maxCutDb) && same (x.maxBoostDb, y.maxBoostDb)
           && same (x.rangeLoHz, y.rangeLoHz) && same (x.rangeHiHz, y.rangeHiHz) && sameBand (x, y);
}

roomeq::GradingConfig gradingFor (const roomeq::CorrectionConfig& c)
{
    roomeq::GradingConfig g;
    g.passbandLo = c.refBandLoHz;
    g.passbandHi = c.refBandHiHz;
    return g;
}

juce::ValueTree bandsToTree (const juce::Identifier& type, const std::vector<roomeq::Band>& bands)
{
    juce::ValueTree t (type);
    for (const auto& b : bands)
    {
        juce::ValueTree bt (ids::correctionBand);
        bt.setProperty (ids::kind, static_cast<int> (b.kind), nullptr);
        bt.setProperty (ids::freq, b.freq, nullptr);
        bt.setProperty (ids::gain, b.gainDb, nullptr);
        bt.setProperty (ids::q, b.q, nullptr);
        t.appendChild (bt, nullptr);
    }
    return t;
}

std::vector<roomeq::Band> bandsFromTree (const juce::ValueTree& t)
{
    std::vector<roomeq::Band> bands;
    for (const auto& bt : t)
    {
        const auto kind = static_cast<int> (bt[ids::kind]);
        roomeq::Band b;
        b.kind = static_cast<roomeq::BandKind> (juce::jlimit (0, 4, kind));
        b.freq = juce::jlimit (10.0, 24000.0, static_cast<double> (bt[ids::freq]));
        b.gainDb = juce::jlimit (-30.0, 30.0, static_cast<double> (bt[ids::gain]));
        b.q = juce::jlimit (0.1, 20.0, static_cast<double> (bt[ids::q]));
        if (bands.size() < 12 && std::isfinite (b.freq) && std::isfinite (b.gainDb) && std::isfinite (b.q))
            bands.push_back (b);
    }
    return bands;
}

// Mean of the finite values over the reference band.
double bandMean (const std::vector<double>& grid, const std::vector<double>& db, const roomeq::CorrectionConfig& band)
{
    double sum = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < grid.size(); ++i)
        if (grid[i] >= band.refBandLoHz && grid[i] <= band.refBandHiHz && std::isfinite (db[i]))
        {
            sum += db[i];
            ++n;
        }
    return n > 0 ? sum / n : std::nan ("");
}
} // namespace

MeasurementEngine::MeasurementEngine (CaptureRecorder& recorderToUse) : recorder (recorderToUse)
{
    startTimerHz (20);
}

MeasurementEngine::~MeasurementEngine()
{
    stopTimer();
    // An analysis can't be interrupted mid-FFT; wait for it (a second or two at
    // worst) rather than let the pool force-kill the thread.
    pool.removeAllJobs (true, 10000);
}

juce::Result MeasurementEngine::startSweep (double sampleRate, const SweepSettings& s, int replaceId, bool verify)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeSweepRequest (sampleRate, s.seconds, s.repeats, s.channel, s.levelDbfs), replaceId, verify);
}

juce::Result MeasurementEngine::startLatencyMeasurement (double sampleRate, const SweepSettings& s)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (recorder.isBusy())
        return juce::Result::fail ("A measurement is already running");
    recorder.start (makeSweepRequest (sampleRate, s.seconds, s.repeats, s.channel, s.levelDbfs));
    pendingName = "Loopback";
    pendingVerify = false;
    pendingLatency = true;
    status = "Measuring the system latency (loopback)";
    sendChangeMessage();
    return juce::Result::ok();
}

void MeasurementEngine::setSystemLatencyMs (std::optional<double> ms)
{
    systemLatencyMs = ms && std::isfinite (*ms) ? std::optional<double> (juce::jlimit (0.0, 1000.0, *ms)) : std::nullopt;
    sendChangeMessage();
}

juce::Result MeasurementEngine::startNoise (double sampleRate, double seconds, int channel, double levelDbfs, int replaceId,
                                            bool verify)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeNoiseRequest (sampleRate, seconds, channel, levelDbfs), replaceId, verify);
}

juce::Result MeasurementEngine::startProgram (double sampleRate, double seconds, int replaceId)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeProgramRequest (sampleRate, seconds, programSettleSeconds), replaceId, false);
}

juce::Result MeasurementEngine::startCalibration (double sampleRate, double seconds, double levelDbfs)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    if (recorder.isBusy())
        return juce::Result::fail ("A measurement is already running");
    auto request = makeNoiseRequest (sampleRate, seconds, 0, levelDbfs);
    request->purpose = CaptureRequest::Purpose::calibration;
    request->allChannels = true;
    request->throughEq = true;
    recorder.start (std::move (request));
    status = "Calibrating";
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result MeasurementEngine::startRequest (std::unique_ptr<CaptureRequest> request, int replaceId, bool verify)
{
    if (recorder.isBusy())
        return juce::Result::fail ("A measurement is already running");

    juce::String name;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        const auto it = std::find_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == replaceId; });
        if (it != entries.end())
        {
            name = juce::String::fromUTF8 (it->capture->name.c_str());
            verify = it->verify;   // a redo is the same kind of capture
        }
        else
        {
            replaceId = -1;
        }
    }
    if (verify && request->kind == CaptureRequest::Kind::program)
        return juce::Result::fail ("Verify with a sweep or pink noise");
    if (name.isEmpty())
        name = captureNumberName (verify ? nextVerifyNumber++ : nextNumber++, verify);

    // A verify capture measures the correction that's applied, not the one being compared.
    if (verify)
        setComparingPrevious (false);

    request->replaceId = replaceId;
    request->throughEq = verify;
    recorder.start (std::move (request));
    pendingName = name;
    pendingVerify = verify;
    pendingLatency = false;
    pendingCorrectionId = appliedId;
    status = "Measuring " + name;
    sendChangeMessage();
    return juce::Result::ok();
}

void MeasurementEngine::cancel()
{
    recorder.cancel();
}

MeasurementEngine::Activity MeasurementEngine::getActivity() const
{
    if (recorder.isBusy())
        return Activity::measuring;
    return analysesPending > 0 ? Activity::analysing : Activity::idle;
}

float MeasurementEngine::getProgress() const
{
    return recorder.getProgress();
}

juce::String MeasurementEngine::getStatus() const
{
    if (const auto* r = recorder.getCurrent())
    {
        if (r->cancelRequested.load())
            return "Stopping...";
        const auto done = r->samplesDone.load();
        if (r->purpose == CaptureRequest::Purpose::calibration)
            return "Calibration noise on both speakers: "
                   + juce::String (juce::jmin (static_cast<int> (static_cast<double> (done) / r->sampleRate),
                                               static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate)))
                   + " of " + juce::String (static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate)) + " s";
        const auto speaker = juce::String (r->sweepChannel == 0 ? "left" : "right")
                             + (r->throughEq ? " speaker, through the EQ" : " speaker, correction bypassed");
        if (r->kind == CaptureRequest::Kind::noise)
            return "Measuring " + pendingName + ": pink noise, "
                   + juce::String (juce::jmin (static_cast<int> (static_cast<double> (done) / r->sampleRate),
                                               static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate)))
                   + " of " + juce::String (static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate))
                   + " s on the " + speaker;
        if (r->kind == CaptureRequest::Kind::program)
        {
            const auto settle = static_cast<std::int64_t> (r->settle);
            if (done < settle)
                return "Recording program for " + pendingName + ": taking the correction out first";
            return "Recording program for " + pendingName + ", correction bypassed: "
                   + juce::String (static_cast<int> (static_cast<double> (done - settle) / r->sampleRate)) + " of "
                   + juce::String (static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate)) + " s";
        }
        const auto take = std::min<std::int64_t> (r->repeats, done / std::max<std::int64_t> (1, static_cast<std::int64_t> (r->excitation.size())) + 1);
        return "Measuring " + pendingName + ": sweep " + juce::String (take) + " of " + juce::String (r->repeats)
               + " on the " + speaker;
    }
    return status;
}

std::vector<MeasurementEngine::Entry> MeasurementEngine::getEntries() const
{
    const std::lock_guard<std::mutex> guard (stateLock);
    return entries;
}

std::shared_ptr<const MeasurementEngine::Display> MeasurementEngine::getDisplay() const
{
    const std::lock_guard<std::mutex> guard (stateLock);
    return display;
}

bool MeasurementEngine::isDisplayCurrent() const
{
    return analysesPending == 0 && ! recorder.isBusy() && displayGeneration == summaryGeneration;
}

void MeasurementEngine::replaceCapture (int id, const std::function<void (roomeq::Capture&)>& change)
{
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        const auto it = std::find_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == id; });
        if (it == entries.end())
            return;
        auto copy = std::make_shared<roomeq::Capture> (*it->capture);
        change (*copy);
        it->capture = std::move (copy);
    }
    requestSummary();
    sendChangeMessage();
}

void MeasurementEngine::regradeAll()
{
    // Exact and quick (band sums over the stored spectra), so it runs here.
    const auto grading = gradingFor (lastSettings.value_or (CorrectionSettings {}).config);
    const std::lock_guard<std::mutex> guard (stateLock);
    for (auto& e : entries)
    {
        auto copy = std::make_shared<roomeq::Capture> (*e.capture);
        roomeq::regradeCapture (*copy, grading);
        e.capture = std::move (copy);
    }
}

void MeasurementEngine::rename (int id, const juce::String& newName)
{
    const auto trimmed = newName.trim();
    if (trimmed.isNotEmpty())
        replaceCapture (id, [&] (roomeq::Capture& c) { c.name = trimmed.toStdString(); });
}

void MeasurementEngine::setExcluded (int id, bool excluded)
{
    replaceCapture (id, [&] (roomeq::Capture& c) { c.excluded = excluded; });
}

void MeasurementEngine::remove (int id)
{
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == id; }),
                       entries.end());
    }
    requestSummary();
    sendChangeMessage();
}

void MeasurementEngine::clearAll()
{
    jassert (getActivity() == Activity::idle);
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        entries.clear();
    }
    nextNumber = nextVerifyNumber = 1;
    applied.clear();
    previous.clear();
    hasPreviousCorrection = false;
    comparing = false;
    appliedId = previousId = 0;   // nextCorrectionId keeps counting, so nothing old can match
    status = "Cleared: no measurements, no correction";
    playingChanged();
    requestSummary();
}

void MeasurementEngine::setSmoothingFraction (int fraction)
{
    if (fraction == smoothingFraction)
        return;
    smoothingFraction = fraction;
    requestSummary();
}

bool MeasurementEngine::canApply() const
{
    const auto d = getDisplay();
    return d != nullptr && d->proposal && d->proposal->bands != applied;
}

void MeasurementEngine::applyProposal()
{
    const auto d = getDisplay();
    if (d == nullptr || ! d->proposal)
        return;
    previous = applied;
    previousId = appliedId;
    hasPreviousCorrection = true;
    applied = d->proposal->bands;
    appliedId = nextCorrectionId++;
    comparing = false;
    status = applied.empty() ? juce::String ("Applied: no correction needed")
                             : "Applied a " + juce::String (static_cast<int> (applied.size())) + "-band correction";
    playingChanged();
    requestSummary();    // verify captures belong to the correction they measured
}

void MeasurementEngine::undoApply()
{
    if (! hasPreviousCorrection)
        return;
    std::swap (applied, previous);
    std::swap (appliedId, previousId);
    comparing = false;
    status = (applied.empty() ? juce::String ("Undo: no correction now")
                              : "Undo: back to the " + juce::String (static_cast<int> (applied.size())) + "-band correction")
             + " (Undo again to swap back)";
    playingChanged();
    requestSummary();
}

void MeasurementEngine::setComparingPrevious (bool shouldCompare)
{
    shouldCompare = shouldCompare && hasPreviousCorrection;
    if (shouldCompare == comparing)
        return;
    comparing = shouldCompare;
    playingChanged();
}

void MeasurementEngine::playingChanged()
{
    if (onPlayingCorrectionChanged)
        onPlayingCorrectionChanged (getPlaying());
    sendChangeMessage();
}

void MeasurementEngine::analyse (std::unique_ptr<CaptureRequest> request)
{
    ++analysesPending;
    status = "Analysing " + pendingName + "...";
    std::shared_ptr<CaptureRequest> req = std::move (request);
    roomeq::AnalysisConfig cfg;
    cfg.grading = gradingFor (lastSettings.value_or (CorrectionSettings {}).config);
    pool.addJob ([mb = mailbox, req, name = pendingName, verify = pendingVerify, correctionId = pendingCorrectionId, cfg,
                  latency = pendingLatency]
    {
        AnalysisResult result;
        result.replaceId = req->replaceId;
        result.verify = verify;
        result.correctionId = correctionId;
        result.name = name;
        result.grading = cfg.grading;
        result.latency = latency;
        try
        {
            roomeq::Capture c;
            if (req->kind == CaptureRequest::Kind::sweep)
            {
                std::vector<std::vector<double>> recordings;
                for (const auto& take : req->mic)
                    recordings.emplace_back (take.begin(), take.end());
                c = roomeq::analyzeSweepCapture (name.toStdString(), recordings, req->sweepConfig, cfg);
            }
            else
            {
                // Noise and program both use the dual-FFT against a known reference.
                c = roomeq::analyzeProgramCapture (name.toStdString(),
                                                   std::vector<double> (req->reference.begin(), req->reference.end()),
                                                   std::vector<double> (req->mic[0].begin(), req->mic[0].end()),
                                                   req->sampleRate, cfg);
                if (req->kind == CaptureRequest::Kind::noise)
                    c.kind = "noise";
            }
            result.arrival = roomeq::estimateArrival (c);   // needs the impulse response, which isn't kept with the session
            c.repeatPowers.clear();   // only needed for grading (a re-grade keeps the spreads it found)
            result.capture = std::make_shared<roomeq::Capture> (std::move (c));
        }
        catch (const std::exception& e)
        {
            result.error = juce::String::fromUTF8 (e.what());
        }
        const std::lock_guard<std::mutex> guard (mb->lock);
        mb->results.push_back (std::move (result));
    });
}

void MeasurementEngine::requestSummary()
{
    std::vector<std::shared_ptr<const roomeq::Capture>> snapshot, verifySnapshot, allVerify;
    std::vector<int> ids, verifyIds;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        for (const auto& e : entries)
        {
            if (e.verify)
            {
                if (e.correctionId == appliedId)
                    verifySnapshot.push_back (e.capture);
                allVerify.push_back (e.capture);
                verifyIds.push_back (e.id);
                continue;
            }
            snapshot.push_back (e.capture);
            ids.push_back (e.id);
        }
    }
    const auto settings = lastSettings.value_or (CorrectionSettings {});
    const auto generation = ++summaryGeneration;
    mailbox->latestRequested = generation;
    pool.addJob ([mb = mailbox, snapshot, verifySnapshot, allVerify, ids, verifyIds, fraction = smoothingFraction, generation,
                  g = grid, settings]
    {
        // A newer request is already queued (e.g. while a target point is dragged): let that one run instead.
        if (generation != mb->latestRequested.load())
            return;
        const auto& band = settings.config;
        auto s = roomeq::summarizeSession (snapshot, fraction, g, band.refBandLoHz, band.refBandHiHz);
        std::shared_ptr<const Display> shared;
        if (s)
        {
            auto d = std::make_shared<Display>();
            d->summary = std::move (*s);
            d->ids = ids;
            for (const auto& c : snapshot)
                d->excluded.push_back (c->excluded);

            const auto& avg = d->summary.averageDb;
            const auto offset = roomeq::anchorOffsetDb (g, avg, settings.target, band.refBandLoHz, band.refBandHiHz);
            d->targetDb = settings.target.db (g);
            for (auto& v : d->targetDb)
                v += offset;
            d->fitFs = settings.fs;
            d->proposal = roomeq::designCorrection (snapshot, d->summary, settings.target, settings.fs, settings.config);

            if (auto v = roomeq::summarizeSession (verifySnapshot, fraction, g, band.refBandLoHz, band.refBandHiHz))
            {
                d->verifiedDb = v->averageDb;
                const auto shift = bandMean (g, avg, band) - bandMean (g, d->verifiedDb, band);
                for (auto& x : d->verifiedDb)
                    x += std::isfinite (shift) ? shift : 0.0;
                d->verifiedCount = v->nGood;
            }

            // Each verify capture's own curve, for highlighting: summarised alongside the
            // fit captures but left out of the average, so it gets the same smoothing and
            // level alignment as every position without changing anything else.
            if (! allVerify.empty())
            {
                auto combined = snapshot;
                for (const auto& v : allVerify)
                {
                    auto c = std::make_shared<roomeq::Capture> (*v);
                    c->excluded = true;
                    combined.push_back (std::move (c));
                }
                if (auto all = roomeq::summarizeSession (combined, fraction, g, band.refBandLoHz, band.refBandHiHz))
                    for (std::size_t i = 0; i < allVerify.size(); ++i)
                    {
                        d->verifyIds.push_back (verifyIds[i]);
                        d->verifyDb.push_back (all->positionDb[snapshot.size() + i]);
                    }
            }
            shared = std::move (d);
        }
        const std::lock_guard<std::mutex> guard (mb->lock);
        if (generation > mb->summaryGeneration)
        {
            mb->display = std::move (shared);
            mb->summaryGeneration = generation;
            mb->summaryReady = true;
        }
    });
}

void MeasurementEngine::update()
{
    auto changed = false;
    if (smoothingSource)
        setSmoothingFraction (smoothingSource());
    if (settingsSource)
    {
        auto s = settingsSource();
        if (! lastSettings || ! sameSettings (*lastSettings, s))
        {
            const auto bandChanged = ! sameBand (lastSettings.value_or (CorrectionSettings {}).config, s.config);
            lastSettings = std::move (s);
            if (bandChanged)
            {
                regradeAll();
                changed = true;
            }
            requestSummary();
        }
    }

    if (auto finished = recorder.collectFinished())
    {
        if (finished->purpose != CaptureRequest::Purpose::capture)
            status = finished->cancelled ? "Calibration stopped" : "";   // the loudness controller reports it
        else if (finished->cancelled)
            status = "Measurement stopped";
        else
            analyse (std::move (finished));
        changed = true;
    }

    std::vector<AnalysisResult> results;
    std::shared_ptr<const Display> newDisplay;
    auto summaryReady = false;
    {
        const std::lock_guard<std::mutex> guard (mailbox->lock);
        results.swap (mailbox->results);
        std::swap (summaryReady, mailbox->summaryReady);
        newDisplay = mailbox->display;
        displayGeneration = mailbox->summaryGeneration;
    }

    for (auto& r : results)
    {
        --analysesPending;
        changed = true;
        if (r.capture == nullptr)
        {
            status = r.name + " failed: " + r.error;
            continue;
        }
        if (r.latency)
        {
            // A cable loopback is one clean arrival, heard clearly, with a flat
            // response (a speaker in a room never is: 63 Hz-8 kHz within 3 dB).
            auto flat = true;
            for (const auto& b : r.capture->grade.bands)
                if (b.center >= 60.0 && b.center <= 8200.0)
                    flat = flat && ! b.outOfRange && std::abs (b.levelDb) <= 3.0;
            if (r.arrival.confidence == roomeq::Confidence::high && r.arrival.reasons.empty() && flat
                && r.capture->grade.overall != roomeq::Grade::redo)
            {
                systemLatencyMs = r.arrival.ms;
                status = "System latency: " + juce::String (r.arrival.ms, 2) + " ms (loopback)";
            }
            else
            {
                status = "That didn't look like a cable loopback (";
                status << (! r.arrival.reasons.empty() ? juce::String::fromUTF8 (r.arrival.reasons.front().c_str())
                           : ! flat                     ? juce::String ("the response isn't flat: a speaker, not a cable?")
                                                        : juce::String ("not heard clearly"))
                       << "). The system latency wasn't changed: patch the output straight into the mic input and try again.";
            }
            continue;
        }
        if (const auto now = gradingFor (lastSettings.value_or (CorrectionSettings {}).config);
            ! same (now.passbandLo, r.grading.passbandLo) || ! same (now.passbandHi, r.grading.passbandHi))
        {
            auto regraded = std::make_shared<roomeq::Capture> (*r.capture);   // the zone changed while it was analysed
            roomeq::regradeCapture (*regraded, now);
            r.capture = std::move (regraded);
        }
        {
            const std::lock_guard<std::mutex> guard (stateLock);
            const auto it = std::find_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == r.replaceId; });
            if (it != entries.end())
            {
                auto redone = std::make_shared<roomeq::Capture> (*r.capture);
                redone->excluded = it->capture->excluded;
                it->capture = std::move (redone);
                it->verify = r.verify;
                it->correctionId = r.correctionId;
                it->arrival = r.arrival;
            }
            else
            {
                entries.push_back ({ nextId++, r.capture, r.verify, r.correctionId, r.arrival });
            }
        }
        status = r.name + " analysed: " + juce::String (roomeq::gradeLabel (r.capture->grade.overall));
        requestSummary();
    }

    if (summaryReady)
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        display = newDisplay;
        changed = true;
    }

    if (changed)
        sendChangeMessage();
}

// ---------------------------------------------------------------------------

juce::ValueTree MeasurementEngine::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty (ids::version, 1, nullptr);
    tree.setProperty (ids::smoothing, smoothingFraction, nullptr);
    tree.setProperty (ids::nextNumber, nextNumber, nullptr);
    tree.setProperty (ids::nextVerifyNumber, nextVerifyNumber, nullptr);
    tree.setProperty (ids::appliedId, appliedId, nullptr);
    tree.setProperty (ids::nextCorrectionId, nextCorrectionId, nullptr);
    if (systemLatencyMs)
        tree.setProperty (ids::systemLatencyMs, *systemLatencyMs, nullptr);
    tree.appendChild (bandsToTree (ids::applied, applied), nullptr);
    if (hasPreviousCorrection)
    {
        auto prev = bandsToTree (ids::previous, previous);
        prev.setProperty (ids::correctionId, previousId, nullptr);
        tree.appendChild (prev, nullptr);
    }

    const std::lock_guard<std::mutex> guard (stateLock);
    for (const auto& e : entries)
    {
        const auto& c = *e.capture;
        juce::ValueTree ct (ids::capture);
        ct.setProperty (ids::id, e.id, nullptr);
        ct.setProperty (ids::name, juce::String::fromUTF8 (c.name.c_str()), nullptr);
        ct.setProperty (ids::kind, juce::String (c.kind), nullptr);
        ct.setProperty (ids::fs, c.fs, nullptr);
        ct.setProperty (ids::excluded, c.excluded, nullptr);
        if (e.verify)
        {
            ct.setProperty (ids::verify, true, nullptr);
            ct.setProperty (ids::correctionId, e.correctionId, nullptr);
        }
        ct.setProperty (ids::overall, static_cast<int> (c.grade.overall), nullptr);
        ct.setProperty (ids::reasons, joinLines (c.grade.reasons), nullptr);
        ct.setProperty (ids::notes, joinLines (c.grade.notes), nullptr);
        ct.setProperty (ids::delays, pack (c.delaysMs), nullptr);
        ct.setProperty (ids::arrivalMs, e.arrival.ms, nullptr);
        ct.setProperty (ids::arrivalConfidence, static_cast<int> (e.arrival.confidence), nullptr);
        ct.setProperty (ids::arrivalReasons, joinLines (e.arrival.reasons), nullptr);
        ct.setProperty (ids::power, pack (c.power), nullptr);
        ct.setProperty (ids::noise, pack (c.noisePower), nullptr);
        ct.setProperty (ids::weight, pack (c.weight), nullptr);
        for (const auto& b : c.grade.bands)
        {
            juce::ValueTree bt (ids::band);
            bt.setProperty (ids::name, juce::String (b.name), nullptr);
            bt.setProperty (ids::center, b.center, nullptr);
            bt.setProperty (ids::lo, b.lo, nullptr);
            bt.setProperty (ids::hi, b.hi, nullptr);
            bt.setProperty (ids::level, b.levelDb, nullptr);
            bt.setProperty (ids::snr, b.snrDb, nullptr);
            if (b.spreadDb)
                bt.setProperty (ids::spread, *b.spreadDb, nullptr);
            if (b.excessSpreadDb)
                bt.setProperty (ids::excess, *b.excessSpreadDb, nullptr);
            bt.setProperty (ids::outOfRange, b.outOfRange, nullptr);
            bt.setProperty (ids::snrGrade, static_cast<int> (b.snrGrade), nullptr);
            bt.setProperty (ids::consistencyGrade, static_cast<int> (b.consistencyGrade), nullptr);
            ct.appendChild (bt, nullptr);
        }
        tree.appendChild (ct, nullptr);
    }
    return tree;
}

void MeasurementEngine::fromValueTree (const juce::ValueTree& tree)
{
    if (! tree.hasType (treeType))
        return;

    std::vector<Entry> loaded;
    auto maxId = 0;
    for (const auto& ct : tree)
    {
        if (! ct.hasType (ids::capture))
            continue;
        auto c = std::make_shared<roomeq::Capture>();
        c->name = ct[ids::name].toString().toStdString();
        c->kind = ct[ids::kind].toString().toStdString();
        c->fs = ct[ids::fs];
        c->excluded = ct[ids::excluded];
        c->power = unpack (ct[ids::power]);
        c->noisePower = unpack (ct[ids::noise]);
        c->weight = unpack (ct[ids::weight]);
        c->delaysMs = unpack (ct[ids::delays]);
        if (c->power.size() < 2 || c->noisePower.size() != c->power.size() || c->weight.size() != c->power.size() || c->fs <= 0.0)
            continue;   // damaged entry
        c->freqs = roomeq::rfftFreqs (2 * (c->power.size() - 1), c->fs);
        c->grade.overall = static_cast<roomeq::Grade> (static_cast<int> (ct[ids::overall]));
        c->grade.reasons = splitLines (ct[ids::reasons]);
        c->grade.notes = splitLines (ct[ids::notes]);
        for (const auto& bt : ct)
        {
            roomeq::BandResult b;
            b.name = bt[ids::name].toString().toStdString();
            b.center = bt[ids::center];
            b.lo = bt[ids::lo];
            b.hi = bt[ids::hi];
            b.levelDb = bt[ids::level];
            b.snrDb = bt[ids::snr];
            if (bt.hasProperty (ids::spread))
                b.spreadDb = static_cast<double> (bt[ids::spread]);
            if (bt.hasProperty (ids::excess))
                b.excessSpreadDb = static_cast<double> (bt[ids::excess]);
            b.outOfRange = bt[ids::outOfRange];
            b.snrGrade = static_cast<roomeq::Grade> (static_cast<int> (bt[ids::snrGrade]));
            b.consistencyGrade = static_cast<roomeq::Grade> (static_cast<int> (bt[ids::consistencyGrade]));
            c->grade.bands.push_back (b);
        }
        const auto id = static_cast<int> (ct[ids::id]);
        maxId = std::max (maxId, id);
        roomeq::Arrival arrival;
        if (ct.hasProperty (ids::arrivalMs))
        {
            arrival.ms = ct[ids::arrivalMs];
            arrival.confidence = static_cast<roomeq::Confidence> (juce::jlimit (0, 2, static_cast<int> (ct[ids::arrivalConfidence])));
            arrival.reasons = splitLines (ct[ids::arrivalReasons]);
        }
        else   // saved before arrivals were recorded: the loop delay, unchecked
        {
            arrival.ms = c->delaysMs.empty() ? std::nan ("") : c->delaysMs.front();
            arrival.confidence = roomeq::Confidence::medium;
            arrival.reasons = { "measured before arrival confidence was recorded" };
        }
        loaded.push_back ({ id, std::move (c), static_cast<bool> (ct[ids::verify]), static_cast<int> (ct[ids::correctionId]),
                            std::move (arrival) });
    }

    {
        const std::lock_guard<std::mutex> guard (stateLock);
        entries = std::move (loaded);
    }
    nextId = maxId + 1;
    systemLatencyMs = tree.hasProperty (ids::systemLatencyMs) ? std::optional<double> (static_cast<double> (tree[ids::systemLatencyMs]))
                                                              : std::nullopt;
    nextNumber = std::max (static_cast<int> (tree.getProperty (ids::nextNumber, 1)), 1);
    nextVerifyNumber = std::max (static_cast<int> (tree.getProperty (ids::nextVerifyNumber, 1)), 1);
    smoothingFraction = tree.getProperty (ids::smoothing, 6);

    applied = bandsFromTree (tree.getChildWithName (ids::applied));
    appliedId = tree.getProperty (ids::appliedId, 0);
    const auto prev = tree.getChildWithName (ids::previous);
    hasPreviousCorrection = prev.isValid();
    previous = bandsFromTree (prev);
    previousId = prev.getProperty (ids::correctionId, 0);
    nextCorrectionId = std::max ({ static_cast<int> (tree.getProperty (ids::nextCorrectionId, 1)), appliedId + 1, previousId + 1 });
    comparing = false;

    playingChanged();
    requestSummary();
}
