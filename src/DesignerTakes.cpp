#include "DesignerView.h"
namespace sp
{
void DesignerView::refreshTakes()
{
    bool previous = updating;
    updating = true;
    juce::String signature = "Live Design";
    for (auto &t : session.document.takes)
        signature += t.id + t.name + (t.complete ? "Ready" : "Incomplete") + (t.kept ? "Kept" : "");
    if (takeContext.getProperties()["signature"].toString() != signature)
    {
        takeContext.getProperties().set("signature", signature);
        takeContext.clear(juce::dontSendNotification);
        takeContext.addItem("Live Design", 1);
        for (size_t i = 0; i < session.document.takes.size(); ++i)
        {
            auto &t = session.document.takes[i];
            takeContext.addItem("Take: " + t.name +
                                    (t.complete ? (t.kept ? " · Kept" : " · Ready") : " · Incomplete"),
                                (int)i + 2);
        }
    }
    int selected = 1;
    for (size_t i = 0; i < session.document.takes.size(); ++i)
        if (session.document.takes[i].id == session.document.activeTake)
            selected = (int)i + 2;
    takeContext.setSelectedId(selected, juce::dontSendNotification);
    takeContext.setTooltip(takeContext.getText());
    takeContext.setEnabled(!session.takeBusy());
    recordTake.setTooltip(hasHostedInserts(session.document.activeMix())
                              ? "Hosted insert capture is not yet qualified; render a durable pass instead"
                              : "Record a performance take");
    recordTake.setEnabled(!hasHostedInserts(session.document.activeMix()) && !session.takeBusy() &&
                          session.document.activeTake.isEmpty() &&
                          !session.document.activeMix().layers.empty() && !session.rendering &&
                          !session.working && !session.transitionPending());
    stopTake.setEnabled(session.takeBusy());
    stopTake.setButtonText(session.takeState == DesignSession::TakeState::arming ? "Cancel arming take"
                                                                                 : "Stop and finalize take");
    auto *t = selectedTake(session.document);
    keepTake.setEnabled(t && !session.takeBusy());
    renderTake.setEnabled(t && t->complete && !session.rendering && !session.takeBusy());
    auto state = audio.status();
    juce::String text;
    switch (session.takeState)
    {
    case DesignSession::TakeState::arming:
        text = "Arming";
        break;
    case DesignSession::TakeState::recording:
    {
        auto rate = session.document.sampleRate;
        auto seconds = (int)((int64_t)state["takeEnd"] / rate);
        text =
            "Recording " + juce::String(seconds / 60) + ":" + juce::String(seconds % 60).paddedLeft('0', 2);
        break;
    }
    case DesignSession::TakeState::finalizing:
        text = "Finalizing";
        break;
    case DesignSession::TakeState::incomplete:
        text = "Incomplete";
        break;
    case DesignSession::TakeState::ready:
        if (t)
            text = "Ready";
        break;
    default:
        break;
    }
    takeStatus.setText(text, juce::dontSendNotification);
    takeStatus.setTitle(text);
    const bool takeContextActive = t != nullptr;
    play.setTooltip(
        takeContextActive
            ? "Audition / Stop (Space). Replay this take from frame zero with complete effect history."
            : "Play / Stop (Space). Live seeks reset effect history with 250 ms of pre-roll. Use Accurate "
              "audition to judge long tails.");
    accurate.setButtonText(takeContextActive ? "Audition take from frame zero"
                                             : "Accurate audition from playhead");
    accurate.setTooltip(takeContextActive
                            ? "Rebuild the frozen take from frame zero, including its full effect history."
                            : "Accurate audition from playhead: rebuild the current recipe's effect history "
                              "from the beginning. Preparation may take longer; Stop cancels. Use this after "
                              "changing long delay or reverb tails.");
    takeStatus.setTooltip(t && t->processing == "stock-layout-takes-v1"
                              ? "Original spatial engine, frozen. Take options can create a corrected "
                                "spatial copy; the original and its outputs remain unchanged."
                          : t && !t->complete
                              ? t->reason
                              : "Take capture starts at frame zero; replay and render rebuild "
                                "complete history. 120 seconds /8192 applied events maximum.");
    updating = previous;
}
void DesignerView::nameTakeDialog()
{
    auto *t = selectedTake(session.document);
    if (!t || session.takeBusy())
        return;
    auto token = session.beginDialog();
    auto id = t->id;
    auto *dialog = new juce::AlertWindow("Keep / rename take",
                                         "The frozen recipe and captured movement remain unchanged.",
                                         juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", t->name, "Take name");
    dialog->addButton("Keep", 1);
    dialog->addButton("Cancel", 0);
    juce::Component::SafePointer<DesignerView> safe(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [safe, dialog, token, id](int result)
                                {
                                    if (result && safe && safe->session.accepts(token) &&
                                        safe->session.document.activeTake == id)
                                        safe->session.nameTake(dialog->getTextEditorContents("name"));
                                }),
                            true);
}
void DesignerView::takePopup()
{
    juce::PopupMenu menu;
    if (session.takeBusy())
        menu.addItem(1, "Cancel draft take");
    if (auto *t = selectedTake(session.document))
    {
        menu.addItem(2, "Delete take (Undo available)", !session.takeBusy());
        if (t->processing == "stock-layout-takes-v1")
            menu.addItem(3, "Create corrected spatial copy (v2)",
                         t->complete && !session.takeBusy() && !session.working && !session.rendering);
        juce::PopupMenu sources;
        for (size_t i = 0; i < t->sources.size(); ++i)
        {
            auto &s = t->sources[i];
            auto l = std::find_if(t->recipe->layers.begin(), t->recipe->layers.end(),
                                  [&](auto &v) { return v.source.id == s.id; });
            sources.addItem(100 + (int)i, "Locate identical source: " +
                                              (l != t->recipe->layers.end() ? l->source.name : s.id));
        }
        menu.addSubMenu("Verified source relink", sources, !session.takeBusy());
    }
    menu.addSectionHeader("Frame zero · fresh history");
    juce::Component::SafePointer<DesignerView> safe(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&takeOptions),
        [safe](int result)
        {
            if (!safe)
                return;
            if (result == 1)
                safe->session.stopTake(true);
            if (result == 2)
                safe->session.deleteTake();
            if (result == 3)
                safe->session.createCorrectedSpatialCopy();
            if (result >= 100)
            {
                auto *t = selectedTake(safe->session.document);
                auto i = result - 100;
                if (!t || i >= (int)t->sources.size())
                    return;
                auto source = t->sources[(size_t)i].id;
                auto id = t->id;
                safe->chooser = std::make_unique<juce::FileChooser>("Locate the identical recorded source");
                safe->chooser->launchAsync(
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                    [safe, source, id](const auto &c)
                    {
                        if (safe && safe->session.document.activeTake == id && c.getResult().existsAsFile())
                            safe->session.relinkTakeSource(source, c.getResult());
                    });
            }
        });
}
} // namespace sp
