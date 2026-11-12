#include "core/ClipGuesses.h"

#include "core/ClipLines.h"
#include "core/Heuristics.h"
#include "core/LiveLines.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace observex {

namespace {

// As many things as the live view shows at most (engine/Recognizer's Found::kMax).
constexpr int kMaxShown = 100;

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '.')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

bool readKindNames(std::string_view line, std::vector<std::string>& kinds) {
    constexpr std::string_view kStart = "# Kinds:";
    if (!line.starts_with(kStart)) {
        return false;
    }
    kinds.clear();
    std::string_view rest = line.substr(kStart.size());
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view item = trimmed(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        // "3 own": its number, then its id.
        unsigned number = 0;
        const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), number);
        const std::string_view name = trimmed(item.substr(static_cast<std::size_t>(end - item.data())));
        if (error != std::errc() || name.empty() || number >= kMaxKinds) {
            continue;
        }
        if (kinds.size() <= number) {
            kinds.resize(number + 1);
        }
        kinds[number] = std::string(name);
    }
    return true;
}

ClipText readClipText(std::string_view text) {
    ClipText clip;
    std::vector<Tracked> shown;
    std::vector<SnapFind> finds;
    std::string note;
    ClipGroundLine ground;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }

        unsigned frame = 0;
        unsigned long long image = 0;
        double timeMs = 0.0;
        if (line.front() == '#') {
            readKindNames(line, clip.kinds);
        } else if (readFrame(line, frame, image, timeMs)) {
            if (frame >= 1 && frame <= 1'000'000) {
                if (clip.frameMs.size() < frame) {
                    clip.frameMs.resize(frame, timeMs);
                    clip.frameImage.resize(frame, image);
                }
                clip.frameMs[frame - 1] = timeMs;
                clip.frameImage[frame - 1] = image;
            }
        } else if (ClipLook look; readLook(line, look.image, look.timeMs, look.found)) {
            look.looked = true;
            clip.looks.push_back(std::move(look));
        } else if (line.starts_with("kinds ")) {
            if (!clip.looks.empty()) {
                readKinds(line, clip.looks.back().image, clip.looks.back().found);
            }
        } else if (readShown(line, image, timeMs, shown)) {
            // Right after its look; else a look of its own with nothing found.
            if (clip.looks.empty() || clip.looks.back().image != image || clip.looks.back().showed) {
                ClipLook alone;
                alone.image = image;
                alone.timeMs = timeMs;
                clip.looks.push_back(std::move(alone));
            }
            clip.looks.back().showed = true;
            clip.looks.back().doneMs = timeMs;
            clip.looks.back().shown = shown;
        } else if (ClipSnap snap; readSnapped(line, snap.image, snap.timeMs, snap.doneMs, snap.leadMs, snap.things)) {
            clip.snaps.push_back(std::move(snap));
        } else if (unsigned long long lookImage = 0; readFinds(line, image, lookImage, finds)) {
            // Right after its snapped line.
            if (!clip.snaps.empty() && clip.snaps.back().image == image && !clip.snaps.back().hasFinds) {
                ClipSnap& last = clip.snaps.back();
                last.hasFinds = true;
                last.lookImage = lookImage;
                last.finds = finds;
            }
        } else if (readNote(line, timeMs, note)) {
            clip.notes.push_back({timeMs, note});
        } else if (readGround(line, ground.image, ground.timeMs, ground.followed, ground.camera)) {
            clip.grounds.push_back(ground);
        }
    }
    return clip;
}

GroundCamera recordedGround(std::span<const ClipGroundLine> lines, double timeMs) {
    const auto after = std::upper_bound(lines.begin(), lines.end(), timeMs,
                                        [](double ms, const ClipGroundLine& line) { return ms < line.timeMs; });
    if (after == lines.begin()) {
        return {};
    }
    return (after - 1)->camera;
}

std::vector<unsigned long long> followedImages(const ClipText& clip) {
    std::vector<unsigned long long> images;
    for (const ClipGroundLine& line : clip.grounds) {
        if (line.followed != 0 && (images.empty() || images.back() != line.followed)) {
            images.push_back(line.followed);
        }
    }
    std::sort(images.begin(), images.end());
    images.erase(std::unique(images.begin(), images.end()), images.end());
    return images;
}

void Guesses::add(double fromMs, std::span<const Tracked> things) {
    from_.push_back(from_.empty() ? fromMs : std::max(fromMs, from_.back()));
    first_.push_back(things_.size());
    things_.insert(things_.end(), things.begin(), things.end());
}

std::span<const Tracked> Guesses::at(double timeMs) const {
    const auto after = std::upper_bound(from_.begin(), from_.end(), timeMs);
    if (after == from_.begin()) {
        return {};
    }
    return set(static_cast<std::size_t>(after - from_.begin()) - 1);
}

std::span<const Tracked> Guesses::set(std::size_t set) const {
    const std::size_t end = set + 1 < first_.size() ? first_[set + 1] : things_.size();
    return {things_.data() + first_[set], end - first_[set]};
}

Guesses::Summary Guesses::summary(double fromMs) const {
    Summary summary;
    const auto start =
        static_cast<std::size_t>(std::lower_bound(from_.begin(), from_.end(), fromMs) - from_.begin());
    if (start >= from_.size()) {
        return summary;
    }
    std::vector<std::uint32_t> before;
    std::vector<std::uint32_t> now;
    std::size_t changes = 0;
    std::size_t things = 0;
    for (std::size_t set = start; set < from_.size(); ++set) {
        const std::size_t end = set + 1 < first_.size() ? first_[set + 1] : things_.size();
        now.clear();
        for (std::size_t i = first_[set]; i < end; ++i) {
            now.push_back(things_[i].id);
        }
        std::sort(now.begin(), now.end());
        things += now.size();
        if (set > start) {
            // Ids in one set and not the other: each came or went.
            std::size_t a = 0;
            std::size_t b = 0;
            while (a < before.size() || b < now.size()) {
                if (b == now.size() || (a < before.size() && before[a] < now[b])) {
                    ++changes;
                    ++a;
                } else if (a == before.size() || now[b] < before[a]) {
                    ++changes;
                    ++b;
                } else {
                    ++a;
                    ++b;
                }
            }
        }
        std::swap(before, now);
    }
    summary.things = static_cast<double>(things) / static_cast<double>(from_.size() - start);
    const double seconds = (from_.back() - from_[start]) / 1000.0;
    summary.changes = seconds > 0.0 ? static_cast<double>(changes) / seconds : 0.0;
    return summary;
}

namespace {

// How much two boxes overlap: their common area over the area both cover (0 to 1).
float overlap(const Detection& a, const Detection& b) {
    const float width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const float height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    if (width <= 0.0f || height <= 0.0f) {
        return 0.0f;
    }
    const float common = width * height;
    return common / (a.width * a.height + b.width * b.height - common);
}

// The tag the clip screen draws (app/Overlay): kind, "?" and percent.
bool sameTag(const Tracked& a, const Tracked& b, int unknownKind) {
    const auto percent = [](const Tracked& thing) { return std::lround(thing.sure * 100.0f); };
    return a.kind == b.kind && cantTellKind(a, unknownKind) == cantTellKind(b, unknownKind) && percent(a) == percent(b);
}

bool sameBox(const Tracked& a, const Tracked& b) {
    return std::lround(a.x) == std::lround(b.x) && std::lround(a.y) == std::lround(b.y) &&
           std::lround(a.width) == std::lround(b.width) && std::lround(a.height) == std::lround(b.height);
}

} // namespace

GuessesDifference compareGuesses(const Guesses& first, const Guesses& second, int unknownKind, double fromMs) {
    GuessesDifference difference;
    constexpr float kSameThing = 0.3f; // Overlapping this much at least: the same thing, paired.
    std::vector<bool> paired;
    for (std::size_t set = 0; set < first.sets(); ++set) {
        if (first.fromMs(set) < fromMs) {
            continue;
        }
        const std::span<const Tracked> a = first.set(set);
        const std::span<const Tracked> b = second.at(first.fromMs(set));
        ++difference.sets;
        bool same = a.size() == b.size();
        paired.assign(b.size(), false);
        for (const Tracked& thing : a) {
            // The other's box it overlaps most, of those not paired yet.
            std::size_t best = b.size();
            float bestOverlap = kSameThing;
            for (std::size_t i = 0; i < b.size(); ++i) {
                if (const float o = overlap(thing, b[i]); !paired[i] && o >= bestOverlap) {
                    best = i;
                    bestOverlap = o;
                }
            }
            if (best == b.size()) {
                ++difference.onlyFirst;
                same = false;
                continue;
            }
            paired[best] = true;
            ++difference.matched;
            if (!sameTag(thing, b[best], unknownKind)) {
                ++difference.tagDiffers;
                same = false;
            } else if (!sameBox(thing, b[best])) {
                ++difference.moved;
                same = false;
            }
        }
        const auto unpaired = static_cast<std::size_t>(std::count(paired.begin(), paired.end(), false));
        difference.onlySecond += unpaired;
        if (same && unpaired == 0) {
            ++difference.same;
            continue;
        }
        if (difference.firstDiffering.size() < GuessesDifference::kExamples) {
            difference.firstDiffering.push_back(first.fromMs(set));
        }
        difference.lastDiffering = first.fromMs(set);
    }
    return difference;
}

BoxPrecision boxPrecision(const ClipText& clip, const Guesses& guesses, double fromMs) {
    std::vector<BoxMoment> moments;
    for (std::size_t frame = 0; frame < clip.frameMs.size(); ++frame) {
        if (clip.frameMs[frame] >= fromMs) {
            moments.push_back({clip.frameMs[frame], clip.frameImage[frame]});
        }
    }
    return boxPrecision(clip, guesses, moments);
}

BoxPrecision boxPrecision(const ClipText& clip, const Guesses& guesses, std::span<const BoxMoment> moments) {
    // The looks by their image, to find each moment's.
    std::vector<const ClipLook*> looked;
    for (const ClipLook& look : clip.looks) {
        if (look.looked) {
            looked.push_back(&look);
        }
    }
    std::sort(looked.begin(), looked.end(), [](const ClipLook* a, const ClipLook* b) { return a->image < b->image; });

    struct Pair {
        float distance; // Between the middles, in the larger side.
        std::size_t box;
        std::size_t found;
    };
    BoxPrecision precision;
    std::vector<float> offAll;
    std::vector<float> offMoving;
    std::vector<Pair> pairs;
    std::vector<char> boxTaken;
    std::vector<char> foundTaken;
    for (const BoxMoment& moment : moments) {
        const double frameMs = moment.timeMs;
        const unsigned long long image = moment.image;
        const auto own = std::lower_bound(looked.begin(), looked.end(), image,
                                          [](const ClipLook* look, unsigned long long i) { return look->image < i; });
        if (own == looked.end() || (*own)->image != image) {
            continue;
        }
        ++precision.frames;
        const std::vector<Detection>& found = (*own)->found;
        const std::span<const Tracked> boxes = guesses.at(frameMs);
        pairs.clear();
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            const Tracked& box = boxes[b];
            for (std::size_t f = 0; f < found.size(); ++f) {
                const Detection& truth = found[f];
                const float area = box.width * box.height;
                const float truthArea = truth.width * truth.height;
                if (truth.sure < BoxPrecision::kTruthSure || std::min(area, truthArea) < 0.5f * std::max(area, truthArea)) {
                    continue;
                }
                const float side = std::max({box.width, box.height, truth.width, truth.height});
                const float distance = std::hypot(box.x + box.width * 0.5f - (truth.x + truth.width * 0.5f),
                                                  box.y + box.height * 0.5f - (truth.y + truth.height * 0.5f)) /
                                       side;
                if (distance < 2.0f) {
                    pairs.push_back({distance, b, f});
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            return a.distance != b.distance ? a.distance < b.distance : (a.box != b.box ? a.box < b.box : a.found < b.found);
        });
        boxTaken.assign(boxes.size(), 0);
        foundTaken.assign(found.size(), 0);
        for (const Pair& pair : pairs) {
            if (boxTaken[pair.box] != 0 || foundTaken[pair.found] != 0) {
                continue;
            }
            boxTaken[pair.box] = 1;
            foundTaken[pair.found] = 1;
            const Tracked& box = boxes[pair.box];
            const Detection& truth = found[pair.found];
            const float offX = box.x + box.width * 0.5f - (truth.x + truth.width * 0.5f);
            const float offY = box.y + box.height * 0.5f - (truth.y + truth.height * 0.5f);
            const float off = std::hypot(offX, offY);
            const bool inMiddle = std::abs(offX) <= truth.width * 0.25f && std::abs(offY) <= truth.height * 0.25f;
            const bool outside = std::abs(offX) > truth.width * 0.5f || std::abs(offY) > truth.height * 0.5f;
            const auto count = [&](BoxPrecision::Part& part, std::vector<float>& offs) {
                ++part.boxes;
                part.meanOff += off;
                part.inMiddle += inMiddle ? 1 : 0;
                part.outside += outside ? 1 : 0;
                offs.push_back(off);
            };
            count(precision.all, offAll);
            if (std::hypot(box.speedX, box.speedY) >= BoxPrecision::kMovingSpeed) {
                count(precision.moving, offMoving);
            }
        }
        precision.unpaired += static_cast<std::size_t>(std::count(boxTaken.begin(), boxTaken.end(), 0));
        for (std::size_t f = 0; f < found.size(); ++f) {
            if (found[f].sure >= BoxPrecision::kTruthSure) {
                ++precision.truths;
                precision.missed += foundTaken[f] == 0 ? 1 : 0;
            }
        }
    }
    const auto finish = [](BoxPrecision::Part& part, std::vector<float>& offs) {
        if (offs.empty()) {
            return;
        }
        part.meanOff /= static_cast<double>(part.boxes);
        const auto nine = offs.begin() + static_cast<std::ptrdiff_t>(offs.size() * 9 / 10);
        std::nth_element(offs.begin(), nine, offs.end());
        part.mostOff = *nine;
    };
    finish(precision.all, offAll);
    finish(precision.moving, offMoving);
    return precision;
}

double settledMs(const ClipText& clip) {
    const auto looked = std::find_if(clip.looks.begin(), clip.looks.end(), [](const ClipLook& look) {
        return look.looked;
    });
    return looked == clip.looks.end() ? 0.0 : std::max(0.0, looked->timeMs + kSettleMs);
}

Guesses recordedGuesses(const ClipText& clip) {
    Guesses guesses;
    const double snapsFrom = clip.snaps.empty() ? std::numeric_limits<double>::infinity() : clip.snaps.front().doneMs;
    for (const ClipLook& look : clip.looks) {
        if (look.showed && look.doneMs < snapsFrom) {
            guesses.add(look.doneMs, look.shown);
        }
    }
    for (const ClipSnap& snap : clip.snaps) {
        guesses.add(snap.doneMs, snap.things);
    }
    return guesses;
}

std::vector<SnapStep> snapSteps(const ClipText& clip) {
    // The looks in the order they were done (their shown lines), to tell which one the snapper took in by when.
    std::vector<const ClipLook*> done;
    for (const ClipLook& look : clip.looks) {
        if (look.looked && look.showed) {
            done.push_back(&look);
        }
    }
    std::size_t next = 0; // The first not taken in yet.
    const auto newestBy = [&](double ms) {
        unsigned long long newest = 0;
        for (; next < done.size() && done[next]->doneMs <= ms; ++next) {
            newest = done[next]->image;
        }
        return newest;
    };

    std::vector<SnapStep> steps;
    if (!clip.snaps.empty()) {
        steps.reserve(clip.snaps.size());
        for (const ClipSnap& snap : clip.snaps) {
            SnapStep step{snap.image, snap.timeMs, snap.doneMs};
            if (snap.hasFinds) {
                step.lookImage = snap.lookImage;
                step.finds = &snap.finds;
            } else {
                step.lookImage = newestBy(snap.timeMs);
            }
            steps.push_back(step);
        }
        return steps;
    }
    // Each frame's image: when the screen showed it as its look says, if it was looked at (a frame's time can be a
    // moment after).
    std::unordered_map<unsigned long long, double> shownMs;
    for (const ClipLook& look : clip.looks) {
        if (look.looked) {
            shownMs.emplace(look.image, look.timeMs);
        }
    }
    for (std::size_t frame = 0; frame < clip.frameMs.size(); ++frame) {
        const unsigned long long image = clip.frameImage[frame];
        if (frame > 0 && image == clip.frameImage[frame - 1]) {
            continue; // A repeat: the screen did not change.
        }
        const auto looked = shownMs.find(image);
        const double ms = looked != shownMs.end() ? looked->second : clip.frameMs[frame];
        SnapStep step{image, ms, ms + kReplayLateMs};
        step.lookImage = newestBy(ms);
        steps.push_back(step);
    }
    return steps;
}

Guesses snapGuesses(const ClipText& clip, const Guesses& today, Snapping& snapping, SnapImages& images,
                    std::stop_token stop, std::atomic<float>* progress) {
    const std::vector<SnapStep> steps = snapSteps(clip);
    // Today's set of each look, by its image: heuristicGuesses makes one for each look with its look line, in order.
    struct Taken {
        double lookMs;
        std::size_t set;
    };
    std::unordered_map<unsigned long long, Taken> looks;
    std::size_t set = 0;
    for (const ClipLook& look : clip.looks) {
        if (look.looked) {
            if (set < today.sets()) {
                looks.emplace(look.image, Taken{look.timeMs, set});
            }
            ++set;
        }
    }

    Guesses guesses;
    const double firstMs = steps.empty() ? std::numeric_limits<double>::infinity() : steps.front().doneMs;
    for (std::size_t s = 0; s < today.sets() && today.fromMs(s) < firstMs; ++s) {
        guesses.add(today.fromMs(s), today.set(s));
    }
    // As engine/BoxSnapper does on each image: the look taken in learned from its own image if still kept, only the
    // parts read made readable.
    snapping.clear();
    std::array<GreyArea, kMaxShown> learnAreas;
    std::array<GreyArea, kMaxShown> snapAreas;
    std::array<Tracked, kMaxShown> out;
    for (std::size_t i = 0; i < steps.size() && !stop.stop_requested(); ++i) {
        const SnapStep& step = steps[i];
        images.make(step.image);
        const auto look = step.lookImage != 0 ? looks.find(step.lookImage) : looks.end();
        const bool taken = look != looks.end();
        const double lookMs = taken ? look->second.lookMs : 0.0;
        const std::span<const Tracked> shown = taken ? today.set(look->second.set) : std::span<const Tracked>();
        const int learnCount = taken ? snapping.learnAreas(shown, learnAreas.data(), kMaxShown) : 0;
        const int snapCount = snapping.snapAreas(step.timeMs, taken, lookMs, shown, snapAreas.data(), kMaxShown);
        if (taken) {
            images.read(step.lookImage, {learnAreas.data(), static_cast<std::size_t>(std::max(learnCount, 0))});
        }
        images.read(step.image, {snapAreas.data(), static_cast<std::size_t>(std::max(snapCount, 0))});
        images.wait();
        if (taken) {
            snapping.looked(lookMs, images.find(step.lookImage), shown);
        }
        snapping.replay(step.finds != nullptr ? std::span<const SnapFind>(*step.finds) : std::span<const SnapFind>());
        const GreyView* image = images.find(step.image);
        const int count = image != nullptr ? snapping.snap(step.timeMs, *image, out.data(), kMaxShown) : 0;
        guesses.add(step.doneMs, {out.data(), static_cast<std::size_t>(std::max(count, 0))});
        if (progress != nullptr) {
            progress->store(static_cast<float>(i + 1) / static_cast<float>(steps.size()), std::memory_order_relaxed);
        }
    }
    return guesses;
}

Guesses heuristicGuesses(const ClipText& clip, Heuristics& heuristics) {
    Guesses guesses;
    heuristics.clear();
    std::array<Tracked, kMaxShown> shown;
    for (const ClipLook& look : clip.looks) {
        if (!look.looked) {
            continue;
        }
        const int count = heuristics.update(look.timeMs, look.found, shown.data(), kMaxShown);
        guesses.add(look.showed ? look.doneMs : look.timeMs,
                    {shown.data(), static_cast<std::size_t>(count)});
    }
    return guesses;
}

} // namespace observex
