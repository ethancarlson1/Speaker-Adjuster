#include "plugin/CaptureList.h"

#include "plugin/Theme.h"

class CaptureList::Row final : public juce::Component
{
public:
    explicit Row (CaptureList& ownerList) : owner (ownerList)
    {
        // Clicks on empty parts of the row fall through to the ListBox (selection).
        setInterceptsMouseClicks (false, true);

        name.setEditable (false, true, false);
        name.addMouseListener (this, false);   // a click on the name selects the row too
        name.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        name.setColour (juce::Label::textColourId, theme::ink);
        name.setTooltip ("Double-click to rename");
        name.onTextChange = [this] { if (owner.callbacks.onRename) owner.callbacks.onRename (id, name.getText()); };
        addAndMakeVisible (name);

        include.setButtonText ("In average");
        include.setTooltip ("Untick to leave this position out of the average");
        include.onClick = [this] { if (owner.callbacks.onInclude) owner.callbacks.onInclude (id, include.getToggleState()); };
        addAndMakeVisible (include);

        redo.setButtonText ("Redo");
        redo.setTooltip ("Measure this position again (keeps the name)");
        redo.onClick = [this] { if (owner.callbacks.onRedo) owner.callbacks.onRedo (id); };
        addAndMakeVisible (redo);

        remove.setButtonText ("Delete");
        remove.onClick = [this] { if (owner.callbacks.onDelete) owner.callbacks.onDelete (id); };
        addAndMakeVisible (remove);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &name && e.getNumberOfClicks() == 1 && ! e.mouseWasDraggedSinceMouseDown())
            owner.selectRow (row);
    }

    void update (const MeasurementEngine::Entry& entry, int rowNumber, bool isSelected, bool measuring)
    {
        row = rowNumber;
        id = entry.id;
        capture = entry.capture;
        verify = entry.verify;
        stale = entry.verify && entry.correctionId != owner.appliedId;
        selected = isSelected;
        name.setText (juce::String::fromUTF8 (capture->name.c_str()), juce::dontSendNotification);
        include.setToggleState (! capture->excluded, juce::dontSendNotification);
        include.setButtonText (verify ? "In verified" : "In average");
        include.setTooltip (verify ? "Untick to leave this position out of the verified average"
                                   : "Untick to leave this position out of the average");
        redo.setEnabled (! measuring);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
        g.setColour (selected ? theme::orange.withAlpha (0.18f) : theme::surface);
        g.fillRoundedRectangle (bounds, 4.0f);
        if (capture == nullptr)
            return;

        const auto dim = capture->excluded || stale ? 0.45f : 1.0f;
        const auto grade = capture->grade.overall;
        const auto badge = juce::Rectangle<float> (12.0f, 7.0f, 104.0f, 22.0f);
        g.setColour (theme::gradeColour (grade).withAlpha (0.25f * dim));
        g.fillRoundedRectangle (badge, 4.0f);
        g.setColour (theme::gradeColour (grade).withAlpha (dim));
        g.drawRoundedRectangle (badge, 4.0f, 1.0f);
        g.setColour (theme::ink.withAlpha (dim));
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawText (theme::gradeText (grade), badge, juce::Justification::centred);

        juce::String detail = capture->kind == "program" ? "music" : capture->kind == "noise" ? "pink noise" : "sweep";
        if (verify)
            detail = "verify " + detail;
        else if (! capture->delaysMs.empty())
            detail << "  " << juce::String (capture->delaysMs.front(), 1) << " ms";
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (detail, juce::Rectangle<float> (12.0f, 32.0f, 104.0f, 16.0f), juce::Justification::centredLeft);

        juce::StringArray lines;
        for (const auto& r : capture->grade.reasons)
            lines.add (juce::String::fromUTF8 (r.c_str()));
        for (const auto& n : capture->grade.notes)
            lines.add (juce::String::fromUTF8 (n.c_str()));
        if (stale)
            lines.insert (0, "Measured with an earlier correction.");
        else if (capture->excluded)
            lines.insert (0, verify ? "Not in the verified average." : "Not in the average.");
        else if (verify)
            lines.insert (0, "Through the EQ.");
        g.setColour (theme::ink2.withAlpha (dim));
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (lines.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 ")), reasonsArea(), juce::Justification::topLeft, 2, 0.9f);
    }

    void resized() override
    {
        name.setBounds (126, 4, 240, 22);
        auto right = getLocalBounds().reduced (8, 13).removeFromRight (250);
        remove.setBounds (right.removeFromRight (64));
        right.removeFromRight (6);
        redo.setBounds (right.removeFromRight (64));
        right.removeFromRight (6);
        include.setBounds (right);
    }

private:
    juce::Rectangle<int> reasonsArea() const
    {
        // Under the name, up to the buttons.
        return getLocalBounds().withTrimmedLeft (130).withTrimmedRight (266).withTrimmedTop (27).withTrimmedBottom (3);
    }

    CaptureList& owner;
    juce::Label name;
    juce::ToggleButton include;
    juce::TextButton redo, remove;
    int id = -1, row = -1;
    std::shared_ptr<const roomeq::Capture> capture;
    bool selected = false, verify = false, stale = false;
};

CaptureList::CaptureList (Callbacks cb) : callbacks (std::move (cb))
{
    list.setRowHeight (56);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    list.setColour (juce::ListBox::outlineColourId, theme::axis);
    addAndMakeVisible (list);
}

void CaptureList::setEntries (std::vector<MeasurementEngine::Entry> newEntries, bool isMeasuring, int newAppliedId)
{
    const auto previouslySelected = getSelectedId();
    entries = std::move (newEntries);
    measuring = isMeasuring;
    appliedId = newAppliedId;
    list.updateContent();
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == previouslySelected)
            list.selectRow (static_cast<int> (i), false, true);
    list.repaint();
    for (int row = 0; row < getNumRows(); ++row)
        if (auto* c = dynamic_cast<Row*> (list.getComponentForRowNumber (row)))
            c->update (entries[static_cast<std::size_t> (row)], row, list.isRowSelected (row), measuring);
}

int CaptureList::getSelectedId() const
{
    const auto row = list.getSelectedRow();
    return row >= 0 && row < static_cast<int> (entries.size()) ? entries[static_cast<std::size_t> (row)].id : -1;
}

void CaptureList::resized()
{
    list.setBounds (getLocalBounds());
}

void CaptureList::paint (juce::Graphics& g)
{
    if (entries.empty())
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Captured positions appear here, graded pass / marginal / redo.", getLocalBounds(),
                    juce::Justification::centred);
    }
}

int CaptureList::getNumRows()
{
    return static_cast<int> (entries.size());
}

juce::Component* CaptureList::refreshComponentForRow (int row, bool selected, juce::Component* existing)
{
    if (row < 0 || row >= getNumRows())
    {
        delete existing;
        return nullptr;
    }
    auto* r = dynamic_cast<Row*> (existing);
    if (r == nullptr)
    {
        delete existing;
        r = new Row (*this);
    }
    r->update (entries[static_cast<std::size_t> (row)], row, selected, measuring);
    return r;
}

void CaptureList::selectedRowsChanged (int)
{
    justSelected = true;
    if (callbacks.onSelect)
        callbacks.onSelect (getSelectedId());
}

void CaptureList::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    // A click that didn't just select this row was on the selected row: clear it.
    if (! justSelected && list.isRowSelected (row))
        list.deselectAllRows();
    justSelected = false;
}

void CaptureList::selectRow (int row)
{
    if (row >= 0 && row < getNumRows() && ! list.isRowSelected (row))
        list.selectRow (row);
    justSelected = false;
}
