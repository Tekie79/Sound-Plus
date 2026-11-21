#include "DesignModel.h"
#include <set>
namespace sp
{
namespace
{
bool exists(const InsertChain &c, const InsertRef &r)
{
    if (r.kind == InsertKind::stock)
        return std::any_of(c.stock.begin(), c.stock.end(), [&](auto &s) { return s.id == r.id; });
    return std::any_of(c.hosted.slots.begin(), c.hosted.slots.end(), [&](auto &s) { return s.id == r.id; });
}
std::vector<InsertRef> legacyOrder(const InsertChain &c)
{
    std::vector<InsertRef> rows;
    for (auto &s : c.stock)
        rows.push_back({InsertKind::stock, s.id});
    for (auto &s : c.hosted.slots)
        rows.push_back({InsertKind::hosted, s.id});
    return rows;
}
} // namespace
std::vector<InsertRef> resolvedInsertOrder(const InsertChain &c)
{
    auto rows = c.order;
    std::set<std::pair<InsertKind, juce::String>> seen;
    for (auto &r : rows)
        if ((r.kind != InsertKind::stock && r.kind != InsertKind::hosted) || !exists(c, r) ||
            !seen.emplace(r.kind, r.id).second)
            throw std::runtime_error("Invalid ordered insert identity");
    // Legacy/programmatic builders can append payloads; only this normalization
    // boundary assigns their initial stock-then-hosted positions.
    for (auto &r : legacyOrder(c))
        if (seen.emplace(r.kind, r.id).second)
            rows.push_back(r);
    return rows;
}
void normaliseInsertChain(InsertChain &c)
{
    std::erase_if(c.order, [&](auto &r) { return !exists(c, r); });
    c.order = resolvedInsertOrder(c);
}
bool requiresOrderedInserts(const InsertChain &c)
{
    return c.bypass || resolvedInsertOrder(c) != legacyOrder(c);
}
void rekeyInsertChain(InsertChain &c)
{
    normaliseInsertChain(c);
    for (auto &r : c.order)
    {
        auto old = r.id;
        r.id = juce::Uuid().toString();
        if (r.kind == InsertKind::stock)
        {
            for (auto &s : c.stock)
                if (s.id == old)
                {
                    s.id = r.id;
                    break;
                }
        }
        else
        {
            for (auto &s : c.hosted.slots)
                if (s.id == old)
                {
                    s.id = r.id;
                    break;
                }
        }
    }
    c.hosted.id = juce::Uuid().toString();
    c.hosted.runtimeEndpoint = juce::File();
    c.hosted.runtimeOwnerRack.clear();
}
} // namespace sp
