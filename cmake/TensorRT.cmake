# TensorRT and the CUDA runtime, for live detection (src/engine/Detector).
#
# NVIDIA's TensorRT zip needs an NVIDIA account login, so the same files come from NVIDIA's public places instead:
# the TensorRT DLLs from its pip server, the C++ headers from its GitHub repository, and the CUDA runtime from its
# redistributable archive. About 2 GB, downloaded once into deps/ (gitignored, outside build/, so a clean build does
# not download them again) and checked against their SHA256. observex loads the DLLs from deps/ at run time
# (Detector.cpp), so nothing is copied next to observex.exe, and it calls TensorRT through the few C functions it
# exports, so no import libraries are needed.

set(OBSERVEX_DEPS_DIR ${CMAKE_SOURCE_DIR}/deps)

set(TENSORRT_VERSION 11.4.0.106)
set(TENSORRT_DIR ${OBSERVEX_DEPS_DIR}/tensorrt-${TENSORRT_VERSION})
set(TENSORRT_HEADERS_COMMIT 16ed0636df41e4a27d46f82d9028fc50fadc7200) # Tag v11.4 of github.com/NVIDIA/TensorRT.
set(CUDART_VERSION 13.3.29) # CUDA 13.3, the newest the driver of 2026-11-01 (610.88) runs.
set(CUDART_DIR ${OBSERVEX_DEPS_DIR}/cuda-${CUDART_VERSION})

# TensorRT's DLLs: the Windows wheel of tensorrt-cu13-libs, a zip. The wheel goes once its DLLs are out.
if(NOT EXISTS ${TENSORRT_DIR}/bin/nvinfer_11.dll)
    set(wheel ${OBSERVEX_DEPS_DIR}/downloads/tensorrt_cu13_libs-${TENSORRT_VERSION}-py3-none-win_amd64.whl)
    message(STATUS "Downloading TensorRT ${TENSORRT_VERSION} (about 2 GB, once) into ${OBSERVEX_DEPS_DIR}")
    file(DOWNLOAD
        https://pypi.nvidia.com/tensorrt-cu13-libs/tensorrt_cu13_libs-${TENSORRT_VERSION}-py3-none-win_amd64.whl
        ${wheel} TLS_VERIFY ON
        EXPECTED_HASH SHA256=4d6a6e8b644f7f24df686c13809de7935eb3878d2a48cda1c7342c31143d4c16)
    set(unpack ${OBSERVEX_DEPS_DIR}/unpack)
    file(REMOVE_RECURSE ${unpack})
    file(ARCHIVE_EXTRACT INPUT ${wheel} DESTINATION ${unpack} PATTERNS "tensorrt_libs/*.dll" "*.dist-info/LICENSE.txt")
    file(GLOB license ${unpack}/*.dist-info/LICENSE.txt)
    file(COPY ${license} DESTINATION ${unpack}/tensorrt_libs)
    file(MAKE_DIRECTORY ${TENSORRT_DIR})
    file(RENAME ${unpack}/tensorrt_libs ${TENSORRT_DIR}/bin)
    file(REMOVE_RECURSE ${unpack})
    file(REMOVE ${wheel})
endif()

# The headers observex includes (NvInfer.h, NvOnnxParser.h and what they include), name then SHA256.
set(TENSORRT_HEADERS
    NvInfer.h               67f401faabd5bc0f5dde8489c9ec79f2a7bd42b0b63f5ebdae0c977f2534e902
    NvInferImpl.h           6aa27b9f29cc9bc0ee3d1ab76d864d0e623c461ae516fab3ccb46dce8cc7be99
    NvInferLegacyDims.h     3e10fe5b85545f2e956e6935271badf0b1c72553b698000a61d46d69c5d53030
    NvInferPluginBase.h     759f354754e64bf4d1e68d7df796c155c46f8a27541bf1912ba13e79a5a26fc2
    NvInferRuntime.h        23ce5d1d5735dbb86b357ce7dca44f4f4a7e908faaee9871149664843ade8a50
    NvInferRuntimeBase.h    fa22f902443d7c832dbf2dd74c8d0744990e9f6118245dca183a8ce23077dac0
    NvInferRuntimeCommon.h  eff4c0251cb4813bbc7fe34c6b28fdb6c08734c27ae53b07974d2557bf1431be
    NvInferRuntimePlugin.h  d4db4236da9c03e543949247a75478b5ce68fc84eb30cd15b35081b94c8d8391
    NvInferVersion.h        6a29a88a274b25cdaf87dbe6e5be1b3b68e6c430308ae9a2c03f6c4282a7f2e7
    NvOnnxParser.h          02f419e6137c6956f4ae1b496ce874aecbc064cdeef5a9bf957c9c4bb73c142f)
while(TENSORRT_HEADERS)
    list(POP_FRONT TENSORRT_HEADERS name hash)
    if(NOT EXISTS ${TENSORRT_DIR}/include/${name})
        file(DOWNLOAD
            https://raw.githubusercontent.com/NVIDIA/TensorRT/${TENSORRT_HEADERS_COMMIT}/include/${name}
            ${TENSORRT_DIR}/include/${name} TLS_VERIFY ON EXPECTED_HASH SHA256=${hash})
    endif()
endwhile()

# The CUDA runtime: its headers (TensorRT's include them) and the static library observex links. Two of CUDA's
# redistributable parts, unpacked into one folder: cudart, and crt (headers cudart's include).
if(NOT EXISTS ${CUDART_DIR}/lib/x64/cudart_static.lib OR NOT EXISTS ${CUDART_DIR}/include/crt/host_defines.h)
    set(CUDA_PARTS
        cuda_cudart ${CUDART_VERSION} 1feb7dd266813ffe8dbc24e115183a5ac35a4795c8d34aca0df85ab616b64d9c
        cuda_crt    13.3.73           9227ec7c80db10b7cb0d4ee71ed62ec7ae36e67890216413ab6f9afa35d577f0)
    set(unpack ${OBSERVEX_DEPS_DIR}/unpack)
    file(REMOVE_RECURSE ${CUDART_DIR})
    while(CUDA_PARTS)
        list(POP_FRONT CUDA_PARTS part version hash)
        set(name ${part}-windows-x86_64-${version}-archive)
        file(DOWNLOAD
            https://developer.download.nvidia.com/compute/cuda/redist/${part}/windows-x86_64/${name}.zip
            ${OBSERVEX_DEPS_DIR}/downloads/${name}.zip TLS_VERIFY ON EXPECTED_HASH SHA256=${hash})
        file(REMOVE_RECURSE ${unpack})
        file(ARCHIVE_EXTRACT INPUT ${OBSERVEX_DEPS_DIR}/downloads/${name}.zip DESTINATION ${unpack})
        file(COPY ${unpack}/${name}/ DESTINATION ${CUDART_DIR})
        file(REMOVE_RECURSE ${unpack})
        file(REMOVE ${OBSERVEX_DEPS_DIR}/downloads/${name}.zip)
    endwhile()
endif()

add_library(observex_tensorrt INTERFACE)
target_include_directories(observex_tensorrt SYSTEM INTERFACE ${TENSORRT_DIR}/include ${CUDART_DIR}/include)
target_link_libraries(observex_tensorrt INTERFACE ${CUDART_DIR}/lib/x64/cudart_static.lib)
# Where Detector.cpp looks for the DLLs when they are not next to observex.exe.
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/TensorRtPath.h
    CONTENT "#pragma once\n#define OBSERVEX_TENSORRT_BIN L\"${TENSORRT_DIR}/bin\"\n")
target_include_directories(observex_tensorrt INTERFACE ${CMAKE_BINARY_DIR}/generated)
