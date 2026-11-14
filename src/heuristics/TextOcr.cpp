#include "heuristics/TextOcr.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

#include <cstring>

namespace observex {

struct TextOcr::Engine {
    winrt::Windows::Media::Ocr::OcrEngine ocr{nullptr};
};

TextOcr::TextOcr() = default;

TextOcr::~TextOcr() {
    engine_.reset();
    // This code may be in a DLL that is unloaded and loaded again (observex_stats.dll): no cached Windows factory may
    // outlive it.
    winrt::clear_factory_cache();
}

std::string TextOcr::read(const std::uint8_t* grey, int width, int height, std::ptrdiff_t stride, std::string* error) {
    using namespace winrt::Windows::Graphics::Imaging;
    if (failed_ || width <= 0 || height <= 0) {
        return {};
    }
    try {
        if (!engine_) {
            // Windows' threads: multi-threaded on this one, unless it already joined otherwise.
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (const winrt::hresult_error&) {
                // Already single-threaded: it works either way.
            }
            auto engine = std::make_unique<Engine>();
            engine->ocr = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine->ocr) {
                failed_ = true;
                if (error != nullptr) {
                    *error = "Windows has no OCR for the user's languages";
                }
                return {};
            }
            engine_ = std::move(engine);
        }
        SoftwareBitmap bitmap(BitmapPixelFormat::Gray8, width, height, BitmapAlphaMode::Ignore);
        {
            BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
            const BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
            winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();
            std::uint8_t* to = reference.data() + plane.StartIndex;
            for (int y = 0; y < height; ++y) {
                std::memcpy(to + static_cast<std::ptrdiff_t>(y) * plane.Stride, grey + y * stride,
                            static_cast<std::size_t>(width));
            }
        }
        const auto result = engine_->ocr.RecognizeAsync(bitmap).get();
        std::string text;
        for (const auto& line : result.Lines()) {
            if (!text.empty()) {
                text += ' ';
            }
            text += winrt::to_string(line.Text());
        }
        return text;
    } catch (const winrt::hresult_error& problem) {
        if (error != nullptr) {
            *error = "Windows' OCR failed: " + winrt::to_string(problem.message());
        }
        return {};
    }
}

} // namespace observex
