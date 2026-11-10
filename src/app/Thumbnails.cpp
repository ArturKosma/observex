#include "app/Thumbnails.h"

#include "engine/ImageFile.h"

#include <imgui.h>

#include <exception>
#include <utility>

namespace observex {

const ScreenImage* Thumbnails::get(const std::filesystem::path& file) {
    if (file.empty()) {
        return nullptr;
    }
    if (const auto known = pictures_.find(file); known != pictures_.end()) {
        return known->second.get();
    }
    if (readFrame_ == ImGui::GetFrameCount()) {
        return nullptr; // One a frame: next frame.
    }
    readFrame_ = ImGui::GetFrameCount();

    std::unique_ptr<ScreenImage> picture;
    try {
        const ImageData image = readImageFile(file, kWidth);
        picture = std::make_unique<ScreenImage>(device_, image.width, image.height);
        picture->upload(image.bgra.data(), image.width * 4);
    } catch (const std::exception&) {
        picture.reset(); // Not tried again: the list shows an empty frame for it.
    }
    return pictures_.emplace(file, std::move(picture)).first->second.get();
}

} // namespace observex
