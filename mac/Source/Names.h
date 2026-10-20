#pragma once
#include <juce_core/juce_core.h>

// Channel names: CSV import/export and WAV file names. Same rules as the web
// version (js/csv.js and js/storage.js) so name lists work in both.
namespace names
{
    // Reads "Channel,Name" rows (either column order, optional header), or a
    // plain list of names one per line. Separators: comma, semicolon or tab.
    // Returns `current` with the names from the file filled in.
    juce::StringArray fromCsv (const juce::String& text, const juce::StringArray& current);

    juce::String toCsv (const juce::StringArray& names);

    // Letters, digits, '-', '_' and spaces only; spaces become '_'.
    juce::String safeName (const juce::String& name);

    // e.g. take 3, channel index 0, "Kick In" -> "T03_01_Kick_In.wav"
    juce::String takeFileName (int take, int channelIndex, const juce::String& name);

    juce::String defaultName (int channelIndex);
}
