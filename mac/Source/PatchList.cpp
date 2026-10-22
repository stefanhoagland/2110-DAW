#include "PatchList.h"
#include "Look.h"
#include "Theme.h"

using namespace juce;

static const String dot = String (CharPointer_UTF8 (" \xc2\xb7 "));
static const String dragPrefix = "patch:";

// True when every word of the filter is somewhere in `text`.
static bool matchesFilter (const String& text, const String& filter)
{
    auto words = StringArray::fromTokens (filter.trim().toLowerCase(), " ", "");
    words.removeEmptyStrings();
    const auto t = text.toLowerCase();
    for (auto& w : words)
        if (! t.contains (w)) return false;
    return true;
}

//==============================================================================
// The tracks down the left: number, name and what feeds it. A drop target for channels.
class PatchListView::Tracks : public ListBox, private ListBoxModel, public DragAndDropTarget
{
public:
    explicit Tracks (PatchListView& o) : owner (o)
    {
        setModel (this);
        setRowHeight (26);
        setMultipleSelectionEnabled (true);
        setColour (ListBox::backgroundColourId, theme::background);
        setColour (ListBox::outlineColourId, theme::line);
        setOutlineThickness (1);
        setHeaderComponent (std::make_unique<Header> (*this));
    }

    // Column titles, like Dante's Channel / Connected to / Status.
    struct Header : Component
    {
        explicit Header (Tracks& t) : tracks (t) { setSize (100, 24); }
        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel2);
            g.setFont (FontOptions (12.5f, Font::bold));
            g.setColour (theme::textMuted);
            g.drawText ("Track", 10, 0, 160, getHeight(), Justification::centredLeft);
            g.drawText ("Connected to", tracks.nameWidth, 0, 200, getHeight(), Justification::centredLeft);
            g.drawText ("Status", getWidth() - 56, 0, 50, getHeight(), Justification::centredLeft);
        }
        Tracks& tracks;
    };

    int getNumRows() override { return AudioEngine::numTracks; }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool rowSelected) override
    {
        const bool dropHere = dropRow >= 0 && row >= dropRow && row < dropRow + dropCount;
        g.fillAll (dropHere ? theme::accent.withAlpha (0.28f)
                            : rowSelected ? theme::accent.withAlpha (0.16f)
                                       : (row % 2 == 0 ? theme::panel : theme::laneB));
        g.setColour (theme::line.withAlpha (0.6f));
        g.fillRect (0, h - 1, w, 1);

        const int src = owner.engine.getTrackSource (row);
        const bool patched = src >= 0 && src < owner.engine.numInputs();
        g.setFont (FontOptions (13.0f));
        g.setColour (theme::textMuted);
        g.drawText (String (row + 1).paddedLeft ('0', 2), 10, 0, 34, h, Justification::centredLeft);
        g.setColour (theme::text);
        g.drawText (owner.trackName (row), 48, 0, nameWidth - 54, h, Justification::centredLeft, true);
        g.setColour (patched ? theme::text : theme::textMuted);
        g.drawText (patched ? owner.sourceLabel (src) : String (CharPointer_UTF8 ("\xe2\x80\x94")),
                    nameWidth, 0, w - nameWidth - 30, h, Justification::centredLeft, true);
        // A dot like Dante's status column: green when patched.
        g.setColour (patched ? theme::ok : theme::line);
        g.fillEllipse ((float) w - 22.0f, (float) h * 0.5f - 4.0f, 8.0f, 8.0f);
    }

    void deleteKeyPressed (int) override { owner.unpatchSelected(); }
    void backgroundClicked (const MouseEvent&) override { deselectAllRows(); }
    void selectedRowsChanged (int) override { owner.unpatch.setEnabled (getNumSelectedRows() > 0); }

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith (dragPrefix); }

    void itemDragMove (const SourceDetails& d) override
    {
        int row = getRowContainingPosition (d.localPosition.x, d.localPosition.y);
        if (row < 0 && d.localPosition.y > 0) row = getNumRows() - 1;
        const int count = StringArray::fromTokens (d.description.toString().fromFirstOccurrenceOf (dragPrefix, false, false), "|", "").size();
        if (row != dropRow || count != dropCount) { dropRow = row; dropCount = count; repaint(); }
    }

    void itemDragExit (const SourceDetails&) override { dropRow = -1; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        const int row = getRowContainingPosition (d.localPosition.x, d.localPosition.y);
        dropRow = -1;
        repaint();
        if (row < 0) return;
        auto items = StringArray::fromTokens (d.description.toString().fromFirstOccurrenceOf (dragPrefix, false, false), "|", "");
        owner.patch (items, row);
        selectRangeOfRows (row, row + items.size() - 1);
    }

    int nameWidth = 220;

private:
    PatchListView& owner;
    int dropRow = -1, dropCount = 0;
};

//==============================================================================
// A row on the right: a group (the interface, or a stream) or one of its channels.
class PatchListView::SourceItem : public TreeViewItem
{
public:
    SourceItem (PatchListView& o, String idIn, String labelIn, String detailIn, bool isGroup)
        : owner (o), id (std::move (idIn)), label (std::move (labelIn)), detail (std::move (detailIn)), group (isGroup) {}

    bool mightContainSubItems() override { return group; }
    String getUniqueName() const override { return id; }
    int getItemHeight() const override { return group ? 28 : 24; }

    void paintItem (Graphics& g, int w, int h) override
    {
        if (isSelected()) { g.setColour (theme::accent.withAlpha (0.22f)); g.fillRoundedRectangle (0.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 4.0f); }
        g.setFont (FontOptions (group ? 14.0f : 13.0f, group ? Font::bold : Font::plain));
        g.setColour (theme::text);
        const int lw = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 12;
        g.drawText (label, 4, 0, w - 8, h, Justification::centredLeft, true);
        if (detail.isNotEmpty())
        {
            g.setFont (FontOptions (12.0f));
            g.setColour (theme::textMuted);
            g.drawText (detail, 4 + lw, 0, w - lw - 8, h, Justification::centredLeft, true);
        }
    }

    void paintOpenCloseButton (Graphics& g, const Rectangle<float>& area, Colour, bool) override
    {
        Path p;
        const auto c = area.getCentre();
        if (isOpen()) p.addTriangle (c.x - 4.5f, c.y - 2.5f, c.x + 4.5f, c.y - 2.5f, c.x, c.y + 3.5f);
        else          p.addTriangle (c.x - 2.5f, c.y - 4.5f, c.x - 2.5f, c.y + 4.5f, c.x + 3.5f, c.y);
        g.setColour (theme::textMuted);
        g.fillPath (p);
    }

    var getDragSourceDescription() override
    {
        const auto d = owner.dragDescription();
        return d.isEmpty() ? var() : var (d);
    }

    void itemDoubleClicked (const MouseEvent&) override
    {
        StringArray ids;
        if (group) { for (int i = 0; i < getNumSubItems(); ++i) ids.add (static_cast<SourceItem*> (getSubItem (i))->id); }
        else ids.add (id);
        owner.patchToSelectedTrack (ids);
    }

    PatchListView& owner;
    String id, label, detail;
    bool group;
};

//==============================================================================
PatchListView::PatchListView (AudioEngine& e, std::function<String (int)> names)
    : engine (e), trackName (std::move (names))
{
    for (auto* l : { &tracksTitle, &sourcesTitle })
    {
        l->setFont (FontOptions (15.0f, Font::bold));
        l->setColour (Label::textColourId, theme::text);
        l->setJustificationType (Justification::centred);
        addAndMakeVisible (*l);
    }
    tracksTitle.setText ("Tracks", dontSendNotification);
    sourcesTitle.setText ("Available channels", dontSendNotification);
    for (auto* l : { &hint, &message })
    {
        l->setFont (FontOptions (12.5f));
        l->setColour (Label::textColourId, theme::textMuted);
        addAndMakeVisible (*l);
    }
    hint.setText ("Drag channels from the right onto a track (select several, or a whole stream, to fill the tracks after it). "
                  "Double-click a channel to patch it to the selected track. Delete unpatches.", dontSendNotification);
    message.setColour (Label::textColourId, Colour (0xffffd98a));

    tracks = std::make_unique<Tracks> (*this);
    addAndMakeVisible (*tracks);
    unpatch.setEnabled (false);
    unpatch.setTooltip ("Disconnect the selected tracks");
    unpatch.onClick = [this] { unpatchSelected(); };
    addAndMakeVisible (unpatch);

    filter.setTextToShowWhenEmpty ("Filter", theme::textMuted.withAlpha (0.7f));
    filter.setColour (TextEditor::backgroundColourId, theme::background);
    filter.setColour (TextEditor::outlineColourId, theme::line);
    filter.setFont (FontOptions (13.0f));
    filter.onTextChange = [this] { rebuildTree(); };
    filter.onEscapeKey = [this] { filter.clear(); rebuildTree(); };
    addAndMakeVisible (filter);

    root = std::make_unique<SourceItem> (*this, "root", "", "", true);
    tree.setRootItem (root.get());
    tree.setRootItemVisible (false);
    tree.setMultiSelectEnabled (true);
    tree.setIndentSize (18);
    tree.setColour (TreeView::backgroundColourId, theme::background);
    tree.setColour (TreeView::linesColourId, theme::line);
    tree.setColour (TreeView::dragAndDropIndicatorColourId, theme::accent);
    tree.setColour (TreeView::selectedItemBackgroundColourId, Colours::transparentBlack);
    addAndMakeVisible (tree);
    refresh();
}

PatchListView::~PatchListView()
{
    tree.setRootItem (nullptr);
}

String PatchListView::sourceLabel (int input) const
{
    const int dev = engine.numDeviceInputs();
    if (input < dev)
    {
        auto* d = engine.devices().getCurrentAudioDevice();
        const auto n = d != nullptr ? d->getInputChannelNames()[input] : String();
        return "In " + String (input + 1) + (n.isNotEmpty() && n != String (input + 1) ? "  " + n : String());
    }
    // Dante style: channel@stream
    for (auto& s : engine.network().status())
    {
        const int ch = input - dev - s.firstChannel;
        if (ch >= 0 && ch < s.info.channels) return String (ch + 1) + "@" + s.info.name;
    }
    return engine.inputName (input);
}

void PatchListView::refresh()
{
    tracks->updateContent();
    tracks->repaint();
    rebuildTree();
    unpatch.setEnabled (tracks->getNumSelectedRows() > 0);
}

void PatchListView::rebuildTree()
{
    struct Group { String id, label, detail; StringArray ids, labels; };
    std::vector<Group> groups;

    // The audio interface's inputs.
    if (const int dev = engine.numDeviceInputs(); dev > 0)
    {
        auto* d = engine.devices().getCurrentAudioDevice();
        Group g { "g:interface", d != nullptr ? d->getName() : String ("Audio interface"), "interface inputs", {}, {} };
        for (int i = 0; i < dev; ++i)
        {
            g.ids.add ("in:" + String (i));
            g.labels.add (sourceLabel (i));
        }
        groups.push_back (g);
    }
    // AES67 streams being received, then the ones only announced.
    auto& net = engine.network();
    for (auto& s : net.status())
    {
        Group g { "g:" + s.info.key(), s.info.name, s.info.format(), {}, {} };
        for (int c = 0; c < s.info.channels; ++c)
        {
            g.ids.add ("net:" + s.info.key() + "#" + String (c));
            g.labels.add (String (c + 1) + "  " + s.info.name + " " + String (c + 1));
        }
        groups.push_back (g);
    }
    for (auto& info : net.discovered())
    {
        if (net.isSubscribed (info.key())) continue;
        Group g { "g:" + info.key(), info.name, info.format() + dot + "not received yet (drag to receive)", {}, {} };
        for (int c = 0; c < info.channels; ++c)
        {
            g.ids.add ("net:" + info.key() + "#" + String (c));
            g.labels.add (String (c + 1) + "  " + info.name + " " + String (c + 1));
        }
        groups.push_back (g);
    }

    // Filter: a group whose name matches shows all its channels; otherwise only matching channels.
    const auto f = filter.getText().trim();
    String shape = f;
    for (auto& g : groups)
    {
        if (f.isNotEmpty() && ! matchesFilter (g.label, f))
        {
            StringArray ids, labels;
            for (int i = 0; i < g.ids.size(); ++i)
                if (matchesFilter (g.label + " " + g.labels[i], f)) { ids.add (g.ids[i]); labels.add (g.labels[i]); }
            g.ids = ids;
            g.labels = labels;
        }
        shape << "|" << g.id << g.label << g.detail << g.labels.joinIntoString (",");
    }
    if (shape == treeShape) return;
    treeShape = shape;

    auto state = tree.getOpennessState (true);
    root->clearSubItems();
    for (auto& g : groups)
    {
        if (g.ids.isEmpty()) continue;
        auto* gi = new SourceItem (*this, g.id, g.label, g.detail, true);
        root->addSubItem (gi);
        for (int i = 0; i < g.ids.size(); ++i)
            gi->addSubItem (new SourceItem (*this, g.ids[i], g.labels[i], {}, false));
    }
    if (state != nullptr) tree.restoreOpennessState (*state, true);
    for (int i = 0; i < root->getNumSubItems(); ++i)
    {
        auto* gi = root->getSubItem (i);
        // New groups start open; while filtering, everything that matches is shown.
        if (f.isNotEmpty() || state == nullptr || state->getChildByAttribute ("id", gi->getUniqueName()) == nullptr)
            gi->setOpen (true);
    }
    if (root->getNumSubItems() == 0)
        message.setText (f.isNotEmpty() ? "No channels match \"" + f + "\"." : "No inputs yet. Open an audio interface or receive an AES67 stream.",
                         dontSendNotification);
    else if (message.getText().startsWith ("No channels match") || message.getText().startsWith ("No inputs"))
        message.setText ({}, dontSendNotification);
}

String PatchListView::dragDescription() const
{
    StringArray ids;
    for (int i = 0; i < root->getNumSubItems(); ++i)
    {
        auto* g = root->getSubItem (i);
        for (int k = 0; k < g->getNumSubItems(); ++k)
        {
            auto* c = static_cast<SourceItem*> (g->getSubItem (k));
            if (g->isSelected() || c->isSelected()) ids.addIfNotAlreadyThere (c->id);
        }
    }
    return ids.isEmpty() ? String() : dragPrefix + ids.joinIntoString ("|");
}

String PatchListView::patch (const StringArray& sources, int firstTrack)
{
    auto& net = engine.network();
    // Start any announced streams that aren't being received yet.
    StringArray started;
    for (auto& id : sources)
    {
        if (! id.startsWith ("net:")) continue;
        const auto key = id.fromFirstOccurrenceOf ("net:", false, false).upToLastOccurrenceOf ("#", false, false);
        if (net.isSubscribed (key)) continue;
        for (auto& info : net.discovered())
            if (info.key() == key)
            {
                auto err = net.subscribe (info);
                if (err.isNotEmpty()) { message.setText (err, dontSendNotification); return err; }
                started.addIfNotAlreadyThere (key);
            }
    }

    const auto status = net.status();
    const int dev = engine.numDeviceInputs();
    // A stream started here feeds only the channels dragged, not a track for each of its
    // channels: the extra tracks it brings are left empty, ready to patch.
    for (auto& s : status)
        if (started.contains (s.info.key()))
            for (int track = 0; track < AudioEngine::numTracks; ++track)
            {
                const int src = engine.getTrackSource (track);
                if (src >= dev + s.firstChannel && src < dev + s.firstChannel + s.info.channels)
                    engine.setTrackSource (track, -1);
            }

    touched.clear();
    int t = firstTrack, placed = 0;
    for (auto& id : sources)
    {
        int input = -1;
        if (id.startsWith ("in:")) input = id.fromFirstOccurrenceOf ("in:", false, false).getIntValue();
        else
        {
            const auto key = id.fromFirstOccurrenceOf ("net:", false, false).upToLastOccurrenceOf ("#", false, false);
            const int ch = id.fromLastOccurrenceOf ("#", false, false).getIntValue();
            for (auto& s : status)
                if (s.info.key() == key) input = dev + s.firstChannel + ch;
        }
        if (input < 0 || input >= engine.numInputs()) continue;
        if (t >= AudioEngine::numTracks) break;
        touched.add (t);
        engine.setTrackSource (t++, input);
        ++placed;
    }
    const int missed = sources.size() - placed;
    message.setText (missed > 0 ? String (missed) + " channel" + (missed == 1 ? " didn't" : "s didn't") + " fit: there are only "
                                  + String (AudioEngine::numTracks) + " tracks."
                                : String(),
                     dontSendNotification);
    if (onChange) onChange();
    refresh();
    return {};
}

void PatchListView::patchToSelectedTrack (const StringArray& sources)
{
    int row = tracks->getSelectedRow();
    if (row < 0)
    {
        message.setText ("Select a track on the left first (or drag the channel onto it).", dontSendNotification);
        return;
    }
    patch (sources, row);
    // Move down past what was just patched, ready for the next channel.
    const int next = jmin (row + sources.size(), tracks->getNumRows() - 1);
    tracks->selectRow (next);
}

void PatchListView::unpatchSelected()
{
    auto rows = tracks->getSelectedRows();
    if (rows.isEmpty()) return;
    touched.clear();
    for (int i = 0; i < rows.size(); ++i) { engine.setTrackSource (rows[i], -1); touched.add (rows[i]); }
    if (onChange) onChange();
    refresh();
}

bool PatchListView::keyPressed (const KeyPress& k)
{
    if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey) { unpatchSelected(); return true; }
    return false;
}

void PatchListView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
}

void PatchListView::resized()
{
    auto r = getLocalBounds().reduced (16, 10);
    hint.setBounds (r.removeFromTop (22));
    message.setBounds (r.removeFromBottom (22));
    r.removeFromTop (6);
    auto left = r.removeFromLeft ((int) (r.getWidth() * 0.56f));
    r.removeFromLeft (16);

    tracksTitle.setBounds (left.removeFromTop (28));
    auto bottom = left.removeFromBottom (32);
    unpatch.setBounds (bottom.withSizeKeepingCentre (120, 28));
    left.removeFromBottom (6);
    tracks->nameWidth = jmax (160, left.getWidth() / 2 - 20);
    tracks->setBounds (left);

    sourcesTitle.setBounds (r.removeFromTop (28));
    filter.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);
    tree.setBounds (r);
}
