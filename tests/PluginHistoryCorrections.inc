// H: actual native publications must merge before truncation at capacity.
for (int i = 0; LiveCorrectionTestAccess::history(session) < 99; ++i)
    session.edit(
        "History filler " + String(i), [&](auto &d) { d.name = "History fixture " + String(i); },
        DesignSession::ChangeImpact::metadata);
require(LiveCorrectionTestAccess::history(session) == 99, "History reaches 99 before native gesture");
const auto beforeHistoryGain = value(false);
auto nativeHistory = [&](int sequence, double x, const char *stage)
{
    auto v = obj();
    put(v, "sequence", sequence);
    put(v, "index", 0);
    put(v, "value", x);
    put(v, "gestureStage", stage);
    saveProcessingJson(root.getChildFile(trackEndpoint["pid"].toString() + ".json"), v);
    wait([&] { return std::abs(value(false) - x) < 1e-5; }, "Native history gesture is actually published");
};
nativeHistory(500, .315, "begin");
require(LiveCorrectionTestAccess::history(session) == 100, "First native gesture reaches history capacity");
auto oldest = LiveCorrectionTestAccess::oldest(session);
nativeHistory(501, .325, "continue");
nativeHistory(502, .335, "end");
require(LiveCorrectionTestAccess::history(session) == 100 &&
            LiveCorrectionTestAccess::oldest(session) == oldest,
        "Native gesture merge/ACK at capacity neither drops old history nor pops its real Undo entry");
session.undo();
wait([&] { return std::abs(value(false) - beforeHistoryGain) < 1e-5; },
     "Merged native Undo restores gesture start");
session.redo();
wait([&] { return std::abs(value(false) - .335) < 1e-5; }, "Merged native Redo restores gesture end");
session.edit(
    "Intervening real edit", [](auto &d) { d.name = "Intervening real edit"; },
    DesignSession::ChangeImpact::metadata);
nativeHistory(503, .345, "begin");
oldest = LiveCorrectionTestAccess::oldest(session);
nativeHistory(504, .355, "continue");
nativeHistory(505, .365, "end");
require(LiveCorrectionTestAccess::history(session) == 100 &&
            LiveCorrectionTestAccess::oldest(session) == oldest,
        "Beyond-capacity native continuation preserves exactly one eviction for its initial push");
session.undo();
require(session.document.name == "Intervening real edit" && std::abs(value(false) - .335) < 1e-5,
        "Native Undo retains intervening real edit boundary");
session.redo();
