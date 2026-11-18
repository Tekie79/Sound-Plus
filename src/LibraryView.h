#pragma once
#include "LibraryService.h"
#include "AudioIPC.h"
#include <juce_gui_extra/juce_gui_extra.h>
namespace sp
{
class Benchmark;
class LibraryView final : public juce::Component,
                          private juce::Timer,
                          private juce::TableListBoxModel,
                          private juce::ListBoxModel,
                          private juce::KeyListener,
                          private juce::AsyncUpdater
{
  public:
    LibraryView(const juce::File &database, const juce::File &executable);
    ~LibraryView() override;
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;
    void addFolder(const juce::File &f)
    {
        library.addFolder(f);
    }
    uint64_t benchmarkSearch(const juce::String &q, const juce::String &root = {});
    void benchmarkScroll(int row);
    Benchmark *benchmark = nullptr;

  private:
    friend class Benchmark;
    double dispatchedMs = 0, appliedMs = 0;
    uint64_t appliedPage = 0;
    int requestedPage = -1;
    std::optional<Asset> selectedAsset;
    int selectedAssetRow = -1;
    const Asset *rowAsset(int row) const;
    void requestPageForRow(int row);
    void handleAsyncUpdate() override;
    int getNumRows() override
    {
        return snapshot.total;
    }
    void paintRowBackground(juce::Graphics &, int, int, int, bool) override;
    void paintCell(juce::Graphics &, int, int, int, int, bool) override;
    void selectedRowsChanged(int) override;
    void cellDoubleClicked(int, int, const juce::MouseEvent &) override;
    void paintListBoxItem(int, juce::Graphics &, int, int, bool) override;
    void listBoxItemClicked(int, const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &, juce::Component *) override;
    void timerCallback() override;
    void chooseFolder();
    void toggle();
    void requestSearch();
    const Asset *selected() const;
    struct SourceModel : juce::ListBoxModel
    {
        LibraryView &owner;
        explicit SourceModel(LibraryView &v) : owner(v) {}
        int getNumRows() override
        {
            return (int)owner.snapshot.roots.size() + 1;
        }
        juce::String getNameForRow(int n) override
        {
            if (n == 0)
                return "All recordings";
            if (!juce::isPositiveAndBelow(n - 1, (int)owner.snapshot.roots.size()))
                return {};
            const auto &root = owner.snapshot.roots[(size_t)n - 1];
            return root.path + (root.online ? " Online" : " Offline") + ", " + juce::String(root.discovered) +
                   " recordings";
        }
        void paintListBoxItem(int n, juce::Graphics &g, int w, int h, bool s) override
        {
            owner.paintListBoxItem(n, g, w, h, s);
        }
        void listBoxItemClicked(int n, const juce::MouseEvent &e) override
        {
            owner.listBoxItemClicked(n, e);
        }
        void selectedRowsChanged(int n) override
        {
            if (!owner.updating && n >= 0)
                owner.chooseSource(n);
        }
    } sourceModel{*this};
    juce::LookAndFeel_V4 look;
    LibraryService library;
    AudioClient audio;
    LibrarySnapshot snapshot;
    struct NamedTable : juce::TableListBox
    {
        LibraryView &owner;
        explicit NamedTable(LibraryView &v) : TableListBox("Recordings", &v), owner(v) {}
        juce::String getNameForRow(int n) override
        {
            const auto *asset = owner.rowAsset(n);
            if (!asset)
                return "Loading recording";
            const auto &a = *asset;
            return a.name + ", " + juce::String(a.duration, 2) + " seconds, " + juce::String(a.channels) +
                   " channels, " + juce::String(a.sampleRate, 0) + " Hz, " +
                   (a.online ? a.state : "Offline") + ", " + a.path;
        }
    } table{*this};
    juce::ListBox sources{"Library folders", &sourceModel};
    juce::TextEditor search;
    juce::TextButton add{"Add Folder"}, rescan{"Rescan"}, play{"Play"}, restart{"Reconnect Audio"};
    juce::TextButton pauseIndex{"Pause Indexing"}, refresh{"Refresh Results"};
    void chooseSource(int);
    juce::ComboBox output;
    juce::Slider gain;
    juce::Label summary, detail, assetId, transport, status, volumeLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String selectedId, rootFilter, playingName, playingId;
    uint64_t requested = 1, appliedVersion = 0, appliedRequest = 0;
    bool updating = false;
};
} // namespace sp
