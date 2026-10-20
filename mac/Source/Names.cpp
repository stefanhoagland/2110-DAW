#include "Names.h"

namespace names
{
using namespace juce;

static StringArray splitRow (const String& line, juce_wchar sep)
{
    StringArray cells;
    String cell;
    bool quoted = false;
    for (int i = 0; i < line.length(); ++i)
    {
        auto c = line[i];
        if (quoted)
        {
            if (c == '"' && line[i + 1] == '"') { cell << '"'; ++i; }
            else if (c == '"') quoted = false;
            else cell << c;
        }
        else if (c == '"') quoted = true;
        else if (c == sep) { cells.add (cell.trim()); cell.clear(); }
        else cell << c;
    }
    cells.add (cell.trim());
    return cells;
}

static bool isNumber (const String& s)
{
    return s.isNotEmpty() && s.containsOnly ("0123456789");
}

StringArray fromCsv (const String& textIn, const StringArray& current)
{
    auto text = textIn.startsWithChar ((juce_wchar) 0xfeff) ? textIn.substring (1) : textIn;
    StringArray lines;
    lines.addLines (text);
    lines.removeEmptyStrings (true);
    if (lines.isEmpty())
        return current;

    juce_wchar sep = ',';
    if (lines[0].containsChar ('\t')) sep = '\t';
    else if (lines[0].containsChar (';') && ! lines[0].containsChar (',')) sep = ';';

    StringArray result (current);
    std::vector<StringArray> rows;
    for (auto& l : lines)
        rows.push_back (splitRow (l, sep));

    // Find which column holds channel numbers (if any) from the data rows.
    int numCol = -1;
    for (int col = 0; col < 2 && numCol < 0; ++col)
    {
        int numeric = 0, total = 0;
        for (auto& r : rows)
        {
            if (r.size() < 2) continue;
            ++total;
            if (isNumber (r[col])) ++numeric;
        }
        if (total > 0 && numeric >= jmax (1, total - 1)) // allow one header row
            numCol = col;
    }

    if (numCol >= 0)
    {
        const int nameCol = 1 - numCol;
        for (auto& r : rows)
        {
            if (r.size() < 2 || ! isNumber (r[numCol])) continue; // header
            const int idx = r[numCol].getIntValue() - 1;
            if (idx >= 0 && idx < result.size() && r[nameCol].isNotEmpty())
                result.set (idx, r[nameCol]);
        }
        return result;
    }

    // Plain list: one name per line (first cell), skipping a "Name" header.
    int idx = 0;
    for (size_t i = 0; i < rows.size() && idx < result.size(); ++i)
    {
        auto name = rows[i][0];
        if (i == 0 && name.equalsIgnoreCase ("name")) continue;
        if (name.isNotEmpty()) result.set (idx, name);
        ++idx;
    }
    return result;
}

String toCsv (const StringArray& list)
{
    String out = "Channel,Name\n";
    for (int i = 0; i < list.size(); ++i)
    {
        auto n = list[i];
        if (n.containsAnyOf (",\"\n"))
            n = "\"" + n.replace ("\"", "\"\"") + "\"";
        out << (i + 1) << ',' << n << '\n';
    }
    return out;
}

String safeName (const String& name)
{
    String out;
    for (auto c : name)
        if (CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '-' || c == ' ')
            out << c;
    out = out.trim();
    while (out.contains ("  ")) out = out.replace ("  ", " ");
    out = out.replace (" ", "_");
    return out.isEmpty() ? String ("Track") : out;
}

String takeFileName (int take, int channelIndex, const String& name)
{
    return "T" + String (take).paddedLeft ('0', 2) + "_" + String (channelIndex + 1).paddedLeft ('0', 2)
         + "_" + safeName (name) + ".wav";
}

String defaultName (int channelIndex)
{
    return "Ch " + String (channelIndex + 1);
}
}
