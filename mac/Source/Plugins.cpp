#include "Plugins.h"
#include "Theme.h"

using namespace juce;

// A plugin's own editor (or a generic one with sliders if it has none) in its own window.
class PluginHost::EditorWindow : public DocumentWindow
{
public:
    EditorWindow (AudioProcessor& p, const String& title, std::function<void (EditorWindow*)> closed)
        : DocumentWindow (title, theme::panel, DocumentWindow::closeButton), processor (p), onClosed (std::move (closed))
    {
        setUsingNativeTitleBar (true);
        AudioProcessorEditor* ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (ed == nullptr) ed = new GenericAudioProcessorEditor (p);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        setTopLeftPosition (120 + Random::getSystemRandom().nextInt (200), 120 + Random::getSystemRandom().nextInt (120));
        setVisible (true);
    }
    ~EditorWindow() override { clearContentComponent(); }
    void closeButtonPressed() override { onClosed (this); }

    AudioProcessor& processor;

private:
    std::function<void (EditorWindow*)> onClosed;
};

static File appDataFolder()
{
    return File::getSpecialLocation (File::userApplicationDataDirectory)
        #if JUCE_MAC
            .getChildFile ("Application Support")
        #endif
            .getChildFile ("Soundbending Live");
}

PluginHost::PluginHost (AudioEngine& e, PropertiesFile& s) : engine (e), settings (s)
{
    formats.addDefaultFormats();
    if (auto xml = settings.getXmlValue ("pluginList"))
        known.recreateFromXml (*xml);
    known.addChangeListener (this);
}

PluginHost::~PluginHost()
{
    known.removeChangeListener (this);
    editors.clear();
    scanner = nullptr;
}

void PluginHost::changeListenerCallback (ChangeBroadcaster*)
{
    // The scanner found or removed plugins: remember the list.
    if (auto xml = known.createXml())
        settings.setValue ("pluginList", xml.get());
}

void PluginHost::showScanner()
{
    if (scanner != nullptr) { scanner->toFront (true); return; }
    appDataFolder().createDirectory();
    auto* list = new PluginListComponent (formats, known, appDataFolder().getChildFile ("plugin-scan-crash.txt"), &settings, true);
    list->setSize (640, 480);
    list->setScanDialogText ("Scanning for plugins", "Looking for VST3 and Audio Unit plugins...");

    struct ScanWindow : DocumentWindow
    {
        ScanWindow (std::unique_ptr<DocumentWindow>& o) : DocumentWindow ("Plugins", theme::panel, closeButton), owner (o) {}
        void closeButtonPressed() override { owner.reset(); }
        std::unique_ptr<DocumentWindow>& owner;
    };
    auto w = std::make_unique<ScanWindow> (scanner);
    w->setUsingNativeTitleBar (true);
    w->setContentOwned (list, true);
    w->setResizable (true, false);
    w->centreWithSize (640, 480);
    w->setVisible (true);
    scanner = std::move (w);
}

void PluginHost::showAddMenu (int track, Component& target)
{
    PopupMenu m;
    const auto types = known.getTypes();
    if (types.isEmpty())
        m.addItem (-2, "No plugins found yet", false);
    else
        KnownPluginList::addToMenu (m, types, KnownPluginList::sortByManufacturer);
    m.addSeparator();
    m.addItem (-1, "Scan for plugins...");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&target),
                     [this, track, types] (int result)
                     {
                         if (result == -1) { showScanner(); return; }
                         const int i = KnownPluginList::getIndexChosenByMenu (types, result);
                         if (isPositiveAndBelow (i, types.size()))
                             add (track, types.getReference (i), {}, false, true);
                     });
}

void PluginHost::add (int track, const PluginDescription& d, const MemoryBlock& state, bool bypass, bool saveAfter)
{
    if ((int) engine.getInserts (track).size() + pending >= AudioEngine::maxInserts) return;
    ++pending;
    const double rate = engine.sampleRate() > 0.0 ? engine.sampleRate() : 48000.0;
    formats.createPluginInstanceAsync (d, rate, 512,
        [this, track, d, state, bypass, saveAfter] (std::unique_ptr<AudioPluginInstance> p, const String& error)
        {
            --pending;
            auto next = loadNext; // continue a saved-session load whatever happens to this plugin
            if (p == nullptr)
            {
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Plugin didn't load",
                                                  d.name + ": " + (error.isNotEmpty() ? error : String ("unknown error")));
                if (next) next();
                return;
            }
            if (state.getSize() > 0)
                p->setStateInformation (state.getData(), (int) state.getSize());
            auto* raw = p.get();
            if (engine.addInsert (track, std::move (p), d, bypass))
            {
                changed (track);
                if (saveAfter) { save(); openEditor (track, (int) engine.getInserts (track).size() - 1); }
            }
            ignoreUnused (raw);
            if (next) next();
        });
}

void PluginHost::remove (int track, int index)
{
    auto list = engine.getInserts (track);
    if (! isPositiveAndBelow (index, (int) list.size())) return;
    closeEditorFor (list[(size_t) index]->plugin.get());
    engine.removeInsert (track, index);
    changed (track);
    save();
}

void PluginHost::toggleBypass (int track, int index)
{
    auto list = engine.getInserts (track);
    if (! isPositiveAndBelow (index, (int) list.size())) return;
    list[(size_t) index]->bypass = ! list[(size_t) index]->bypass.load();
    changed (track);
    save();
}

void PluginHost::showInsertMenu (int track, int index, Component& target)
{
    auto list = engine.getInserts (track);
    if (! isPositiveAndBelow (index, (int) list.size())) return;
    PopupMenu m;
    m.addItem (1, "Open");
    m.addItem (2, "Bypass", true, list[(size_t) index]->bypass.load());
    m.addItem (3, "Move up", index > 0);
    m.addItem (4, "Move down", index < (int) list.size() - 1);
    m.addSeparator();
    m.addItem (5, "Remove");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&target),
                     [this, track, index] (int r)
                     {
                         if (r == 1) openEditor (track, index);
                         if (r == 2) toggleBypass (track, index);
                         if (r == 3 || r == 4) { engine.moveInsert (track, index, r == 3 ? index - 1 : index + 1); changed (track); save(); }
                         if (r == 5) remove (track, index);
                     });
}

void PluginHost::openEditor (int track, int index)
{
    auto list = engine.getInserts (track);
    if (! isPositiveAndBelow (index, (int) list.size())) return;
    auto* p = list[(size_t) index]->plugin.get();
    for (auto* w : editors)
        if (&w->processor == p) { w->toFront (true); return; }
    editors.add (new EditorWindow (*p, list[(size_t) index]->description.name + " - Ch " + String (track + 1),
                                   [this] (EditorWindow* w) { editors.removeObject (w); save(); }));
}

void PluginHost::closeEditorFor (AudioProcessor* p)
{
    for (int i = editors.size(); --i >= 0;)
        if (&editors[i]->processor == p) editors.remove (i);
}

void PluginHost::changed (int track)
{
    if (onInsertsChanged) onInsertsChanged (track);
}

String PluginHost::saveInserts() const
{
    Array<var> tracks;
    for (int t = 0; t < AudioEngine::maxChannels; ++t)
    {
        auto list = engine.getInserts (t);
        if (list.empty()) continue;
        Array<var> items;
        for (auto& ins : list)
        {
            MemoryBlock state;
            ins->plugin->getStateInformation (state);
            auto* o = new DynamicObject();
            o->setProperty ("plugin", ins->description.createXml()->toString (XmlElement::TextFormat().singleLine()));
            o->setProperty ("state", state.toBase64Encoding());
            o->setProperty ("bypass", ins->bypass.load());
            items.add (var (o));
        }
        auto* o = new DynamicObject();
        o->setProperty ("track", t);
        o->setProperty ("inserts", items);
        tracks.add (var (o));
    }
    return JSON::toString (var (tracks), true);
}

void PluginHost::save()
{
    settings.setValue ("inserts", saveInserts());
}

void PluginHost::clearAll()
{
    loadNext = nullptr; // stop a load still in progress
    for (int t = 0; t < AudioEngine::maxChannels; ++t)
    {
        auto list = engine.getInserts (t);
        if (list.empty()) continue;
        for (int i = (int) list.size(); --i >= 0;)
        {
            closeEditorFor (list[(size_t) i]->plugin.get());
            engine.removeInsert (t, i);
        }
        changed (t);
    }
}

void PluginHost::loadInserts (const String& json)
{
    // Load one plugin at a time so each track's chain comes back in the saved order.
    struct Item { int track; PluginDescription d; MemoryBlock state; bool bypass; };
    auto queue = std::make_shared<std::vector<Item>>();
    auto v = JSON::parse (json);
    if (auto* tracks = v.getArray())
        for (auto& t : *tracks)
            if (auto* items = t["inserts"].getArray())
                for (auto& item : *items)
                {
                    Item it { (int) t["track"], {}, {}, (bool) item["bypass"] };
                    auto xml = parseXML (item["plugin"].toString());
                    if (xml == nullptr || ! it.d.loadFromXml (*xml)) continue;
                    it.state.fromBase64Encoding (item["state"].toString());
                    queue->push_back (std::move (it));
                }
    std::reverse (queue->begin(), queue->end());
    loadNext = [this, queue]
    {
        if (queue->empty()) { loadNext = nullptr; return; }
        auto it = std::move (queue->back());
        queue->pop_back();
        add (it.track, it.d, it.state, it.bypass, false);
    };
    loadNext();
}
