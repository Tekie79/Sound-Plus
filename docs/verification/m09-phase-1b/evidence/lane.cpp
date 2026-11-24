#include "MultitrackEditor.h"
#include <iostream>

void require(bool ok, const char* explanation)
{
    if (!ok) throw std::runtime_error(explanation);
}

template<class Visitor>
void visit(juce::Component& component, const Visitor& fn)
{
    if (!component.isVisible()) return;
    fn(component);
    for (auto* child : component.getChildren()) visit(*child, fn);
}

int main(int argc, char** argv)
{
    try {
        require(argc == 3, "usage: lane saved-project isolated-directory");
        juce::ScopedJuceInitialiser_GUI initialise;
        const juce::File root(argv[2]);
        sp::DesignSessionServices services;
        services.automaticRecovery = services.timers = false;
        services.now = [] { return 0.; };
        services.submit = [](auto) { return true; };
        services.stop = [] {};
        services.status = [] { return sp::message("status"); };
        services.parameters = [](auto&, auto, auto) {};
        sp::DesignSession session(root.getChildFile("lane.sqlite"), services);
        session.document = sp::loadDesign(juce::File(argv[1]));
        juce::PropertiesFile::Options options;
        juce::PropertiesFile prefs(root.getChildFile("lane.settings"), options);
        sp::MultitrackEditor editor(session, [](const auto&) { return std::shared_ptr<const sp::WaveformData>(); }, prefs);
        editor.setVisible(true);
        editor.setSize(1000,700);
        editor.refresh();
        auto& tracks = session.document.activeMix().tracks;
        const auto owner = tracks.front().id;
        for (bool expand : {true, false}) {
            auto* lane = sp::automationLane(session.document.activeMix(), owner);
            require(lane && lane->curves.size() == 2 && lane->expanded != expand, "wrong starting mode");
            juce::Button* disclosure = nullptr;
            visit(editor,[&](auto& c){
                if (!disclosure && c.getTitle() == "Automation disclosure") disclosure = dynamic_cast<juce::Button*>(&c);
            });
            require(disclosure && disclosure->onClick, "missing actual track disclosure");
            disclosure->onClick();
            editor.refresh();
            lane = sp::automationLane(session.document.activeMix(), owner);
            require(lane && lane->expanded == expand && lane->shown.parameter == "volume", "wrong display target");
            require(lane->curves[0].read && lane->curves[1].read, "changing visibility changed read state");
            int visibleLanes = 0, volumeLabels = 0;
            visit(editor,[&](auto& c){
                if (c.getTitle() == "Automation lane") ++visibleLanes;
                if (auto* b = dynamic_cast<juce::Button*>(&c))
                    if (b->getButtonText().startsWith("Volume")) ++volumeLabels;
            });
            require(visibleLanes == int(expand) && volumeLabels == int(expand), "wrong number of visible controls");
            std::cout << "Expanded " << expand << ", visible lanes " << visibleLanes
                      << ", Volume controls " << volumeLabels << ", enabled stored curves 2\n";
        }
    } catch(const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}
