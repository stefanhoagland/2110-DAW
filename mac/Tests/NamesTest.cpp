// Checks for channel-name CSV handling and file names. Run: ./NamesTest
#include "../Source/Names.h"
#include <iostream>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (! ok) ++failures;
}

int main()
{
    using namespace juce;
    StringArray blank;
    for (int i = 0; i < 4; ++i) blank.add (names::defaultName (i));

    auto a = names::fromCsv ("Channel,Name\n1,Kick\n3,\"Snare, top\"\n", blank);
    check (a[0] == "Kick" && a[1] == "Ch 2" && a[2] == "Snare, top", "channel,name with header and quotes");

    auto b = names::fromCsv ("Name;Channel\nBass;2\nKeys;4\n", blank);
    check (b[1] == "Bass" && b[3] == "Keys", "name;channel with semicolons");

    auto c = names::fromCsv (String::charToString ((juce_wchar) 0xfeff) + "Vox\nGtr\n", blank);
    check (c[0] == "Vox" && c[1] == "Gtr" && c[2] == "Ch 3", "plain list with BOM");

    auto d = names::fromCsv ("1\tKick\n2\tSnare\n", blank);
    check (d[0] == "Kick" && d[1] == "Snare", "tab separated");

    auto e = names::fromCsv ("9,Too far\n", blank);
    check (e == blank, "out of range channel ignored");

    StringArray round { "Kick", "Snare, top", "Say \"hi\"" };
    check (names::fromCsv (names::toCsv (round), StringArray { "", "", "" }) == round, "export then import");

    check (names::takeFileName (3, 0, "Kick In!") == "T03_01_Kick_In.wav", "take file name");
    check (names::safeName ("  ") == "Track", "empty name");

    std::cout << (failures ? "FAILED\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
