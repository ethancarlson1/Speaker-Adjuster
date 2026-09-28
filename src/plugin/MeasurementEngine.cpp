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

juce::String captureNumberName (int n)
{
    return "P" + juce::String (n);
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

juce::Result MeasurementEngine::startSweep (double sampleRate, const SweepSettings& s, int replaceId)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeSweepRequest (sampleRate, s.seconds, s.repeats, s.channel, s.levelDbfs), replaceId);
}

juce::Result MeasurementEngine::startNoise (double sampleRate, double seconds, int channel, double levelDbfs, int replaceId)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeNoiseRequest (sampleRate, seconds, channel, levelDbfs), replaceId);
}

juce::Result MeasurementEngine::startProgram (double sampleRate, double seconds, int replaceId)
{
    if (sampleRate <= 0.0)
        return juce::Result::fail ("Audio isn't running yet");
    return startRequest (makeProgramRequest (sampleRate, seconds), replaceId);
}

juce::Result MeasurementEngine::startRequest (std::unique_ptr<CaptureRequest> request, int replaceId)
{
    if (recorder.isBusy())
        return juce::Result::fail ("A measurement is already running");

    juce::String name;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        const auto it = std::find_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == replaceId; });
        if (it != entries.end())
            name = juce::String::fromUTF8 (it->capture->name.c_str());
        else
            replaceId = -1;
    }
    if (name.isEmpty())
        name = captureNumberName (nextNumber++);

    request->replaceId = replaceId;
    recorder.start (std::move (request));
    pendingName = name;
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
        const auto speaker = juce::String (r->sweepChannel == 0 ? "left" : "right");
        if (r->kind == CaptureRequest::Kind::noise)
            return "Measuring " + pendingName + ": pink noise, "
                   + juce::String (juce::jmin (static_cast<int> (static_cast<double> (done) / r->sampleRate),
                                               static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate)))
                   + " of " + juce::String (static_cast<int> (static_cast<double> (r->reference.size()) / r->sampleRate))
                   + " s on the " + speaker + " speaker";
        if (r->kind == CaptureRequest::Kind::program)
            return "Recording program for " + pendingName + ": "
                   + juce::String (static_cast<int> (static_cast<double> (done) / r->sampleRate)) + " of "
                   + juce::String (static_cast<int> (static_cast<double> (r->totalSamples) / r->sampleRate)) + " s";
        const auto take = std::min<std::int64_t> (r->repeats, done / std::max<std::int64_t> (1, static_cast<std::int64_t> (r->excitation.size())) + 1);
        return "Measuring " + pendingName + ": sweep " + juce::String (take) + " of " + juce::String (r->repeats)
               + " on the " + speaker + " speaker";
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

void MeasurementEngine::setSmoothingFraction (int fraction)
{
    if (fraction == smoothingFraction)
        return;
    smoothingFraction = fraction;
    requestSummary();
}

void MeasurementEngine::analyse (std::unique_ptr<CaptureRequest> request)
{
    ++analysesPending;
    status = "Analysing " + pendingName + "...";
    std::shared_ptr<CaptureRequest> req = std::move (request);
    pool.addJob ([mb = mailbox, req, name = pendingName]
    {
        AnalysisResult result;
        result.replaceId = req->replaceId;
        result.name = name;
        try
        {
            roomeq::Capture c;
            if (req->kind == CaptureRequest::Kind::sweep)
            {
                std::vector<std::vector<double>> recordings;
                for (const auto& take : req->mic)
                    recordings.emplace_back (take.begin(), take.end());
                c = roomeq::analyzeSweepCapture (name.toStdString(), recordings, req->sweepConfig);
            }
            else
            {
                // Noise and program both use the dual-FFT against a known reference.
                c = roomeq::analyzeProgramCapture (name.toStdString(),
                                                   std::vector<double> (req->reference.begin(), req->reference.end()),
                                                   std::vector<double> (req->mic[0].begin(), req->mic[0].end()),
                                                   req->sampleRate);
                if (req->kind == CaptureRequest::Kind::noise)
                    c.kind = "noise";
            }
            c.repeatPowers.clear();   // only needed for grading
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
    std::vector<std::shared_ptr<const roomeq::Capture>> snapshot;
    std::vector<int> ids;
    {
        const std::lock_guard<std::mutex> guard (stateLock);
        for (const auto& e : entries)
        {
            snapshot.push_back (e.capture);
            ids.push_back (e.id);
        }
    }
    const auto generation = ++summaryGeneration;
    pool.addJob ([mb = mailbox, snapshot, ids, fraction = smoothingFraction, generation, g = grid]
    {
        auto s = roomeq::summarizeSession (snapshot, fraction, g);
        std::shared_ptr<const Display> shared;
        if (s)
        {
            auto d = std::make_shared<Display>();
            d->summary = std::move (*s);
            d->ids = ids;
            for (const auto& c : snapshot)
                d->excluded.push_back (c->excluded);
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

    if (auto finished = recorder.collectFinished())
    {
        if (finished->cancelled)
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
        {
            const std::lock_guard<std::mutex> guard (stateLock);
            const auto it = std::find_if (entries.begin(), entries.end(), [&] (const Entry& e) { return e.id == r.replaceId; });
            if (it != entries.end())
            {
                auto redone = std::make_shared<roomeq::Capture> (*r.capture);
                redone->excluded = it->capture->excluded;
                it->capture = std::move (redone);
            }
            else
            {
                entries.push_back ({ nextId++, r.capture });
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
        ct.setProperty (ids::overall, static_cast<int> (c.grade.overall), nullptr);
        ct.setProperty (ids::reasons, joinLines (c.grade.reasons), nullptr);
        ct.setProperty (ids::notes, joinLines (c.grade.notes), nullptr);
        ct.setProperty (ids::delays, pack (c.delaysMs), nullptr);
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
        loaded.push_back ({ id, std::move (c) });
    }

    {
        const std::lock_guard<std::mutex> guard (stateLock);
        entries = std::move (loaded);
    }
    nextId = maxId + 1;
    nextNumber = std::max (static_cast<int> (tree.getProperty (ids::nextNumber, 1)), 1);
    smoothingFraction = tree.getProperty (ids::smoothing, 6);
    requestSummary();
    sendChangeMessage();
}
