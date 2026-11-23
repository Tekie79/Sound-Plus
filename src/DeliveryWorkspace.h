#pragma once
#include "DesignerView.h"

namespace sp
{
class DeliveryWorkspace final : public juce::Component, private juce::Timer
{
  public:
    explicit DeliveryWorkspace(DesignerView &view) : designer(view)
    {
        setTitle("Deliver workspace");
        addAndMakeVisible(settingsViewport);
        settingsViewport.setScrollBarsShown(true, false);
        history.setTitle("Finalized exports");
        for (auto *c : std::initializer_list<juce::Component *>{&history, &render, &selection, &stems,
                                                                 &folder, &reveal, &drag, &preview, &cancel, &status})
            addAndMakeVisible(c);
        render.onClick = [this] { designer.session.render(); };
        selection.onClick = [this] { designer.session.render(true); };
        stems.onClick = [this] { designer.exportStems(stems); };
        folder.onClick = [this] { designer.exportFolderFromShell(); };
        preview.onClick = [this] { designer.session.play(); };
        cancel.onClick = [this] { designer.session.cancelRender(); };
        history.onChange = [this]
        {
            auto index = history.getSelectedId() - 1;
            auto &outputs = designer.session.document.outputs;
            if (juce::isPositiveAndBelow(index, (int)outputs.size()))
                designer.session.chooseOutput(outputs[(size_t)index].path);
            timerCallback();
        };
        reveal.onClick = [this] { designer.session.lastOutput.revealToUser(); };
        drag.resolveFile = [this] { return designer.session.lastOutput; };
        drag.resolveFiles = [this]
        {
            for (const auto &output : designer.session.document.outputs)
                if (output.path == designer.session.lastOutput.getFullPathName())
                    return outputFiles(output);
            return juce::StringArray{};
        };
        status.setTitle("Export status");
    }

    void enter()
    {
        resetSettings();
        signature.clear();
        timerCallback();
        startTimerHz(5);
    }
    void leave() { stopTimer(); }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ui::background);
        g.setColour(ui::surface);
        g.fillRoundedRectangle(settingsCard.toFloat(), 12);
        g.fillRoundedRectangle(outputCard.toFloat(), 12);
        g.setColour(ui::text);
        g.setFont(juce::FontOptions(18.f, juce::Font::bold));
        g.drawText("Export", settingsCard.withHeight(44).reduced(14, 0), juce::Justification::centredLeft);
        g.drawText("Exports", outputCard.withHeight(44).reduced(14, 0), juce::Justification::centredLeft);
        g.setColour(ui::muted);
        g.setFont(juce::FontOptions(12.f));
        g.drawText(designer.session.outputFolder().getFullPathName(),
                   outputCard.reduced(14, 0).withTrimmedTop(142).withHeight(22),
                   juce::Justification::centredLeft, true);
    }
    void resized() override
    {
        auto area = getLocalBounds().reduced(20);
        settingsCard = area.removeFromLeft(std::min(416, area.getWidth() / 2)).withTrimmedRight(12);
        outputCard = area.withHeight(208);
        settingsViewport.setBounds(settingsCard.reduced(12, 0).withTrimmedTop(44).withTrimmedBottom(12));
        auto controls = outputCard.reduced(14, 0).withTrimmedTop(48);
        auto files = controls.removeFromTop(32);
        drag.setBounds(files.removeFromRight(34));
        reveal.setBounds(files.removeFromRight(34));
        history.setBounds(files.withTrimmedRight(7));
        controls.removeFromTop(8);
        auto actions = controls.removeFromTop(44);
        render.setBounds(actions.removeFromLeft(120));
        selection.setBounds(actions.removeFromLeft(104));
        stems.setBounds(actions.removeFromLeft(86));
        folder.setBounds(actions.removeFromLeft(34));
        preview.setBounds(actions.removeFromLeft(34));
        cancel.setBounds(actions.removeFromLeft(34));
        status.setBounds(controls.withTrimmedTop(48).withHeight(24));
    }

  private:
    DesignerView &designer;
    juce::Viewport settingsViewport;
    juce::ComboBox history;
    juce::Label status;
    ui::TextActionButton render{"Render Design"}, selection{"Selection"}, stems{"Stems"};
    IconButton folder{"Choose output folder", ActionIcon::folder};
    IconButton reveal{"Reveal in Finder", ActionIcon::open};
    IconButton preview{"Play design", ActionIcon::play};
    IconButton cancel{"Cancel render", ActionIcon::cancel};
    ui::FileDragButton drag{"Drag verified export"};
    juce::Rectangle<int> settingsCard, outputCard;
    juce::String signature, settingsContext;

    void resetSettings()
    {
        settingsViewport.setViewedComponent(designer.exportSettings().release(), true);
        settingsContext = designer.session.document.id + "/" + designer.session.document.activeTake;
    }
    void timerCallback() override
    {
        auto &session = designer.session;
        auto &d = session.document;
        if (settingsContext != d.id + "/" + d.activeTake)
            resetSettings();
        auto key = d.id;
        int selected = 0;
        for (int i = 0; i < (int)d.outputs.size(); ++i)
        {
            auto &o = d.outputs[(size_t)i];
            key += "\n" + o.path + (outputFiles(o).isEmpty() ? "missing" : "ready");
            if (session.lastOutput.getFullPathName() == o.path)
                selected = i + 1;
        }
        if (signature != key)
        {
            signature = key;
            history.clear(juce::dontSendNotification);
            for (int i = (int)d.outputs.size() - 1; i >= 0; --i)
            {
                auto &o = d.outputs[(size_t)i];
                bool available = !outputFiles(o).isEmpty();
                history.addItem(juce::File(o.path).getFileName() + (available ? "" : " (missing)"), i + 1);
                history.setItemEnabled(i + 1, available);
            }
        }
        history.setTextWhenNothingSelected(d.outputs.empty() ? "No finalized exports" : "Choose an export");
        history.setSelectedId(selected, juce::dontSendNotification);
        const bool available = selected > 0 && !outputFiles(d.outputs[(size_t)selected - 1]).isEmpty();
        reveal.setEnabled(available);
        drag.setEnabled(available);
        const auto &mix = d.activeMix();
        const bool ready = !session.rendering && !session.transitionPending() && !session.takeBusy();
        render.setButtonText(d.activeTake.isEmpty() ? "Render Design" : "Render Take");
        render.setEnabled(ready && (d.activeTake.isNotEmpty() || !mix.layers.empty() || hasMidi(mix)));
        preview.setEnabled(!session.transitionPending() && (render.isEnabled() || session.ownsPlayback()));
        preview.setIcon(session.ownsPlayback() ? ActionIcon::stop : ActionIcon::play);
        preview.setButtonText(session.ownsPlayback() ? "Stop playback" : "Play design");
        selection.setEnabled(ready && d.activeTake.isEmpty() && session.selectedLayer() != nullptr);
        stems.setEnabled(ready && d.activeTake.isEmpty() && mix.trackSchema);
        cancel.setVisible(session.rendering);
        status.setText(session.rendering ? juce::String("Rendering") : session.message,
                       juce::dontSendNotification);
        repaint(outputCard);
    }
};
} // namespace sp
