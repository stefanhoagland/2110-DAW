#include "NetworkView.h"
#include "Look.h"
#include "Theme.h"

using namespace juce;

static const String dot = String (CharPointer_UTF8 (" \xc2\xb7 "));

// True when every word of the search is in the stream's name, address, sender or format.
static bool matchesSearch (const aes67::StreamInfo& s, const String& query)
{
    auto words = StringArray::fromTokens (query.trim(), " ", "");
    words.removeEmptyStrings();
    if (words.isEmpty()) return true;
    const auto text = (s.name + " " + s.address + ":" + String (s.port) + " " + s.origin + " " + s.format()).toLowerCase();
    for (auto& w : words)
        if (! text.contains (w.toLowerCase())) return false;
    return true;
}

NetworkView::NetworkView (AudioEngine& e) : engine (e)
{
    auto& net = engine.network();
    for (auto* l : { &portLabel, &latencyLabel, &streamsLabel, &hint, &error, &sdpLabel })
    {
        l->setFont (FontOptions (13.0f));
        l->setColour (Label::textColourId, theme::textMuted);
        addAndMakeVisible (*l);
    }
    portLabel.setText ("Network port", dontSendNotification);
    latencyLabel.setText ("Buffer", dontSendNotification);
    streamsLabel.setText ("Streams on the network", dontSendNotification);
    streamsLabel.setColour (Label::textColourId, theme::text);
    sdpLabel.setText ("A stream that isn't listed? Paste its SDP here, or load the .sdp file:", dontSendNotification);
    hint.setText ("Tick a stream to receive it. Its channels become tracks after your interface's inputs, "
                  "and show up in the Patch tab. The app follows each stream's clock, so it works without PTP. "
                  "Uncompressed L16/L24 streams (AES67, SMPTE ST 2110-30) are supported.", dontSendNotification);
    hint.setJustificationType (Justification::topLeft);
    error.setColour (Label::textColourId, Colour (0xffffd98a));

    interfaces = aes67::localInterfaces();
    int sel = 0;
    for (int i = 0; i < (int) interfaces.size(); ++i)
    {
        port.addItem (interfaces[(size_t) i].name + "  " + interfaces[(size_t) i].ip, i + 1);
        if (interfaces[(size_t) i].ip == net.getInterface()) sel = i + 1;
    }
    port.setSelectedId (sel, dontSendNotification);
    port.onChange = [this]
    {
        const int i = port.getSelectedId() - 1;
        if (isPositiveAndBelow (i, (int) interfaces.size())) engine.network().setInterface (interfaces[(size_t) i].ip);
    };
    addAndMakeVisible (port);

    const int lat[] = { 2, 5, 10, 20, 40 };
    for (int ms : lat) latency.addItem (String (ms) + " ms", ms);
    latency.setSelectedId ((int) net.getLatencyMs(), dontSendNotification);
    if (latency.getSelectedId() == 0) latency.setText (String (net.getLatencyMs(), 0) + " ms", dontSendNotification);
    latency.setTooltip ("How much audio is held back to ride out network hiccups. Raise it if you hear dropouts.");
    latency.onChange = [this] { engine.network().setLatencyMs (latency.getSelectedId()); engine.network().onChange(); };
    addAndMakeVisible (latency);

    search.setTextToShowWhenEmpty ("Search streams (name, address, sender)", theme::textMuted.withAlpha (0.7f));
    search.setColour (TextEditor::backgroundColourId, theme::background);
    search.setColour (TextEditor::outlineColourId, theme::line);
    search.setFont (FontOptions (13.0f));
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { search.clear(); refresh(); };
    addAndMakeVisible (search);

    list.setOpaque (false);
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    sdpText.setMultiLine (true, false);
    sdpText.setReturnKeyStartsNewLine (true);
    sdpText.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
    sdpText.setTextToShowWhenEmpty ("v=0\no=- 1 1 IN IP4 192.168.1.10\ns=Stage box\nc=IN IP4 239.69.1.1/32\nm=audio 5004 RTP/AVP 98\na=rtpmap:98 L24/48000/8\na=ptime:1",
                                    theme::textMuted.withAlpha (0.6f));
    sdpText.setColour (TextEditor::backgroundColourId, theme::background);
    sdpText.setColour (TextEditor::outlineColourId, theme::line);
    addAndMakeVisible (sdpText);
    WebLook::setStyle (addButton, "primary");
    addButton.onClick = [this] { addFromText (sdpText.getText()); };
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Load an SDP file", File(), "*.sdp;*.txt");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f.existsAsFile()) addFromText (f.loadFileAsString());
        });
    };
    addAndMakeVisible (addButton);
    addAndMakeVisible (loadButton);
    reportButton.setTooltip ("Copy a troubleshooting report of every stream (timing, gaps, dropouts) to paste into a message");
    reportButton.onClick = [this]
    {
        SystemClipboard::copyTextToClipboard ("Audio: " + engine.deviceDescription() + "\n" + engine.network().report());
        showError ("Report copied. Paste it into a message. The timing figures start over now.");
        engine.network().resetStats();
    };
    addAndMakeVisible (reportButton);

    refresh();
    setSize (640, 560);
    startTimer (500);
}

void NetworkView::showError (const String& text)
{
    error.setText (text, dontSendNotification);
}

void NetworkView::addFromText (const String& sdp)
{
    aes67::StreamInfo info;
    String why;
    if (! aes67::parseSdp (sdp, info, why)) { showError (why); return; }
    auto err = engine.network().subscribe (info);
    if (err.isNotEmpty()) { showError (err); return; }
    showError ({});
    sdpText.clear();
}

void NetworkView::refresh()
{
    auto& net = engine.network();
    // Streams being received first, then the rest of what's announced.
    std::vector<std::pair<aes67::StreamInfo, bool>> items;
    const auto query = search.getText();
    int total = 0;
    for (auto& s : net.status())
    {
        ++total;
        if (matchesSearch (s.info, query)) items.push_back ({ s.info, true });
    }
    for (auto& d : net.discovered())
        if (! net.isSubscribed (d.key()))
        {
            ++total;
            if (matchesSearch (d, query)) items.push_back ({ d, false });
        }
    streamsLabel.setText (query.trim().isEmpty() ? "Streams on the network"
                                                 : "Streams on the network" + dot + String ((int) items.size()) + " of " + String (total),
                          dontSendNotification);

    rows.clear();
    for (auto& [info, on] : items)
    {
        auto* r = rows.add (new Row());
        r->key = info.key();
        r->info = info;
        r->receive.setToggleState (on, dontSendNotification);
        r->receive.setTooltip ("Receive this stream");
        r->receive.onClick = [this, r]
        {
            const auto key = r->key;
            const auto stream = r->info;
            const bool wanted = r->receive.getToggleState();
            MessageManager::callAsync ([this, key, stream, wanted, safe = SafePointer<NetworkView> (this)]
            {
                if (safe == nullptr) return;
                if (wanted)
                {
                    auto err = engine.network().subscribe (stream);
                    showError (err);
                    if (err.isNotEmpty()) refresh();
                }
                else engine.network().unsubscribe (key);
            });
        };
        r->name.setText (info.name, dontSendNotification);
        r->name.setFont (FontOptions (14.0f, Font::bold));
        r->detail.setText (info.format() + dot + info.address + ":" + String (info.port)
                           + (info.origin.isNotEmpty() ? dot + "from " + info.origin : String()), dontSendNotification);
        for (auto* l : { &r->detail, &r->state }) { l->setFont (FontOptions (12.0f)); l->setColour (Label::textColourId, theme::textMuted); }
        r->state.setJustificationType (Justification::centredRight);
        for (Component* c : { (Component*) &r->receive, (Component*) &r->name, (Component*) &r->detail, (Component*) &r->state })
            r->addAndMakeVisible (c);
        list.addAndMakeVisible (r);
    }
    if (rows.isEmpty())
    {
        auto* r = rows.add (new Row());
        const bool filtered = total > 0;
        r->name.setText (filtered ? "No streams match \"" + query.trim() + "\"" : "No streams found yet", dontSendNotification);
        r->detail.setText (filtered ? "Try part of the name, the address (239.69...) or the sender's IP. Esc clears the search."
                                    : "Streams announced with SAP appear here within about 30 seconds. Check the network port above.",
                           dontSendNotification);
        r->detail.setFont (FontOptions (12.0f));
        r->detail.setColour (Label::textColourId, theme::textMuted);
        r->addAndMakeVisible (r->name);
        r->addAndMakeVisible (r->detail);
        list.addAndMakeVisible (r);
    }
    updateStates();
    resized();
}

void NetworkView::updateStates()
{
    auto status = engine.network().status();
    const int first = engine.numDeviceInputs();
    for (auto* r : rows)
    {
        if (r->key.isEmpty()) continue;
        auto it = std::find_if (status.begin(), status.end(), [r] (auto& s) { return s.info.key() == r->key; });
        String text = "Announced";
        Colour c = theme::textMuted;
        if (it != status.end())
        {
            const int a = first + it->firstChannel + 1, b = a + it->info.channels - 1;
            const String tracks = a == b ? "Track " + String (a) : "Tracks " + String (a) + "-" + String (b);
            text = (it->receiving ? "Receiving" : "Waiting for audio") + dot + tracks;
            if (it->lost > 0 || it->dropouts > 0)
                text << dot << String (it->lost) << " lost packets, " << String (it->dropouts) << " dropouts";
            if (it->dropouts > 0)
                text << dot << "buffer " << String (it->bufferMs, 0) << " ms";
            c = it->receiving ? theme::ok : Colour (0xffffd98a);
        }
        r->state.setText (text, dontSendNotification);
        r->state.setColour (Label::textColourId, c);
    }
}

void NetworkView::timerCallback() { updateStates(); }

void NetworkView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::line);
    g.drawRect (viewport.getBounds().expanded (1));
}

void NetworkView::Row::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    if (key.isNotEmpty()) receive.setBounds (b.removeFromLeft (28));
    auto top = b.removeFromTop (20);
    state.setBounds (top.removeFromRight (jmin (440, top.getWidth() * 2 / 3)));
    name.setBounds (top);
    detail.setBounds (b);
}

void NetworkView::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto top = r.removeFromTop (28);
    portLabel.setBounds (top.removeFromLeft (100));
    port.setBounds (top.removeFromLeft (260));
    top.removeFromLeft (16);
    latencyLabel.setBounds (top.removeFromLeft (56));
    latency.setBounds (top.removeFromLeft (90));
    r.removeFromTop (8);
    hint.setBounds (r.removeFromTop (48));
    auto head = r.removeFromTop (26);
    search.setBounds (head.removeFromRight (jmin (280, head.getWidth() / 2)).reduced (0, 1));
    streamsLabel.setBounds (head);
    r.removeFromTop (4);

    auto bottom = r.removeFromBottom (24);
    error.setBounds (bottom);
    r.removeFromBottom (6);
    auto buttons = r.removeFromBottom (28);
    addButton.setBounds (buttons.removeFromRight (110));
    buttons.removeFromRight (8);
    loadButton.setBounds (buttons.removeFromRight (130));
    reportButton.setBounds (buttons.removeFromLeft (110));
    r.removeFromBottom (6);
    sdpText.setBounds (r.removeFromBottom (90));
    sdpLabel.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (6);

    viewport.setBounds (r);
    const int w = viewport.getMaximumVisibleWidth();
    int y = 0;
    for (auto* row : rows) { row->setBounds (0, y, w, 50); y += 50; }
    list.setSize (w, jmax (y, 1));
}
