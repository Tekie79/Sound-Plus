#include "PackagePanel.h"
#include "ProjectPackage.h"
#include "UIComponents.h"
namespace sp
{
namespace
{
using namespace juce;
using Worker = ThreadWithProgressWindow;
struct PackageJob final : Worker
{
    std::function<void(PackageJob &)> work;
    std::function<void(String)> finished;
    String error;
    PackageJob(String title, std::function<void(PackageJob &)> w, std::function<void(String)> f)
        : Worker(title, true, true, -1), work(std::move(w)), finished(std::move(f))
    {
    }
    void run() override
    {
        try
        {
            work(*this);
        }
        catch (const std::exception &e)
        {
            error = String::fromUTF8(e.what());
        }
    }
    void threadComplete(bool cancelled) override
    {
        auto done = std::move(finished);
        auto result = cancelled ? "Collection cancelled" : error;
        delete this;
        done(result);
    }
};
void status(Component::SafePointer<DesignSession> s, const String &text)
{
    if (s)
    {
        s->message = text;
        if (s->onChange)
            s->onChange();
    }
}
struct RightsPanel final : Component, ListBoxModel
{
    std::shared_ptr<PackagePlan> plan;
    File target;
    std::set<String> approved;
    Label title, details;
    ListBox list{"Package files", this};
    TextButton all{"Approve all"}, collect{"Collect"};
    Component::SafePointer<DesignSession> session;
    RightsPanel(std::shared_ptr<PackagePlan> p, File output, Component::SafePointer<DesignSession> s)
        : plan(std::move(p)), target(output), session(s)
    {
        for (auto *c : std::initializer_list<Component *>{&title, &details, &list, &all, &collect})
            addAndMakeVisible(c);
        title.setText("Review rights to copy these files", dontSendNotification);
        details.setText(
            String((int)plan->files.size()) + " files   " +
                File::descriptionOfSizeInBytes(plan->uniqueBytes) + "   " +
                File::descriptionOfSizeInBytes(target.getParentDirectory().getBytesFreeOnVolume()) + " free",
            dontSendNotification);
        list.setRowHeight(28);
        list.setColour(ListBox::backgroundColourId, ui::surface);
        all.onClick = [this]
        {
            for (auto &f : plan->files)
                approved.insert(f.original.getFullPathName());
            update();
        };
        collect.onClick = [this]
        {
            auto *job = new PackageJob(
                "Collect project",
                [p = plan, a = approved, dir = target](auto &j)
                {
                    collectPackage(
                        *p, a, dir, [&] { return j.threadShouldExit(); },
                        [&](auto n, auto total) { j.setProgress(double(n) / std::max<int64_t>(1, total)); });
                },
                [s = session, f = target](String error)
                { status(s, error.isEmpty() ? "Collected " + f.getFileName() : error); });
            if (auto *box = findParentComponentOfClass<CallOutBox>())
                box->dismiss();
            job->launchThread();
        };
        update();
        setSize(410, 340);
    }
    ~RightsPanel() override
    {
        list.setModel(nullptr);
    }
    void update()
    {
        collect.setEnabled(approved.size() == plan->files.size());
        list.repaint();
    }
    int getNumRows() override
    {
        return (int)plan->files.size();
    }
    void paintListBoxItem(int row, Graphics &g, int w, int h, bool) override
    {
        if (!isPositiveAndBelow(row, getNumRows()))
            return;
        auto &file = plan->files[(size_t)row].original;
        auto r = Rectangle<int>(6, 7, 14, 14);
        g.setColour(approved.contains(file.getFullPathName()) ? ui::accent : ui::muted);
        g.drawRoundedRectangle(r.toFloat(), 3, 1);
        if (approved.contains(file.getFullPathName()))
            g.fillRoundedRectangle(r.reduced(3).toFloat(), 2);
        g.setColour(ui::text);
        g.drawText(file.getFileName() + "  /  " + file.getParentDirectory().getFileName(), 28, 0, w - 32, h,
                   Justification::centredLeft);
    }
    void listBoxItemClicked(int i, const MouseEvent &) override
    {
        toggle(i);
    }
    void returnKeyPressed(int i) override
    {
        toggle(i);
    }
    void toggle(int i)
    {
        if (isPositiveAndBelow(i, getNumRows()))
        {
            auto path = plan->files[(size_t)i].original.getFullPathName();
            if (!approved.erase(path))
                approved.insert(path);
            update();
        }
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        title.setBounds(r.removeFromTop(28));
        details.setBounds(r.removeFromTop(24));
        auto buttons = r.removeFromBottom(34);
        all.setBounds(buttons.removeFromLeft(100));
        collect.setBounds(buttons.removeFromRight(95));
        list.setBounds(r.reduced(0, 5));
    }
};
} // namespace
void reviewCollection(DesignSession &s, const juce::File &parent, bool withOutput, juce::Component &anchor)
{
    auto snapshot = s.document;
    auto target =
        parent.getNonexistentChildFile(File::createLegalFileName(snapshot.name) + " Project", "", true);
    if (withOutput)
    {
        auto chosen = s.readyOutput().getFullPathName();
        std::erase_if(snapshot.outputs, [&](const auto &o) { return o.path != chosen; });
    }
    auto p = std::make_shared<PackagePlan>();
    Component::SafePointer<DesignSession> safe(&s);
    Component::SafePointer<Component> button(&anchor);
    auto *job = new PackageJob(
        "Inspect project sources", [p, snapshot, withOutput](auto &j)
        { *p = planPackage(snapshot, withOutput, [&] { return j.threadShouldExit(); }); },
        [p, target, safe, button](String e)
        {
            if (!safe || !button)
                return;
            if (e.isNotEmpty())
            {
                status(safe, e);
                return;
            }
            CallOutBox::launchAsynchronously(std::make_unique<RightsPanel>(p, target, safe),
                                             button->getScreenBounds(), nullptr);
        });
    job->launchThread();
}
} // namespace sp
