#include "Benchmark.h"
#include "LibraryView.h"
#include "DesignerView.h"
#include "Performance.h"
#include "LibraryLayout.h"
#include <cmath>
namespace sp
{
struct Benchmark::ReliabilityRun
{
    int semanticPinUpdates = 0;
    bool semanticRefreshSeen = false;
    int stage = 0, step = 0, cycles = 0, selections = 0, fullViewports = 0, nonzeroChecks = 0,
        audioChecks = 0, indexOverlaps = 0, batches = 0;
    double activeStart = 0, cycleStart = 0, scrollAt = 0, selectionAt = 0, lastControl = 0, lastLog = 0,
           lastBatch = 0, activeEnd = 0;
    bool semantic = false, selectedReady = false, cached = false, offlineMoved = false, reconnected = false,
         offlineObserved = false, reconnectObserved = false;
    juce::String expectedSelection;
    int64_t rangeFirst = 0, rangeLast = 0, lastFrame = 0;
    juce::var initialAudio, stopAudio;
    juce::Array<juce::var> queryTimes, selectedTimes, viewportTimes, progress;
};
namespace
{
juce::var distribution(const juce::Array<juce::var> &rows, const char *field)
{
    std::vector<double> values;
    for (auto &r : rows)
        values.push_back((double)r[field]);
    std::sort(values.begin(), values.end());
    auto *o = new juce::DynamicObject();
    o->setProperty("n", (int)values.size());
    if (!values.empty())
        for (auto [k, q] : {std::pair{"p50", .5}, {"p95", .95}, {"p99", .99}})
            o->setProperty(k, values[(size_t)std::ceil(q * values.size()) - 1]);
    return juce::var(o);
}
} // namespace
juce::var Benchmark::reliabilityReport() const
{
    if (!reliability)
        return {};
    auto &r = *reliability;
    auto *o = new juce::DynamicObject();
    o->setProperty("activeMs",
                   r.activeStart ? (r.activeEnd ? r.activeEnd : monotonicMs()) - r.activeStart : 0);
    o->setProperty("audioAtStop", r.stopAudio);
    o->setProperty("cycles", r.cycles);
    o->setProperty("selectedFiles", r.selections);
    o->setProperty("semanticPinnedRefreshes", r.semanticPinUpdates);
    o->setProperty("wholeViewportsPainted", r.fullViewports);
    o->setProperty("queryToPaintMs", distribution(r.queryTimes, "ms"));
    o->setProperty("selectedWaveformMs", distribution(r.selectedTimes, "ms"));
    o->setProperty("wholeViewportMs", distribution(r.viewportTimes, "ms"));
    o->setProperty("queries", r.queryTimes);
    o->setProperty("selectedWaveforms", r.selectedTimes);
    o->setProperty("viewports", r.viewportTimes);
    o->setProperty("audioChecks", r.audioChecks);
    o->setProperty("nonzeroAudioChecks", r.nonzeroChecks);
    o->setProperty("editingSamplesDuringIndexing", r.indexOverlaps);
    o->setProperty("addedBatches", r.batches);
    o->setProperty("offlineObserved", r.offlineObserved);
    o->setProperty("reconnectObserved", r.reconnectObserved);
    o->setProperty("audioAtStart", r.initialAudio);
    o->setProperty("progress", r.progress);
    return juce::var(o);
}
void Benchmark::reliabilityPainted(uint64_t id, double now, double sent, double applied)
{
    if (!reliability || !waiting || id != waiting || completed)
        return;
    auto &r = *reliability;
    if (view.snapshot.request != waiting || (r.semantic && view.snapshot.semanticCompletedRequest != waiting))
        return;
    auto *o = new juce::DynamicObject();
    o->setProperty("ms", now - sent);
    o->setProperty("appliedMs", applied - sent);
    o->setProperty("semantic", r.semantic);
    o->setProperty("request", (juce::int64)id);
    o->setProperty("total", view.snapshot.total);
    o->setProperty("modelComplete", view.snapshot.semantic.coverage.complete);
    o->setProperty("modelPending", view.snapshot.semantic.coverage.pending);
    r.queryTimes.add(juce::var(o));
    waiting = 0;
    r.step = 1;
}
void Benchmark::reliabilityTick(double now)
{
    if (!reliability)
        reliability = std::make_shared<ReliabilityRun>();
    auto &r = *reliability;
    const double duration = std::max(1000., (double)plan["durationMs"]);
    if (now - startMs > duration + 300000)
    {
        save(false, "Reliability workload exceeded startup/active deadline");
        return;
    }
    auto state = view.audio.status();
    if ((bool)plan["recordTake"] && r.stage >= 4)
    {
        auto &session = designer->session;
        if (session.takeState == DesignSession::TakeState::incomplete)
        {
            save(false, "Sustained recording became Incomplete: " + session.message);
            return;
        }
        auto path = output.getSiblingFile("Sustained QA Take.soundplus");
        if (r.stage == 4 && session.takeState == DesignSession::TakeState::ready)
        {
            session.nameTake("Sustained recorded motion");
            session.save(path);
            r.stage = 5;
            return;
        }
        if (r.stage == 5 && session.savedFile == path && !session.dirty)
        {
            session.render();
            r.stage = 6;
            return;
        }
        if (r.stage == 6 && !session.rendering)
        {
            if (!session.lastOutput.existsAsFile())
            {
                save(false, "Sustained take render failed");
                return;
            }
            session.save(path);
            r.stage = 7;
            return;
        }
        if (r.stage == 7 && !session.dirty)
        {
            save(true);
            return;
        }
        return;
    }
    if (view.snapshot.error.isNotEmpty())
    {
        save(false, view.snapshot.error);
        return;
    }
    if (r.stage == 0)
    {
        view.audio.gain(.015f);
        view.audio.bufferSize(plan["bufferFrames"].isVoid() ? 512 : (int)plan["bufferFrames"]);
        view.library.addFolder(juce::File(plan["corpusFolder"].toString()));
        if (plan["offlineFolder"].toString().isNotEmpty())
            view.library.addFolder(juce::File(plan["offlineFolder"].toString()));
        view.benchmarkSearch("");
        r.stage = 1;
        return;
    }
    if (r.stage == 1)
    {
        int ready = 0;
        for (auto &root : view.snapshot.roots)
            ready += root.ready;
        if (ready < (int)plan["corpusFiles"] || !view.snapshot.semantic.ready ||
            (designer->session.working || designer->session.transitionPending()))
            return;
        designer->preparePerformanceBenchmark();
        designer->session.seek(0);
        if ((bool)plan["recordTake"])
            designer->session.recordTake();
        else
            designer->session.play();
        r.stage = 2;
        return;
    }
    if (r.stage == 2)
    {
        if ((bool)plan["recordTake"] && (state["takeId"].toString() != designer->session.captureId ||
                                         designer->session.takeState != DesignSession::TakeState::recording))
            return;
        if (!(bool)state["playing"] || state["id"].toString() != "design:" + designer->session.document.id)
            return;
        if ((int)state["bufferFrames"] != (plan["bufferFrames"].isVoid() ? 512 : (int)plan["bufferFrames"]) ||
            (double)state["sampleRate"] != 48000)
        {
            save(false, "Requested 48 kHz / buffer-frame device setup not active");
            return;
        }
        r.initialAudio = state;
        r.activeStart = now;
        r.lastFrame = (int64_t)state["sourceSample"];
        r.stage = 3;
        r.cycleStart = now;
        if (!(bool)plan["recordTake"])
            designer->benchmarkPerformance(0, .5);
        return;
    }
    const double elapsed = now - r.activeStart;
    if ((bool)plan["recordTake"] && (int)state["takePhase"] >= 2)
    {
        if ((int)state["takePhase"] >= 4 || (int64_t)state["takeEnd"] < (duration / 1000 - .1) * 48000 ||
            r.fullViewports < 5 || !r.offlineObserved || !r.reconnectObserved ||
            r.nonzeroChecks < r.audioChecks * .95)
        {
            save(false, "Unexpected capture end or incomplete sustained activity");
            return;
        }
        r.activeEnd = now;
        r.stopAudio = state;
        r.stage = 4;
        return;
    }
    ++r.audioChecks;
    if ((double)state["preMonitorPeak"] > 1e-8)
        ++r.nonzeroChecks;
    if (!(bool)state["playing"] || state["id"] != r.initialAudio["id"] ||
        state["helperPid"] != r.initialAudio["helperPid"] ||
        state["processGeneration"] != r.initialAudio["processGeneration"] ||
        state["playGeneration"] != r.initialAudio["playGeneration"] ||
        state["graphEpoch"] != r.initialAudio["graphEpoch"])
    {
        save(false, "Unexpected transport ownership, generation change or helper restart during active "
                    "reliability workload");
        return;
    }
    if ((int64_t)state["sourceSample"] < r.lastFrame)
    {
        save(false, "Designer source clock moved backwards");
        return;
    }
    r.lastFrame = (int64_t)state["sourceSample"];
    if ((int64_t)state["underruns"] > 0)
    {
        save(false, "Audio underrun during active reliability workload");
        return;
    }
    if (now - r.lastControl >= 25)
    {
        if ((bool)plan["recordTake"])
            designer->session.edit("Blend position",
                                   [&](auto &d)
                                   {
                                       auto &p = d.activeMix().performance;
                                       p.x = .5 + .3 * std::sin(elapsed * .001);
                                       p.y = 1 - p.x;
                                       for (int i = 0; i < 4; ++i)
                                       {
                                           p.macros[(size_t)i].value = .5 + .3 * std::sin(elapsed * .001 + i);
                                           p.macros[(size_t)i].enabled = ((int)(elapsed / 500) + i) % 4 != 0;
                                       }
                                   });
        else
            designer->benchmarkPerformance(1, .5 + .3 * std::sin(elapsed * .001));
        r.lastControl = now;
        if (view.snapshot.progress.activeWorkers > 0 || view.snapshot.semantic.coverage.pending > 0)
            ++r.indexOverlaps;
    }
    if (now - r.lastBatch >= 30000)
    {
        auto batches = plan["indexBatches"].getArray();
        if (batches && r.batches < batches->size())
            view.library.addFolder(juce::File((*batches)[r.batches++].toString()));
        r.lastBatch = now;
    }
    auto offline = juce::File(plan["offlineFolder"].toString());
    auto absent = offline.getSiblingFile(offline.getFileName() + ".disconnected");
    if (plan["offlineFolder"].toString().isNotEmpty())
    {
        if (!r.offlineMoved && elapsed > duration * .33)
        {
            r.offlineMoved = offline.moveFileTo(absent);
            view.library.rescan();
        }
        if (r.offlineMoved && !r.reconnected && elapsed > duration * .5)
        {
            r.reconnected = absent.moveFileTo(offline);
            view.library.rescan();
        }
        for (auto &root : view.snapshot.roots)
            if (root.path == offline.getFullPathName())
            {
                if (r.offlineMoved && !r.reconnected && !root.online)
                    r.offlineObserved = true;
                if (r.reconnected && root.online)
                    r.reconnectObserved = true;
            }
    }
    if (now - r.lastLog >= 10000)
    {
        auto *o = new juce::DynamicObject();
        o->setProperty("activeMs", elapsed);
        o->setProperty("cycles", r.cycles);
        o->setProperty("queries", r.queryTimes.size());
        o->setProperty("selectedReady", r.selectedTimes.size());
        o->setProperty("wholeViewports", r.fullViewports);
        o->setProperty("audio", state);
        o->setProperty("audioHealth", view.audio.diagnostics());
        o->setProperty("waveforms", view.peakPool->metrics());
        o->setProperty("process", performance.json());
        o->setProperty("coverageComplete", view.snapshot.semantic.coverage.complete);
        o->setProperty("coveragePending", view.snapshot.semantic.coverage.pending);
        o->setProperty("coverageFailed", view.snapshot.semantic.coverage.failed);
        r.progress.add(juce::var(o));
        output.getSiblingFile(output.getFileName() + ".progress.json")
            .replaceWithText(juce::JSON::toString(juce::var(o), true));
        r.lastLog = now;
    }
    if (elapsed >= duration)
    {
        if (!(bool)plan["recordTake"])
            designer->benchmarkPerformance(2, .5);
        if (r.fullViewports < 5 || r.selectedTimes.size() < 5 || r.nonzeroChecks < r.audioChecks * .95 ||
            !r.offlineObserved || !r.reconnectObserved)
        {
            save(false, "Reliability activity/whole-viewport/offline evidence incomplete");
            return;
        }
        if ((bool)plan["recordTake"])
        {
            r.activeEnd = now;
            r.stopAudio = state;
            designer->session.stopTake();
            r.stage = 4;
            return;
        }
        save(true);
        return;
    }
    if (waiting && now - view.dispatchedMs > 10000)
    {
        save(false, "Query did not paint within ten seconds");
        return;
    }
    if (r.step == 0 && !waiting)
    {
        r.semantic = (r.cycles % 4) == 3;
        auto q = r.semantic ? "The sound of an impact" : r.cycles % 2 ? "effect" : "";
        waiting = r.semantic ? view.benchmarkSemanticSearch(q) : view.benchmarkSearch(q);
        // Old visible rows must not pin the newly pending query.
        view.selectedRowsChanged(view.table.getSelectedRow());
        if ((bool)view.library.queryDiagnostics()["pinned"])
        {
            save(false, "Old displayed selection pinned a new pending query");
            return;
        }
        r.cycleStart = now;
        return;
    }
    if (r.step == 1)
    {
        if (view.snapshot.total == 0)
        {
            r.step = 4;
            return;
        }
        int target = (r.cycles * 17) % view.snapshot.total;
        view.benchmarkScroll(target);
        view.table.repaint();
        r.scrollAt = now;
        r.step = 2;
        return;
    }
    if (r.step == 2)
    {
        auto interval = visibleLibraryRows(view.table.getViewport()->getViewArea(), view.table.getRowHeight(),
                                           view.snapshot.total);
        int top = interval.getStart(), count = interval.getLength();
        int row = std::min(view.snapshot.total - 1, top + std::min(5, count - 1));
        auto *a = view.rowAsset(row);
        if (a && !a->online)
            for (int candidate = top; candidate < std::min(view.snapshot.total, top + count); ++candidate)
                if (auto *online = view.rowAsset(candidate); online && online->online)
                {
                    a = online;
                    row = candidate;
                    break;
                }
        if (!a)
        {
            view.requestPageForRow(row);
            if (now - r.scrollAt > 10000)
            {
                save(false, "Visible page did not arrive");
            }
            return;
        }
        r.cached = view.peakPool->get(*a) != nullptr;
        r.selectionAt = now;
        view.table.selectRow(row);
        view.selectedRowsChanged(row);
        if (view.selectedId != a->id)
            return;
        r.expectedSelection = a->id;
        r.rangeFirst = view.regionStart;
        r.rangeLast = view.regionEnd;
        r.selectedReady = false;
        r.semanticRefreshSeen = false;
        if (r.semantic)
            view.library.semanticNow();
        ++r.selections;
        r.step = 3;
    }
    if (r.step >= 3 && r.expectedSelection.isNotEmpty())
    {
        if (view.selectedId != r.expectedSelection || view.regionStart != r.rangeFirst ||
            view.regionEnd != r.rangeLast)
        {
            save(false, "Selected identity/range changed during pinned hold");
            return;
        }
        if (r.semantic && !r.semanticRefreshSeen && view.snapshot.newResultsAvailable)
        {
            ++r.semanticPinUpdates;
            r.semanticRefreshSeen = true;
        }
    }
    if (r.step == 3)
    {
        if (view.selectedId != r.expectedSelection || view.regionStart != r.rangeFirst ||
            view.regionEnd != r.rangeLast)
        {
            save(false, "Selected asset/range retargeted without an intentional selection");
            return;
        }
        if (!(bool)view.library.queryDiagnostics()["pinned"])
        {
            save(false, "Loaded selection is not membership-pinned");
            return;
        }
        if (!r.selectedReady)
        {
            auto d = view.waveform.data();
            if (d && d->assetId == r.expectedSelection && d->error.isEmpty())
            {
                auto *o = new juce::DynamicObject();
                o->setProperty("ms", now - r.selectionAt);
                o->setProperty("cachedAtSelection", r.cached);
                o->setProperty("id", r.expectedSelection);
                r.selectedTimes.add(juce::var(o));
                r.selectedReady = true;
            }
        }
        auto interval = visibleLibraryRows(view.table.getViewport()->getViewArea(), view.table.getRowHeight(),
                                           view.snapshot.total);
        int top = interval.getStart(), count = interval.getLength();
        int visible = 0, painted = 0, unavailable = 0;
        for (int i = top; i < std::min(view.snapshot.total, top + count); ++i)
        {
            ++visible;
            auto *a = view.rowAsset(i);
            if (a && view.rowPreviewPaintTimes.contains(a->id) &&
                view.rowPreviewPaintTimes[a->id] >= r.scrollAt)
                ++painted;
            else if (a && view.rowUnavailablePaintTimes.contains(a->id) &&
                     view.rowUnavailablePaintTimes[a->id] >= r.scrollAt)
                ++unavailable;
        }
        if (visible > 0 && painted + unavailable == visible && r.selectedReady)
        {
            auto *o = new juce::DynamicObject();
            o->setProperty("ms", now - r.scrollAt);
            o->setProperty("visibleRows", visible);
            o->setProperty("paintedRows", painted + unavailable);
            o->setProperty("readyWaveformRows", painted);
            o->setProperty("offlineOrErrorRows", unavailable);
            r.viewportTimes.add(juce::var(o));
            ++r.fullViewports;
            r.step = 4;
        }
        else if (now - r.scrollAt > 10000)
        {
            save(false, "Entire visible waveform viewport/selected preview did not become ready");
            return;
        }
    }
    if (r.step == 4 && now - r.cycleStart >= std::max(100., (double)plan["cycleMs"]))
    {
        ++r.cycles;
        r.step = 0;
    }
}
} // namespace sp
