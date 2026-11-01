#pragma once

#include "engine/Gpu.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace observex {

class ImageChannel;
class ImageReader;
class LiveLog;

// Saves the images handed to it through an ImageChannel as numbered JPEG files (000001.jpg, 000002.jpg, ...)
// in one folder. It works on its own low-priority thread with its own GPU device, so reading the images back
// and encoding them never slows the pipeline. The pipeline decides how often it hands over an image
// (Pipeline::setRecordingInterval); images that arrive while one is still being saved replace each other.
class Recorder {
public:
    // Starts saving the images of `channel`, which come from `screen`, into `folder` (created if missing),
    // numbering them from `firstNumber`. Each saved image also goes into `log`, if given. Throws on failure.
    Recorder(const Screen& screen, ImageChannel& channel, std::filesystem::path folder, unsigned firstNumber,
             std::shared_ptr<LiveLog> log = nullptr);
    // Stops once the image being saved, if any, is written. Destroy it before the channel.
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Images saved so far, and the bytes their files take on disk.
    unsigned saved() const { return saved_.load(std::memory_order_acquire); }
    std::uint64_t savedBytes() const { return savedBytes_.load(std::memory_order_acquire); }

    // True once saving has stopped because of an error (e.g. the disk is full); error() says why.
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    const std::string& error() const { return error_; } // Valid once failed() is true.

private:
    void run(std::stop_token stop);
    void fail(std::string message);

    GpuDevice gpu_;
    std::unique_ptr<ImageReader> reader_;
    ComPtr<ID3D11Texture2D> staging_; // CPU-readable copy of the image being saved.
    std::filesystem::path folder_;
    std::shared_ptr<LiveLog> log_;
    unsigned nextNumber_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    std::atomic<unsigned> saved_{0};
    std::atomic<std::uint64_t> savedBytes_{0};
    std::atomic<bool> failed_{false};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
