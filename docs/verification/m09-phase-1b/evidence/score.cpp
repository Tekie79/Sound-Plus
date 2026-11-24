#include "AcousticMetadata.h"
#include <iostream>
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    juce::File database(argv[1]);
    sp::Catalog catalog(database, true);
    auto asset = catalog.asset(argv[2]);
    auto result = sp::acousticSuggestions(database, asset, true, [] { return false; });
    if (result.choices.empty() || asset.name != "clip-021.wav") return 3;
    const auto& first = result.choices.front();
    auto json = juce::var(new juce::DynamicObject());
    auto* data = json.getDynamicObject();
    data->setProperty("name", asset.name);
    data->setProperty("sourceId", asset.id);
    data->setProperty("category", first.category);
    data->setProperty("similarity", static_cast<double>(first.similarity));
    data->setProperty("first", static_cast<juce::int64>(first.moment.startSample));
    data->setProperty("last", static_cast<juce::int64>(first.moment.endSample));
    std::cout << juce::JSON::toString(json) << std::endl;
}
