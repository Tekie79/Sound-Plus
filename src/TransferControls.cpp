#include "TransferControls.h"

namespace sp
{
namespace
{
juce::var command(const char *name)
{
    auto value = juce::var(new juce::DynamicObject());
    value.getDynamicObject()->setProperty("operation", name);
    return value;
}
juce::String scopeKey(int workspace)
{
    return "transfer.scope." + juce::String(workspace);
}
} // namespace

TransferController::TransferController(const juce::File &catalog)
    : receipts(catalog.getParentDirectory().getChildFile("transfer-receipts")),
      bridge(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()
                 .getParentDirectory().getChildFile("Resources/resolve-transfer.py"))
{
}
TransferController::~TransferController()
{
    cancelled = true;
    if (worker.joinable())
        worker.join();
}
void TransferController::update(const std::function<void(State &)> &change)
{
    std::lock_guard lock(mutex);
    change(state);
}
TransferController::State TransferController::snapshot() const
{
    std::lock_guard lock(mutex);
    return state;
}
bool TransferController::reserve(int workspace, const juce::String &selectionKey)
{
    std::lock_guard lock(mutex);
    if (state.running)
        return false;
    cancelled = false;
    retryRequest.reset();
    state = {};
    state.workspace = workspace;
    state.selectionKey = selectionKey;
    state.running = state.cancellable = true;
    state.status = "Preparing";
    return true;
}
void TransferController::fail(const juce::String &error)
{
    update([&](auto &s)
    {
        s.status = cancelled ? "Cancelled" : "Failed";
        s.error = cancelled ? juce::String() : error;
        s.running = s.cancellable = false;
    });
}
void TransferController::cancel()
{
    std::lock_guard lock(mutex);
    if (state.cancellable)
        cancelled = true;
}
void TransferController::start(TransferRequest request)
{
    if (cancelled)
    {
        fail({});
        return;
    }
    if (worker.joinable())
        worker.join();
    worker = std::thread([this, request = std::move(request)]() mutable { execute(std::move(request)); });
}
void TransferController::retry(int track)
{
    std::optional<TransferRequest> request;
    TransferArtifact artifact;
    {
        std::lock_guard lock(mutex);
        if (state.running || !state.canRetry || !retryRequest)
            return;
        request = std::move(retryRequest);
        artifact = state.artifact;
        request->resolveTrack = track;
        cancelled = false;
        state.running = state.cancellable = true;
        state.canRetry = false;
        state.status = "Preparing";
        state.error.clear();
    }
    if (worker.joinable())
        worker.join();
    worker = std::thread([this, request = std::move(*request), artifact = std::move(artifact)]() mutable
                         { execute(std::move(request), std::move(artifact)); });
}
void TransferController::execute(TransferRequest r, TransferArtifact prepared)
{
    juce::var record;
    bool safeRetry = false;
    const auto stop = [this] { return cancelled.load(); };
    try
    {
        juce::var target;
        juce::String connectionError;
        if (r.action == TransferAction::resolve)
        {
            try
            {
                target = callResolveTransfer(bridge, command("probe"), stop);
                if (!(bool)target["ok"])
                    connectionError = target["error"].toString();
            }
            catch (const QueryCancelled &) { throw; }
            catch (const std::exception &error) { connectionError = juce::String::fromUTF8(error.what()); }
        }
        const bool retrying = prepared.file != juce::File();
        const auto artifact = retrying ? prepared : prepareTransfer(r, stop);
        record = transferReceipt(r, artifact);
        auto *fields = record.getDynamicObject();
        if (retrying)
        {
            const auto old = juce::JSON::parse(receipts.getChildFile(r.id + ".json"));
            juce::Array<juce::var> attempts;
            if (const auto *previous = old["previousAttempts"].getArray())
                attempts = *previous;
            auto attempt = juce::var(new juce::DynamicObject());
            for (auto key : {"date", "status", "error", "target", "receiver"})
                attempt.getDynamicObject()->setProperty(key, old[key]);
            attempts.add(attempt);
            fields->setProperty("previousAttempts", std::move(attempts));
        }
        fields->setProperty("status", artifact.copied ? "Copied" : "Prepared");
        writeTransferReceipt(receipts, r.id, record);
        update([&](auto &s)
        {
            s.artifact = artifact;
            s.receipt = receipts.getChildFile(r.id + ".json");
            s.status = artifact.copied ? "Copied" : "Preparing";
        });
        if (stop())
            throw QueryCancelled();
        if (r.action == TransferAction::resolve)
        {
            if (connectionError.isNotEmpty())
            {
                safeRetry = true;
                throw std::runtime_error(connectionError.toStdString());
            }
            verifyTransferArtifact(artifact);
            bool proceed = false;
            update([&](auto &s)
            {
                proceed = !stop();
                s.cancellable = false;
                s.status = proceed ? "Waiting for Resolve" : "Cancelled";
            });
            if (!proceed)
                throw QueryCancelled();
            fields->setProperty("status", "Waiting for Resolve");
            fields->setProperty("target", target);
            writeTransferReceipt(receipts, r.id, record);
            auto request = record.clone();
            auto *toResolve = request.getDynamicObject();
            toResolve->setProperty("operation", "place");
            toResolve->setProperty("track", r.resolveTrack);
            auto result = callResolveTransfer(bridge, request, stop);
            fields->setProperty("receiver", result);
            if (!(bool)result["ok"] || result["item"].toString().isEmpty())
            {
                safeRetry = !(bool)result["placementAttempted"];
                auto message = result["error"].toString();
                if (result["observedTimelineItem"].toString().isNotEmpty())
                    message += " Inspect the observed timeline item before retrying.";
                else if (result["media"].toString().isNotEmpty())
                    message += " Imported media was retained.";
                throw std::runtime_error(message.isNotEmpty() ? message.toStdString() : "Placement was not verified");
            }
            fields->setProperty("status", "Placed");
        }
        else
            fields->setProperty("status", "Ready to drag");
        writeTransferReceipt(receipts, r.id, record);
        update([&](auto &s)
        {
            s.status = record["status"].toString();
            s.running = s.cancellable = false;
        });
    }
    catch (const std::exception &error)
    {
        auto explanation = juce::String::fromUTF8(error.what());
        if (auto *fields = record.getDynamicObject())
        {
            fields->setProperty("status", stop() ? "Cancelled" : "Failed");
            fields->setProperty("error", explanation);
            try { writeTransferReceipt(receipts, r.id, record); }
            catch (...) { explanation += " The latest receipt could not be saved."; }
        }
        if (safeRetry && !stop())
        {
            std::lock_guard lock(mutex);
            retryRequest = std::move(r);
            state.status = "Failed";
            state.error = explanation;
            state.canRetry = true;
            state.running = state.cancellable = false;
        }
        else
            fail(explanation);
    }
}

std::vector<TransferScope> TransferPanel::scopes(int workspace)
{
    using S = TransferScope;
    if (workspace == 0) return {S::sourceRange, S::original};
    if (workspace == 1) return {S::track, S::trackRange, S::mixRange, S::clip, S::mix};
    if (workspace == 2) return {S::pad, S::padRange, S::allPads, S::take};
    return {};
}
TransferScope TransferPanel::defaultScope(int workspace)
{
    return workspace == 0 ? TransferScope::sourceRange : workspace == 1 ? TransferScope::track : TransferScope::pad;
}
TransferScope TransferPanel::preferredScope(const juce::PropertiesFile &prefs, int workspace)
{
    auto value = (TransferScope)prefs.getIntValue(scopeKey(workspace), (int)defaultScope(workspace));
    auto choices = scopes(workspace);
    return std::find(choices.begin(), choices.end(), value) == choices.end() ? defaultScope(workspace) : value;
}
bool TransferPanel::projectRange(TransferScope s)
{
    return s == TransferScope::trackRange || s == TransferScope::mixRange || s == TransferScope::padRange;
}
TransferPanel::TransferPanel(juce::PropertiesFile &p, TransferController &s, int w, juce::String context,
                             const std::function<bool(TransferScope)> &available, int64_t in, int64_t out,
                             Submit send)
    : prefs(p), service(s), workspace(w), submitRequest(std::move(send))
{
    setTitle("Workspace transfer");
    setSize(404, 368);
    for (auto *c : std::initializer_list<juce::Component *>{&heading, &selected, &selectionLabel,
         &actionLabel, &rangeLabel, &trackLabel, &status, &filename, &selection, &action, &first, &last,
         &track, &copy, &folder, &submit, &cancelButton, &drag, &reveal, &retryButton})
        addAndMakeVisible(c);
    heading.setFont(juce::FontOptions(16.f, juce::Font::bold));
    heading.setText(w == 3 ? "Transfer receipts" : "Transfer options", juce::dontSendNotification);
    selected.setText(context, juce::dontSendNotification);
    selectionLabel.setText("Selection", juce::dontSendNotification);
    actionLabel.setText("Default action", juce::dontSendNotification);
    rangeLabel.setText("Project frames", juce::dontSendNotification);
    trackLabel.setText("Track override", juce::dontSendNotification);
    selection.setTitle("Transfer selection scope");
    action.setTitle("Default transfer action");
    first.setTitle("First project frame inclusive");
    last.setTitle("Last project frame exclusive");
    track.setTitle("Optional Resolve audio track number for this send");
    first.setText(juce::String(in));
    last.setText(juce::String(out));
    first.setInputRestrictions(14, "0123456789");
    last.setInputRestrictions(14, "0123456789");
    track.setInputRestrictions(3, "0123456789");
    track.setTextToShowWhenEmpty("Active audio track (Fairlight)", ui::muted);
    if (prefs.getIntValue("transfer.track") > 0)
        track.setText(juce::String(prefs.getIntValue("transfer.track")));
    auto defaultChoice = preferredScope(prefs, w);
    int fallback = 0;
    for (auto scope : scopes(w))
    {
        selection.addItem(transferScopeName(scope), (int)scope);
        const bool allowed = available(scope);
        selection.setItemEnabled((int)scope, allowed);
        if (allowed && fallback == 0)
            fallback = (int)scope;
    }
    selection.setSelectedId(w != 3 && available(defaultChoice) ? (int)defaultChoice : fallback,
                            juce::dontSendNotification);
    action.addItem("Send to Resolve at playhead", (int)TransferAction::resolve);
    action.addItem("Drag WAV", (int)TransferAction::drag);
    action.setSelectedId(prefs.getIntValue("transfer.action", (int)TransferAction::drag), juce::dontSendNotification);
    copy.setToggleState(prefs.getBoolValue("transfer.saveCopy"), juce::dontSendNotification);
    action.onChange = [this] { prefs.setValue("transfer.action", action.getSelectedId()); updateOptions(); };
    selection.onChange = [this] { prefs.setValue(scopeKey(workspace), selection.getSelectedId()); updateOptions(); };
    copy.onClick = [this]
    {
        folderError.clear();
        prefs.setValue("transfer.saveCopy", copy.getToggleState());
        updateOptions();
    };
    folder.onClick = [this] { chooseFolder(); };
    cancelButton.onClick = [this] { service.cancel(); };
    retryButton.onClick = [this]
    {
        const auto target = workspace == 3 ? prefs.getIntValue("transfer.track") : track.getText().getIntValue();
        prefs.setValue("transfer.track", target);
        service.retry(target);
    };
    reveal.setButtonText(workspace == 3 ? "Open receipts" : "Show receipt");
    reveal.onClick = [this]
    {
        auto file = workspace == 3 ? service.receiptDirectory() : service.snapshot().receipt;
        if (file.exists())
            file.revealToUser();
    };
    submit.onClick = [this]
    {
        prefs.setValue("transfer.action", action.getSelectedId());
        prefs.setValue(scopeKey(workspace), selection.getSelectedId());
        prefs.setValue("transfer.track", track.getText().getIntValue());
        submitRequest((TransferScope)selection.getSelectedId(), (TransferAction)action.getSelectedId(),
                      copy.getToggleState(), first.getText().getLargeIntValue(), last.getText().getLargeIntValue(),
                      track.getText().getIntValue());
        timerCallback();
    };
    drag.setTooltip("Drag the verified file to the receiving app");
    drag.resolveFile = [this]
    {
        auto state = service.snapshot();
        return !state.running && transferArtifactUnchanged(state.artifact) ? state.artifact.file : juce::File();
    };
    status.setTitle("Transfer status");
    updateOptions();
    timerCallback();
    startTimerHz(12);
}
void TransferPanel::chooseFolder()
{
    folderError.clear();
    auto old = prefs.getValue("transfer.copyDisplay");
    chooser = std::make_unique<juce::FileChooser>("Choose where to save transfer copies",
                          old.isNotEmpty() ? juce::File(old) : juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                          juce::String(), true);
    const auto safe = juce::Component::SafePointer<TransferPanel>(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe](const auto &window)
        {
            if (!safe || window.getResult() == juce::File())
                return;
            try
            {
                safe->prefs.setValue("transfer.copyBookmark", makeTransferFolderBookmark(window.getResult()));
                safe->prefs.setValue("transfer.copyDisplay", window.getResult().getFullPathName());
                safe->updateOptions();
                safe->timerCallback();
            }
            catch (const std::exception &error)
            {
                safe->folderError = juce::String::fromUTF8(error.what());
                safe->timerCallback();
            }
        });
}
void TransferPanel::updateOptions()
{
    const bool edit = workspace != 3;
    const bool ranged = edit && projectRange((TransferScope)selection.getSelectedId());
    const bool resolve = edit && action.getSelectedId() == (int)TransferAction::resolve;
    for (auto *c : std::initializer_list<juce::Component *>{&selectionLabel, &selection, &actionLabel, &action, &copy, &submit})
        c->setVisible(edit);
    rangeLabel.setVisible(ranged);
    first.setVisible(ranged);
    last.setVisible(ranged);
    track.setVisible(resolve);
    trackLabel.setVisible(resolve);
    folder.setVisible(edit && copy.getToggleState());
    const auto path = prefs.getValue("transfer.copyDisplay");
    folder.setButtonText(path.isEmpty() ? "Choose folder" : juce::File(path).getFileName());
    folder.setTooltip("Choose copy folder");
    submit.setButtonText(resolve ? "Send" : "Prepare drag");
    resized();
}
void TransferPanel::timerCallback()
{
    auto state = service.snapshot();
    const bool show = workspace == 3 || workspace == state.workspace;
    const bool have = show && state.artifact.file != juce::File();
    status.setText(folderError.isNotEmpty() ? folderError : show && state.status.isNotEmpty() ?
        state.status + (state.error.isEmpty() ? juce::String() : "\n" + state.error) :
        (workspace == 3 ? "No transfers in this session" : juce::String()), juce::dontSendNotification);
    status.setTooltip(state.error);
    filename.setText(have ? state.artifact.file.getFileName() : juce::String(), juce::dontSendNotification);
    drag.setVisible(have && !state.running);
    reveal.setVisible(workspace == 3 || (show && state.receipt != juce::File()));
    reveal.setEnabled(workspace != 3 || service.receiptDirectory().isDirectory());
    folder.setEnabled(!state.running);
    retryButton.setVisible(show && state.canRetry);
    cancelButton.setVisible(show && state.running && state.cancellable);
    submit.setEnabled(!state.running && selection.getSelectedId() > 0 &&
        (!copy.getToggleState() || prefs.getValue("transfer.copyBookmark").isNotEmpty()));
}
void TransferPanel::paint(juce::Graphics &g) { g.fillAll(ui::surface); }
void TransferPanel::resized()
{
    auto space = getLocalBounds().reduced(12);
    heading.setBounds(space.removeFromTop(28));
    selected.setBounds(space.removeFromTop(22));
    auto row = [&](auto &label, auto &component)
    {
        auto line = space.removeFromTop(29);
        label.setBounds(line.removeFromLeft(108));
        component.setBounds(line);
        space.removeFromTop(5);
    };
    if (workspace != 3)
    {
        row(selectionLabel, selection);
        row(actionLabel, action);
        if (first.isVisible())
        {
            auto interval = space.removeFromTop(29);
            rangeLabel.setBounds(interval.removeFromLeft(108));
            first.setBounds(interval.removeFromLeft(interval.getWidth() / 2).reduced(1));
            last.setBounds(interval.reduced(1));
            space.removeFromTop(5);
        }
        if (track.isVisible())
            row(trackLabel, track);
        auto flags = space.removeFromTop(30);
        copy.setBounds(flags.removeFromLeft(118));
        folder.setBounds(flags);
        auto actions = space.removeFromTop(31);
        submit.setBounds(actions.removeFromLeft(112));
        cancelButton.setBounds(actions.removeFromLeft(72));
    }
    status.setBounds(space.removeFromTop(std::min(64, space.getHeight() - 32)));
    auto files = space.removeFromTop(31);
    drag.setBounds(files.removeFromLeft(34));
    reveal.setBounds(files.removeFromRight(94));
    retryButton.setBounds(files.removeFromRight(60));
    filename.setBounds(files);
}
} // namespace sp
