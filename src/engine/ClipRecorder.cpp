#include "engine/ClipRecorder.h"

#include "core/ClipLines.h"
#include "core/LiveLines.h"
#include "engine/ImageFile.h"
#include "engine/LiveLog.h"
#include "engine/Recognizer.h"
#include "engine/ScreenCapture.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <nvEncodeAPI.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>

// The clip buffer. Its thread (run) takes each new screen image from the pipeline's clip channel and has the GPU's
// video encoder (NVENC, through the NVIDIA driver's nvEncodeAPI64.dll) compress it into H.264, synchronously, on a GPU
// device of its own. The compressed frames stay in memory (Impl::packets), cut at key frames to the last kClipSeconds;
// the recogniser's looks come through a fixed queue that never waits (Impl::queue) and stay beside them. Saving hands
// a copy of all that to a writer thread (writeClip), which muxes the frames into clip.mp4 with Media Foundation's sink
// writer (as they are: no second encode) and writes clip.txt, picture.jpg and clip.observex.
//
// The video has a frame each time the screen changed, at the time the screen showed it (so its frame rate varies), no
// B-frames, and a key frame (IDR) at least every kKeyEvery, so the last kClipSeconds can be cut out at a key frame;
// while the screen does not change, the last image is repeated as a key frame every kKeyEvery. Frames are at least
// a frame gap apart (1 / Impl::framesPerSecond: at most kMaxFramesPerSecond, and what H.264 level 5.2 allows at the
// screen's size). Within a gap the thread copies nothing: the image the screen shows at its end is the next frame,
// and if the screen showed it sooner than a gap after the last frame, the frame's time is a moment later than that
// (less than a gap), so the video keeps to its level.

namespace observex {

namespace {

constexpr double kKeyEvery = 0.5; // Seconds between key frames, at most.
constexpr unsigned kMaxFramesPerSecond = 144;
// H.264 level 5.2's most macroblocks (16 x 16 pixels) a second, which decoders are built for (2560 x 1440: 144 a
// second).
constexpr std::uint64_t kLevelMacroblocksPerSecond = 2073600;
// Constant quality (0 best to 51), within a highest rate (bits a second at the declared frame rate), which bounds
// what the clip buffer holds.
constexpr std::uint8_t kQuality = 22;
constexpr std::uint32_t kMaxBitRate = 120'000'000;
constexpr unsigned kWaitMs = 20;        // Most the thread sleeps between looks at its queues.
constexpr std::size_t kQueueLooks = 64; // Looks waiting for the thread (about 0.3 s of looking).

std::int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::int64_t qpcFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

// One compressed frame (Annex B: NAL units after start codes).
struct Packet {
    std::int64_t qpc = 0;    // When the screen showed it.
    std::uint64_t image = 0; // Its screen image number (ImageStamp::number); the one before for a repeat.
    bool key = false;        // An IDR frame, with SPS and PPS before it: the video can start here.
    std::vector<std::uint8_t> bytes;
};

// A look of the recogniser as the thread keeps it.
struct Look {
    ImageStamp stamp;
    std::int64_t qpc = 0;     // When the screen showed the image.
    std::int64_t doneQpc = 0; // When the recogniser had it.
    std::vector<Detection> found;
    std::vector<Tracked> followed;
};

struct Note {
    std::int64_t qpc = 0;
    std::string text;
};

// A look on its way from the recogniser's thread, in a fixed slot so handing it over allocates nothing.
struct QueuedLook {
    ImageStamp stamp;
    std::int64_t doneQpc = 0;
    std::uint32_t found = 0;
    std::uint32_t followed = 0;
    std::array<Detection, Found::kMax> things{};
    std::array<Tracked, Found::kMax> shown{};
};

// Everything a save writes, copied out of the buffer.
struct SaveJob {
    std::filesystem::path folder;
    ClipAbout about;
    std::int64_t savedQpc = 0;
    std::int64_t frequency = 1;
    unsigned width = 0;
    unsigned height = 0;
    unsigned framesPerSecond = 0;
    std::vector<std::uint8_t> sequenceHeader; // SPS and PPS, Annex B.
    std::vector<std::shared_ptr<const Packet>> packets;
    std::vector<Look> looks;
    std::vector<Note> notes;
    std::vector<std::uint8_t> picture; // The screen at the save, BGRA, rows width * 4 bytes apart; empty: none.
};

const char* nvencStatusName(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "no encode device";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "unsupported device";
    case NV_ENC_ERR_INVALID_PARAM: return "invalid parameter";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "out of memory";
    case NV_ENC_ERR_ENCODER_BUSY: return "encoder busy";
    case NV_ENC_ERR_INVALID_VERSION: return "invalid version";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "unsupported parameter";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY: return "too many encode sessions";
    default: return "error";
    }
}

// Milliseconds from `origin` to `qpc`.
double msSince(std::int64_t qpc, std::int64_t origin, std::int64_t frequency) {
    return static_cast<double>(qpc - origin) * 1000.0 / static_cast<double>(frequency);
}

// Media Foundation's time unit (100 ns) from `origin` to `qpc`.
LONGLONG hundredNs(std::int64_t qpc, std::int64_t origin, std::int64_t frequency) {
    const std::int64_t ticks = qpc - origin;
    return static_cast<LONGLONG>((ticks / frequency) * 10'000'000 + (ticks % frequency) * 10'000'000 / frequency);
}

// COM for this thread, if it has none yet.
class ComScope {
public:
    ComScope() : started_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (started_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool started_;
};

// The frames as clip.mp4, muxed as they are (H.264 in MP4) by Media Foundation's sink writer.
void writeVideo(const std::filesystem::path& file, const SaveJob& job) {
    const ComScope com;
    check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
    struct Mf {
        ~Mf() { MFShutdown(); }
    } mf;

    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
    check(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4), "SetGUID container");
    check(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "SetUINT32 throttling");
    ComPtr<IMFSinkWriter> writer;
    check(MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &writer), "MFCreateSinkWriterFromURL");

    const std::int64_t origin = job.packets.front()->qpc;
    std::uint64_t bytes = 0;
    for (const auto& packet : job.packets) {
        bytes += packet->bytes.size();
    }
    const double seconds = std::max(0.001, msSince(job.savedQpc, origin, job.frequency) / 1000.0);

    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "MFCreateMediaType");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "SetGUID major type");
    check(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "SetGUID subtype");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, job.width, job.height), "MF_MT_FRAME_SIZE");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, job.framesPerSecond, 1), "MF_MT_FRAME_RATE");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MF_MT_PIXEL_ASPECT_RATIO");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "MF_MT_INTERLACE_MODE");
    check(type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "MF_MT_MPEG2_PROFILE");
    check(type->SetUINT32(MF_MT_AVG_BITRATE,
                          static_cast<UINT32>(std::min(4e9, 8.0 * static_cast<double>(bytes) / seconds))),
          "MF_MT_AVG_BITRATE");
    check(type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, job.sequenceHeader.data(),
                        static_cast<UINT32>(job.sequenceHeader.size())),
          "MF_MT_MPEG_SEQUENCE_HEADER");
    DWORD stream = 0;
    check(writer->AddStream(type.Get(), &stream), "IMFSinkWriter::AddStream");
    check(writer->SetInputMediaType(stream, type.Get(), nullptr), "IMFSinkWriter::SetInputMediaType");
    check(writer->BeginWriting(), "IMFSinkWriter::BeginWriting");

    for (std::size_t i = 0; i < job.packets.size(); ++i) {
        const Packet& packet = *job.packets[i];
        const std::int64_t endQpc = i + 1 < job.packets.size() ? job.packets[i + 1]->qpc : job.savedQpc;
        const LONGLONG time = hundredNs(packet.qpc, origin, job.frequency);
        // The last frame lasts until the save; any frame at least 0.1 ms.
        const LONGLONG duration = std::max<LONGLONG>(1000, hundredNs(endQpc, origin, job.frequency) - time);

        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &buffer), "MFCreateMemoryBuffer");
        BYTE* data = nullptr;
        check(buffer->Lock(&data, nullptr, nullptr), "IMFMediaBuffer::Lock");
        std::memcpy(data, packet.bytes.data(), packet.bytes.size());
        check(buffer->Unlock(), "IMFMediaBuffer::Unlock");
        check(buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size())), "IMFMediaBuffer::SetCurrentLength");
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "MFCreateSample");
        check(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
        check(sample->SetSampleTime(time), "IMFSample::SetSampleTime");
        check(sample->SetSampleDuration(duration), "IMFSample::SetSampleDuration");
        if (packet.key) {
            check(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE), "MFSampleExtension_CleanPoint");
        }
        check(writer->WriteSample(stream, sample.Get()), "IMFSinkWriter::WriteSample");
    }
    check(writer->Finalize(), "IMFSinkWriter::Finalize");
}

// clip.txt: what the clip holds, line by line, in the order it happened (see its header and core/ClipLines). Counts
// the looks into `looks`.
std::string clipText(const SaveJob& job, unsigned& looks) {
    const std::int64_t origin = job.packets.front()->qpc;
    const auto ms = [&](std::int64_t qpc) { return msSince(qpc, origin, job.frequency); };

    std::string text;
    text += "# observex clip: the last seconds of the observed screen (clip.mp4, nothing drawn on it) and everything\n"
            "# live recognition saw and showed on them.\n";
    text += "# Saved " + job.about.saved + ", screen " + job.about.screen + " (" + std::to_string(job.width) + " x " +
            std::to_string(job.height) + "), model " + (job.about.model.empty() ? "none" : job.about.model) + ".\n";
    if (job.about.kinds.empty()) {
        text += "# The model has no kinds.\n";
    } else {
        text += "# Kinds:";
        for (std::size_t i = 0; i < job.about.kinds.size(); ++i) {
            text += (i == 0 ? " " : ", ") + std::to_string(i) + " " + job.about.kinds[i];
        }
        text += ".\n";
    }
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "# Times are milliseconds since the first video frame: of when the screen showed the image (frame, look), or\n"
        "#   of when it happened (shown's DONE, note).\n"
        "# frame FRAME IMAGE TIME: video frame FRAME of clip.mp4 (from 1) is screen image number IMAGE (the numbers\n"
        "#   looks use), shown at TIME. The screen changed between two frames; images the video encoder had no time\n"
        "#   for are not in it (it takes at most %u a second). The same IMAGE again repeats the frame before: the\n"
        "#   screen did not change for %.1f s.\n",
        job.framesPerSecond, kKeyEvery);
    text += line;
    std::snprintf(
        line, sizeof(line),
        "# look IMAGE TIME [SURE X Y WIDTH HEIGHT]...: the model looked at screen image IMAGE and found these things,\n"
        "#   surest first, in screen pixels, at least %.0f%% sure (the app shows what the tracker makes of the ones\n"
        "#   at least %.0f%% sure: see shown).\n",
        static_cast<double>(LiveLog::kMinSure * 100.0f), static_cast<double>(Recognizer::kMinSure * 100.0f));
    text += line;
    text += "# kinds IMAGE [KIND SURE SECOND SURE]...: for a model with kinds, right after its look: each thing's\n"
            "#   surest kind and how sure of it, then its second-surest, in the same order.\n"
            "# shown IMAGE DONE [ID SURE KIND KINDSURE SECOND SECONDSURE X Y WIDTH HEIGHT SPEEDX SPEEDY HELD]...: what\n"
            "#   the app showed from DONE on, after its look at IMAGE: the tracker's things, surest first. Each one's\n"
            "#   id (the same while it is followed), how sure (smoothed), its kind and how sure of it, its second kind\n"
            "#   and how sure of that (-1: no kinds), its box in screen pixels, its speed in pixels per millisecond,\n"
            "#   and 1 if it was not found on this look but held where its speed takes it.\n"
            "# note TIME TEXT: something that happened then, e.g. a recording started.\n";

    // Frames, looks and notes in time order; at the same time a frame comes first, then the look at its image.
    struct Item {
        std::int64_t qpc;
        int order; // 0 frame, 1 look, 2 note.
        std::size_t index;
    };
    std::vector<Item> items;
    items.reserve(job.packets.size() + job.looks.size() + job.notes.size());
    for (std::size_t i = 0; i < job.packets.size(); ++i) {
        items.push_back({job.packets[i]->qpc, 0, i});
    }
    for (std::size_t i = 0; i < job.looks.size(); ++i) {
        items.push_back({job.looks[i].qpc, 1, i});
    }
    for (std::size_t i = 0; i < job.notes.size(); ++i) {
        items.push_back({job.notes[i].qpc, 2, i});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.qpc != b.qpc ? a.qpc < b.qpc : a.order < b.order;
    });

    looks = 0;
    std::vector<char> lookLines(kLookChars);
    for (const Item& item : items) {
        if (item.order == 0) {
            const Packet& packet = *job.packets[item.index];
            appendFrame(text, static_cast<unsigned>(item.index + 1), packet.image, ms(packet.qpc));
        } else if (item.order == 1) {
            const Look& look = job.looks[item.index];
            const auto image = static_cast<unsigned long long>(look.stamp.number);
            text.append(lookLines.data(), writeLook(lookLines, image, ms(look.qpc), look.found));
            appendShown(text, image, ms(look.doneQpc), look.followed);
            ++looks;
        } else {
            const Note& note = job.notes[item.index];
            appendNote(text, ms(note.qpc), note.text);
        }
    }
    return text;
}

// Writes the clip's files into its folder; clip.observex last, so a clip the review lists is whole. Throws on failure.
ClipInfo writeClip(const SaveJob& job) {
    if (job.packets.empty()) {
        throw std::runtime_error("it holds no frames");
    }
    std::filesystem::create_directories(job.folder);
    writeVideo(job.folder / kClipVideo, job);

    unsigned looks = 0;
    const std::string text = clipText(job, looks);
    {
        std::ofstream out(job.folder / kClipText, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            throw std::runtime_error("cannot write " + toUtf8((job.folder / kClipText).c_str()));
        }
    }
    if (!job.picture.empty()) {
        writeImageFile(job.folder / kClipPicture, ImageFormat::Jpeg, job.picture.data(), job.width, job.height,
                       job.width * 4);
    }

    ClipInfo info;
    info.saved = job.about.saved;
    info.seconds = msSince(job.savedQpc, job.packets.front()->qpc, job.frequency) / 1000.0;
    info.frames = static_cast<unsigned>(job.packets.size());
    info.looks = looks;
    info.screen = job.about.screen;
    info.width = job.width;
    info.height = job.height;
    info.model = job.about.model;
    if (!writeClipInfo(job.folder, info)) {
        throw std::runtime_error("cannot write " + toUtf8((job.folder / kClipFile).c_str()));
    }
    return info;
}

} // namespace

struct ClipRecorder::Impl {
    unsigned width = 0;
    unsigned height = 0;
    unsigned framesPerSecond = kMaxFramesPerSecond;
    std::int64_t frequency = qpcFrequency();
    GpuDevice gpu; // The thread's own device.
    std::unique_ptr<ImageReader> reader;
    ComPtr<ID3D11Texture2D> input;   // The encoder's input: the newest image taken from the channel.
    ComPtr<ID3D11Texture2D> staging; // For the picture of a save; made on the first.

    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    std::vector<std::uint8_t> sequenceHeader;

    // The clip thread's: what it holds.
    std::deque<std::shared_ptr<const Packet>> packets;
    std::uint64_t packetBytes = 0;
    std::deque<Look> looks;
    std::deque<Note> notes;
    std::atomic<bool> holding{false}; // It holds at least one frame.

    // From the recogniser's thread (one writer) to the clip thread (one reader).
    std::array<QueuedLook, kQueueLooks> queue{};
    std::atomic<std::uint64_t> queueHead{0}; // Looks put in, ever.
    std::atomic<std::uint64_t> queueTail{0}; // Looks taken out, ever.
    std::atomic<std::uint64_t> dropped{0};

    // From the UI thread.
    std::mutex mutex;
    std::vector<Note> newNotes;
    bool saveAsked = false;
    SaveJob asked; // The folder, what it is about and when; the rest is filled on the clip thread.

    std::thread writer; // Writing the last save; joined before the next and when the ClipRecorder goes.

    ~Impl() {
        if (library != nullptr) {
            FreeLibrary(library);
        }
    }

    void nvCheck(NVENCSTATUS status, const char* what) const {
        if (status == NV_ENC_SUCCESS) {
            return;
        }
        std::string message = std::string(what) + " failed: NVENC " + nvencStatusName(status) + " (" +
                              std::to_string(static_cast<int>(status)) + ")";
        if (encoder != nullptr && api.nvEncGetLastErrorString != nullptr) {
            const char* last = api.nvEncGetLastErrorString(encoder);
            if (last != nullptr && last[0] != '\0') {
                message += std::string(": ") + last;
            }
        }
        throw std::runtime_error(message);
    }

    void openEncoder();
    void closeEncoder();
    // Encodes the input as the frame of screen image `image`, shown at `qpc`; as a key frame if `key`. Returns how
    // long it took, in milliseconds.
    float encode(std::int64_t qpc, std::uint64_t image, bool key);
    // Drops what is older than the last key frame shown at or before `cutoffQpc`.
    void trim(std::int64_t cutoffQpc);
    void takeLooks();
    void takeNotes();
    // The save the UI asked for, with a copy of all it holds.
    SaveJob takeSave();
};

void ClipRecorder::Impl::openEncoder() {
    library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr) {
        throw std::runtime_error("No NVIDIA video encoder: nvEncodeAPI64.dll is not installed (it comes with the "
                                 "NVIDIA GPU driver)");
    }
    using GetMaxVersion = NVENCSTATUS(NVENCAPI*)(std::uint32_t*);
    using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto getMaxVersion =
        reinterpret_cast<GetMaxVersion>(GetProcAddress(library, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto createInstance =
        reinterpret_cast<CreateInstance>(GetProcAddress(library, "NvEncodeAPICreateInstance"));
    if (getMaxVersion == nullptr || createInstance == nullptr) {
        throw std::runtime_error("nvEncodeAPI64.dll is not NVIDIA's video encoder");
    }
    std::uint32_t version = 0;
    nvCheck(getMaxVersion(&version), "NvEncodeAPIGetMaxSupportedVersion");
    constexpr std::uint32_t kNeeded = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (version < kNeeded) {
        throw std::runtime_error("The NVIDIA driver is too old for the clip buffer: its video encoder has API " +
                                 std::to_string(version >> 4) + "." + std::to_string(version & 0xF) + ", it needs " +
                                 std::to_string(NVENCAPI_MAJOR_VERSION) + "." +
                                 std::to_string(NVENCAPI_MINOR_VERSION) + " (update the driver)");
    }
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    nvCheck(createInstance(&api), "NvEncodeAPICreateInstance");

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = gpu.device.Get();
    session.apiVersion = NVENCAPI_VERSION;
    nvCheck(api.nvEncOpenEncodeSessionEx(&session, &encoder), "NvEncOpenEncodeSessionEx");

    const auto caps = [&](NV_ENC_CAPS what) {
        NV_ENC_CAPS_PARAM query{};
        query.version = NV_ENC_CAPS_PARAM_VER;
        query.capsToQuery = what;
        int value = 0;
        nvCheck(api.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_H264_GUID, &query, &value), "NvEncGetEncodeCaps");
        return value;
    };
    const int maxWidth = caps(NV_ENC_CAPS_WIDTH_MAX);
    const int maxHeight = caps(NV_ENC_CAPS_HEIGHT_MAX);
    if (static_cast<int>(width) > maxWidth || static_cast<int>(height) > maxHeight) {
        throw std::runtime_error("The screen (" + std::to_string(width) + " x " + std::to_string(height) +
                                 ") is bigger than the video encoder takes (" + std::to_string(maxWidth) + " x " +
                                 std::to_string(maxHeight) + ")");
    }

    // At most what H.264 level 5.2 allows at this size, so any decoder plays it.
    const std::uint64_t macroblocks =
        static_cast<std::uint64_t>((width + 15) / 16) * static_cast<std::uint64_t>((height + 15) / 16);
    framesPerSecond = static_cast<unsigned>(std::clamp<std::uint64_t>(
        kLevelMacroblocksPerSecond / std::max<std::uint64_t>(1, macroblocks), 10, kMaxFramesPerSecond));

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    nvCheck(api.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
                                             NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset),
            "NvEncGetEncodePresetConfigEx");
    NV_ENC_CONFIG config = preset.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    config.gopLength = NVENC_INFINITE_GOPLENGTH; // Key frames only when asked (by time, see run).
    config.frameIntervalP = 1;                   // No B-frames (P3 and up with high quality would add them).
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = 0; // Constant quality, within maxBitRate.
    config.rcParams.maxBitRate = kMaxBitRate;
    config.rcParams.targetQuality = kQuality;
    config.rcParams.targetQualityLSB = 0;
    config.rcParams.enableLookahead = 0;
    config.rcParams.zeroReorderDelay = 1;
    NV_ENC_CONFIG_H264& h264 = config.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.level = NV_ENC_LEVEL_AUTOSELECT;
    h264.chromaFormatIDC = 1;
    // The usual colours of HD video (BT.709, limited range), said in the stream so players turn them back alike.
    NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui = h264.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P4_GUID;
    init.encodeWidth = width;
    init.encodeHeight = height;
    init.darWidth = width;
    init.darHeight = height;
    init.frameRateNum = framesPerSecond;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    init.encodeConfig = &config;
    init.maxEncodeWidth = width;
    init.maxEncodeHeight = height;
    init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    nvCheck(api.nvEncInitializeEncoder(encoder, &init), "NvEncInitializeEncoder");

    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.width = width;
    resource.height = height;
    resource.pitch = 0;
    resource.subResourceIndex = 0;
    resource.resourceToRegister = input.Get();
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; // Bytes B, G, R, A: DXGI_FORMAT_B8G8R8A8_UNORM.
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    nvCheck(api.nvEncRegisterResource(encoder, &resource), "NvEncRegisterResource");
    registered = resource.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};
    buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    nvCheck(api.nvEncCreateBitstreamBuffer(encoder, &buffer), "NvEncCreateBitstreamBuffer");
    bitstream = buffer.bitstreamBuffer;

    std::array<std::uint8_t, 1024> header{};
    std::uint32_t headerSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<std::uint32_t>(header.size());
    payload.spsppsBuffer = header.data();
    payload.outSPSPPSPayloadSize = &headerSize;
    nvCheck(api.nvEncGetSequenceParams(encoder, &payload), "NvEncGetSequenceParams");
    sequenceHeader.assign(header.begin(), header.begin() + std::min<std::size_t>(headerSize, header.size()));
}

void ClipRecorder::Impl::closeEncoder() {
    if (encoder == nullptr) {
        return;
    }
    NV_ENC_PIC_PARAMS end{};
    end.version = NV_ENC_PIC_PARAMS_VER;
    end.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    api.nvEncEncodePicture(encoder, &end);
    if (bitstream != nullptr) {
        api.nvEncDestroyBitstreamBuffer(encoder, bitstream);
    }
    if (registered != nullptr) {
        api.nvEncUnregisterResource(encoder, registered);
    }
    api.nvEncDestroyEncoder(encoder);
    encoder = nullptr;
}

float ClipRecorder::Impl::encode(std::int64_t qpc, std::uint64_t image, bool key) {
    const std::int64_t start = qpcNow();
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered;
    nvCheck(api.nvEncMapInputResource(encoder, &map), "NvEncMapInputResource");
    struct Unmap {
        Impl& impl;
        NV_ENC_INPUT_PTR mapped;
        ~Unmap() { impl.api.nvEncUnmapInputResource(impl.encoder, mapped); }
    } unmap{*this, map.mappedResource};

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = width;
    picture.inputHeight = height;
    picture.inputPitch = width;
    picture.inputBuffer = map.mappedResource;
    picture.bufferFmt = map.mappedBufferFmt;
    picture.outputBitstream = bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = static_cast<std::uint64_t>(qpc);
    picture.encodePicFlags = key ? NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS : 0;
    nvCheck(api.nvEncEncodePicture(encoder, &picture), "NvEncEncodePicture");

    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstream;
    nvCheck(api.nvEncLockBitstream(encoder, &lock), "NvEncLockBitstream");
    auto packet = std::make_shared<Packet>();
    packet->qpc = qpc;
    packet->image = image;
    packet->key = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
    const auto* bytes = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
    packet->bytes.assign(bytes, bytes + lock.bitstreamSizeInBytes);
    api.nvEncUnlockBitstream(encoder, bitstream);

    // The video starts at a key frame; a first frame that is not one (never seen) is left out.
    if (packet->key || !packets.empty()) {
        packetBytes += packet->bytes.size();
        packets.push_back(std::move(packet));
        holding.store(true, std::memory_order_release);
    }
    return static_cast<float>(msSince(qpcNow(), start, frequency));
}

void ClipRecorder::Impl::trim(std::int64_t cutoffQpc) {
    for (;;) {
        std::size_t next = 1; // The next key frame.
        while (next < packets.size() && !packets[next]->key) {
            ++next;
        }
        if (next >= packets.size() || packets[next]->qpc > cutoffQpc) {
            break;
        }
        for (std::size_t i = 0; i < next; ++i) {
            packetBytes -= packets.front()->bytes.size();
            packets.pop_front();
        }
    }
    const std::int64_t first = packets.empty() ? cutoffQpc : packets.front()->qpc;
    while (!looks.empty() && looks.front().qpc < first) {
        looks.pop_front();
    }
    while (!notes.empty() && notes.front().qpc < first) {
        notes.pop_front();
    }
}

void ClipRecorder::Impl::takeLooks() {
    const std::uint64_t head = queueHead.load(std::memory_order_acquire);
    std::uint64_t tail = queueTail.load(std::memory_order_relaxed);
    for (; tail != head; ++tail) {
        const QueuedLook& queued = queue[static_cast<std::size_t>(tail % kQueueLooks)];
        Look look;
        look.stamp = queued.stamp;
        look.doneQpc = queued.doneQpc;
        look.qpc = queued.stamp.shownQpc != 0 ? queued.stamp.shownQpc : queued.doneQpc;
        look.found.assign(queued.things.begin(), queued.things.begin() + queued.found);
        look.followed.assign(queued.shown.begin(), queued.shown.begin() + queued.followed);
        // Looks come in image order, so in time order; keep it so (the replay needs it).
        if (!looks.empty() && look.qpc < looks.back().qpc) {
            look.qpc = looks.back().qpc;
        }
        looks.push_back(std::move(look));
    }
    queueTail.store(tail, std::memory_order_release);
}

void ClipRecorder::Impl::takeNotes() {
    std::scoped_lock lock(mutex);
    for (Note& note : newNotes) {
        notes.push_back(std::move(note));
    }
    newNotes.clear();
}

SaveJob ClipRecorder::Impl::takeSave() {
    SaveJob job;
    {
        std::scoped_lock lock(mutex);
        job = std::move(asked);
        asked = {};
        saveAsked = false;
    }
    trim(job.savedQpc - static_cast<std::int64_t>(kClipSeconds * static_cast<double>(frequency)));
    job.frequency = frequency;
    job.width = width;
    job.height = height;
    job.framesPerSecond = framesPerSecond;
    job.sequenceHeader = sequenceHeader;
    for (const auto& packet : packets) {
        if (packet->qpc <= job.savedQpc || job.packets.empty()) {
            job.packets.push_back(packet);
        }
    }
    for (const Look& look : looks) {
        if (look.qpc <= job.savedQpc) {
            job.looks.push_back(look);
        }
    }
    for (const Note& note : notes) {
        if (note.qpc <= job.savedQpc) {
            job.notes.push_back(note);
        }
    }

    // The picture: the newest image, through a texture the CPU can read.
    if (staging == nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        input->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &staging))) {
            staging = nullptr;
        }
    }
    if (staging != nullptr) {
        gpu.context->CopyResource(staging.Get(), input.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const std::size_t row = static_cast<std::size_t>(width) * 4;
            job.picture.resize(row * height);
            for (unsigned y = 0; y < height; ++y) {
                std::memcpy(job.picture.data() + row * y,
                            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * y,
                            row);
            }
            gpu.context->Unmap(staging.Get(), 0);
        }
    }
    return job;
}

ClipRecorder::ClipRecorder(const Screen& screen, ImageChannel& channel) : impl_(std::make_unique<Impl>()) {
    Impl& impl = *impl_;
    impl.width = channel.width();
    impl.height = channel.height();
    // A device of its own, so encoding never holds up the pipeline's or the UI's (see findOnOwnAdapter).
    const ComPtr<IDXGIAdapter1> adapter = findOwnAdapter(screen.adapter.Get());
    impl.gpu = createGpuDevice(adapter.Get());
    impl.reader = std::make_unique<ImageReader>(impl.gpu.device.Get(), channel);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = impl.width;
    desc.Height = impl.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ScreenCapture::kImageFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET; // The video encoder takes render targets.
    check(impl.gpu.device->CreateTexture2D(&desc, nullptr, &impl.input), "CreateTexture2D (clip input)");

    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

ClipRecorder::~ClipRecorder() {
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    // A save still being written finishes first: it writes into the members below.
    if (impl_->writer.joinable()) {
        impl_->writer.join();
    }
}

void ClipRecorder::looked(const ImageStamp& stamp, std::int64_t doneQpc, std::span<const Detection> found,
                          std::span<const Tracked> followed) {
    if (state() == State::Failed) {
        return;
    }
    Impl& impl = *impl_;
    const std::uint64_t head = impl.queueHead.load(std::memory_order_relaxed);
    if (head - impl.queueTail.load(std::memory_order_acquire) >= kQueueLooks) {
        impl.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    QueuedLook& slot = impl.queue[static_cast<std::size_t>(head % kQueueLooks)];
    slot.stamp = stamp;
    slot.doneQpc = doneQpc;
    slot.found = static_cast<std::uint32_t>(std::min(found.size(), slot.things.size()));
    slot.followed = static_cast<std::uint32_t>(std::min(followed.size(), slot.shown.size()));
    std::copy_n(found.begin(), slot.found, slot.things.begin());
    std::copy_n(followed.begin(), slot.followed, slot.shown.begin());
    impl.queueHead.store(head + 1, std::memory_order_release);
}

void ClipRecorder::note(std::string_view text) {
    std::scoped_lock lock(impl_->mutex);
    impl_->newNotes.push_back({qpcNow(), std::string(text)});
}

bool ClipRecorder::save(std::filesystem::path folder, ClipAbout about) {
    if (state() != State::Running || !impl_->holding.load(std::memory_order_acquire) ||
        saveState() == SaveState::Saving) {
        return false;
    }
    savedFolder_ = folder;
    // Saving before the clip thread can see the request, so its Saved or Failed always comes after.
    saveState_.store(SaveState::Saving, std::memory_order_release);
    std::scoped_lock lock(impl_->mutex);
    impl_->asked = {};
    impl_->asked.folder = std::move(folder);
    impl_->asked.about = std::move(about);
    impl_->asked.savedQpc = qpcNow();
    impl_->saveAsked = true;
    return true;
}

const ClipRecorderStats& ClipRecorder::stats() {
    stats_.fetch();
    return stats_.readSlot();
}

void ClipRecorder::run(std::stop_token stop) {
    Impl& impl = *impl_;
    try {
        impl.openEncoder();
    } catch (const std::exception& error) {
        impl.closeEncoder();
        fail(error.what());
        return;
    }
    state_.store(State::Running, std::memory_order_release);

    const auto keyEvery = static_cast<std::int64_t>(kKeyEvery * static_cast<double>(impl.frequency));
    // Rounded up, so the video's times (in its own units) never come closer either.
    const std::int64_t minGap = (impl.frequency + impl.framesPerSecond - 1) / impl.framesPerSecond;
    const auto keep = static_cast<std::int64_t>(kClipSeconds * static_cast<double>(impl.frequency));
    bool waiting = false;        // The channel may hold an image not taken yet.
    bool encoded = false;        // At least one frame so far:
    std::uint64_t lastImage = 0; // the last one's image,
    std::int64_t lastQpc = 0;    // its time,
    std::int64_t lastKeyQpc = 0; // and the last key frame's.
    unsigned windowFrames = 0;
    float windowSumMs = 0.0f;
    float windowMaxMs = 0.0f;
    std::int64_t windowStart = qpcNow();

    try {
        while (!stop.stop_requested()) {
            // Wait out the gap after the last frame (an image coming meanwhile waits in the channel, newer ones
            // replacing it), or, with no image waiting, for one.
            std::int64_t now = qpcNow();
            const std::int64_t gapLeft = encoded ? lastQpc + minGap - now : 0;
            if (gapLeft > 0 || !waiting) {
                const std::int64_t gapMs = (gapLeft * 1000 + impl.frequency - 1) / impl.frequency; // Rounded up.
                const auto waitMs =
                    gapLeft > 0 ? static_cast<unsigned>(std::min<std::int64_t>(kWaitMs, gapMs)) : kWaitMs;
                waiting = impl.reader->waitForImage(waitMs) || waiting;
                now = qpcNow();
            }

            float encodeMs = -1.0f;
            bool took = false;
            if (waiting && (!encoded || now - lastQpc >= minGap)) {
                waiting = false;
                ImageStamp stamp;
                if (impl.reader->copyNewImage(impl.gpu.context.Get(), impl.input.Get(), &stamp)) {
                    took = true;
                    const std::int64_t shown = stamp.shownQpc != 0 ? stamp.shownQpc : now;
                    const std::int64_t qpc = encoded ? std::max(shown, lastQpc + minGap) : shown;
                    const bool key = !encoded || qpc - lastKeyQpc >= keyEvery;
                    encodeMs = impl.encode(qpc, stamp.number, key);
                    encoded = true;
                    lastImage = stamp.number;
                    lastQpc = qpc;
                    lastKeyQpc = key ? qpc : lastKeyQpc;
                }
            }
            if (!took && encoded && now - lastQpc >= keyEvery) {
                // The screen has not changed for a while: the same image again, as a key frame.
                encodeMs = impl.encode(now, lastImage, true);
                lastQpc = now;
                lastKeyQpc = now;
            }
            if (encodeMs >= 0.0f) {
                ++windowFrames;
                windowSumMs += encodeMs;
                windowMaxMs = std::max(windowMaxMs, encodeMs);
                impl.trim(now - keep);
            }

            impl.takeLooks();
            impl.takeNotes();
            bool saveAsked = false;
            {
                std::scoped_lock lock(impl.mutex);
                saveAsked = impl.saveAsked;
            }
            if (saveAsked) {
                SaveJob job = impl.takeSave();
                if (impl.writer.joinable()) {
                    impl.writer.join();
                }
                impl.writer = std::thread([this, job = std::move(job)] {
                    try {
                        savedInfo_ = writeClip(job);
                        saveState_.store(SaveState::Saved, std::memory_order_release);
                    } catch (const std::exception& error) {
                        saveError_ = error.what();
                        saveState_.store(SaveState::Failed, std::memory_order_release);
                    }
                });
            }

            if (now - windowStart >= impl.frequency) {
                const double seconds = static_cast<double>(now - windowStart) / static_cast<double>(impl.frequency);
                ClipRecorderStats& stats = stats_.writeSlot();
                stats.framesPerSecond = static_cast<float>(windowFrames / seconds);
                stats.encodeAvgMs = windowFrames > 0 ? windowSumMs / static_cast<float>(windowFrames) : 0.0f;
                stats.encodeMaxMs = windowMaxMs;
                stats.seconds =
                    impl.packets.empty() ? 0.0 : msSince(now, impl.packets.front()->qpc, impl.frequency) / 1000.0;
                stats.bytes = impl.packetBytes;
                stats.dropped = impl.dropped.load(std::memory_order_relaxed);
                stats_.publish();
                windowFrames = 0;
                windowSumMs = 0.0f;
                windowMaxMs = 0.0f;
                windowStart = now;
            }
        }
    } catch (const std::exception& error) {
        fail(error.what());
        // A save asked for but not taken yet can no longer happen.
        std::scoped_lock lock(impl.mutex);
        if (impl.saveAsked) {
            impl.saveAsked = false;
            saveError_ = "the clip buffer stopped: " + error_;
            saveState_.store(SaveState::Failed, std::memory_order_release);
        }
    }
    impl.closeEncoder();
}

void ClipRecorder::fail(std::string message) {
    error_ = std::move(message);
    state_.store(State::Failed, std::memory_order_release);
}

} // namespace observex
