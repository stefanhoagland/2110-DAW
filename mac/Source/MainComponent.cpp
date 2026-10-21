#include "MainComponent.h"
#include "Names.h"
#include "Theme.h"
#include "BinaryData.h"

using namespace juce;

static const String sep = String (CharPointer_UTF8 ("  \xc2\xb7  "));
static const String sep1 = String (CharPointer_UTF8 (" \xc2\xb7 "));

static constexpr int defaultTracks = 32; // shown when no device is open

// Width of a web-style button: text plus 10 px padding each side.
static int widthOf (TextButton& b)
{
    return GlyphArrangement::getStringWidthInt (FontOptions (13.0f), b.getButtonText()) + 22;
}

MainComponent::MainComponent (PropertiesFile& s) : settings (s)
{
    LookAndFeel::setDefaultLookAndFeel (&look);
    logo = Drawable::createFromImageData (BinaryData::logo_svg, BinaryData::logo_svgSize);

    for (auto* l : { &deviceInfo, &folderInfo, &takeInfo, &zoomLabel })
    {
        l->setColour (Label::textColourId, theme::textMuted);
        l->setFont (FontOptions (13.0f));
        l->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (*l);
    }
    zoomLabel.setText ("Zoom", dontSendNotification);
    zoomLabel.setJustificationType (Justification::centred);
    notice.setColour (Label::textColourId, Colour (0xffffd98a));
    notice.setFont (FontOptions (13.0f));
    addChildComponent (notice);

    clock.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 18.0f, Font::plain));
    clock.setJustificationType (Justification::centred);
    addAndMakeVisible (clock);
    tcInfo.setFont (FontOptions (11.0f));
    tcInfo.setJustificationType (Justification::centred);
    tcInfo.setColour (Label::textColourId, theme::textMuted);
    addChildComponent (tcInfo);

    // Mode tabs. Only the recorder (Studio) exists in the Mac app so far.
    for (auto* t : { &liveTab, &studioTab, &soundcheckTab, &patchTab })
    {
        WebLook::setStyle (*t, "mode");
        addAndMakeVisible (*t);
    }
    studioTab.setToggleState (true, dontSendNotification);
    soundcheckTab.setEnabled (false);
    soundcheckTab.setTooltip ("Coming in a later step");
    liveTab.onClick = [this] { setMode (liveMode); };
    studioTab.onClick = [this] { setMode (studioMode); };
    patchTab.onClick = [this] { setMode (patchMode); };
    patchTab.setTooltip ("Patch interface inputs to tracks, and sources to interface outputs");

    addChildComponent (patchView);
    patchView.onChange = [this] { savePatch(); console.refresh(); console.setNames (trackNames()); };
    PatchView::loadInputs (engine, settings.getValue ("patchIn"));
    PatchView::loadOutputs (engine, settings.getValue ("patchOut"));

    addChildComponent (console);
    console.onChange = [this] { saveLive(); };
    console.onGoLive = [this] (bool on)
    {
        setStatus (on ? "Live mix is going to the output" : "Live mix stopped");
    };
    addAndMakeVisible (master);
    master.onChange = [this] { saveLive(); };

    for (auto* b : { &rtzButton, &stopButton, &playButton, &recordButton })
    {
        WebLook::setStyle (*b, "transport");
        addAndMakeVisible (*b);
    }
    rtzButton.setTooltip ("Return to zero");
    playButton.setTooltip ("Playback is coming to the Mac app in a later step");
    rtzButton.setEnabled (false);
    playButton.setEnabled (false);
    stopButton.setTooltip ("Stop");
    recordButton.setTooltip ("Record the armed tracks (R)");
    recordButton.getProperties().set ("textColour", (int64) theme::record.getARGB());
    recordButton.setColour (TextButton::buttonOnColourId, theme::record);
    recordButton.setColour (TextButton::textColourOnId, Colours::white);

    WebLook::setStyle (audioButton, "primary");
    timecodeButton.setTooltip ("External timecode: LTC on an input, or MIDI timecode. Takes are stamped with it.");
    timecodeButton.onClick = [this] { showTimecodeMenu(); };
    addAndMakeVisible (timecodeButton);
    networkButton.setTooltip ("Receive AES67 / SMPTE ST 2110-30 audio streams from the network as extra inputs");
    networkButton.onClick = [this] { showNetwork(); };
    addAndMakeVisible (networkButton);
    for (auto* b : { &audioButton, &folderButton, &armAll, &disarmAll, &importButton, &exportButton, &zoomOut, &zoomIn })
        addAndMakeVisible (*b);
    importButton.setTooltip ("Load channel names from a CSV file (Channel, Name)");
    exportButton.setTooltip ("Save channel names as a CSV file");

    audioButton.onClick = [this] { showAudioSettings(); };
    folderButton.onClick = [this] { chooseFolder(); };
    armAll.onClick = [this] { for (auto* r : rows) r->setArmed (true); saveTracks(); };
    disarmAll.onClick = [this] { for (auto* r : rows) r->setArmed (false); saveTracks(); };
    importButton.onClick = [this] { importNames(); };
    exportButton.onClick = [this] { exportNames(); };
    recordButton.onClick = [this] { toggleRecord(); };
    stopButton.onClick = [this] { if (engine.isRecording()) stopRecording(); };
    zoomOut.onClick = [this] { trackArea.zoom (1.0 / 1.5); };
    zoomIn.onClick = [this] { trackArea.zoom (1.5); };

    viewport.setViewedComponent (&trackArea, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    // Default save folder; keeps using the folder from before the rename if it exists.
    auto music = File::getSpecialLocation (File::userMusicDirectory);
    auto defaultFolder = music.getChildFile ("2110 DAW").isDirectory() ? music.getChildFile ("2110 DAW")
                                                                       : music.getChildFile ("Soundbending Live");
    folder = File (settings.getValue ("folder", defaultFolder.getFullPathName()));
    take = nextTakeNumber() - 1;
    takeInfo.setText ("No takes yet", dontSendNotification);

    auto deviceXml = settings.getXmlValue ("audioDevice");
    engine.initialise (deviceXml.get());
    engine.network().loadState (settings.getValue ("aes67"));
    engine.network().onChange = [this] { networkChanged(); };
    engine.devices().addChangeListener (this);
    rebuildTracks();
    applyTimecode (settings.getValue ("timecode", "off"), false);
    console.loadState (settings.getValue ("live"));
    plugins.onInsertsChanged = [this] (int t) { console.refreshInserts (t); };
    plugins.loadInserts (settings.getValue ("inserts"));
    master.refreshFromEngine();
    console.refresh();
    refreshLabels();
    const auto savedMode = settings.getValue ("mode");
    setMode (savedMode == "live" ? liveMode : savedMode == "patch" ? patchMode : studioMode);

    setWantsKeyboardFocus (true);
    setSize (1360, 820);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    engine.network().onChange = nullptr;
    if (networkWindow != nullptr) delete networkWindow.getComponent();
    engine.devices().removeChangeListener (this);
    engine.setLive (false);
    engine.stopRecording();
    saveTracks();
    saveLive();
    plugins.save();
    LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (theme::background);

    // Top bar, setup bar (panel colour) and toolbar, each with a line underneath.
    g.setColour (theme::panel);
    g.fillRect (topBar);
    g.fillRect (setupBar);
    g.setColour (theme::line);
    for (auto r : { topBar, setupBar, mode != studioMode ? Rectangle<int>() : toolBar })
        g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
    if (notice.isVisible())
    {
        g.setColour (Colour (0xff3a2d10));
        g.fillRect (noticeBar);
    }

    // "Soundbending Live" with "Live" in the accent colour, like the web brand.
    g.setFont (FontOptions (18.0f, Font::bold));
    const String first = "Soundbending ", second = "Live";
    const int w1 = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), first);
    auto brand = topBar.withTrimmedLeft (16).withTrimmedBottom (1);
    if (logo != nullptr)
    {
        // The icon's artwork sits inside a 100 px margin of its 1024 px canvas; trim that off.
        auto box = brand.removeFromLeft (34).withSizeKeepingCentre (34, 34).toFloat().expanded (34.0f * 100.0f / 824.0f);
        logo->drawWithin (g, box, RectanglePlacement::centred, 1.0f);
        brand.removeFromLeft (10);
    }
    g.setColour (theme::text);
    g.drawText (first, brand, Justification::centredLeft);
    g.setColour (theme::accent);
    g.drawText (second, brand.withTrimmedLeft (w1), Justification::centredLeft);

    g.setColour (theme::background);
    g.fillRoundedRectangle (modesPill.toFloat(), 8.0f);
    g.setColour (theme::clockBack);
    g.fillRoundedRectangle (clockBox.toFloat(), 6.0f);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    // Top bar: brand, modes, then the transport on the right.
    topBar = r.removeFromTop (98);
    {
        auto b = topBar.reduced (16, 10).withTrimmedBottom (1);
        const int brandW = GlyphArrangement::getStringWidthInt (FontOptions (18.0f, Font::bold), "Soundbending Live");
        b.removeFromLeft (brandW + 24 + (logo != nullptr ? 44 : 0));
        int pillW = 6 + 4 * 2;
        for (auto* t : { &liveTab, &studioTab, &soundcheckTab, &patchTab }) pillW += widthOf (*t) + 8;
        modesPill = b.removeFromLeft (pillW).withSizeKeepingCentre (pillW, 34);
        auto p = modesPill.reduced (3);
        for (auto* t : { &liveTab, &studioTab, &soundcheckTab, &patchTab })
        {
            t->setBounds (p.removeFromLeft (widthOf (*t) + 8));
            p.removeFromLeft (4);
        }

        // The master section needs room; on a narrow window it hides rather than overlap the transport.
        const bool roomForMaster = b.getWidth() >= 330 + 24 + 150 + 4 * 46 + 12;
        master.setVisible (roomForMaster);
        if (roomForMaster)
        {
            master.setBounds (b.removeFromRight (330).withSizeKeepingCentre (330, 78));
            b.removeFromRight (24);
        }
        clockBox = b.removeFromRight (150).withSizeKeepingCentre (150, 34);
        clock.setBounds (clockBox);
        tcInfo.setBounds (clockBox.getX() - 20, clockBox.getBottom() + 2, clockBox.getWidth() + 40, 14);
        b.removeFromRight (6);
        for (auto* t : { &recordButton, &playButton, &stopButton, &rtzButton })
        {
            t->setBounds (b.removeFromRight (40).withSizeKeepingCentre (40, 34));
            b.removeFromRight (6);
        }
    }

    // Setup bar: audio device on the left, save folder on the right.
    setupBar = r.removeFromTop (46);
    {
        auto b = setupBar.reduced (16, 8).withTrimmedBottom (1);
        audioButton.setBounds (b.removeFromLeft (widthOf (audioButton)));
        b.removeFromLeft (10);
        timecodeButton.setBounds (b.removeFromLeft (jmin (widthOf (timecodeButton), 240)));
        b.removeFromLeft (10);
        networkButton.setBounds (b.removeFromLeft (widthOf (networkButton)));
        b.removeFromLeft (10);
        const int folderW = jmin (GlyphArrangement::getStringWidthInt (FontOptions (13.0f), folderInfo.getText()) + 12,
                                  b.getWidth() / 2);
        folderInfo.setBounds (b.removeFromRight (folderW));
        b.removeFromRight (10);
        folderButton.setBounds (b.removeFromRight (widthOf (folderButton)));
        b.removeFromRight (10);
        deviceInfo.setBounds (b);
    }

    noticeBar = notice.isVisible() ? r.removeFromTop (34) : Rectangle<int>();
    notice.setBounds (noticeBar.reduced (12, 0));

    if (mode == liveMode) { console.setBounds (r); return; }
    if (mode == patchMode) { patchView.setBounds (r); return; }

    // Toolbar: names and arming, take info, zoom.
    toolBar = r.removeFromTop (46);
    {
        auto b = toolBar.reduced (16, 8).withTrimmedBottom (1);
        for (auto* t : { &armAll, &disarmAll, &importButton, &exportButton })
        {
            t->setBounds (b.removeFromLeft (widthOf (*t)));
            b.removeFromLeft (10);
        }
        zoomIn.setBounds (b.removeFromRight (30));
        zoomLabel.setBounds (b.removeFromRight (50));
        zoomOut.setBounds (b.removeFromRight (30));
        b.removeFromRight (10);
        takeInfo.setBounds (b);
    }

    viewport.setBounds (r);
    trackArea.layout (viewport.getMaximumVisibleWidth());
}

void MainComponent::rebuildTracks()
{
    const int n = engine.numInputs() > 0 ? engine.numInputs() : defaultTracks;
    if (n == rows.size())
        return;

    StringArray savedNames;
    savedNames.addLines (settings.getValue ("names"));
    const auto armed = settings.getValue ("armed");

    rows.clear();
    for (int i = 0; i < n; ++i)
    {
        auto* row = rows.add (new TrackRow (i));
        // Network tracks start out named after their stream.
        row->setName (savedNames[i].isNotEmpty() ? savedNames[i]
                      : i >= engine.numDeviceInputs() ? engine.inputName (i) : names::defaultName (i));
        row->setArmed (armed.isEmpty() ? true : armed[i] == '1');
        row->onChange = [this] { saveTracks(); };
        row->onMoveFocus = [this, i] (int d)
        {
            const int to = i + d;
            if (isPositiveAndBelow (to, rows.size()))
            {
                rows[to]->focusName();
                viewport.setViewPosition (0, jmax (0, rows[to]->getY() - viewport.getHeight() / 2));
            }
            else grabKeyboardFocus();
        };
        trackArea.addAndMakeVisible (row);
    }
    console.rebuild (n, trackNames());
    resized();
}

StringArray MainComponent::trackNames() const
{
    StringArray list;
    for (auto* r : rows) list.add (r->getName());
    return list;
}

void MainComponent::setMode (Mode m)
{
    mode = m;
    liveTab.setToggleState (m == liveMode, dontSendNotification);
    studioTab.setToggleState (m == studioMode, dontSendNotification);
    patchTab.setToggleState (m == patchMode, dontSendNotification);
    for (Component* c : { (Component*) &armAll, (Component*) &disarmAll, (Component*) &importButton,
                          (Component*) &exportButton, (Component*) &takeInfo, (Component*) &zoomIn,
                          (Component*) &zoomOut, (Component*) &zoomLabel, (Component*) &viewport })
        c->setVisible (m == studioMode);
    console.setVisible (m == liveMode);
    patchView.setVisible (m == patchMode);
    if (m == liveMode) { console.setNames (trackNames()); console.refresh(); }
    if (m == patchMode) patchView.refresh();
    settings.setValue ("mode", m == liveMode ? "live" : m == patchMode ? "patch" : "studio");
    resized();
    repaint();
}

void MainComponent::savePatch()
{
    settings.setValue ("patchIn", PatchView::saveInputs (engine));
    settings.setValue ("patchOut", PatchView::saveOutputs (engine));
}

void MainComponent::saveLive()
{
    settings.setValue ("live", console.saveState());
}

void MainComponent::saveTracks()
{
    // Keep names of channels beyond this device's count, so switching devices loses nothing.
    StringArray all;
    all.addLines (settings.getValue ("names"));
    String armed = settings.getValue ("armed");
    while (all.size() < rows.size()) all.add ({});
    while (armed.length() < rows.size()) armed << '1';
    for (auto* r : rows)
    {
        all.set (r->channel(), r->getName());
        armed = armed.replaceSection (r->channel(), 1, r->isArmed() ? "1" : "0");
    }
    settings.setValue ("names", all.joinIntoString ("\n"));
    settings.setValue ("armed", armed);
}

void MainComponent::saveDevice()
{
    if (auto xml = engine.devices().createStateXml())
        settings.setValue ("audioDevice", xml.get());
}

void MainComponent::changeListenerCallback (ChangeBroadcaster*)
{
    // Device changed in Audio settings.
    saveDevice();
    rebuildTracks();
    if (tcSetting.startsWith ("ltc")) applyTimecode (tcSetting, false); // new device: new input names
    console.setNames (trackNames());
    console.refresh();
    patchView.refresh();
    refreshLabels();
}

void MainComponent::showAudioSettings()
{
    if (engine.isRecording()) { setStatus ("Stop recording before changing the audio device.", true); return; }
    auto* selector = new AudioDeviceSelectorComponent (engine.devices(), 0, AudioEngine::maxChannels, 0, 2,
                                                       false, false, false, false);
    selector->setSize (520, 460);
    DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio settings";
    o.dialogBackgroundColour = theme::panel;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::chooseFolder()
{
    chooser = std::make_unique<FileChooser> ("Choose where takes are saved", folder);
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                          [this] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == File()) return;
                              folder = f;
                              settings.setValue ("folder", folder.getFullPathName());
                              take = nextTakeNumber() - 1;
                              refreshLabels();
                          });
}

void MainComponent::importNames()
{
    chooser = std::make_unique<FileChooser> ("Import channel names", File(), "*.csv;*.txt");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (! f.existsAsFile()) return;
                              StringArray current;
                              for (auto* r : rows) current.add (r->getName());
                              auto updated = names::fromCsv (f.loadFileAsString(), current);
                              for (auto* r : rows) r->setName (updated[r->channel()]);
                              saveTracks();
                              setStatus ("Imported names from " + f.getFileName());
                          });
}

void MainComponent::exportNames()
{
    chooser = std::make_unique<FileChooser> ("Export channel names",
                                             File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("channel-names.csv"),
                                             "*.csv");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == File()) return;
                              StringArray list;
                              for (auto* r : rows) list.add (r->getName());
                              if (f.replaceWithText (names::toCsv (list)))
                                  setStatus ("Saved names to " + f.getFileName());
                              else
                                  setStatus ("Couldn't save " + f.getFullPathName(), true);
                          });
}

int MainComponent::nextTakeNumber() const
{
    // Continue after the highest take already in the folder (files named T##_...).
    int highest = 0;
    for (auto& f : folder.findChildFiles (File::findFiles, false, "T*_*.wav"))
    {
        auto n = f.getFileName().substring (1).upToFirstOccurrenceOf ("_", false, false);
        if (n.containsOnly ("0123456789")) highest = jmax (highest, n.getIntValue());
    }
    return highest + 1;
}

void MainComponent::toggleRecord()
{
    if (engine.isRecording()) stopRecording();
    else startRecording();
}

void MainComponent::startRecording()
{
    std::vector<AudioEngine::Track> armed;
    for (auto* r : rows)
        if (r->isArmed() && r->channel() < engine.numInputs())
            armed.push_back ({ r->channel(), r->getName() });

    const int next = nextTakeNumber();
    auto err = engine.startRecording (folder, next, armed);
    if (err.isNotEmpty()) { setStatus (err, true); return; }

    take = next;
    for (auto* r : rows) r->setLocked (true);
    for (auto* r : rows) if (r->isArmed()) r->startWave(); else r->clearWave();
    audioButton.setEnabled (false);
    folderButton.setEnabled (false);
    recordButton.setToggleState (true, dontSendNotification);
    setStatus ("Recording take " + String (take) + sep1 + String ((int) armed.size()) + " tracks");
}

void MainComponent::stopRecording()
{
    const auto seconds = engine.recordedSeconds();
    const auto drops = engine.droppedBlocks();
    const auto files = engine.lastTakeFiles();
    engine.stopRecording();
    for (auto* r : rows) r->setLocked (false);
    audioButton.setEnabled (true);
    folderButton.setEnabled (true);
    recordButton.setToggleState (false, dontSendNotification);
    recordButton.setAlpha (1.0f);
    lastTakeSeconds = seconds;
    if (drops > 0)
        setStatus ("Take " + String (take) + " saved, but the disk fell behind " + String (drops)
                   + " times and some audio is missing. Use a faster drive or a larger buffer.", true);
    else
    {
        const auto start = engine.takeTimecode();
        setStatus ("Take " + String (take) + " saved: " + String (files.size()) + " files, "
                   + String (seconds, 1) + " s"
                   + (start.seen ? sep1 + "starts at " + tc::toLabel (start.frames, start.rate) : String()));
    }
}

void MainComponent::setStatus (const String& text, bool error)
{
    // Errors go in the yellow notice bar (like the web version); everything else next to the toolbar.
    if (error)
    {
        notice.setText (text, dontSendNotification);
        notice.setVisible (true);
    }
    else
    {
        takeInfo.setText (text, dontSendNotification);
        notice.setVisible (false);
    }
    resized();
    repaint();
}

void MainComponent::refreshLabels()
{
    const int netCh = engine.numInputs() - engine.numDeviceInputs();
    deviceInfo.setText (engine.deviceDescription() + (netCh > 0 ? sep + "+ " + String (netCh) + " AES67" : String()),
                        dontSendNotification);
    const auto free = folder.getBytesFreeOnVolume();
    const double rate = jmax (44100.0, engine.sampleRate());
    const double hours = free > 0 ? (double) free / (rate * 3.0 * jmax (1, engine.numInputs()) * 3600.0) : 0.0;
    folderInfo.setText (folder.getFullPathName() + (free > 0 ? sep + File::descriptionOfSizeInBytes (free)
                        + " free (about " + String (hours, 1) + " h at all inputs)" : String()),
                        dontSendNotification);
    resized();
}

void MainComponent::timerCallback()
{
    const bool rec = engine.isRecording();
    // Network streams added during a take become tracks once it ends.
    if (! rec && engine.numInputs() > 0 && engine.numInputs() != rows.size()) networkChanged();
    const double secs = rec ? engine.recordedSeconds() : lastTakeSeconds;
    for (auto* r : rows)
    {
        const float peak = engine.takePeak (r->channel());
        r->setLevel (peak);
        if (rec && r->hasTake()) r->addWave (secs, peak);
    }
    trackArea.setTake (take, secs, rec);
    master.tick();
    if (mode == liveMode) console.tick();
    // Plugins change their own settings in their windows; save them now and then.
    if (++ticks % 300 == 0) plugins.save();

    updateClock (rec, secs);

    // Blink the record button while recording, like the web version.
    recordButton.setAlpha (rec && (Time::getMillisecondCounter() / 500) % 2 ? 0.6f : 1.0f);
}

bool MainComponent::keyPressed (const KeyPress& key)
{
    if (key == KeyPress ('r') || key == KeyPress ('R')) { toggleRecord(); return true; }
    if (key == KeyPress::escapeKey) { grabKeyboardFocus(); return true; }
    return false;
}

bool MainComponent::canQuit()
{
    if (! engine.isRecording())
        return true;
    stopRecording(); // finish the files cleanly rather than lose the take
    return true;
}

//==============================================================================
static String elapsedText (double secs)
{
    const int ms = (int) std::fmod (secs * 1000.0, 1000.0);
    const int h = (int) secs / 3600, m = ((int) secs / 60) % 60, s = (int) secs % 60;
    return String (h).paddedLeft ('0', 2) + ":" + String (m).paddedLeft ('0', 2) + ":"
         + String (s).paddedLeft ('0', 2) + "." + String (ms).paddedLeft ('0', 3);
}

void MainComponent::updateClock (bool rec, double secs)
{
    const auto now = engine.timecodeNow();
    tcInfo.setVisible (now.active);
    if (! now.active)
    {
        clock.setText (elapsedText (secs), dontSendNotification);
        clock.setColour (Label::textColourId, rec ? theme::record : theme::text);
        return;
    }
    // With a timecode source, the clock shows timecode; the take time moves underneath.
    clock.setText (now.seen ? tc::toLabel (now.frames, now.rate) : String ("--:--:--:--"), dontSendNotification);
    clock.setColour (Label::textColourId, ! now.locked ? theme::textMuted : rec ? theme::record : theme::text);
    const String kind = tcSetting.startsWith ("ltc") ? "LTC" : "MTC";
    String info = kind + sep1 + (now.locked ? now.rate.name() : String ("no signal"));
    if (rec) info << sep1 << "REC " << elapsedText (secs).dropLastCharacters (4);
    tcInfo.setText (info, dontSendNotification);
    tcInfo.setColour (Label::textColourId, now.locked ? theme::textMuted : Colour (0xffffd98a));
}

void MainComponent::applyTimecode (const String& setting, bool save)
{
    String label = "Off";
    if (setting.startsWith ("ltc:"))
    {
        const int input = setting.fromFirstOccurrenceOf (":", false, false).getIntValue();
        engine.setTimecodeLtc (input);
        String name = "In " + String (input + 1);
        if (auto* dev = engine.devices().getCurrentAudioDevice())
            if (auto n = dev->getInputChannelNames()[input]; n.isNotEmpty()) name = n;
        label = "LTC" + sep1 + name;
        tcSetting = setting;
    }
    else if (setting.startsWith ("mtc:"))
    {
        const auto id = setting.fromFirstOccurrenceOf (":", false, false);
        String name;
        for (auto& d : MidiInput::getAvailableDevices())
            if (d.identifier == id) name = d.name;
        if (name.isNotEmpty() && engine.setTimecodeMtc (id))
        {
            label = "MTC" + sep1 + name;
            tcSetting = setting;
        }
        else
        {
            // Keep the choice so it works again when the MIDI device is plugged back in.
            engine.setTimecodeOff();
            tcSetting = "off";
            setStatus ("The MIDI timecode input isn't connected. Pick it again under Timecode when it is.", true);
            save = false;
        }
    }
    else
    {
        engine.setTimecodeOff();
        tcSetting = "off";
    }
    timecodeButton.setButtonText ("Timecode: " + label);
    timecodeButton.setToggleState (tcSetting != "off", dontSendNotification);
    if (save) settings.setValue ("timecode", tcSetting);
    resized();
}

void MainComponent::showTimecodeMenu()
{
    if (engine.isRecording()) { setStatus ("Stop recording before changing the timecode source.", true); return; }
    PopupMenu menu, ltc, mtc;
    StringArray inputs;
    if (auto* dev = engine.devices().getCurrentAudioDevice())
        inputs = dev->getInputChannelNames();
    for (int i = 0; i < inputs.size(); ++i)
        ltc.addItem (100 + i, String (i + 1) + "  " + inputs[i], true, tcSetting == "ltc:" + String (i));
    const auto midi = MidiInput::getAvailableDevices();
    for (int i = 0; i < midi.size(); ++i)
        mtc.addItem (10000 + i, midi[i].name, true, tcSetting == "mtc:" + midi[i].identifier);
    if (midi.isEmpty()) mtc.addItem (-1, "No MIDI inputs found", false, false);

    menu.addSectionHeader ("Timecode source");
    menu.addItem (1, "Off (show take time)", true, tcSetting == "off");
    menu.addSubMenu ("LTC on an audio input", ltc, ! inputs.isEmpty(), nullptr, tcSetting.startsWith ("ltc"));
    menu.addSubMenu ("MIDI timecode (MTC)", mtc, true, nullptr, tcSetting.startsWith ("mtc"));
    menu.addSeparator();
    menu.addItem (-1, "Frame rate is detected automatically. Takes are stamped", false, false);
    menu.addItem (-1, "with their start timecode (Broadcast WAV).", false, false);
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (timecodeButton),
                        [this, midi] (int r)
                        {
                            if (r == 1) applyTimecode ("off", true);
                            else if (r >= 10000 && r - 10000 < midi.size()) applyTimecode ("mtc:" + midi[r - 10000].identifier, true);
                            else if (r >= 100 && r < 10000) applyTimecode ("ltc:" + String (r - 100), true);
                        });
}

//==============================================================================
void MainComponent::showNetwork()
{
    if (networkWindow != nullptr) { networkWindow->toFront (true); return; }
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new NetworkView (engine));
    o.dialogTitle = "AES67 network audio";
    o.dialogBackgroundColour = theme::panel;
    o.useNativeTitleBar = true;
    o.resizable = true;
    networkWindow = o.launchAsync();
}

void MainComponent::networkChanged()
{
    settings.setValue ("aes67", engine.network().saveState());
    if (networkWindow != nullptr)
        if (auto* v = dynamic_cast<NetworkView*> (networkWindow->getContentComponent())) v->refresh();
    if (engine.isRecording()) return; // tracks change after the take
    saveTracks();
    rebuildTracks();
    console.setNames (trackNames());
    console.refresh();
    patchView.refresh();
    refreshLabels();
}
