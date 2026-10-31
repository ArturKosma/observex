#pragma once

#include <d3d11.h>

namespace observex {

// Saves `texture` (8-bit RGBA or BGRA, e.g. the window's back buffer) as an opaque PNG at `path`.
// For checking the UI without showing it; not for the hot path. Throws on failure.
void savePng(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const wchar_t* path);

} // namespace observex
