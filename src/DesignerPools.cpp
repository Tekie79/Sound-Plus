#include "DesignerView.h"
#include "PoolMappingReview.h"
#include <set>
namespace sp
{
namespace
{
class PoolPopupOwner final : public juce::Component
{
  public:
    std::unique_ptr<juce::Component> content;
    std::unique_ptr<juce::CallOutBox> window;
    PoolPopupOwner(std::unique_ptr<juce::Component> c, juce::Rectangle<int> target, juce::LookAndFeel &look,
                   std::function<void()> closed)
        : content(std::move(c)), window(std::make_unique<juce::CallOutBox>(*content, target, nullptr))
    {
        window->setLookAndFeel(&look);
        window->setVisible(true);
        window->enterModalState(true, juce::ModalCallbackFunction::create([closed](int) { closed(); }),
                                false);
    }
    ~PoolPopupOwner() override
    {
        window->exitModalState(0);
        window->setLookAndFeel(nullptr);
        window.reset();
    }
};
void dismiss(std::unique_ptr<juce::Component> &owner)
{
    if (auto *p = dynamic_cast<PoolPopupOwner *>(owner.get()))
        p->window->dismiss();
}

} // namespace
void DesignerView::choosePoolFiles(const juce::String &layer, const juce::String &relocate)
{
    if (session.takeBusy() || session.document.activeTake.isNotEmpty())
        return;
    auto token = session.beginDialog();
    auto document = session.document.id, variation = session.document.activeVariation;
    poolChooser = std::make_unique<juce::FileChooser>(
        relocate.isEmpty() ? "Add alternatives (64 per layer maximum)"
                           : "Relocate identical alternate content",
        juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf;*.ogg", true);
    juce::Component::SafePointer<DesignerView> safe(this);
    poolChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
            (relocate.isEmpty() ? juce::FileBrowserComponent::canSelectMultipleItems : 0),
        [safe, token, document, variation, layer, relocate](const juce::FileChooser &choice)
        {
            if (!safe || !safe->session.accepts(token) || safe->session.document.id != document ||
                safe->session.document.activeVariation != variation)
                return;
            if (relocate.isNotEmpty())
            {
                if (choice.getResult() != juce::File())
                    safe->session.relinkPoolEntry(layer, relocate, choice.getResult());
            }
            else
            {
                juce::StringArray paths;
                for (auto &f : choice.getResults())
                    paths.add(f.getFullPathName());
                safe->session.addPoolFiles(layer, paths);
            }
        });
}
std::unique_ptr<PoolMappingReview> DesignerView::makePoolReviewPanel(const juce::String &id)
{
    auto *layer = session.selectedLayer();
    if (!layer || !layer->pool || session.takeBusy() || session.document.activeTake.isNotEmpty())
        return {};
    auto at = std::find_if(layer->pool->entries.begin(), layer->pool->entries.end(),
                           [&](auto &e) { return e.id == id; });
    if (at == layer->pool->entries.end())
        return {};
    auto original = juce::JSON::toString(poolEntryToJson(*at), true);
    auto document = session.document.id, variation = session.document.activeVariation, layerId = layer->id;
    auto bus = session.document.activeMix().layout;
    auto token = session.beginDialog();
    auto serial = ++poolReviewSerial;
    auto panel = std::make_unique<PoolMappingReview>(*at, bus, session.document.activeMix().routingVersion);
    auto *review = panel.get();
    juce::Component::SafePointer<DesignerView> safe(this);
    panel->onCancel = [safe, serial]
    {
        if (safe && safe->poolReviewSerial == serial)
            dismiss(safe->poolReview);
    };
    panel->onUse = [safe, review, token, document, variation, layerId, id, original, bus, serial]
    {
        if (!safe || safe->poolReviewSerial != serial || !safe->session.accepts(token) ||
            safe->session.document.id != document || safe->session.document.activeVariation != variation ||
            safe->session.document.activeMix().layout != bus)
            return;
        auto *l = safe->session.selectedLayer();
        if (!l || l->id != layerId || !l->pool)
            return;
        auto e = std::find_if(l->pool->entries.begin(), l->pool->entries.end(),
                              [&](auto &v) { return v.id == id; });
        if (e == l->pool->entries.end() || juce::JSON::toString(poolEntryToJson(*e), true) != original)
        {
            safe->session.message = "Alternate changed during review; reopen its mapping";
            dismiss(safe->poolReview);
            return;
        }
        safe->session.reviewAndSelectPoolEntry(layerId, id, review->resolved, review->pair);
        dismiss(safe->poolReview);
    };
    return panel;
}
void DesignerView::reviewPoolSource(const juce::String &id)
{
    auto panel = makePoolReviewPanel(id);
    if (!panel)
        return;
    auto serial = poolReviewSerial;
    juce::Component::SafePointer<DesignerView> safe(this);
    poolReview = std::make_unique<PoolPopupOwner>(std::move(panel),
                                                  getScreenBounds().withSizeKeepingCentre(20, 20), look,
                                                  [safe, serial]
                                                  {
                                                      if (safe && safe->poolReviewSerial == serial)
                                                          safe->poolReview.reset();
                                                  });
}
void DesignerView::showPoolPopup(const juce::String &id)
{
    session.selectLayer(id);
    auto serial = ++poolPopoverSerial;
    auto panel = std::make_unique<SourcePoolView>(session, peakPool, preferences, 7);
    auto *view = panel.get();
    panel->expandForPopup();
    panel->setSize(306, panel->preferredHeight());
    juce::Component::SafePointer<SourcePoolView> weak(view);
    juce::Component::SafePointer<DesignerView> safe(this);
    panel->onHeightChanged = [weak]
    {
        if (weak)
            weak->setSize(306, weak->preferredHeight());
    };
    panel->onAdd = [safe, id]
    {
        if (safe)
            safe->choosePoolFiles(id);
    };
    panel->onReview = [safe](const auto &entry)
    {
        if (safe)
            safe->reviewPoolSource(entry);
    };
    panel->onRelocate = [safe, id](const auto &entry)
    {
        if (safe)
            safe->choosePoolFiles(id, entry);
    };
    poolPopover = std::make_unique<PoolPopupOwner>(
        std::move(panel), perform->getScreenBounds().withSizeKeepingCentre(20, 20), look,
        [safe, serial]
        {
            if (safe && safe->poolPopoverSerial == serial)
                safe->poolPopover.reset();
        });
}
} // namespace sp
