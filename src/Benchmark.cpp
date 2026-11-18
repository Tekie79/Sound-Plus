#include "Benchmark.h"
#include "LibraryView.h"
#include "Performance.h"
#include <cmath>
#include "MacSources.h"
#include "DesignerView.h"
namespace sp
{
Benchmark::Benchmark(LibraryView &v, const juce::File &file, const juce::File &out, DesignerView *designView)
    : view(v), designer(designView), output(out), plan(juce::JSON::parse(file)), startMs(monotonicMs()),
      phaseStart(startMs)
{
    view.benchmark = this;
    startTimer(2);
}
void Benchmark::painted(uint64_t id, int rows, double sent, double applied)
{
    const auto now = monotonicMs();
    lastPainted = id;
    lastPaintedOffset = view.snapshot.offset;
    if (!firstPaint)
        firstPaint = now - startMs;
    if (rows && !firstNonempty)
        firstNonempty = now - startMs;
    if (id != waiting || completed)
        return;
    if (plan["mode"].toString().startsWith("semantic"))
    {
        if (view.snapshot.semanticCompletedRequest != id)
            return;
        if (!firstSemanticPaint)
            firstSemanticPaint = now - startMs;
        auto *o = new juce::DynamicObject();
        o->setProperty("workload", workload);
        o->setProperty("run", run);
        o->setProperty("index", index);
        o->setProperty("dispatchToPaintMs", now - sent);
        o->setProperty("dispatchToAppliedMs", applied - sent);
        o->setProperty("request", (juce::int64)id);
        o->setProperty("debounceMs", 0);
        o->setProperty("query", view.search.getText());
        o->setProperty("sourceFilter", view.rootFilter);
        o->setProperty("analysisPending", view.snapshot.semantic.coverage.pending);
        o->setProperty("auditionActive", (bool)view.audio.status()["playing"]);
        o->setProperty("audioId", view.audio.status()["id"]);
        o->setProperty("designLayers", view.audio.status()["designLayers"]);
        bool newRegionHit = false;
        if (plan["updateFolder"].toString().isNotEmpty())
            for (auto &a : view.snapshot.results)
                for (auto &r : view.snapshot.roots)
                    if (a.rootId == r.id && r.path == plan["updateFolder"].toString())
                        newRegionHit = true;
        o->setProperty("newRegionHit", newRegionHit);
        const auto &t = view.snapshot.semanticTiming;
        o->setProperty("textPreprocessMs", t.textPreprocessMs);
        o->setProperty("textEncodeMs", t.textEncodeMs);
        o->setProperty("workerRoundTripMs", t.workerRoundTripMs);
        o->setProperty("fetchFilterMs", t.fetchFilterMs);
        o->setProperty("vectorScoreMs", t.vectorScoreMs);
        o->setProperty("rankGroupMs", t.rankGroupMs);
        o->setProperty("fusionMs", t.fusionMs);
        o->setProperty("vectors", t.vectors);
        o->setProperty("textCacheHit", t.textCacheHit);
        o->setProperty("rows", rows);
        const auto &reference = view.snapshot.reference;
        o->setProperty("referenceEncodingMs", reference.encodingMs);
        o->setProperty("referenceRetrievalMs", reference.retrievalMs);
        o->setProperty("referenceCacheHit", reference.cacheHit);
        o->setProperty("referenceWindows", reference.totalWindows);
        o->setProperty("referenceFile", reference.reference.source.name);
        o->setProperty("referenceStartSample", (juce::int64)reference.reference.startSample);
        o->setProperty("referenceEndSample", (juce::int64)reference.reference.endSample);
        juce::Array<juce::var> hits;
        if (workload == "evaluation" || workload == "example-evaluation" ||
            plan["mode"].toString() == "semantic-reopen" || plan["mode"].toString() == "semantic-offline")
            for (auto &a : view.snapshot.results)
            {
                auto *hit = new juce::DynamicObject();
                hit->setProperty("file", a.name);
                hit->setProperty("assetId", a.id);
                hit->setProperty("revision", (juce::int64)a.revision);
                hit->setProperty("reason", a.matchReason);
                hit->setProperty("sourceRate", a.sampleRate);
                juce::Array<juce::var> ranges;
                for (auto &m : a.matches)
                {
                    auto *r = new juce::DynamicObject();
                    r->setProperty("startSample", (juce::int64)m.startSample);
                    r->setProperty("endSample", (juce::int64)m.endSample);
                    r->setProperty("similarity", m.similarity);
                    r->setProperty("groupedWindows", m.groupedWindows);
                    ranges.add(juce::var(r));
                }
                hit->setProperty("ranges", ranges);
                hits.add(juce::var(hit));
            }
        o->setProperty("hits", hits);
        samples.add(juce::var(o));
        waiting = 0;
        ++index;
        return;
    }
    auto *o = new juce::DynamicObject();
    o->setProperty("workload", workload);
    o->setProperty("run", run);
    o->setProperty("index", index);
    o->setProperty("dispatchToPaintMs", now - sent);
    o->setProperty("dispatchToAppliedMs", applied - sent);
    o->setProperty("debounceMs", 0);
    o->setProperty("indexingActive",
                   view.snapshot.progress.activeWorkers > 0 || view.snapshot.progress.metadataBacklog > 0);
    o->setProperty("rows", rows);
    o->setProperty("request", (juce::int64)id);
    samples.add(juce::var(o));
    waiting = 0;
    ++index;
}
void Benchmark::dispatch()
{
    auto *queries = plan[workload == "evaluation" ? "evaluationQueries" : "queries"].getArray();
    if (!queries || queries->isEmpty())
    {
        save(false, "No queries");
        return;
    }
    auto q = (*queries)[index % queries->size()];
    auto root = q["source"].toString();
    if (root.isNotEmpty())
        for (const auto &r : view.snapshot.roots)
            if (r.path.endsWith(root))
            {
                root = r.id;
                break;
            }
    waiting = plan["mode"].toString().startsWith("semantic")
                  ? view.benchmarkSemanticSearch(q["text"].toString(), root)
                  : view.benchmarkSearch(q["text"].toString(), root);
    sendMs = monotonicMs();
}
void Benchmark::save(bool success, const juce::String &error)
{
    if (completed)
        return;
    completed = true;
    stopTimer();
    view.benchmark = nullptr;
    auto *o = new juce::DynamicObject();
    o->setProperty("success", success);
    o->setProperty("error", error);
    o->setProperty("mode", plan["mode"]);
    o->setProperty("startMonotonicMs", startMs);
    o->setProperty("elapsedMs", monotonicMs() - startMs);
    o->setProperty("firstPaintMs", firstPaint);
    o->setProperty("firstNonemptyPaintMs", firstNonempty);
    o->setProperty("samples", samples);
    o->setProperty("auditions", auditions);
    o->setProperty("incrementalChanges", changes);
    o->setProperty("importStats", importStats);
    o->setProperty("counters", performance.json());
    o->setProperty("peakDurableMetadataBacklog", peakPending);
    o->setProperty("maxPendingSearches", 1);
    o->setProperty("audio", view.audio.status());
    if (designer)
        o->setProperty("interactions", designer->interactionMetrics());
    o->setProperty("controls", controls);
    o->setProperty("finalPhase", phase);
    auto *state = new juce::DynamicObject();
    state->setProperty("pauseRequested", view.snapshot.progress.pauseRequested);
    state->setProperty("paused", view.snapshot.progress.paused);
    state->setProperty("activeWorkers", view.snapshot.progress.activeWorkers);
    state->setProperty("parkedWorkers", view.snapshot.progress.parkedWorkers);
    state->setProperty("queuedBatches", view.snapshot.progress.queuedBatches);
    state->setProperty("stage", view.snapshot.progress.stage);
    state->setProperty("request", (juce::int64)view.snapshot.request);
    state->setProperty("semanticCompletedRequest", (juce::int64)view.snapshot.semanticCompletedRequest);
    o->setProperty("finalState", juce::var(state));
    o->setProperty("firstSemanticPaintMs", firstSemanticPaint);
    o->setProperty("thermalSamples", thermalSamples);
    if (plan["mode"].toString().startsWith("semantic"))
    {
        const auto &s = view.snapshot.semantic;
        auto *ai = new juce::DynamicObject();
        ai->setProperty("model", s.model);
        ai->setProperty("preprocessing", s.preprocessing);
        ai->setProperty("ready", s.ready);
        ai->setProperty("error", s.error);
        ai->setProperty("modelLoadMs", s.modelLoadMs);
        ai->setProperty("files", s.coverage.complete);
        ai->setProperty("pending", s.coverage.pending);
        ai->setProperty("failed", s.coverage.failed);
        ai->setProperty("regions", s.coverage.regions);
        ai->setProperty("vectorBytes", (juce::int64)s.coverage.vectorBytes);
        ai->setProperty("sourceSecondsThisSession", s.sourceSecondsThisSession);
        ai->setProperty("windowsThisSession", (juce::int64)s.windowsThisSession);
        ai->setProperty("analysisWallMs", s.analysisWallMs);
        ai->setProperty("firstWindowStartedMs", s.firstWindowStartedMs);
        ai->setProperty("lastWindowPublishedMs", s.lastWindowPublishedMs);
        ai->setProperty("helperPeakResidentBytes", (juce::int64)s.helperPeakBytes);
        ai->setProperty("indexReady", s.indexReady);
        ai->setProperty("indexReused", s.indexReused);
        ai->setProperty("indexBuilding", s.indexBuilding);
        ai->setProperty("indexRegions", (juce::int64)s.indexRegions);
        ai->setProperty("indexBytes", (juce::int64)s.indexBytes);
        ai->setProperty("indexOpenMs", s.indexOpenMs);
        ai->setProperty("indexBuildMs", s.indexBuildMs);
        ai->setProperty("indexStatus", s.indexStatus);
        o->setProperty("semantic", juce::var(ai));
    }
    output.getParentDirectory().createDirectory();
    output.replaceWithText(juce::JSON::toString(juce::var(o)));
    juce::JUCEApplicationBase::quit();
}
void Benchmark::timerCallback()
{
    const auto now = monotonicMs();
    int total = 0, done = 0;
    bool scanning = false;
    for (auto &r : view.snapshot.roots)
    {
        total += r.discovered;
        done += r.ready + r.failed;
        scanning |= r.scanning;
    }
    peakPending = std::max(peakPending, total - done);
    if (now - startMs > 1800000)
    {
        save(false, "Benchmark timed out");
        return;
    }
    if (view.snapshot.error.isNotEmpty())
    {
        save(false, view.snapshot.error);
        return;
    }
    if (plan["mode"].toString() == "slow")
    {
        for (auto &a : view.snapshot.results)
            if (a.name == "healthy.wav")
            {
                save(true);
                return;
            }
        return;
    }
    if (plan["mode"].toString() == "reopen")
    {
        if (now - startMs > 35000)
        {
            save(true);
            return;
        }
        return;
    }
    const int count = (int)plan["queryCount"];
    if (plan["mode"].toString().startsWith("semantic"))
    {
        semanticTick(now);
        return;
    }
    if (plan["mode"].toString() == "controls")
    {
        controlTick(total, done, now);
        return;
    }
    if (phase == 0)
    {
        workload = "indexing";
        if (!waiting && index < count)
            dispatch();
        if (index >= count)
        {
            phase = 1;
            index = 0;
        }
        return;
    }
    if (phase == 1)
    {
        if (total < (int)plan["expectedFiles"] || done < total || scanning)
            return;
        importStats = performance.json();
        importStats.getDynamicObject()->setProperty("allHeadersCommittedElapsedMs", now - startMs);
        importStats.getDynamicObject()->setProperty("filenameStageElapsedMs",
                                                    performance.discoveryFinishedMs.load() -
                                                        performance.firstEligibleMs.load());
        phase = 2;
        phaseStart = now;
        index = 0;
        run = 0;
        workload = "warm";
    }
    if (phase == 2 || phase == 3)
    {
        if (phase == 3 && !(bool)view.audio.status()["playing"])
        {
            return; // Every recorded audition query starts after the device reports playback.
        }
        if (!waiting && index < count)
            dispatch();
        if (index >= count)
        {
            index = 0;
            if (++run >= 3)
            {
                run = 0;
                if (phase == 2)
                {
                    phase = 3;
                    workload = "auditioning";
                    view.audio.play("benchmark-long", juce::File(plan["longAudio"].toString()));
                }
                else
                {
                    phase = 4;
                    index = 0;
                    view.audio.stop();
                }
            }
        }
        return;
    }
    if (phase == 4)
    {
        if (index >= 20)
        {
            phase = 5;
            index = 0;
            return;
        }
        auto state = view.audio.status();
        if (audioDispatch == 0)
        {
            audioDispatch = now;
            view.audio.play("benchmark-start-" + juce::String(index),
                            juce::File(plan["longAudio"].toString()));
            return;
        }
        if (state["id"].toString() == "benchmark-start-" + juce::String(index) &&
            (double)state["firstCallbackMs"] > audioDispatch)
        {
            auditions.add((double)state["firstCallbackMs"] - audioDispatch);
            ++index;
            audioDispatch = 0;
            view.audio.stop();
        }
        return;
    }
    if (phase == 5)
    {
        if (index >= 5 && done >= total && !scanning && view.snapshot.progress.activeWorkers == 0)
        {
            save(true);
            return;
        }
        if (index >= 5)
            return;
        if (audioDispatch == 0)
        {
            auto target = juce::File(plan["changeFolder"].toString())
                              .getChildFile("benchmarklive " + juce::String(index) + ".wav");
            juce::File(plan["templateAudio"].toString()).copyFileTo(target);
            audioDispatch = monotonicMs();
            view.benchmarkSearch("benchmarklive");
            return;
        }
        for (auto &a : view.snapshot.results)
            if (a.name == "benchmarklive " + juce::String(index) + ".wav")
            {
                changes.add(now - audioDispatch);
                ++index;
                audioDispatch = 0;
                break;
            }
    }
}
} // namespace sp
namespace sp
{
void Benchmark::semanticTick(double now)
{
    if (plan["trace"].isBool() && (bool)plan["trace"])
    {
        static int previous = -1;
        if (phase != previous)
        {
            std::fprintf(stderr,
                         "semantic phase %d: paused=%d active=%d parked=%d batches=%d aiActive=%d "
                         "request=%llu semantic=%llu\n",
                         phase, view.snapshot.progress.paused, view.snapshot.progress.activeWorkers,
                         view.snapshot.progress.parkedWorkers, view.snapshot.progress.queuedBatches,
                         view.snapshot.semantic.active, (unsigned long long)view.snapshot.request,
                         (unsigned long long)view.snapshot.semanticCompletedRequest);
            previous = phase;
        }
    }
    if (now - lastThermalSample >= 1000)
    {
        auto state = powerAndThermalState();
        state.getDynamicObject()->setProperty("elapsedMs", now - startMs);
        thermalSamples.add(state);
        lastThermalSample = now;
    }
    const auto mode = plan["mode"].toString();
    if (mode == "semantic-example")
    {
        referenceTick(now);
        return;
    }
    const auto &ai = view.snapshot.semantic;
    auto record = [&](const juce::String &key, bool value)
    { controls.getDynamicObject()->setProperty(key, value); };
    auto complete = [&]
    {
        for (auto &property : controls.getDynamicObject()->getProperties())
            if (property.value.isBool() && !(bool)property.value)
            {
                save(false, "Semantic UI assertion failed: " + property.name.toString());
                return;
            }
        save(true);
    };
    auto count = (int)plan["queryCount"];
    bool analyzed =
        ai.ready && ai.coverage.complete >= (int)plan["expectedFiles"] && ai.coverage.pending == 0;
    if (mode == "semantic-analyze")
    {
        if (analyzed)
            complete();
        return;
    }
    if (mode == "semantic-designer-edit")
    {
        if (!ai.ready)
            return;
        if (phase == 0)
        {
            if (!designer || designer->session.working ||
                designer->session.document.activeMix().layers.size() != 8)
                return;
            semanticDemoId = "design:" + designer->session.document.id;
            view.audio.gain(.04f);
            designer->session.play();
            phase = 1;
            return;
        }
        auto state = view.audio.status();
        if (phase == 1 && (bool)state["playing"] && state["id"].toString() == semanticDemoId)
        {
            phase = 2;
            audioDispatch = now;
            workload = "continuous-designer-edits";
            controls.getDynamicObject()->setProperty("audioAtStart", state);
            if (plan["updateFolder"].toString().isNotEmpty())
                view.library.addFolder(juce::File(plan["updateFolder"].toString()));
        }
        if (phase == 2)
        {
            if (!(bool)state["playing"] || state["id"].toString() != semanticDemoId)
            {
                save(false, "Transport interrupted during continuous editing");
                return;
            }
            auto elapsed = now - audioDispatch;
            if ((int64_t)state["sourceSample"] < lastDesignerFrame)
            {
                save(false, "Clock moved backwards during parameter-only editing");
                return;
            }
            lastDesignerFrame = (int64_t)state["sourceSample"];
            if (elapsed < 8000 && now - lastDesignerInput >= 25)
            {
                int stage = elapsed < 4000 ? 0 : 1;
                designer->benchmarkGesture(stage, (stage == 0 ? 20 : 35) * std::sin(elapsed * .003));
                lastDesignerInput = now;
                if (ai.coverage.pending > 0 || view.snapshot.progress.activeWorkers > 0)
                    ++editingIndexOverlap;
            }
            else if (elapsed >= 8000 && !designerGestureEnded)
            {
                designer->benchmarkEndGesture();
                designerGestureEnded = true;
            }
            if (!waiting && index < count)
                dispatch();
            if (index >= count && run < 2)
            {
                ++run;
                index = 0;
            }
            else if (index >= count && elapsed >= 12000)
            {
                record("eightLayers", (int)state["designLayers"] == 8);
                record("noUnderruns",
                       (int64_t)state["underruns"] == (int64_t)controls["audioAtStart"]["underruns"]);
                record("editingOverlappedIndexing", editingIndexOverlap > 0);
                controls.getDynamicObject()->setProperty("editingSamplesWithIndexing", editingIndexOverlap);
                controls.getDynamicObject()->setProperty("playbackWallMs", elapsed);
                complete();
            }
        }
        return;
    }
    if (mode == "semantic-designer")
    {
        if (!ai.ready)
            return;
        if (phase == 0)
        {
            if (!designer || designer->session.working ||
                designer->session.document.activeMix().layers.size() != 8)
                return;
            semanticDemoId = "design:" + designer->session.document.id;
            view.audio.gain(.05f);
            designer->session.play();
            phase = 1;
            return;
        }
        auto state = view.audio.status();
        if (phase == 1 && (bool)state["playing"] && state["id"].toString() == semanticDemoId)
        {
            audioDispatch = now;
            phase = 2;
            workload = "designer-search";
            controls.getDynamicObject()->setProperty("audioAtStart", state);
        }
        if (phase == 2)
        {
            if (!(bool)state["playing"] || state["id"].toString() != semanticDemoId)
            {
                save(false, "Designer transport interrupted during search");
                return;
            }
            if (!waiting && index < count)
                dispatch();
            if (index >= count && run < 2)
            {
                ++run;
                index = 0;
            }
            else if (index >= count && now - audioDispatch >= 30000)
            {
                record("eightLayers", (int)state["designLayers"] == 8);
                record("noUnderruns",
                       (int64_t)state["underruns"] == (int64_t)controls["audioAtStart"]["underruns"]);
                controls.getDynamicObject()->setProperty("playbackWallMs", now - audioDispatch);
                complete();
            }
        }
        return;
    }
    if (mode == "semantic-capacity")
    {
        if (!ai.ready || ai.coverage.regions < (int)plan["expectedRegions"])
            return;
        if (phase == 0 && (!ai.indexReady || ai.indexBuilding))
            return; // Cold construction/opening is separate.
        if (phase == 0)
        {
            workload = "capacity-warm";
            phase = 1;
        }
        if (phase == 1 || phase == 3)
        {
            if (phase == 3 && !(bool)view.audio.status()["playing"])
            {
                view.audio.play("capacity-audition", juce::File(plan["auditionFile"].toString()));
                return;
            }
            if (!waiting && index < count)
                dispatch();
            if (index >= count)
            {
                index = 0;
                if (++run < 3)
                    return;
                run = 0;
                if (phase == 1)
                {
                    phase = 2;
                    view.audio.play("capacity-audition", juce::File(plan["auditionFile"].toString()));
                }
                else if (plan["updateFolder"].toString().isNotEmpty())
                {
                    view.library.addFolder(juce::File(plan["updateFolder"].toString()));
                    phase = 4;
                }
                else
                {
                    view.audio.stop();
                    complete();
                }
            }
        }
        if (phase == 2 && (bool)view.audio.status()["playing"])
        {
            workload = "capacity-audition";
            phase = 3;
        }
        if (phase == 4 && ai.coverage.regions > (int)plan["expectedRegions"] && ai.coverage.pending > 0)
        {
            workload = "capacity-updates";
            index = 0;
            run = 0;
            phase = 5;
        }
        if (phase == 5)
        {
            if (!(bool)view.audio.status()["playing"])
            {
                view.audio.play("capacity-audition", juce::File(plan["auditionFile"].toString()));
                return;
            }
            if (!waiting && index < count)
                dispatch();
            if (index >= count)
            {
                view.audio.stop();
                controls.getDynamicObject()->setProperty("newRegionsSearchable",
                                                         ai.coverage.regions > (int)plan["expectedRegions"]);
                complete();
            }
        }
        return;
    }
    if (mode == "semantic-reopen" || mode == "semantic-offline")
    {
        if (!analyzed)
            return;
        workload = mode;
        run = 0;
        if (!waiting && index < count)
            dispatch();
        if (index >= count)
        {
            record("completedEmbeddingsReused", ai.windowsThisSession == 0);
            record("completedHeadersReused", view.snapshot.headersRead == 0);
            complete();
        }
        return;
    }
    const double workflowTimeout =
        plan["workflowTimeoutMs"].isVoid() ? 180000 : (double)plan["workflowTimeoutMs"];
    if (now - startMs > workflowTimeout)
    {
        save(false, "Semantic UI workflow timed out");
        return;
    }
    if (phase == 0)
    {
        if (!ai.ready || ai.coverage.regions < 1 || ai.coverage.pending < 1)
            return;
        view.pauseIndex.triggerClick();
        phase = 100;
        return;
    }
    if (phase == 100)
    {
        if (!view.snapshot.progress.paused)
            return;
        pausedAiWindows = ai.windowsThisSession;
        record("pausedWithPendingAnalysis", ai.coverage.pending > 0);
        controlRequest = view.benchmarkSemanticSearch("Rain pattering steadily outside.");
        phase = 101;
        return;
    }
    if (phase == 101)
    {
        if (view.snapshot.semanticCompletedRequest != controlRequest)
            return;
        record("semanticQueryWhilePaused",
               ai.windowsThisSession == pausedAiWindows && !view.snapshot.results.empty());
        view.table.selectRow(0);
        view.playWhole.triggerClick();
        phase = 102;
        return;
    }
    if (phase == 102)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"])
            return;
        semanticAudioFrames = (juce::int64)state["frames"];
        controlRequest = view.benchmarkSemanticSearch("A fire crackling and popping.");
        phase = 103;
        return;
    }
    if (phase == 103)
    {
        if (view.snapshot.semanticCompletedRequest != controlRequest)
            return;
        auto state = view.audio.status();
        if ((juce::int64)state["frames"] <= semanticAudioFrames)
            return;
        record("semanticAndAuditionWhilePaused", (bool)state["playing"] && view.snapshot.progress.paused &&
                                                     ai.windowsThisSession == pausedAiWindows);
        view.audio.stop();
        view.pauseIndex.triggerClick();
        controlRequest = view.benchmarkSearch(plan["longFilename"].toString());
        phase = 1;
        return;
    }
    if (phase == 1 || phase == 7)
    {
        if (view.snapshot.request != controlRequest || !view.rowAsset(0) ||
            view.rowAsset(0)->name != plan["longFilename"].toString())
            return;
        view.table.selectRow(0);
        view.playWhole.triggerClick();
        phase = phase == 1 ? 2 : 8;
        return;
    }
    if (phase == 2 || phase == 8)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || (double)state["firstCallbackMs"] <= 0)
            return;
        workload = phase == 2 ? "analyzing-and-auditioning" : "audition-semantic";
        index = 0;
        run = 0;
        phase = phase == 2 ? 3 : 9;
        return;
    }
    if (phase == 3 || phase == 6 || phase == 9)
    {
        if (!waiting && index < count)
            dispatch();
        if (index >= count)
        {
            index = 0;
            if (phase == 3)
            {
                phase = 4;
                return;
            }
            if (++run < 3)
                return;
            run = 0;
            if (phase == 6)
            {
                controlRequest = view.benchmarkSearch(plan["longFilename"].toString());
                phase = 7;
            }
            else
            {
                view.audio.stop();
                phase = 10;
            }
        }
        return;
    }
    if (phase == 4)
    {
        if (!analyzed)
            return;
        record("resumeCompleted", true);
        view.audio.stop();
        phase = 5;
        return;
    }
    if (phase == 5)
    {
        if ((bool)view.audio.status()["playing"] || (bool)view.audio.status()["pendingPlayback"])
            return;
        workload = "warm-semantic";
        index = 0;
        run = 0;
        phase = 6;
        return;
    }
    if (phase == 10)
    {
        if ((bool)view.audio.status()["playing"])
            return;
        controlRequest = view.benchmarkSemanticSearch(plan["matchQuery"].toString());
        phase = 11;
        return;
    }
    if (phase == 11)
    {
        if (view.snapshot.semanticCompletedRequest != controlRequest)
            return;
        int row = -1;
        for (size_t i = 0; i < view.snapshot.results.size(); ++i)
            if (view.snapshot.results[i].name == plan["longFilename"].toString())
                row = (int)i + view.snapshot.offset;
        if (row < 0)
        {
            save(false, "Expected long-file match is absent from first painted batch");
            return;
        }
        view.table.selectRow(row);
        const auto *asset = view.selected();
        if (!asset || asset->matches.empty())
        {
            save(false, "Long recording has no source match range");
            return;
        }
        semanticDemoId = asset->id;
        semanticDemoRate = asset->sampleRate;
        semanticRangeStart = view.regionStart;
        semanticRangeEnd = view.regionEnd;
        record("longMatchAwayFromStart",
               semanticRangeStart > 0 &&
                   semanticRangeStart <= (double)plan["eventStartSeconds"] * semanticDemoRate &&
                   semanticRangeEnd >= (double)plan["eventEndSeconds"] * semanticDemoRate);
        controls.getDynamicObject()->setProperty("matchedStartSample", (juce::int64)semanticRangeStart);
        controls.getDynamicObject()->setProperty("matchedEndSample", (juce::int64)semanticRangeEnd);
        controls.getDynamicObject()->setProperty("originalSampleRate", semanticDemoRate);
        if (asset->matches.size() > 1)
        {
            semanticOtherMoment = asset->matches[1].startSample;
            view.moments.setSelectedId(2, juce::sendNotificationSync);
            phase = 111;
            return;
        }
        save(false, "Long recording exposes no alternate moment");
        return;
    }
    if (phase == 111)
    {
        if (view.regionStart != semanticOtherMoment || view.moments.getSelectedId() != 2)
            return;
        semanticPageBefore = view.appliedPage;
        semanticJobsBefore = view.snapshot.progress.completedJobs;
        view.library.rescan();
        phase = 112;
        return;
    }
    if (phase == 112)
    {
        if (view.appliedPage == semanticPageBefore || view.snapshot.progress.activeWorkers > 0 ||
            view.snapshot.progress.completedJobs <= semanticJobsBefore)
            return;
        record("alternateMomentSurvivesRefresh",
               view.regionStart == semanticOtherMoment && view.moments.getSelectedId() == 2);
        view.moments.setSelectedId(1, juce::sendNotificationSync);
        phase = 113;
        return;
    }
    if (phase == 113)
    {
        if (view.regionStart != semanticRangeStart || view.regionEnd != semanticRangeEnd)
            return;
        view.playMatch.triggerClick();
        phase = 12;
        return;
    }
    if (phase == 12)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || state["id"].toString() != semanticDemoId)
            return;
        auto wave = view.waveform.data();
        if (!wave || wave->assetId != semanticDemoId || !wave->error.isEmpty())
            return;
        record("waveformReady", !wave->peaks.empty());
        record("playMatchUsesOriginalBounds", (int64_t)state["startSample"] == semanticRangeStart &&
                                                  (int64_t)state["endSample"] == semanticRangeEnd);
        view.rangeIn.setText(juce::String((double)plan["eventStartSeconds"] + 0.25, 6),
                             juce::dontSendNotification);
        view.rangeOut.setText(juce::String((double)plan["eventEndSeconds"] - 0.25, 6),
                              juce::dontSendNotification);
        view.rangeOut.onReturnKey();
        view.context.setToggleState(true, juce::dontSendNotification);
        view.playMatch.triggerClick();
        phase = 13;
        return;
    }
    if (phase == 13)
    {
        auto state = view.audio.status();
        auto first = (int64_t)std::llround((double)plan["eventStartSeconds"] * semanticDemoRate);
        auto last = (int64_t)std::llround((double)plan["eventEndSeconds"] * semanticDemoRate);
        if (!(bool)state["playing"] || (int64_t)state["startSample"] != first ||
            (int64_t)state["endSample"] != last)
            return;
        record("adjustedRangeAndContext", view.regionStart > first && view.regionEnd < last);
        auto position = (int64_t)std::llround(((double)plan["eventStartSeconds"] + 3) * semanticDemoRate);
        view.waveform.onSeek(position);
        semanticRangeStart = position;
        phase = 14;
        return;
    }
    if (phase == 14)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || (int64_t)state["startSample"] != semanticRangeStart)
            return;
        record("waveformSeekUsesOriginalSample", (int64_t)state["sourceSample"] >= semanticRangeStart);
        semanticAudioFrames = (juce::int64)state["frames"];
        controlRequest = view.benchmarkSemanticSearch("A horse neighing beside a trumpet playing a melody.");
        phase = 15;
        return;
    }
    if (phase == 15)
    {
        if (view.snapshot.semanticCompletedRequest != controlRequest)
            return;
        auto state = view.audio.status();
        if ((juce::int64)state["frames"] <= semanticAudioFrames)
            return;
        record("searchPreservesPinnedRegion", (bool)state["playing"] &&
                                                  state["id"].toString() == semanticDemoId &&
                                                  (int64_t)state["startSample"] == semanticRangeStart);
        controlRequest = view.benchmarkSearch(plan["longFilename"].toString());
        phase = 16;
        return;
    }
    if (phase == 16)
    {
        if (view.snapshot.request != controlRequest || !view.rowAsset(0) ||
            view.rowAsset(0)->name != plan["longFilename"].toString())
            return;
        view.table.selectRow(0);
        view.playWhole.triggerClick();
        phase = 17;
        return;
    }
    if (phase == 17)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || (int64_t)state["startSample"] != 0 ||
            (int64_t)state["endSample"] <= semanticRangeEnd)
            return;
        record("switchToWholeFile", true);
        view.audio.stop();
        workload = "evaluation";
        index = 0;
        run = 0;
        phase = 18;
        return;
    }
    if (phase == 18)
    {
        auto *queries = plan["evaluationQueries"].getArray();
        if (!queries)
        {
            save(false, "Missing evaluation queries");
            return;
        }
        if (!waiting && index < queries->size())
            dispatch();
        if (index >= queries->size())
            complete();
    }
}
} // namespace sp
namespace sp
{
void Benchmark::referenceTick(double now)
{
    if (now - startMs > 180000)
    {
        save(false, "Native reference workflow timed out at phase " + juce::String(phase));
        return;
    }
    const auto &ref = view.snapshot.reference;
    if (ref.loading && ref.completedWindows > 0 && ref.completedWindows < ref.totalWindows)
        referenceProgressSeen = true;
    auto record = [&](const juce::String &key, bool value)
    { controls.getDynamicObject()->setProperty(key, value); };
    auto submit = [&](const AudioReference &reference, bool measured = false)
    {
        view.submitReference(reference);
        if (measured)
        {
            waiting = view.requested;
            sendMs = monotonicMs();
        }
        return view.requested;
    };
    auto finished = [&] { return view.snapshot.semanticCompletedRequest == controlRequest; };
    if (phase == 0)
    {
        if (!view.snapshot.semantic.ready || view.snapshot.semantic.coverage.complete < 43 ||
            !view.snapshot.semantic.indexReady || view.snapshot.semantic.indexBuilding ||
            view.snapshot.semantic.indexRegions != view.snapshot.semantic.coverage.regions)
            return;
        controlRequest = view.benchmarkSearch("clip-029.wav");
        phase = 1;
        return;
    }
    if (phase == 1)
    {
        auto *asset = view.rowAsset(0);
        if (view.snapshot.request != controlRequest || !asset || asset->name != "clip-029.wav")
            return;
        view.table.selectRow(0);
        exampleSource = *asset;
        view.findSimilar.triggerClick();
        phase = 2;
        return;
    }
    if (phase == 2)
    {
        if (!view.similarMode || ref.reference.source.id != exampleSource.id ||
            view.snapshot.semanticCompletedRequest != view.requested)
            return;
        bool excluded = true;
        for (auto &a : view.snapshot.results)
            excluded &= a.id != exampleSource.id;
        record("indexedReferenceAndSourceExclusion", excluded && !view.snapshot.results.empty());
        view.table.selectRow(0);
        view.playMatch.triggerClick();
        phase = 3;
        return;
    }
    if (phase == 3)
    {
        if (!(bool)view.audio.status()["playing"])
            return;
        record("candidateKeepsReferencePinned", ref.reference.source.id == exampleSource.id);
        view.referencePlay.triggerClick();
        phase = 4;
        return;
    }
    if (phase == 4)
    {
        auto audio = view.audio.status();
        if (!(bool)audio["playing"] || audio["id"].toString() != "reference-" + exampleSource.id)
            return;
        record("referenceAudition", true);
        view.audio.stop();
        view.referenceIn.setText("0.100000", juce::dontSendNotification);
        view.referenceOut.setText("4.800000", juce::dontSendNotification);
        view.referenceOut.onReturnKey();
        controlRequest = view.requested;
        phase = 5;
        return;
    }
    if (phase == 5)
    {
        if (!finished())
            return;
        record("referenceRangeEdited",
               ref.reference.startSample == 4410 && ref.reference.endSample == 211680);
        auto desired = ref.reference;
        for (int n = 0; n < 100; ++n)
        {
            auto obsolete = desired;
            obsolete.startSample = n * 17;
            view.submitReference(obsolete);
        }
        controlRequest = submit(desired);
        phase = 6;
        return;
    }
    if (phase == 6)
    {
        if (!finished())
            return;
        record("latestReferenceWins", ref.reference.startSample == 4410 && ref.generation == view.requested);
        view.filesDropped({plan["externalFile"].toString()}, 0, 0);
        controlRequest = view.requested;
        phase = 7;
        return;
    }
    if (phase == 7)
    {
        if (!finished())
            return;
        record("externalFileIsTemporary", ref.reference.external && view.snapshot.roots.size() == 1 &&
                                              view.snapshot.semantic.coverage.total == 43);
        record("externalShortReference", ref.totalWindows == 1 && ref.error.isEmpty());
        view.filesDropped({plan["longFile"].toString()}, 0, 0);
        controlRequest = view.requested;
        phase = 8;
        return;
    }
    if (phase == 8)
    {
        if (!finished())
            return;
        record("longReferenceWithProgress",
               ref.totalWindows == 11 && referenceProgressSeen && ref.error.isEmpty());
        controls.getDynamicObject()->setProperty("longReferenceEncodingMs", ref.encodingMs);
        controls.getDynamicObject()->setProperty("longReferenceRetrievalMs", ref.retrievalMs);
        if ((bool)plan["transportControls"])
        {
            view.table.deselectAllRows();
            view.table.selectRow(0);
            phase = 100;
            return;
        }
        view.clearReference.triggerClick();
        phase = 9;
        return;
    }
    if (phase == 100)
    {
        if (!view.selectedAsset)
            return;
        auto wave = view.waveform.data();
        if (!wave || wave->assetId != view.selectedAsset->id || wave->peaks.empty())
            return;
        auto state = view.audio.status();
        if ((bool)state["playing"] || (bool)state["pendingPlayback"])
            return;
        controlSelectedId = view.selectedAsset->id;
        semanticDemoRate = view.selectedAsset->sampleRate;
        transportPosition = (int64_t)std::llround(1.25 * semanticDemoRate);
        transportGeneration = (uint64_t)(juce::int64)state["playGeneration"];
        view.waveform.onSeek(transportPosition);
        phaseStart = now;
        phase = 101;
        return;
    }
    if (phase == 101)
    {
        if (now - phaseStart < 250)
            return;
        auto state = view.audio.status();
        record("stoppedCandidateSeekDoesNotPlay",
               !(bool)state["playing"] && !(bool)state["pendingPlayback"] &&
                   (uint64_t)(juce::int64)state["playGeneration"] == transportGeneration);
        record("stoppedCandidateSeekMovesPlayhead", view.waveform.cursorSample() == transportPosition);
        record("noStoppedFilenameLine", view.transport.getText().isEmpty() && !view.transport.isVisible());
        view.play.triggerClick();
        phase = 102;
        return;
    }
    if (phase == 102)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || state["id"].toString() != controlSelectedId)
            return;
        record("playStartsAtParkedCandidate", view.playingSpec.positionSample == transportPosition &&
                                                  (int64_t)state["sourceSample"] >= transportPosition);
        transportGeneration = (uint64_t)(juce::int64)state["playGeneration"];
        transportPosition = (int64_t)std::llround(2.25 * semanticDemoRate);
        view.waveform.onSeek(transportPosition);
        phase = 103;
        return;
    }
    if (phase == 103)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || (uint64_t)(juce::int64)state["playGeneration"] <= transportGeneration)
            return;
        record("activeCandidateSeekKeepsPlaying", state["id"].toString() == controlSelectedId &&
                                                      view.playingSpec.positionSample == transportPosition &&
                                                      (int64_t)state["sourceSample"] >= transportPosition);
        transportGeneration = (uint64_t)(juce::int64)state["playGeneration"] + 1;
        // Same message turn: Stop intent must win even before the helper acknowledges it.
        view.play.onClick();
        view.waveform.onSeek((int64_t)std::llround(1.75 * semanticDemoRate));
        phaseStart = now;
        phase = 104;
        return;
    }
    if (phase == 104)
    {
        if (now - phaseStart < 250)
            return;
        auto state = view.audio.status();
        record("immediateStopThenSeekStaysStopped",
               !(bool)state["playing"] && !(bool)state["pendingPlayback"] &&
                   (uint64_t)(juce::int64)state["playGeneration"] == transportGeneration);
        transportPosition = (int64_t)std::llround(2.25 * ref.reference.source.sampleRate);
        view.referenceWaveform.onSeek(transportPosition);
        phaseStart = now;
        phase = 105;
        return;
    }
    if (phase == 105)
    {
        if (now - phaseStart < 250)
            return;
        auto state = view.audio.status();
        record("stoppedReferenceSeekDoesNotPlay",
               !(bool)state["playing"] && !(bool)state["pendingPlayback"] &&
                   (uint64_t)(juce::int64)state["playGeneration"] == transportGeneration);
        record("stoppedReferenceSeekMovesPlayhead",
               view.referenceWaveform.cursorSample() == transportPosition);
        view.referencePlay.triggerClick();
        phase = 106;
        return;
    }
    if (phase == 106)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || state["id"].toString() != "reference-" + ref.reference.source.id)
            return;
        record("referencePlayStartsAtParkedCursor", view.playingSpec.positionSample == transportPosition &&
                                                        (int64_t)state["sourceSample"] >= transportPosition);
        transportGeneration = (uint64_t)(juce::int64)state["playGeneration"];
        transportPosition = (int64_t)std::llround(.75 * semanticDemoRate);
        view.waveform.onSeek(transportPosition);
        phaseStart = now;
        phase = 107;
        return;
    }
    if (phase == 107)
    {
        if (now - phaseStart < 250)
            return;
        auto state = view.audio.status();
        record("inactiveWaveformSeekDoesNotRetargetAudition",
               (bool)state["playing"] && state["id"].toString() == "reference-" + ref.reference.source.id &&
                   (uint64_t)(juce::int64)state["playGeneration"] == transportGeneration &&
                   view.waveform.cursorSample() == transportPosition);
        record("noPlayingFilenameLine", view.transport.getText().isEmpty() && !view.transport.isVisible());
        transportPosition = (int64_t)std::llround(4.5 * ref.reference.source.sampleRate);
        view.referenceWaveform.onSeek(transportPosition);
        phase = 108;
        return;
    }
    if (phase == 108)
    {
        auto state = view.audio.status();
        if (!(bool)state["playing"] || (uint64_t)(juce::int64)state["playGeneration"] <= transportGeneration)
            return;
        record("activeReferenceSeekKeepsPlaying",
               state["id"].toString() == "reference-" + ref.reference.source.id &&
                   view.playingSpec.positionSample == transportPosition &&
                   (int64_t)state["sourceSample"] >= transportPosition);
        transportGeneration = (uint64_t)(juce::int64)state["playGeneration"] + 1;
        view.referencePlay.onClick();
        view.referenceWaveform.onSeek((int64_t)std::llround(3.25 * ref.reference.source.sampleRate));
        phaseStart = now;
        phase = 109;
        return;
    }
    if (phase == 109)
    {
        if (now - phaseStart < 250)
            return;
        auto state = view.audio.status();
        record("immediateReferenceStopSeekStaysStopped",
               !(bool)state["playing"] && !(bool)state["pendingPlayback"] &&
                   (uint64_t)(juce::int64)state["playGeneration"] == transportGeneration);
        view.clearReference.triggerClick();
        phase = 9;
        return;
    }
    if (phase == 9)
    {
        if (ref.present)
            return;
        record("clearReference", true);
        if ((bool)plan["controlsOnly"])
        {
            for (auto &property : controls.getDynamicObject()->getProperties())
                if (property.value.isBool() && !(bool)property.value)
                {
                    save(false, "Native transport assertion: " + property.name.toString());
                    return;
                }
            save(true);
            return;
        }
        workload = "example-uncached";
        index = 0;
        phase = 10;
        return;
    }
    if (phase == 10 || phase == 11)
    {
        int count = (int)plan["queryCount"];
        if (!waiting && index < count)
        {
            AudioReference reference;
            if (phase == 10)
            {
                reference.source = exampleSource;
                reference.startSample = 1000 + index * 37;
                reference.endSample = exampleSource.samples - 1000 - index * 23;
                exampleLast = reference;
            }
            else
                reference = exampleLast;
            submit(reference, true);
        }
        if (index >= count)
        {
            index = 0;
            if (phase == 10)
            {
                phase = 11;
                workload = "example-cached";
            }
            else
            {
                phase = 12;
                workload = "example-evaluation";
            }
        }
        return;
    }
    auto *evaluation = plan["examples"].getArray();
    if (!evaluation)
    {
        save(false, "Missing reference evaluation cases");
        return;
    }
    if (phase == 12)
    {
        if (index >= evaluation->size())
        {
            for (auto &property : controls.getDynamicObject()->getProperties())
                if (property.value.isBool() && !(bool)property.value)
                {
                    save(false, "Native Find Similar assertion: " + property.name.toString());
                    return;
                }
            view.audio.stop();
            save(true);
            return;
        }
        controlRequest = view.benchmarkSearch((*evaluation)[index]["file"].toString());
        phase = 13;
        return;
    }
    if (phase == 13)
    {
        auto *asset = view.rowAsset(0);
        auto example = (*evaluation)[index];
        if (view.snapshot.request != controlRequest || !asset || asset->name != example["file"].toString())
            return;
        view.table.selectRow(0);
        AudioReference reference;
        reference.source = *asset;
        reference.startSample = (int64_t)std::llround((double)example["startSeconds"] * asset->sampleRate);
        reference.endSample = (int64_t)std::llround((double)example["endSeconds"] * asset->sampleRate);
        submit(reference, true);
        phase = 14;
        return;
    }
    if (phase == 14 && !waiting)
        phase = 12;
}
void Benchmark::controlTick(int total, int done, double now)
{
    // A pinned/result page may deliberately retain an older membership count.
    // Compare pause invariants against one current service snapshot instead.
    const auto live = view.library.snapshot();
    total = 0;
    done = 0;
    for (const auto &root : live.roots)
    {
        total += root.discovered;
        done += root.ready + root.failed;
    }
    auto record = [&](const juce::String &key, bool value)
    { controls.getDynamicObject()->setProperty(key, value); };
    if (now - startMs > 120000)
    {
        save(false, "Native controls timed out");
        return;
    }
    if (phase == 0)
    {
        if (total < 3000)
            return;
        view.pauseIndex.triggerClick();
        phase = 1;
        return;
    }
    if (phase == 1)
    {
        if (!live.progress.paused)
            return;
        pausedCount = total;
        record("pausedDuringImport", total < (int)plan["expectedFiles"]);
        controls.getDynamicObject()->setProperty("committedAtPause", total);
        controlRequest = view.benchmarkSearch("");
        view.audio.play("controls-audition", juce::File(plan["longAudio"].toString()));
        phase = 2;
        return;
    }
    if (phase == 2)
    {
        if (lastPainted != controlRequest)
            return;
        record("clearQueryPaintedWhilePaused", view.snapshot.total == pausedCount);
        view.benchmarkScroll(1536);
        phase = 3;
        return;
    }
    if (phase == 3)
    {
        if (lastPainted != controlRequest || !view.rowAsset(1536) || lastPaintedOffset < 1000)
            return;
        record("paintedBeyondRow1000", view.snapshot.offset <= 1536 && view.getNumRows() > 1536);
        controls.getDynamicObject()->setProperty(
            "viewportTopRow", view.table.getViewport()->getViewPositionY() / view.table.getRowHeight());
        auto roots = view.snapshot.roots;
        for (int i = 0; i < 500; ++i)
            view.benchmarkSearch("discarded " + juce::String(i));
        controlRequest = view.benchmarkSearch("effect", roots.back().id);
        phase = 4;
        return;
    }
    if (phase == 4)
    {
        if (lastPainted != controlRequest)
            return;
        bool correct = !view.snapshot.results.empty();
        for (auto &row : view.snapshot.results)
            correct &= row.rootId == view.snapshot.roots.back().id;
        record("latestSourceFilterPainted", correct);
        controlRequest = view.benchmarkSearch("");
        phase = 5;
        return;
    }
    if (phase == 5)
    {
        if (lastPainted != controlRequest)
            return;
        auto state = view.audio.status();
        if (!(bool)state["playing"])
            return;
        if (!pausedFrames)
        {
            pausedFrames = (juce::int64)state["frames"];
            return;
        }
        if ((juce::int64)state["frames"] <= pausedFrames + 4800)
            return;
        record("searchAndAuditionWhilePaused", live.progress.paused && total == pausedCount);
        view.audio.stop();
        view.pauseIndex.triggerClick();
        phase = 6;
        return;
    }
    if (phase == 6)
    {
        if (done < (int)plan["expectedFiles"] || done != total || view.snapshot.progress.activeWorkers > 0)
            return;
        record("resumeCompleted", true);
        view.table.selectRow(0);
        if (auto *selected = view.selected())
            controlSelectedId = selected->id;
        if (controlSelectedId.isEmpty())
        {
            save(false, "No selected row for concurrent import check");
            return;
        }
        auto inserted = juce::File(plan["changeFolder"].toString()).getChildFile("000 selection.wav");
        if (inserted.exists() || !juce::File(plan["templateAudio"].toString()).copyFileTo(inserted))
        {
            save(false, "Cannot create isolated selection fixture");
            return;
        }
        phase = 7;
        return;
    }
    if (phase == 7)
    {
        if (total < (int)plan["expectedFiles"] + 1 || done != total || !view.snapshot.newResultsAvailable)
            return;
        auto *selected = view.selected();
        record("selectionSurvivesEarlierImport", selected && selected->id == controlSelectedId &&
                                                     view.table.getViewport()->getViewPositionY() == 0);
        controlRequest = view.benchmarkSearch("");
        phase = 8;
        return;
    }
    if (phase == 8)
    {
        if (lastPainted != controlRequest)
            return;
        auto *selected = view.selected();
        record("refreshRetainsSelectedId", selected && selected->id == controlSelectedId &&
                                               view.rowAsset(0) &&
                                               view.rowAsset(0)->name == "000 selection.wav");
        juce::File(plan["changeFolder"].toString()).getChildFile("000 selection.wav").deleteFile();
        for (auto &property : controls.getDynamicObject()->getProperties())
        {
            if (property.value.isBool() && !(bool)property.value)
            {
                save(false, "Native control assertion failed: " + property.name.toString());
                return;
            }
        }
        save(true);
    }
}
} // namespace sp
