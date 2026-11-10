# NVENC, the GPU's video encoder, for the clip buffer (src/engine/ClipRecorder).
#
# Only its header is needed: the encoder itself is in the NVIDIA driver (nvEncodeAPI64.dll), which observex loads at
# run time. The header (MIT licence) comes from FFmpeg's copy of NVIDIA's Video Codec SDK headers, release n13.0.19.1
# (Video Codec API 13.0), downloaded once into deps/ and checked against its SHA256. Needs OBSERVEX_DEPS_DIR
# (cmake/TensorRT.cmake).

set(NVENC_HEADERS_COMMIT 88fee5c37318c991a8762d423530f91681e32e3a) # Tag n13.0.19.1 of FFmpeg/nv-codec-headers.
set(NVENC_DIR ${OBSERVEX_DEPS_DIR}/nvenc-13.0)

if(NOT EXISTS ${NVENC_DIR}/include/nvEncodeAPI.h)
    file(DOWNLOAD
        https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/${NVENC_HEADERS_COMMIT}/include/ffnvcodec/nvEncodeAPI.h
        ${NVENC_DIR}/include/nvEncodeAPI.h TLS_VERIFY ON
        EXPECTED_HASH SHA256=4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e)
endif()

add_library(observex_nvenc INTERFACE)
target_include_directories(observex_nvenc SYSTEM INTERFACE ${NVENC_DIR}/include)
