#pragma once

namespace dp
{
// All user-visible text, in English and Bangla. Chosen from the Windows display language.
struct Strings
{
    const wchar_t* appName;
    const wchar_t* trayTip;
    const wchar_t* menuHover;
    const wchar_t* menuStartup;
    const wchar_t* menuExit;
    const wchar_t* balloonTitle;
    const wchar_t* balloonText;

    const wchar_t* loading;
    const wchar_t* failed;
    const wchar_t* open;
    const wchar_t* showInFolder;
    const wchar_t* copyFonts;
    const wchar_t* copied;

    const wchar_t* labelSize;
    const wchar_t* labelResolution;
    const wchar_t* labelColor;
    const wchar_t* labelFileSize;
    const wchar_t* labelModified;
    const wchar_t* labelLayers;
    const wchar_t* labelTopLevel;
    const wchar_t* labelFonts;

    const wchar_t* groupsFormat;     // "%d groups"
    const wchar_t* moreFormat;       // "+%d more"
    const wchar_t* bitFormat;        // "%d-bit"
    const wchar_t* flattened;        // no layers
    const wchar_t* noComposite;
    const wchar_t* groupPrefix;      // shown before group names
};

const Strings& Text();
} // namespace dp
