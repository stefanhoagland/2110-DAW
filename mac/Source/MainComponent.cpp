#include "MainComponent.h"
#include "Names.h"
#include "Theme.h"

using namespace juce;

static const String sep = String (CharPointer_UTF8 ("  \xc2\xb7  "));
static const String sep1 = String (CharPointer_UTF8 (" \xc2\xb7 "));

static constexpr int rowHeight = 34;
static constexpr int defaultTracks = 32; // shown when no device is open

MainComponent::MainComponent (PropertiesFile& s) : settings (s)
{
    title.setText ("2110 DAW", dontSendNotification);
    title.setFont (FontOptions (20.0f, Font::bold));
    addAndMakeVisible (title);

    for (auto* l : { &deviceInfo, &folderInfo, &status })
    {
        l->setColour (Label::textColourId, theme::textMuted);
        addAndMakeVisible (*l);
    }
    clock.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 22.0f, Font::plain));
    clock.setJustificationType (Justification::centred);
    clock.setColour (Label::backgroundColourId, theme::meterBack);
    addAndMakeVisible (clock);

    for (auto* b : { &audioButton, &folderButton, &armAll, &disarmAll, &importButton, &exportButton, &recordButton })
        addAndMakeVisible (*b);

    recordButton.setColour (TextButton::buttonColourId, theme::record.darker (0.3f));
    recordButton.setTooltip ("Record the armed tracks (R)");
    audioButton.onClick = [this] { showAudioSettings(); };
    folderButton.onClick = [this] { chooseFolder(); };
    armAll.onClick = [this] { for (auto* r : rows) r->setArmed (true); saveTracks(); };
    disarmAll.onClick = [this] { for (auto* r : rows) r->setArmed (false); saveTracks(); };
    importButton.onClick = [this] { importNames(); };
    exportButton.onClick = [this] { exportNames(); };
    recordButton.onClick = [this] { toggleRecord(); };

    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    folder = File (settings.getValue ("folder",
        File::getSpecialLocation (File::userMusicDirectory).getChildFile ("2110 DAW").getFullPathName()));
    take = nextTakeNumber() - 1;

    auto deviceXml = settings.getXmlValue ("audioDevice");
    engine.initialise (deviceXml.get());
    engine.devices().addChangeListener (this);
    rebuildTracks();
    refreshLabels();

    setWantsKeyboardFocus (true);
    setSize (980, 720);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    engine.devices().removeChangeListener (this);
    engine.stopRecording();
    saveTracks();
}

void MainComponent::paint (Graphics& g)
{
    g.fillAll (theme::background);
    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromTop (112));
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (112).reduced (14, 10);

    auto row1 = top.removeFromTop (36);
    title.setBounds (row1.removeFromLeft (130));
    recordButton.setBounds (row1.removeFromRight (110));
    row1.removeFromRight (10);
    clock.setBounds (row1.removeFromRight (150));
    row1.removeFromRight (10);
    audioButton.setBounds (row1.removeFromLeft (130).reduced (0, 3));
    row1.removeFromLeft (8);
    deviceInfo.setBounds (row1);

    top.removeFromTop (6);
    auto row2 = top.removeFromTop (30);
    folderButton.setBounds (row2.removeFromLeft (110));
    row2.removeFromLeft (8);
    for (auto* b : { &exportButton, &importButton, &disarmAll, &armAll })
    {
        b->setBounds (row2.removeFromRight (b == &armAll || b == &disarmAll ? 90 : 110));
        row2.removeFromRight (6);
    }
    folderInfo.setBounds (row2);

    status.setBounds (top.removeFromTop (24));

    viewport.setBounds (r);
    rowHolder.setSize (viewport.getMaximumVisibleWidth(), rows.size() * rowHeight);
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * rowHeight, rowHolder.getWidth(), rowHeight);
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
        row->setName (savedNames[i].isNotEmpty() ? savedNames[i] : names::defaultName (i));
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
        rowHolder.addAndMakeVisible (row);
    }
    resized();
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
    audioButton.setEnabled (false);
    folderButton.setEnabled (false);
    recordButton.setButtonText ("Stop");
    recordButton.setColour (TextButton::buttonColourId, theme::record);
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
    recordButton.setButtonText ("Record");
    recordButton.setColour (TextButton::buttonColourId, theme::record.darker (0.3f));
    if (drops > 0)
        setStatus ("Take " + String (take) + " saved, but the disk fell behind " + String (drops)
                   + " times and some audio is missing. Use a faster drive or a larger buffer.", true);
    else
        setStatus ("Take " + String (take) + " saved: " + String (files.size()) + " files, "
                   + String (seconds, 1) + " s");
}

void MainComponent::setStatus (const String& text, bool error)
{
    status.setText (text, dontSendNotification);
    status.setColour (Label::textColourId, error ? theme::record : theme::textMuted);
}

void MainComponent::refreshLabels()
{
    deviceInfo.setText (engine.deviceDescription(), dontSendNotification);
    const auto free = folder.getBytesFreeOnVolume();
    const double rate = jmax (44100.0, engine.sampleRate());
    const double hours = free > 0 ? (double) free / (rate * 3.0 * jmax (1, engine.numInputs()) * 3600.0) : 0.0;
    folderInfo.setText (folder.getFullPathName() + (free > 0 ? sep + File::descriptionOfSizeInBytes (free)
                        + " free (about " + String (hours, 1) + " h at all inputs)" : String()),
                        dontSendNotification);
}

void MainComponent::timerCallback()
{
    for (auto* r : rows)
        r->setLevel (engine.takePeak (r->channel()));

    const double secs = engine.isRecording() ? engine.recordedSeconds() : 0.0;
    const int h = (int) secs / 3600, m = ((int) secs / 60) % 60;
    const double s = std::fmod (secs, 60.0);
    clock.setText (String (h).paddedLeft ('0', 2) + ":" + String (m).paddedLeft ('0', 2) + ":"
                   + String (s, 1).paddedLeft ('0', 4), dontSendNotification);
    clock.setColour (Label::textColourId, engine.isRecording() ? theme::record : theme::text);
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
