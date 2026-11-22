#include "Strings.h"

#include <windows.h>

namespace dp
{
namespace
{
constexpr Strings kEnglish = {
    L"DesignPeek",
    L"DesignPeek — hover over a design file in File Explorer",
    L"Hover preview",
    L"Start with Windows",
    L"Exit",
    L"DesignPeek is running",
    L"Rest the mouse on a PSD file in File Explorer to see a preview and design details.",

    L"Loading…",
    L"This file could not be read.",
    L"Open",
    L"Show in folder",
    L"Copy fonts",
    L"Copied!",

    L"Size",
    L"Resolution",
    L"Color",
    L"File size",
    L"Modified",
    L"Layers",
    L"Top level",
    L"Fonts",

    L"%d groups",
    L"+%d more",
    L"%d-bit",
    L"Flattened (no layers)",
    L"Saved without “Maximize Compatibility” — showing the small embedded preview.",
    L"▸ ",
};

constexpr Strings kBangla = {
    L"DesignPeek",
    L"DesignPeek — File Explorer-এ ডিজাইন ফাইলের ওপর মাউস রাখুন",
    L"হোভার প্রিভিউ",
    L"Windows চালু হলে চালু হবে",
    L"বন্ধ করুন",
    L"DesignPeek চালু আছে",
    L"File Explorer-এ কোনো PSD ফাইলের ওপর মাউস রাখুন — পাশে প্রিভিউ আর ডিটেইলস দেখাবে।",

    L"লোড হচ্ছে…",
    L"ফাইলটি পড়া যায়নি।",
    L"খুলুন",
    L"ফোল্ডারে দেখুন",
    L"ফন্ট কপি",
    L"কপি হয়েছে!",

    L"মাপ",
    L"রেজোলিউশন",
    L"কালার",
    L"ফাইল সাইজ",
    L"পরিবর্তন",
    L"লেয়ার",
    L"উপরের সারি",
    L"ফন্ট",

    L"%dটি গ্রুপ",
    L"আরও %dটি",
    L"%d-বিট",
    L"ফ্ল্যাট (কোনো লেয়ার নেই)",
    L"“Maximize Compatibility” ছাড়া সেভ করা — তাই ছোট প্রিভিউ দেখাচ্ছে।",
    L"▸ ",
};
} // namespace

const Strings& Text()
{
    static const Strings* strings =
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_BENGALI ? &kBangla : &kEnglish;
    return *strings;
}
} // namespace dp
