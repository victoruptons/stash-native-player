#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Text recognition in images with the OCR engine built into Windows
// (Windows.Media.Ocr; no model to download, runs on the CPU in tens of ms).
// Used to spot subtitles burned into the picture. Plain std types on purpose:
// the WinRT headers stay out of Qt code.
namespace screenocr {

// Installed OCR languages as BCP-47 tags, e.g. {"en-US", "zh-Hant-TW"}.
std::vector<std::wstring> languages();

// Lines of text found in an encoded image (PNG, JPEG, BMP). `language` is a
// tag from languages(); empty = the user's profile languages. Blocks: call it
// off the GUI thread. Returns nothing if the image or language is unusable.
std::vector<std::wstring> recognize(const std::vector<std::uint8_t> &image, const std::wstring &language);

} // namespace screenocr
