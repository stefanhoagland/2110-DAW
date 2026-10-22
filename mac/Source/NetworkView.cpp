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

NetworkView::NetworkView (AudioEngine& e, int page) : engine (e)
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
    hint.setText ("Tick a stream to receive it (or drag its channels onto tracks in the Patch tab). "
                  "New streams land on the tracks after your interface's inputs until you patch them. The app follows each stream's clock, so it works without PTP. "
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
        if (isPositiveAndBelow (i, (int) interfaces.size()))
        {
            engine.network().setInterface (interfaces[(size_t) i].ip);
            engine.sending().setInterface (interfaces[(size_t) i].ip);
        }
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

    // ---- (no tabs: each page is its own window) ----
    // Each page is its own window now (Receivers and Transmitters in the setup bar), so no tabs.

    // ---- Send ----
    for (auto* l : { &ptpState, &domainLabel, &sendLabel, &sendHint, &newLabel, &nameLabel, &chLabel, &ptimeLabel, &addrLabel, &feedLabel })
    {
        l->setFont (FontOptions (13.0f));
        l->setColour (Label::textColourId, theme::textMuted);
        addChildComponent (*l);
    }
    ptpState.setFont (FontOptions (13.0f, Font::bold));
    ptpState.setJustificationType (Justification::topLeft);
    domainLabel.setText ("PTP domain", dontSendNotification);
    sendLabel.setText ("Streams this Mac sends", dontSendNotification);
    sendLabel.setColour (Label::textColourId, theme::text);
    newLabel.setText ("New stream", dontSendNotification);
    newLabel.setColour (Label::textColourId, theme::text);
    nameLabel.setText ("Name", dontSendNotification);
    chLabel.setText ("Channels", dontSendNotification);
    ptimeLabel.setText ("Packet", dontSendNotification);
    addrLabel.setText ("Address", dontSendNotification);
    feedLabel.setText ("Feed from", dontSendNotification);
    sendHint.setText ("Each stream's channels show up as outputs in Patch > Outputs, so any main, aux or track can feed them. "
                      "For Dante gear in AES67 mode, use 1 ms packets and up to 8 channels.", dontSendNotification);
    sendHint.setJustificationType (Justification::topLeft);
    sendHint.setMinimumHorizontalScale (1.0f);

    for (int d = 0; d <= 127; ++d) domain.addItem (String (d), d + 1);
    domain.setSelectedId (engine.sending().getPtpDomain() + 1, dontSendNotification);
    domain.setTooltip ("The PTP domain your network's grandmaster uses (0 unless it was changed)");
    domain.onChange = [this] { engine.sending().setPtpDomain (domain.getSelectedId() - 1); updateSendStates(); };
    addChildComponent (domain);

    for (int c : { 1, 2, 4, 6, 8, 16, 32, 64 }) channels.addItem (String (c), c);
    channels.setSelectedId (2, dontSendNotification);
    ptime.addItem ("1 ms (up to 8 ch)", 1000);
    ptime.addItem (String (CharPointer_UTF8 ("250 \xc2\xb5s (up to 32 ch)")), 250);
    ptime.addItem (String (CharPointer_UTF8 ("125 \xc2\xb5s (up to 64 ch)")), 125);
    ptime.setSelectedId (1000, dontSendNotification);
    ptime.setTooltip ("Packet time. 1 ms is the AES67 default and what Dante uses.");
    channels.onChange = [this]
    {
        // Pick the longest packet time that fits the channel count.
        const int c = channels.getSelectedId();
        if (c > aes67::SendConfig::maxChannelsFor (ptime.getSelectedId()))
            ptime.setSelectedId (c <= 8 ? 1000 : c <= 32 ? 250 : 125, dontSendNotification);
    };

    // Feed choices: each picks the first source; the stream's channels take it and the ones after.
    for (int m = 0; m < AudioEngine::numMains; ++m)
        for (int side = 0; side < 2; ++side) feedSources.push_back (AudioEngine::mainSource (m, side));
    for (int a = 0; a < AudioEngine::numAux; ++a) feedSources.push_back (AudioEngine::firstAux + a);
    for (int t = 0; t < AudioEngine::numTracks; ++t) feedSources.push_back (AudioEngine::firstDirect + t);
    feed.setTooltip ("What the stream's first channel carries; the next channels take the next sources. Change it any time in Patch > Outputs.");
    feed.onChange = [this]
    {
        const int id = feed.getSelectedId();
        if (id >= 2 && id < 2 + 2 * AudioEngine::numMains && (id - 2) % 2 == 0 && channels.getSelectedId() == 1) channels.setSelectedId (2);
    };

    for (auto* ed : { &nameEdit, &addrEdit })
    {
        ed->setFont (FontOptions (13.0f));
        ed->setColour (TextEditor::backgroundColourId, theme::background);
        ed->setColour (TextEditor::outlineColourId, theme::line);
        ed->setIndents (6, 5);
        addChildComponent (*ed);
    }
    nameEdit.setTextToShowWhenEmpty ("Main mix", theme::textMuted.withAlpha (0.6f));
    addrEdit.setTooltip ("Multicast address the stream goes to. Each stream needs its own.");
    for (auto* c : { &channels, &ptime, &feed }) addChildComponent (*c);
    WebLook::setStyle (createButton, "primary");
    createButton.onClick = [this] { createStream(); };
    addChildComponent (createButton);

    sendList.setOpaque (false);
    sendViewport.setViewedComponent (&sendList, false);
    sendViewport.setScrollBarsShown (true, false);
    addChildComponent (sendViewport);

    refresh();
    showTab (page == 1 ? 1 : 0);
    setSize (720, 620);
    startTimer (500);
}

void NetworkView::showTab (int t)
{
    tab = t;
    for (Component* c : { (Component*) &latencyLabel, (Component*) &latency, (Component*) &streamsLabel, (Component*) &hint,
                          (Component*) &sdpLabel, (Component*) &search, (Component*) &viewport, (Component*) &sdpText,
                          (Component*) &addButton, (Component*) &loadButton, (Component*) &reportButton })
        c->setVisible (t == 0);
    for (Component* c : { (Component*) &ptpState, (Component*) &domainLabel, (Component*) &domain, (Component*) &sendLabel,
                          (Component*) &sendHint, (Component*) &sendViewport, (Component*) &newLabel, (Component*) &nameLabel,
                          (Component*) &nameEdit, (Component*) &chLabel, (Component*) &channels, (Component*) &ptimeLabel,
                          (Component*) &ptime, (Component*) &addrLabel, (Component*) &addrEdit, (Component*) &feedLabel,
                          (Component*) &feed, (Component*) &createButton })
        c->setVisible (t == 1);
    showError ({});
    if (t == 1) refreshSend();
    resized();
    repaint();
}

void NetworkView::refreshSend()
{
    auto& send = engine.sending();
    // Feed choices, with the buses' current names.
    const int keep = feed.getSelectedId();
    feed.clear (dontSendNotification);
    feed.addItem ("Nothing yet (patch it later)", 1);
    for (int m = 0; m < AudioEngine::numMains; ++m)
        feed.addItem (engine.busFullName (true, m) + (m == 0 ? " (master)" : "") + " L/R", 2 + 2 * m);
    for (int a = 0; a < AudioEngine::numAux; ++a)
        feed.addItem (engine.busFullName (false, a), 2 + 2 * AudioEngine::numMains + a);
    for (int t = 0; t < AudioEngine::numTracks; t += 8)
        feed.addItem ("Tracks from " + String (t + 1) + " (direct)", 2 + 2 * AudioEngine::numMains + AudioEngine::numAux + t);
    feed.setSelectedId (keep > 0 ? keep : 2, dontSendNotification);

    StringArray taken;
    for (auto& d : engine.network().discovered()) taken.add (d.address);
    for (auto& r : engine.network().status()) taken.add (r.info.address);
    const auto configs = send.streams();
    if (addrEdit.getText().isEmpty() || taken.contains (addrEdit.getText())
        || std::any_of (configs.begin(), configs.end(), [this] (auto& c) { return c.address == addrEdit.getText(); }))
        addrEdit.setText (send.suggestAddress (taken), false);

    sendRows.clear();
    for (auto& c : configs)
    {
        auto* r = sendRows.add (new SendRow());
        r->id = c.id();
        r->name.setText (c.name, dontSendNotification);
        r->name.setFont (FontOptions (14.0f, Font::bold));
        r->detail.setText (c.format() + dot + c.address + ":" + String (c.port), dontSendNotification);
        for (auto* l : { &r->detail, &r->state }) { l->setFont (FontOptions (12.0f)); l->setColour (Label::textColourId, theme::textMuted); }
        r->state.setJustificationType (Justification::centredRight);
        for (auto* b : { &r->copy, &r->remove }) WebLook::setStyle (*b, "small");
        r->copy.setTooltip ("Copy this stream's SDP, to paste into a receiver that can't see SAP announcements");
        const auto id = c.id();
        r->copy.onClick = [this, id]
        {
            SystemClipboard::copyTextToClipboard (engine.sending().sdpFor (id));
            showError ("SDP copied.");
        };
        r->remove.onClick = [this, id]
        {
            MessageManager::callAsync ([this, id, safe = SafePointer<NetworkView> (this)]
            {
                if (safe == nullptr) return;
                for (auto& cfg : engine.sending().streams())
                    if (cfg.id() == id)
                        for (int ch = 0; ch < cfg.channels; ++ch) engine.clearOutput (AudioEngine::firstNetOut + cfg.firstChannel + ch);
                engine.sending().removeStream (id);
            });
        };
        for (Component* comp : { (Component*) &r->name, (Component*) &r->detail, (Component*) &r->state, (Component*) &r->copy, (Component*) &r->remove })
            r->addAndMakeVisible (comp);
        sendList.addAndMakeVisible (r);
    }
    if (sendRows.isEmpty())
    {
        auto* r = sendRows.add (new SendRow());
        r->name.setText ("Not sending any streams yet", dontSendNotification);
        r->detail.setText ("Create one below. It's announced on the network (SAP), so AES67 receivers can find it.", dontSendNotification);
        r->detail.setFont (FontOptions (12.0f));
        r->detail.setColour (Label::textColourId, theme::textMuted);
        r->addAndMakeVisible (r->name);
        r->addAndMakeVisible (r->detail);
        sendList.addAndMakeVisible (r);
    }
    updateSendStates();
    resized();
}

void NetworkView::updateSendStates()
{
    const auto p = engine.sending().ptpStatus();
    String text;
    Colour c = theme::ok;
    if (p.error.isNotEmpty()) { text = "PTP: " + p.error; c = Colour (0xffffd98a); }
    else if (p.locked)
    {
        text = "PTP locked to grandmaster " + p.grandmaster + " (domain " + String (p.domain) + ", from " + p.masterAddress + ")\n";
        text << "Jitter " << String (roundToInt (p.jitterUs)) << String (CharPointer_UTF8 (" \xc2\xb5s"))
             << dot << (p.delayMeasured ? "path delay " + String (roundToInt (p.delayUs)) + String (CharPointer_UTF8 (" \xc2\xb5s")) : String ("measuring path delay"))
             << dot << "Mac clock " << String (p.rateErrorPpm, 1) << " ppm";
    }
    else if (p.masterSeen)
    {
        text = "PTP: locking to grandmaster " + p.grandmaster + "...";
        c = Colour (0xffffd98a);
    }
    else
    {
        text = "PTP: no grandmaster found on domain " + String (p.domain) + ".\n"
               "Streams still go out on the Mac's own clock, but Dante and most AES67 gear need a PTP grandmaster to accept them.";
        c = Colour (0xffffd98a);
    }
    ptpState.setText (text, dontSendNotification);
    ptpState.setColour (Label::textColourId, c);

    const auto status = engine.sending().status();
    for (auto* r : sendRows)
    {
        if (r->id.isEmpty()) continue;
        auto it = std::find_if (status.begin(), status.end(), [r] (auto& s) { return s.config.id() == r->id; });
        if (it == status.end()) continue;
        String t = it->sending ? "Sending" : "Starting";
        t << dot << (it->audio ? "audio flowing" : "silence (no audio yet)");
        t << dot << "outputs " << String (it->config.firstChannel + 1) << "-" << String (it->config.firstChannel + it->config.channels);
        if (it->underruns > 0) t << dot << String (it->underruns) << " underruns";
        r->state.setText (t, dontSendNotification);
        r->state.setColour (Label::textColourId, it->sending && it->audio ? theme::ok : Colour (0xffffd98a));
    }
}

void NetworkView::createStream()
{
    aes67::SendConfig c;
    c.name = nameEdit.getText().trim();
    if (c.name.isEmpty()) c.name = feed.getSelectedId() > 1 ? feed.getText().upToLastOccurrenceOf (" L/R", false, false) : String ("Soundbending");
    c.address = addrEdit.getText().trim();
    c.channels = channels.getSelectedId();
    c.ptimeUs = ptime.getSelectedId();
    c.firstChannel = -1;
    auto err = engine.sending().addStream (c);
    if (err.isNotEmpty()) { showError (err); return; }
    const int start = feed.getSelectedId() - 2;
    if (start >= 0)
        for (int ch = 0; ch < c.channels && start + ch < (int) feedSources.size(); ++ch)
            engine.setPatch (feedSources[(size_t) (start + ch)], AudioEngine::firstNetOut + c.firstChannel + ch, true);
    nameEdit.clear();
    addrEdit.clear();
    showError ("Sending " + c.name + " to " + c.address + ".");
    refreshSend();
}

void NetworkView::SendRow::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    if (id.isNotEmpty())
    {
        auto buttons = b.removeFromRight (150).withSizeKeepingCentre (150, 24);
        remove.setBounds (buttons.removeFromRight (64));
        buttons.removeFromRight (6);
        copy.setBounds (buttons);
        b.removeFromRight (8);
    }
    auto top = b.removeFromTop (20);
    state.setBounds (top.removeFromRight (jmin (380, top.getWidth() * 2 / 3)));
    name.setBounds (top);
    detail.setBounds (b);
}

void NetworkView::layoutSend (Rectangle<int> r)
{
    auto head = r.removeFromTop (40);
    auto dom = head.removeFromRight (170).removeFromTop (28);
    domainLabel.setBounds (dom.removeFromLeft (80));
    domain.setBounds (dom);
    ptpState.setBounds (head);
    r.removeFromTop (8);
    sendHint.setBounds (r.removeFromTop (40));
    sendLabel.setBounds (r.removeFromTop (24));
    r.removeFromTop (4);

    error.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (6);
    auto row2 = r.removeFromBottom (28);
    r.removeFromBottom (8);
    auto row1 = r.removeFromBottom (28);
    r.removeFromBottom (4);
    newLabel.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (6);

    nameLabel.setBounds (row1.removeFromLeft (70));
    nameEdit.setBounds (row1.removeFromLeft (190));
    row1.removeFromLeft (14);
    chLabel.setBounds (row1.removeFromLeft (64));
    channels.setBounds (row1.removeFromLeft (64));
    row1.removeFromLeft (14);
    ptimeLabel.setBounds (row1.removeFromLeft (52));
    ptime.setBounds (row1.removeFromLeft (jmin (180, row1.getWidth())));

    addrLabel.setBounds (row2.removeFromLeft (70));
    addrEdit.setBounds (row2.removeFromLeft (130));
    row2.removeFromLeft (14);
    createButton.setBounds (row2.removeFromRight (130));
    row2.removeFromRight (10);
    feedLabel.setBounds (row2.removeFromLeft (70));
    feed.setBounds (row2);

    sendViewport.setBounds (r);
    const int w = sendViewport.getMaximumVisibleWidth();
    int y = 0;
    for (auto* row : sendRows) { row->setBounds (0, y, w, 50); y += 50; }
    sendList.setSize (w, jmax (y, 1));
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
    if (tab == 1) refreshSend();
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

// Which tracks this stream's channels are patched to, like "Tracks 3-4, 9-10".
String NetworkView::tracksFedBy (int firstInput, int channels) const
{
    Array<int> fed;
    for (int t = 0; t < AudioEngine::numTracks; ++t)
    {
        const int src = engine.getTrackSource (t);
        if (src >= firstInput && src < firstInput + channels) fed.add (t + 1);
    }
    if (fed.isEmpty()) return "not patched to a track";
    StringArray runs;
    for (int i = 0; i < fed.size();)
    {
        int j = i;
        while (j + 1 < fed.size() && fed[j + 1] == fed[j] + 1) ++j;
        runs.add (i == j ? String (fed[i]) : String (fed[i]) + "-" + String (fed[j]));
        i = j + 1;
    }
    return (fed.size() == 1 ? "Track " : "Tracks ") + runs.joinIntoString (", ");
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
            const String tracks = tracksFedBy (first + it->firstChannel, it->info.channels);
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

void NetworkView::timerCallback()
{
    if (tab == 0) updateStates();
    else updateSendStates();
}

void NetworkView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::line);
    g.drawRect ((tab == 0 ? viewport : sendViewport).getBounds().expanded (1));
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
    r.removeFromTop (12);
    if (tab == 1) { layoutSend (r); return; }
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
