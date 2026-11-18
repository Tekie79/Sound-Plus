#include "Benchmark.h"
#include "LibraryView.h"
#include "Performance.h"
namespace sp
{
Benchmark::Benchmark(LibraryView &v, const juce::File &file, const juce::File &out)
    : view(v), output(out), plan(juce::JSON::parse(file)), startMs(monotonicMs()), phaseStart(startMs)
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
    auto *queries = plan["queries"].getArray();
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
    waiting = view.benchmarkSearch(q["text"].toString(), root);
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
    o->setProperty("controls", controls);
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
            performance.discoveryFinishedMs.load() - performance.firstEligibleMs.load());
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
            view.audio.play("benchmark-long", juce::File(plan["longAudio"].toString()));
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
void Benchmark::controlTick(int total, int done, double now)
{
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
        if (!view.snapshot.progress.paused)
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
        record("searchAndAuditionWhilePaused", view.snapshot.progress.paused && total == pausedCount);
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
            view.rowAsset(0) && view.rowAsset(0)->name == "000 selection.wav");
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
