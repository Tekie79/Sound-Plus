#pragma once
#include "SourcePoolView.h"
namespace sp
{
class PoolMappingReview final : public juce::Component
{
  public:
    SourcePoolEntry entry;
    ChannelLayout bus;
    LayerRouting resolved;
    int pair = 0;
    juce::ComboBox physical, interpretation, mapping;
    juce::TextEditor custom, explanation;
    juce::Label title, inputLabel, roleLabel, mapLabel;
    ui::TextActionButton use{"Use source"}, cancel{"Cancel"};
    std::function<void()> onUse, onCancel;
    std::vector<ChannelLayout> choices;
    bool updating = false;
    PoolMappingReview(SourcePoolEntry e, ChannelLayout destination, int version);
    std::vector<int> inputs() const;
    void refresh(bool resetRoles);
    void paint(juce::Graphics &g) override;
    void resized() override;
};
} // namespace sp
