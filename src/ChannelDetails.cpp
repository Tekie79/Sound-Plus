#include "ChannelDetails.h"
namespace sp
{
namespace
{
class DetailLanes final : public juce::Component, private juce::Timer
{
    std::shared_ptr<WaveformPool> pool;
    std::vector<Asset> requests;
    juce::StringArray labels;
    juce::OwnedArray<juce::Label> rowLabels;
    int owner;
    int64_t first, last;
    bool reverse;
    uint64_t generation = 0;

  public:
    DetailLanes(std::shared_ptr<WaveformPool> p, Asset a, ChannelLayout l, std::vector<int> inputs, int o,
                int64_t f, int64_t e, bool r)
        : pool(std::move(p)), owner(o), first(f), last(e < 0 ? a.samples : e), reverse(r)
    {
        if (l.id.isEmpty() && !a.layoutOverride.id.isEmpty())
            l = a.layoutOverride;
        if (l.id.isEmpty())
            l = a.detectedChannels.id.isEmpty()
                    ? (a.channels <= 2 ? channelLayout(a.channels == 1 ? "mono-v1" : "stereo-v1")
                                       : discreteLayout(a.channels, true))
                    : a.detectedChannels;
        if (inputs.empty())
            for (int i = 0; i < std::min(a.channels, 16); ++i)
                inputs.push_back(i);
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            auto v = a;
            v.waveformChannel = inputs[i];
            v.waveformLayoutKey = l.id + ":" + l.roles.joinIntoString(",");
            requests.push_back(v);
            int labelIndex = l.size() == a.channels ? inputs[i] : (int)i;
            auto name =
                (labelIndex < l.roles.size() ? l.roles[labelIndex] : "Ch" + juce::String(inputs[i] + 1)) +
                juce::String::fromUTF8(" · source ") + juce::String(inputs[i] + 1);
            labels.add(name);
            auto *label = new juce::Label();
            rowLabels.add(label);
            addAndMakeVisible(label);
            label->setText(name, juce::dontSendNotification);
            label->setTitle(name + ", per-channel waveform");
            label->setFont(juce::FontOptions(11.f));
            label->setColour(juce::Label::textColourId, ui::muted);
            label->setBounds(8, 32 + (int)i * 46, 110, 40);
        }
        setTitle("Per-channel source waveforms: " + a.name + ", " + layoutBadge(l));
        setSize(500, 32 + (int)requests.size() * 46);
        pool->demand(owner, requests);
        startTimerHz(20);
    }
    ~DetailLanes() override
    {
        pool->demand(owner, {});
    }
    void timerCallback() override
    {
        pool->tick();
        if (generation != pool->revision())
        {
            generation = pool->revision();
            repaint();
        }
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ui::surface);
        g.setColour(ui::muted);
        g.setFont(juce::FontOptions(12.f));
        g.drawFittedText(juce::String::fromUTF8("Per-channel source overview · up to 16 selected channels"),
                         8, 4, getWidth() - 16, 24, juce::Justification::left, 1);
        for (size_t i = 0; i < requests.size(); ++i)
        {
            auto y = 32 + (int)i * 46;
            g.setColour(ui::muted);

            auto d = pool->get(requests[i]);
            if (d && d->status == WaveformData::Status::ready)
            {
                auto c = pool->columns(requests[i], first, last, reverse, getWidth() - 132);
                if (c)
                    paintCompactWaveform(g, {124.f, (float)y + 3, (float)getWidth() - 132, 36}, *c,
                                         ui::accent);
            }
            else
            {
                auto text = !d                                           ? "Loading"
                            : d->status == WaveformData::Status::offline ? "Offline"
                            : d->status == WaveformData::Status::changed ? "Source changed"
                                                                         : "Decode error";
                g.drawText(text, 124, y, getWidth() - 132, 40, juce::Justification::left);
            }
        }
    }
};
struct DetailPanel : juce::Component
{
    juce::Viewport viewport;
    juce::ComboBox channelPage;
    std::unique_ptr<DetailLanes> lanes;
    std::function<std::unique_ptr<DetailLanes>(int)> build;
    DetailPanel(std::function<std::unique_ptr<DetailLanes>(int)> factory, int pages)
        : build(std::move(factory))
    {
        addAndMakeVisible(viewport);
        addAndMakeVisible(channelPage);
        channelPage.setVisible(pages > 1);
        channelPage.setTitle("Source channel page");
        for (int i = 0; i < pages; ++i)
            channelPage.addItem("Channels " + juce::String(i * 16 + 1) + juce::String::fromUTF8("–") +
                                    juce::String((i + 1) * 16),
                                i + 1);
        channelPage.setSelectedId(1, juce::dontSendNotification);
        channelPage.onChange = [this] { show(channelPage.getSelectedId() - 1); };
        show(0);
        setSize(520, std::min(440, lanes->getHeight() + (pages > 1 ? 48 : 16)));
    }
    void show(int page)
    {
        viewport.setViewedComponent(nullptr, false);
        lanes.reset();
        lanes = build(page);
        viewport.setViewedComponent(lanes.get(), false);
    }
    void resized() override
    {
        channelPage.setBounds(8, 4, getWidth() - 16, 28);
        viewport.setBounds(8, channelPage.isVisible() ? 36 : 8, getWidth() - 16,
                           getHeight() - (channelPage.isVisible() ? 44 : 16));
    }
};
} // namespace
void showChannelDetails(juce::Component &parent, juce::Rectangle<int> anchor,
                        std::shared_ptr<WaveformPool> pool, Asset a, ChannelLayout layout,
                        std::vector<int> inputs, int owner, int64_t first, int64_t last, bool reverse)
{
    if (!pool || a.channels < 1)
        return;
    int pages = inputs.empty() ? (a.channels + 15) / 16 : 1;
    auto p = std::make_unique<DetailPanel>(
        [=](int page) mutable
        {
            auto selected = inputs;
            if (selected.empty())
                for (int i = page * 16; i < std::min(a.channels, page * 16 + 16); ++i)
                    selected.push_back(i);
            return std::make_unique<DetailLanes>(pool, a, layout, std::move(selected), owner, first, last,
                                                 reverse);
        },
        pages);
    juce::CallOutBox::launchAsynchronously(std::move(p), anchor, &parent);
}
} // namespace sp
