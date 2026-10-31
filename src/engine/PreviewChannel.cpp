#include "engine/PreviewChannel.h"

namespace observex {

PreviewChannel::PreviewChannel(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format)
    : width_(width), height_(height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (Slot& slot : slots_) {
        check(device->CreateTexture2D(&desc, nullptr, &slot.texture), "CreateTexture2D (preview)");
        check(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex (preview)");
        ComPtr<IDXGIResource> resource;
        check(slot.texture.As(&resource), "IDXGIResource (preview)");
        check(resource->GetSharedHandle(&slot.sharedHandle), "GetSharedHandle (preview)");
    }
}

void PreviewChannel::write(ID3D11DeviceContext* context, ID3D11Texture2D* image) {
    Slot& slot = slots_[static_cast<std::size_t>(handover_.writeIndex())];

    // Zero timeout: the triple buffer guarantees the UI is not using this texture, so this never waits.
    if (slot.mutex->AcquireSync(writeKey_, 0) != S_OK) {
        return;
    }
    context->CopyResource(slot.texture.Get(), image);
    slot.mutex->ReleaseSync(kReaderKey);

    handover_.writeSlot() = ++written_;
    // A texture handed back unread is still released with kReaderKey; one the UI returned has kWriterKey.
    writeKey_ = handover_.publish() ? kReaderKey : kWriterKey;
}

PreviewReader::PreviewReader(ID3D11Device* device, PreviewChannel& channel) : channel_(channel) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        check(device->OpenSharedResource(channel.slots_[i].sharedHandle, IID_PPV_ARGS(&slot.texture)),
              "OpenSharedResource (preview)");
        check(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex (preview)");
        check(device->CreateShaderResourceView(slot.texture.Get(), nullptr, &slot.view),
              "CreateShaderResourceView (preview)");
    }
}

PreviewReader::~PreviewReader() {
    if (holding_) {
        slots_[static_cast<std::size_t>(channel_.handover_.readIndex())].mutex->ReleaseSync(PreviewChannel::kWriterKey);
    }
}

void PreviewReader::update() {
    TripleBuffer<std::uint64_t>& handover = channel_.handover_;
    if (!handover.hasNew()) {
        return;
    }
    // Hand the current texture back first. The release is queued after this device's earlier draws
    // that use it, so the pipeline cannot overwrite it while it is still on screen.
    if (holding_) {
        slots_[static_cast<std::size_t>(handover.readIndex())].mutex->ReleaseSync(PreviewChannel::kWriterKey);
        holding_ = false;
    }
    handover.fetch();
    Slot& slot = slots_[static_cast<std::size_t>(handover.readIndex())];
    holding_ = slot.mutex->AcquireSync(PreviewChannel::kReaderKey, 0) == S_OK;
}

ID3D11ShaderResourceView* PreviewReader::view() const {
    return holding_ ? slots_[static_cast<std::size_t>(channel_.handover_.readIndex())].view.Get() : nullptr;
}

} // namespace observex
