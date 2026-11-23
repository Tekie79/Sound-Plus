#include "SurroundPanner.h"
#include <locale>
#include <sstream>

namespace sp
{
namespace
{
constexpr auto sendNothing = juce::dontSendNotification;

std::optional<double> number(juce::String text)
{
    std::istringstream input(text.trim().toStdString());
    input.imbue(std::locale::classic());
    double result;
    if (!(input >> result) || input.peek() != std::char_traits<char>::eof() || !std::isfinite(result))
        return {};
    return result;
}

float contribution(const ChannelMatrix &matrix, int channel, int inputs)
{
    float power = 0;
    for (int i = 0; i < inputs; ++i)
        power += matrix[(size_t)channel][(size_t)i] * matrix[(size_t)channel][(size_t)i];
    return power;
}
} // namespace

SurroundPanner::SurroundPanner(DesignSession &s, juce::String id, std::function<int64_t(int64_t)> snap,
                               std::function<bool(const juce::KeyPress &)> onCommand)
    : session(s), owner(std::move(id)), doc(s.document.id), variation(s.document.activeVariation),
      incarnation(s.documentGeneration()), snapTime(std::move(snap)), command(std::move(onCommand))
{
    setLookAndFeel(&look);
    setTitle("Square Surround panner");
    setDescription("Square sound field. Front is up. Drag or use arrow keys. Shift makes fine moves. "
                   "Double-click to add a motion point at the playhead.");
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    for (auto *c : std::initializer_list<juce::Component *>{&title, &busLabel, &timeLabel, &xLabel, &yLabel,
                                                            &xValue, &yValue, &spreadLabel, &spread, &enable,
                                                            &add, &remove, &options, &touch})
        addAndMakeVisible(c);
    title.setFont(juce::FontOptions(16.f, juce::Font::bold));
    for (auto *label : {&busLabel, &timeLabel, &xLabel, &yLabel, &spreadLabel})
    {
        label->setFont(juce::FontOptions(12.f));
        label->setColour(juce::Label::textColourId, ui::muted);
    }
    xLabel.setText("X", sendNothing);
    yLabel.setText("Y", sendNothing);
    spreadLabel.setText("Spread", sendNothing);
    auto coordinateChanged = [this]
    {
        auto x = number(xValue.getText()), y = number(yValue.getText());
        if (x && y)
        {
            grabKeyboardFocus();
            editPosition({*x, *y});
        }
    };
    xValue.setTitle("Position X, left -1 to right +1");
    yValue.setTitle("Position Y, rear -1 to front +1");
    for (auto *e : {&xValue, &yValue})
    {
        e->setFont(juce::FontOptions(13.f));
        e->onReturnKey = coordinateChanged;
        e->onEscapeKey = [this]
        {
            grabKeyboardFocus();
            refresh();
        };
    }
    spread.setSliderStyle(juce::Slider::LinearHorizontal);
    spread.setRange(0, 1, .001);
    spread.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 24);
    spread.setTitle("Stereo Spread");
    spread.setTooltip("Spread the independent left and right source positions");
    spread.onDragStart = [this]
    {
        if (valid())
            session.beginGesture();
    };
    spread.onDragEnd = [this]
    {
        if (valid())
            session.endGesture();
    };
    spread.onValueChange = [this]
    {
        if (!updating)
            editTrack("Stereo spread", [&](auto &t) { t.surroundXY->spread = spread.getValue(); });
    };
    enable.setTooltip("Activate the previewed Square Surround routing. Undo restores the previous routing.");
    enable.onClick = [this]
    {
        finish();
        if (!valid() || !session.document.activeTake.isEmpty())
            return;
        session.edit("Square Surround routing",
                     [&](auto &d)
                     {
                         if (auto *t = findTrack(d.activeMix(), owner))
                         {
                             if (t->surroundXY)
                                 t->surroundXY.reset();
                             else
                                 t->surroundXY = SurroundPosition{};
                         }
                     });
        refresh();
    };
    touch.setTooltip("Record Position X and Y while playing. Release returns to Read over 100 ms.");
    touch.onClick = [this]
    {
        touchArmed = !touchArmed;
        touch.setToggleState(touchArmed, sendNothing);
    };
    add.onClick = [this]
    {
        if (auto *t = track())
        {
            auto p = surroundPositionAt(*t, session.document.playhead);
            addPoint({p.x, p.y}, true);
        }
    };
    remove.onClick = [this] { removePoints(); };
    options.onClick = [this] { motionMenu(); };
    setSize(390, 500);
    refresh();
    startTimerHz(24);
}
SurroundPanner::~SurroundPanner()
{
    finish();
    setLookAndFeel(nullptr);
}
bool SurroundPanner::valid() const
{
    return session.documentGeneration() == incarnation && session.document.id == doc &&
           session.document.activeVariation == variation && !session.transitionPending() &&
           findTrack(session.document.activeMix(), owner);
}
const DesignTrack *SurroundPanner::track() const
{
    return valid() ? findTrack(session.document.activeMix(), owner) : nullptr;
}
void SurroundPanner::timerCallback()
{
    refresh();
}
void SurroundPanner::refresh()
{
    auto *t = track();
    const bool supported = t && trackSquareSurroundSupported(session.document.activeMix(), *t);
    const bool active = supported && t->surroundXY.has_value();
    const bool writable = active && session.document.activeTake.isEmpty() && !session.takeBusy() &&
                          !session.inputBusy() && !session.midiBusy();
    title.setText(t ? t->name : "Track unavailable", sendNothing);
    busLabel.setText(supported ? layoutName(trackOutputLayout(session.document.activeMix(), *t))
                               : "Requires Mono/Stereo to named 5.1",
                     sendNothing);
    enable.setButtonText(active ? "XY enabled" : "Enable XY");
    enable.setToggleState(active, sendNothing);
    enable.setEnabled(supported && !session.squareTouchBusy() && session.document.activeTake.isEmpty() &&
                      !session.takeBusy() && !session.inputBusy() && !session.midiBusy());
    if (!active)
    {
        nodes.clear();
        pathRevision.reset();
    }
    else if (pathRevision != session.document.revision)
    {
        nodes = surroundNodes(*t);
        pathRevision = session.document.revision;
    }
    if (!dragging && !selecting)
        std::erase_if(
            selected, [&](auto frame)
            { return std::none_of(nodes.begin(), nodes.end(), [&](auto &n) { return n.frame == frame; }); });
    touch.setEnabled(writable);
    add.setEnabled(writable && !session.squareTouchBusy());
    remove.setEnabled(writable && !selected.empty());
    options.setEnabled(active);
    const bool stereo = supported && t->layout.size() == 2;
    spread.setVisible(stereo);
    spreadLabel.setVisible(stereo);
    spread.setEnabled(writable);
    xValue.setEnabled(writable);
    yValue.setEnabled(writable);
    auto frame =
        selected.size() == 1 && !session.ownsPlayback() ? selectedFrame() : session.document.playhead;
    auto p = active ? surroundPositionAt(*t, frame, selected.empty() || session.ownsPlayback())
                    : SurroundPosition{};
    if (writing)
    {
        auto cursor = position(pointer);
        p.x = cursor.x;
        p.y = cursor.y;
    }
    updating = true;
    if (!xValue.hasKeyboardFocus(true))
        xValue.setText(juce::String(p.x, 3), false);
    if (!yValue.hasKeyboardFocus(true))
        yValue.setText(juce::String(p.y, 3), false);
    if (!spread.isMouseButtonDown())
        spread.setValue(p.spread, sendNothing);
    timeLabel.setText(juce::String(frame / session.document.sampleRate, 3) + " s", sendNothing);
    updating = false;
    repaint();
}
void SurroundPanner::resized()
{
    auto bounds = getLocalBounds().reduced(14);
    auto header = bounds.removeFromTop(30);
    enable.setBounds(header.removeFromRight(90));
    busLabel.setBounds(header.removeFromRight(94));
    title.setBounds(header);
    auto controls = bounds.removeFromBottom(66);
    auto actions = controls.removeFromTop(32);
    add.setBounds(actions.removeFromLeft(28));
    remove.setBounds(actions.removeFromLeft(28));
    options.setBounds(actions.removeFromLeft(28));
    timeLabel.setBounds(actions.removeFromRight(58));
    xLabel.setBounds(actions.removeFromLeft(16));
    xValue.setBounds(actions.removeFromLeft(55).reduced(1, 3));
    actions.removeFromLeft(7);
    yLabel.setBounds(actions.removeFromLeft(16));
    yValue.setBounds(actions.removeFromLeft(55).reduced(1, 3));
    auto slider = controls.removeFromTop(27);
    touch.setBounds(slider.removeFromRight(59));
    spreadLabel.setBounds(slider.removeFromLeft(54));
    spread.setBounds(slider);
    auto available = bounds.reduced(22, 26).toFloat();
    const auto side = std::min(available.getWidth(), available.getHeight());
    field = available.withSizeKeepingCentre(side, side);
}
juce::Point<float> SurroundPanner::pixel(double x, double y) const
{
    return {field.getX() + (float)((x + 1) * .5 * field.getWidth()),
            field.getY() + (float)((1 - y) * .5 * field.getHeight())};
}
juce::Point<double> SurroundPanner::position(juce::Point<float> p) const
{
    return {std::clamp(2. * (p.x - field.getX()) / field.getWidth() - 1, -1., 1.),
            std::clamp(1. - 2. * (p.y - field.getY()) / field.getHeight(), -1., 1.)};
}
void SurroundPanner::paint(juce::Graphics &g)
{
    g.fillAll(ui::background);
    g.setColour(ui::surface);
    g.fillRoundedRectangle(field, 8);
    g.setColour(hasKeyboardFocus(false) ? ui::accent.withAlpha(.6f) : ui::line);
    g.drawRoundedRectangle(field, 8, 1);
    for (float t : {.25f, .5f, .75f})
    {
        g.setColour(ui::line.withAlpha(t == .5f ? 1.f : .6f));
        g.drawVerticalLine((int)(field.getX() + t * field.getWidth()), field.getY(), field.getBottom());
        g.drawHorizontalLine((int)(field.getY() + t * field.getHeight()), field.getX(), field.getRight());
    }
    g.setColour(ui::muted);
    auto centre = field.getCentre();
    g.drawEllipse(centre.x - 4, centre.y - 4, 8, 8, 1);

    auto *t = track();
    if (!t || !trackSquareSurroundSupported(session.document.activeMix(), *t))
        return;
    const auto bus = trackOutputLayout(session.document.activeMix(), *t);
    auto p = surroundPositionAt(*t, session.document.playhead);
    if (writing)
    {
        auto cursor = position(pointer);
        p.x = cursor.x;
        p.y = cursor.y;
    }
    auto matrix = squareSurroundMatrix(t->layout, bus, p);
    auto legacy = t->surroundXY ? matrix : trackMatrix(*t, bus, t->pan, t->azimuth, t->width);
    g.setFont(juce::FontOptions(12.f, juce::Font::bold));
    for (const auto &role : bus.roles)
    {
        if (role == "LFE")
            continue;
        auto pos = squareSpeakerPosition(role);
        auto at = pixel(pos.x, pos.y);
        float x = at.x - 19;
        float y = at.y + (pos.y == -1 ? 8 : -28);
        const auto c = bus.roles.indexOf(role);
        g.setColour(ui::text);
        g.drawText(role, (int)x, (int)y, 38, 15, juce::Justification::centred);
        g.setColour(ui::line);
        g.fillRoundedRectangle(x + 3, y + 17, 32, 3, 1.5f);
        g.setColour(ui::accent);
        g.fillRoundedRectangle(x + 3, y + 17, 32 * contribution(matrix, c, t->layout.size()), 3, 1.5f);
        if (!t->surroundXY)
        {
            g.setColour(ui::muted);
            g.fillRoundedRectangle(x + 3, y + 22, 32 * contribution(legacy, c, t->layout.size()), 2, 1.f);
        }
    }
    g.setColour(ui::muted);
    g.setFont(juce::FontOptions(11.f));
    g.drawText(t->surroundXY ? "LFE  Separate route" : "Preview  Cyan XY / gray current",
               getLocalBounds().withY(getHeight() - 96).withHeight(15), juce::Justification::centred);
    for (size_t i = 1; i < nodes.size(); ++i)
    {
        const auto &a = nodes[i - 1];
        const auto &b = nodes[i];
        if (a.shape == MotionShape::hold)
            continue;
        auto last = pixel(a.x, a.y);
        g.setColour(ui::accent.withAlpha(.6f));
        const int segments = std::clamp(1024 / (int)nodes.size(), 1, 10);
        for (int k = 1; k <= segments; ++k)
        {
            auto v = surroundPositionAt(*t, a.frame + (b.frame - a.frame) * k / segments, false);
            auto next = pixel(v.x, v.y);
            g.drawLine({last, next}, 1.3f);
            last = next;
        }
    }
    for (auto &node : nodes)
    {
        auto at = pixel(node.x, node.y);
        const bool chosen = selected.contains(node.frame);
        auto dot = juce::Rectangle<float>(chosen ? 9.f : 6.f, chosen ? 9.f : 6.f).withCentre(at);
        g.setColour(chosen ? ui::text : ui::accent);
        g.fillEllipse(dot);
    }
    if (selecting)
    {
        g.setColour(ui::accent.withAlpha(.14f));
        g.fillRect(juce::Rectangle<float>(anchor, pointer));
        g.setColour(ui::accent.withAlpha(.65f));
        g.drawRect(juce::Rectangle<float>(anchor, pointer), 1);
    }
    auto live = pixel(p.x, p.y);
    if (t->layout.size() == 2 && p.spread > 0)
    {
        const auto half = .5 * p.spread;
        auto left = pixel(p.x * (1 - half) - half, p.y);
        auto right = pixel(p.x * (1 - half) + half, p.y);
        g.setColour(ui::accent.withAlpha(.6f));
        g.drawLine({left, right}, 1);
        for (auto pin : {left, right})
            g.fillEllipse(juce::Rectangle<float>(5, 5).withCentre(pin));
    }
    g.setColour(ui::background);
    g.fillEllipse(juce::Rectangle<float>(16, 16).withCentre(live));
    g.setColour(ui::accent);
    g.drawEllipse(juce::Rectangle<float>(14, 14).withCentre(live), 1.6f);
    g.fillEllipse(juce::Rectangle<float>(6, 6).withCentre(live));
}
void SurroundPanner::editTrack(const juce::String &label, const std::function<void(DesignTrack &)> &fn)
{
    if (!add.isEnabled() || !valid())
        return;
    session.edit(label,
                 [&](auto &d)
                 {
                     if (auto *t = findTrack(d.activeMix(), owner); t && t->surroundXY)
                         fn(*t);
                 });
    refresh();
}
int64_t SurroundPanner::selectedFrame() const
{
    return selected.empty() ? session.document.playhead : *selected.begin();
}
void SurroundPanner::editPosition(juce::Point<double> p)
{
    p.x = std::clamp(p.x, -1., 1.);
    p.y = std::clamp(p.y, -1., 1.);
    editTrack("Surround position",
              [&](auto &t)
              {
                  if (!selected.empty())
                  {
                      for (auto &node : nodes)
                          if (selected.contains(node.frame))
                              setSurroundNode(t, {node.frame, p.x, p.y, node.shape});
                  }
                  else if (!session.ownsPlayback() &&
                           std::any_of(t.automation.curves.begin(), t.automation.curves.end(),
                                       [](auto &c) {
                                           return c.read && !c.points.empty() &&
                                                  (c.target == surroundX || c.target == surroundY);
                                       }))
                      setSurroundNode(t, {session.document.playhead, p.x, p.y});
                  else
                  {
                      t.surroundXY->x = p.x;
                      t.surroundXY->y = p.y;
                  }
              });
}
void SurroundPanner::addPoint(juce::Point<double> p, bool shouldSnap)
{
    auto frame = session.document.playhead;
    if (shouldSnap && snapTime)
        frame = snapTime(frame);
    editTrack("Add surround motion point",
              [&](auto &t)
              {
                  setSurroundNode(t, {frame, p.x, p.y});
                  automationCurve(t.automation, surroundX)->read = true;
                  automationCurve(t.automation, surroundY)->read = true;
              });
    selected = {frame};
    refresh();
}
void SurroundPanner::removePoints()
{
    editTrack("Delete surround motion points", [&](auto &t) { eraseSurroundNodes(t, selected); });
    selected.clear();
    refresh();
}
void SurroundPanner::applyShape(MotionShape shape)
{
    editTrack("Surround interpolation",
              [&](auto &t)
              {
                  for (auto &node : nodes)
                      if (selected.contains(node.frame))
                          setSurroundNode(t, {node.frame, node.x, node.y, shape});
              });
}
int64_t SurroundPanner::nearest(juce::Point<float> p) const
{
    float nearest = 9;
    int64_t result = -1;
    for (auto &node : nodes)
    {
        auto distance = pixel(node.x, node.y).getDistanceFrom(p);
        if (distance < nearest || (distance == nearest && std::abs(node.frame - session.document.playhead) <
                                                              std::abs(result - session.document.playhead)))
        {
            nearest = distance;
            result = node.frame;
        }
    }
    return result;
}
void SurroundPanner::mouseMove(const juce::MouseEvent &e)
{
    auto frame = nearest(e.position);
    if (frame >= 0)
        setTooltip("Motion point  " + juce::String(frame / session.document.sampleRate, 6) +
                   " s  Position X and Y");
    else if (auto *t = track())
        setTooltip(t->name + "  Square Surround position. Double-click to add a point at the playhead.");
}
void SurroundPanner::mouseDown(const juce::MouseEvent &e)
{
    if (!field.expanded(8).contains(e.position) || !add.isEnabled())
        return;
    if (e.mods.isPopupMenu())
    {
        motionMenu();
        return;
    }
    grabKeyboardFocus();
    anchor = pointer = e.position;
    const auto frame = nearest(e.position);
    if (frame >= 0 && e.getNumberOfClicks() == 1 && !(touchArmed && session.automationFrame()))
    {
        if (!selected.contains(frame) && !e.mods.isShiftDown())
            selected.clear();
        selected.insert(frame);
        dragStartNodes = nodes;
        dragging = true;
    }
    else if (e.mods.isShiftDown())
    {
        selectionAtDown = selected;
        selecting = true;
    }
    else
    {
        selected.clear();
        dragging = true;
        dragStartNodes.clear();
    }
    moved = false;
    refresh();
}
void SurroundPanner::mouseDrag(const juce::MouseEvent &e)
{
    pointer = e.position;
    if (selecting)
    {
        selected = selectionAtDown;
        for (auto &node : nodes)
            if (juce::Rectangle<float>(anchor, pointer).contains(pixel(node.x, node.y)))
                selected.insert(node.frame);
        refresh();
        return;
    }
    if (!dragging || !valid() || (!moved && e.getDistanceFromDragStart() <= 2))
        return;
    auto p = position(e.position);
    if (!moved)
    {
        moved = true;
        if (touchArmed && session.automationFrame() && dragStartNodes.empty())
        {
            writing = session.beginSquareTouch(owner, p);
            if (!writing)
                dragging = false;
            return;
        }
        session.beginGesture();
        edited = true;
    }
    if (writing)
    {
        session.moveSquareTouch(p);
        return;
    }
    if (dragStartNodes.empty())
        editPosition(p);
    else
    {
        auto delta = p - position(anchor);
        editTrack("Move surround motion points",
                  [&](auto &t)
                  {
                      for (auto node : dragStartNodes)
                          if (selected.contains(node.frame))
                          {
                              node.x += delta.x;
                              node.y += delta.y;
                              setSurroundNode(t, node);
                          }
                  });
    }
}
void SurroundPanner::mouseUp(const juce::MouseEvent &)
{
    finishDrag();
}
void SurroundPanner::mouseDoubleClick(const juce::MouseEvent &e)
{
    finishDrag();
    if (field.expanded(4).contains(e.position))
        addPoint(position(e.position), !e.mods.isCommandDown());
}
void SurroundPanner::finishDrag(bool cancel)
{
    dragging = selecting = moved = false;
    if (writing)
    {
        writing = false;
        session.endSquareTouch();
    }
    if (edited)
    {
        edited = false;
        if (valid())
        {
            if (cancel)
                session.cancelGesture();
            else
                session.endGesture();
        }
    }
}
void SurroundPanner::finish()
{
    finishDrag();
}
void SurroundPanner::focusLost(FocusChangeType)
{
    finish();
}
void SurroundPanner::numericPoint()
{
    if (selected.size() != 1)
        return;
    auto found =
        std::find_if(nodes.begin(), nodes.end(), [&](auto &n) { return n.frame == selectedFrame(); });
    if (found == nodes.end())
        return;
    auto node = *found;
    auto *box = new juce::AlertWindow("Motion point", "Enter absolute project time and square position",
                                      juce::MessageBoxIconType::NoIcon);
    box->addTextEditor("time", juce::String(node.frame / session.document.sampleRate, 7), "Seconds");
    box->addTextEditor("x", juce::String(node.x, 6), "Position X");
    box->addTextEditor("y", juce::String(node.y, 6), "Position Y");
    box->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    box->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SurroundPanner> safe(this);
    box->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe, box, node](int result)
            {
                if (result != 1 || !safe || !safe->valid() || !safe->selected.contains(node.frame))
                    return;
                auto time = number(box->getTextEditorContents("time"));
                auto x = number(box->getTextEditorContents("x")), y = number(box->getTextEditorContents("y"));
                if (!time || *time < 0 || *time > 7 * 86400 || !x || !y || std::abs(*x) > 1 ||
                    std::abs(*y) > 1)
                    return;
                auto frame = (int64_t)std::llround(*time * safe->session.document.sampleRate);
                safe->editTrack(
                    "Set motion point time and position",
                    [&](auto &t)
                    {
                        if (frame != node.frame && std::any_of(safe->nodes.begin(), safe->nodes.end(),
                                                               [&](auto &n) { return n.frame == frame; }))
                            throw std::runtime_error("A motion point already occupies that frame");
                        setSurroundNode(t, {node.frame, *x, *y, node.shape});
                        for (const auto &axis : {surroundX, surroundY})
                            if (auto *curve = automationCurve(t.automation, axis))
                            {
                                for (auto &point : curve->points)
                                    if (point.frame == node.frame)
                                        point.frame = frame;
                                std::sort(curve->points.begin(), curve->points.end(),
                                          [](auto &a, auto &b) { return a.frame < b.frame; });
                            }
                    });
                if (std::any_of(safe->nodes.begin(), safe->nodes.end(),
                                [&](auto &n) { return n.frame == frame; }))
                    safe->selected = {frame};
                safe->refresh();
            }),
        true);
}
void SurroundPanner::copy()
{
    juce::Array<juce::var> values;
    for (auto &node : nodes)
        if (selected.contains(node.frame))
            values.add(juce::Array<juce::var>{node.frame / session.document.sampleRate, node.x, node.y,
                                              (int)node.shape});
    if (!values.isEmpty())
        juce::SystemClipboard::copyTextToClipboard("SoundPlus Square Motion\n" +
                                                   juce::JSON::toString(values));
}
void SurroundPanner::paste()
{
    auto text = juce::SystemClipboard::getTextFromClipboard();
    if (!text.startsWith("SoundPlus Square Motion\n") || text.getNumBytesAsUTF8() > 512 * 1024)
        return;
    auto decoded = juce::JSON::parse(text.fromFirstOccurrenceOf("\n", false, false));
    auto *rows = decoded.getArray();
    if (!rows || rows->isEmpty() || rows->size() > 2048)
        return;
    auto anchorSeconds = (double)(*rows)[0][0];
    auto frame = session.document.playhead;
    if (snapTime)
        frame = snapTime(frame);
    std::vector<SurroundNode> incoming;
    for (auto &row : *rows)
    {
        const auto seconds = (double)row[0] - anchorSeconds;
        const auto x = (double)row[1], y = (double)row[2];
        const auto shape = (int)row[3];
        if (!row.isArray() || row.size() != 4 || !std::isfinite(seconds) || seconds < 0 ||
            seconds > 7 * 86400 || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 ||
            std::abs(y) > 1 || shape < 0 || shape > 2)
            return;
        incoming.push_back(
            {frame + (int64_t)std::llround(seconds * session.document.sampleRate), x, y, (MotionShape)shape});
    }
    editTrack("Paste surround motion points",
              [&](auto &t)
              {
                  for (auto &node : incoming)
                      setSurroundNode(t, node);
              });
    selected.clear();
    for (auto &node : incoming)
        selected.insert(node.frame);
    refresh();
}
void SurroundPanner::motionMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Numeric time / position…", selected.size() == 1 && add.isEnabled());
    menu.addItem(2, "Linear", !selected.empty() && add.isEnabled());
    menu.addItem(3, "Hold", !selected.empty() && add.isEnabled());
    menu.addItem(4, "Smooth", !selected.empty() && add.isEnabled());
    menu.addSeparator();
    menu.addItem(5, "Copy points", !selected.empty());
    menu.addItem(6, "Paste at playhead", add.isEnabled());
    menu.addItem(7, "Select all points", !nodes.empty());
    menu.addItem(8, "Delete points", !selected.empty() && add.isEnabled());
    juce::Component::SafePointer<SurroundPanner> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(options),
                       [safe](int id)
                       {
                           if (!safe || !safe->valid())
                               return;
                           switch (id)
                           {
                           case 1:
                               safe->numericPoint();
                               break;
                           case 2:
                               safe->applyShape(MotionShape::linear);
                               break;
                           case 3:
                               safe->applyShape(MotionShape::hold);
                               break;
                           case 4:
                               safe->applyShape(MotionShape::smooth);
                               break;
                           case 5:
                               safe->copy();
                               break;
                           case 6:
                               safe->paste();
                               break;
                           case 7:
                               for (auto &node : safe->nodes)
                                   safe->selected.insert(node.frame);
                               safe->refresh();
                               break;
                           case 8:
                               safe->removePoints();
                               break;
                           default:
                               break;
                           }
                       });
}
bool SurroundPanner::keyPressed(const juce::KeyPress &k)
{
    auto *t = track();
    if (!t)
        return false;
    if (k.isKeyCode(juce::KeyPress::escapeKey) && (dragging || selecting))
    {
        finishDrag(true);
        return true;
    }
    if (k.getModifiers().isCommandDown())
    {
        switch (juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode()))
        {
        case 'z':
            finish();
            if (k.getModifiers().isShiftDown())
                session.redo();
            else
                session.undo();
            refresh();
            return true;
        case 'a':
            for (auto &node : nodes)
                selected.insert(node.frame);
            refresh();
            return true;
        case 'c':
            copy();
            return true;
        case 'v':
            paste();
            return true;
        default:
            return command && command(k);
        }
    }
    if (k.isKeyCode(juce::KeyPress::spaceKey))
        return command && command(k);
    if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey))
    {
        removePoints();
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::returnKey) && !selected.empty())
    {
        numericPoint();
        return true;
    }
    auto nudge = k.getModifiers().isShiftDown() ? .005 : .025;
    juce::Point<double> delta{
        (k.isKeyCode(juce::KeyPress::rightKey) - k.isKeyCode(juce::KeyPress::leftKey)) * nudge,
        (k.isKeyCode(juce::KeyPress::upKey) - k.isKeyCode(juce::KeyPress::downKey)) * nudge};
    if (delta == juce::Point<double>())
        return false;
    if (selected.empty())
    {
        auto p = surroundPositionAt(*t, session.document.playhead);
        editPosition(juce::Point<double>{p.x, p.y} + delta);
    }
    else
        editTrack("Nudge motion points",
                  [&](auto &track)
                  {
                      for (auto node : nodes)
                          if (selected.contains(node.frame))
                          {
                              node.x += delta.x;
                              node.y += delta.y;
                              setSurroundNode(track, node);
                          }
                  });
    return true;
}
} // namespace sp
