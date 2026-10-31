#include "engine/ImageChannel.h"

namespace observex {

ImageChannel::ImageChannel(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format)
    : width_(width), height_(height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // Shared textures need both.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (Slot& slot : slots_) {
        check(device->CreateTexture2D(&desc, nullptr, &slot.texture), "CreateTexture2D (image channel)");
        check(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex (image channel)");
        ComPtr<IDXGIResource> resource;
        check(slot.texture.As(&resource), "IDXGIResource (image channel)");
        check(resource->GetSharedHandle(&slot.sharedHandle), "GetSharedHandle (image channel)");
    }
}

void ImageChannel::write(ID3D11DeviceContext* context, ID3D11Texture2D* image) {
    Slot& slot = slots_[static_cast<std::size_t>(handover_.writeIndex())];

    // Zero timeout: the triple buffer guarantees the reader is not using this texture, so this never waits.
    if (slot.mutex->AcquireSync(writeKey_, 0) != S_OK) {
        return;
    }
    context->CopyResource(slot.texture.Get(), image);
    slot.mutex->ReleaseSync(kReaderKey);

    handover_.writeSlot() = ++written_;
    // A texture handed back unread is still released with kReaderKey; one the reader returned has kWriterKey.
    writeKey_ = handover_.publish() ? kReaderKey : kWriterKey;
}

ImageReader::ImageReader(ID3D11Device* device, ImageChannel& channel) : channel_(channel) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        check(device->OpenSharedResource(channel.slots_[i].sharedHandle, IID_PPV_ARGS(&slot.texture)),
              "OpenSharedResource (image channel)");
        check(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex (image channel)");
    }
}

bool ImageReader::copyNewImage(ID3D11DeviceContext* context, ID3D11Texture2D* target) {
    TripleBuffer<std::uint64_t>& handover = channel_.handover_;
    if (!handover.fetch()) {
        return false;
    }
    // Hold the texture only while queuing the copy: the release is queued after it on the GPU, so the
    // pipeline cannot overwrite the image before it is copied, and it never waits for the reader.
    Slot& slot = slots_[static_cast<std::size_t>(handover.readIndex())];
    if (slot.mutex->AcquireSync(ImageChannel::kReaderKey, 0) != S_OK) {
        return false;
    }
    context->CopySubresourceRegion(target, 0, 0, 0, 0, slot.texture.Get(), 0, nullptr);
    slot.mutex->ReleaseSync(ImageChannel::kWriterKey);
    return true;
}

} // namespace observex
