#pragma once
#include "DesignControls.h"
#include "UIComponents.h"
#include "WorkspaceTransfer.h"

namespace sp
{
class TransferController
{
  public:
    struct State
    {
        int workspace = -1;
        bool running = false, cancellable = false, canRetry = false;
        juce::String status, error;
        TransferArtifact artifact;
        juce::File receipt;
    };
    explicit TransferController(const juce::File &catalog);
    ~TransferController();
    bool reserve(int workspace);
    void start(TransferRequest);
    void fail(const juce::String &);
    void cancel();
    void retry(int track);
    State snapshot() const;
    const juce::File &receiptDirectory() const { return receipts; }

  private:
    juce::File receipts, bridge;
    mutable std::mutex mutex;
    State state;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    std::optional<TransferRequest> retryRequest;
    void update(const std::function<void(State &)> &);
    void execute(TransferRequest, TransferArtifact prepared = {});
};

class TransferPanel final : public juce::Component, private juce::Timer
{
  public:
    using Submit = std::function<void(TransferScope, TransferAction, bool, int64_t, int64_t, int)>;
    TransferPanel(juce::PropertiesFile &, TransferController &, int workspace, juce::String context,
                  const std::function<bool(TransferScope)> &available, int64_t first, int64_t last,
                  Submit);
    static std::vector<TransferScope> scopes(int workspace);
    static TransferScope defaultScope(int workspace);
    static TransferScope preferredScope(const juce::PropertiesFile &, int workspace);
    static bool projectRange(TransferScope);
    void paint(juce::Graphics &) override;
    void resized() override;

  private:
    juce::PropertiesFile &prefs;
    TransferController &service;
    int workspace;
    juce::Label heading, selected, selectionLabel, actionLabel, rangeLabel, trackLabel, status, filename;
    juce::ComboBox selection, action;
    juce::TextEditor first, last, track;
    juce::ToggleButton copy{"Save a copy"};
    ui::TextActionButton folder{"Choose folder"}, submit{"Prepare"}, cancelButton{"Cancel"}, reveal{"Show receipt"},
                         retryButton{"Retry"};
    ui::FileDragButton drag{"Drag prepared audio"};
    std::unique_ptr<juce::FileChooser> chooser;
    Submit submitRequest;
    juce::String folderError;
    void updateOptions();
    void timerCallback() override;
    void chooseFolder();
};
} // namespace sp
