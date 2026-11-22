#include "screenocr.h"

#ifdef _WIN32

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

namespace screenocr {

namespace {

// WinRT needs an initialized apartment on the calling thread; worker threads
// (Qt's thread pool) have none. MTA, since .get() is not allowed on an STA.
void ensureApartment()
{
    thread_local bool done = false;
    if (done)
        return;
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error &) {
        // already initialized on this thread (possibly as STA: then .get()
        // below throws, and recognize() returns nothing)
    }
    done = true;
}

} // namespace

std::vector<std::wstring> languages()
{
    ensureApartment();
    std::vector<std::wstring> out;
    try {
        for (const auto &lang : OcrEngine::AvailableRecognizerLanguages())
            out.emplace_back(lang.LanguageTag());
    } catch (const hresult_error &) {
    }
    return out;
}

std::vector<std::wstring> recognize(const std::vector<std::uint8_t> &image, const std::wstring &language)
{
    ensureApartment();
    std::vector<std::wstring> lines;
    try {
        OcrEngine engine = language.empty()
                               ? OcrEngine::TryCreateFromUserProfileLanguages()
                               : OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(language));
        if (!engine)
            return lines;

        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(array_view<const uint8_t>(image.data(), image.data() + image.size()));
        writer.StoreAsync().get();
        writer.DetachStream();
        stream.Seek(0);

        BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
        // The engine refuses images larger than MaxImageDimension: scale down.
        const uint32_t maxDim = OcrEngine::MaxImageDimension();
        const uint32_t w = decoder.PixelWidth(), h = decoder.PixelHeight();
        SoftwareBitmap bitmap{nullptr};
        if (w > maxDim || h > maxDim) {
            const double scale = double(maxDim) / double(std::max(w, h));
            BitmapTransform transform;
            transform.ScaledWidth(uint32_t(w * scale));
            transform.ScaledHeight(uint32_t(h * scale));
            bitmap = decoder
                         .GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, transform,
                                                 ExifOrientationMode::IgnoreExifOrientation,
                                                 ColorManagementMode::DoNotColorManage)
                         .get();
        } else {
            bitmap = decoder.GetSoftwareBitmapAsync().get();
        }
        OcrResult result = engine.RecognizeAsync(bitmap).get();
        for (const auto &line : result.Lines())
            lines.emplace_back(line.Text());
    } catch (const hresult_error &) {
        lines.clear();
    }
    return lines;
}

} // namespace screenocr

#else

namespace screenocr {
std::vector<std::wstring> languages() { return {}; }
std::vector<std::wstring> recognize(const std::vector<std::uint8_t> &, const std::wstring &) { return {}; }
} // namespace screenocr

#endif
