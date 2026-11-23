#pragma once
#include "DesignControls.h"

namespace sp::ui
{
class WorkspaceCommands final : public juce::Component, private juce::ListBoxModel
{
  public:
    struct Command
    {
        juce::String name;
        std::function<void()> run;
    };

    WorkspaceCommands(std::vector<Command> actions, juce::Component *previousFocus)
        : commands(std::move(actions)), previous(previousFocus), list("Commands", this)
    {
        setTitle("Command search");
        addAndMakeVisible(query);
        addAndMakeVisible(list);
        query.setTitle("Search commands");
        query.setTextToShowWhenEmpty("Search commands", muted);
        query.onTextChange = [this] { filter(); };
        query.onReturnKey = [this] { run(list.getSelectedRow()); };
        list.setRowHeight(31);
        filter();
        setSize(370, 288);
    }

    ~WorkspaceCommands() override
    {
        list.setModel(nullptr);
        auto oldFocus = previous;
        if (!invoked && oldFocus)
            juce::MessageManager::callAsync([oldFocus]
            {
                if (oldFocus && oldFocus->isShowing())
                    oldFocus->grabKeyboardFocus();
            });
    }
    void focusSearch() { query.grabKeyboardFocus(); }
    void paint(juce::Graphics &g) override { g.fillAll(surface); }
    void resized() override
    {
        auto content = getLocalBounds().reduced(10);
        query.setBounds(content.removeFromTop(36));
        content.removeFromTop(8);
        list.setBounds(content);
    }

  private:
    std::vector<Command> commands;
    std::vector<size_t> matches;
    juce::Component::SafePointer<juce::Component> previous;
    bool invoked = false;
    class SearchField final : public juce::TextEditor
    {
      public:
        std::function<void(int)> move;
        bool keyPressed(const juce::KeyPress &key) override
        {
            if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
            {
                if (move) move(key == juce::KeyPress::upKey ? -1 : 1);
                return true;
            }
            return TextEditor::keyPressed(key);
        }
    } query;
    juce::ListBox list;

    void filter()
    {
        matches.clear();
        const auto words = juce::StringArray::fromTokens(query.getText().trim(), " ", "");
        for (size_t i = 0; i < commands.size(); ++i)
            if (std::all_of(words.begin(), words.end(),
                            [&](const auto &word) { return commands[i].name.containsIgnoreCase(word); }))
                matches.push_back(i);
        list.updateContent();
        list.selectRow(matches.empty() ? -1 : 0);
        query.move = [this](int direction)
        {
            if (!matches.empty())
                list.selectRow(juce::jlimit(0, (int)matches.size() - 1, list.getSelectedRow() + direction));
        };
    }
    void run(int row)
    {
        if (invoked || !juce::isPositiveAndBelow(row, (int)matches.size()))
            return;
        invoked = true;
        auto action = commands[matches[(size_t)row]].run;
        if (auto *box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
        juce::MessageManager::callAsync(std::move(action));
    }
    int getNumRows() override { return (int)matches.size(); }
    juce::String getNameForRow(int row) override
    {
        return juce::isPositiveAndBelow(row, (int)matches.size()) ? commands[matches[(size_t)row]].name
                                                               : juce::String();
    }
    void paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) override
    {
        if (selected)
        {
            g.setColour(raised);
            g.fillRoundedRectangle(1, 1, (float)width - 2, (float)height - 2, radius);
        }
        g.setColour(selected ? accent : text);
        g.setFont(juce::FontOptions(13.f));
        g.drawText(getNameForRow(row), 9, 0, width - 18, height, juce::Justification::centredLeft);
    }
    void listBoxItemClicked(int row, const juce::MouseEvent &) override { run(row); }
    void returnKeyPressed(int row) override { run(row); }
};
} // namespace sp::ui
