#include "core/ClipGuesses.h"
#include "core/ClipLines.h"
#include "heuristics/Rules.h"
#include "heuristics/SnapRules.h"
#include "core/LiveLines.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

using namespace observex;

namespace {

Detection character(float x, float y, float sure, std::uint8_t kind = 0) {
    Detection thing;
    thing.x = x;
    thing.y = y;
    thing.width = 40;
    thing.height = 80;
    thing.sure = sure;
    thing.kind = kind;
    thing.kindSure = sure;
    thing.secondKind = kind == 0 ? 1 : 0;
    thing.second = 0.05f;
    return thing;
}

Tracked shownThing(std::uint32_t id, float x) {
    Tracked thing;
    thing.id = id;
    thing.x = x;
    thing.width = 10;
    thing.height = 10;
    thing.sure = 0.9f;
    return thing;
}

} // namespace

TEST_CASE("the kinds header reads back by number") {
    std::vector<std::string> kinds;
    REQUIRE(readKindNames("# Kinds: 0 monster, 1 player, 2 npc, 3 own, 4 unknown, 5 dead.", kinds));
    REQUIRE(kinds.size() == 6);
    CHECK(kinds[0] == "monster");
    CHECK(kinds[3] == "own");
    CHECK(kinds[5] == "dead");
    CHECK_FALSE(readKindNames("# The model has no kinds.", kinds));
    CHECK_FALSE(readKindNames("frame 1 2 0.0", kinds));
}

TEST_CASE("a clip's text reads back with its frames, looks, kinds, what was shown and notes") {
    std::string text = "# observex clip\n# Kinds: 0 monster, 1 player.\n";
    appendFrame(text, 1, 70, 0.0);
    char look[kLookChars];
    const std::vector<Detection> found{character(100, 200, 0.9f, 1), character(500, 200, 0.2f)};
    text.append(look, writeLook(look, 71, 4.2, found));
    appendShown(text, 71, 11.5, std::vector<Tracked>{shownThing(6, 100)});
    appendFrame(text, 2, 72, 6.9);
    appendNote(text, 9.0, "save last 10 s (the button)");

    const ClipText clip = readClipText(text);
    REQUIRE(clip.kinds.size() == 2);
    CHECK(clip.kinds[1] == "player");
    REQUIRE(clip.frameMs.size() == 2);
    CHECK(clip.frameMs[1] == doctest::Approx(6.9));
    REQUIRE(clip.frameImage.size() == 2);
    CHECK(clip.frameImage[1] == 72);
    REQUIRE(clip.looks.size() == 1);
    const ClipLook& first = clip.looks[0];
    CHECK(first.image == 71);
    CHECK(first.looked);
    CHECK(first.timeMs == doctest::Approx(4.2));
    REQUIRE(first.found.size() == 2);
    CHECK(first.found[0].kind == 1);
    CHECK(first.found[1].sure == doctest::Approx(0.2f));
    CHECK(first.showed);
    CHECK(first.doneMs == doctest::Approx(11.5));
    REQUIRE(first.shown.size() == 1);
    CHECK(first.shown[0].id == 6);
    REQUIRE(clip.notes.size() == 1);
    CHECK(clip.notes[0].text == "save last 10 s (the button)");
}

TEST_CASE("a guesser shows each set from its time until the next") {
    Guesses guesses;
    guesses.add(10.0, std::vector<Tracked>{shownThing(1, 0), shownThing(2, 50)});
    guesses.add(20.0, std::vector<Tracked>{shownThing(2, 52)});
    guesses.add(30.0, std::vector<Tracked>{shownThing(2, 54), shownThing(3, 90)});
    CHECK(guesses.at(5.0).empty());
    CHECK(guesses.at(10.0).size() == 2);
    CHECK(guesses.at(19.9).size() == 2);
    REQUIRE(guesses.at(25.0).size() == 1);
    CHECK(guesses.at(25.0)[0].x == 52.0f);
    CHECK(guesses.at(1000.0).size() == 2);

    // 1 went, then 3 came: 2 changes in 20 ms.
    const Guesses::Summary summary = guesses.summary(0.0);
    CHECK(summary.things == doctest::Approx(5.0 / 3.0));
    CHECK(summary.changes == doctest::Approx(100.0));
}

TEST_CASE("today's heuristics on a clip's recorded looks show what the live view showed") {
    // As live: each look through the heuristics, written down the way the clip buffer does.
    Rules live;
    std::array<Tracked, 100> shown;
    std::string text = "# Kinds: 0 monster, 1 player.\n";
    char look[kLookChars];
    for (int i = 0; i < 20; ++i) {
        const double timeMs = 8.0 * i;
        std::vector<Detection> found{character(100.0f + 2.0f * static_cast<float>(i), 300, 0.9f)};
        if (i >= 5 && i <= 9) {
            found.push_back(character(900, 300, 0.8f, 1)); // Comes for a while.
        }
        found.push_back(character(1500, 300, 0.5f)); // Never sure enough to start being followed.
        found.push_back(character(1800, 300, 0.2f)); // Under what is followed at all.
        const auto image = static_cast<unsigned long long>(100 + i);
        appendFrame(text, static_cast<unsigned>(i + 1), image, timeMs);
        text.append(look, writeLook(look, image, timeMs, found, true));
        const int count = live.update(timeMs, found, shown.data(), static_cast<int>(shown.size()));
        appendShown(text, image, timeMs + 3.5, {shown.data(), static_cast<std::size_t>(count)});
    }

    const ClipText clip = readClipText(text);
    const Guesses recorded = recordedGuesses(clip);
    Rules rules;
    const Guesses today = heuristicGuesses(clip, rules);
    REQUIRE(recorded.sets() == 20);
    REQUIRE(today.sets() == 20);
    for (int i = 0; i < 20; ++i) {
        const double at = 8.0 * i + 3.5;
        const std::span<const Tracked> was = recorded.at(at);
        const std::span<const Tracked> now = today.at(at);
        REQUIRE(was.size() == now.size());
        for (std::size_t t = 0; t < was.size(); ++t) {
            CHECK(now[t].id == was[t].id);
            CHECK(now[t].x == doctest::Approx(was[t].x).epsilon(0.001));
            CHECK(now[t].kind == was[t].kind);
            CHECK(now[t].sure == doctest::Approx(was[t].sure).epsilon(0.01));
            CHECK(now[t].held == was[t].held);
        }
    }
    CHECK(today.at(8.0 * 2 + 3.5).size() == 1);  // The first thing shows from its third look on,
    CHECK(today.at(8.0 * 8 + 3.5).size() == 2);  // the second while it comes,
    CHECK(today.at(8.0 * 19 + 3.5).size() == 1); // and is gone once it was held long enough.

    CHECK(settledMs(clip) == doctest::Approx(kSettleMs)); // No looks before its first frame.
    const GuessesDifference difference = compareGuesses(recorded, today, -1, 0.0);
    CHECK(difference.sets == 20);
    CHECK(difference.same == 20);
    CHECK(difference.firstDiffering.empty());
}

TEST_CASE("comparing guessers finds boxes only one draws, tags that differ and boxes that moved") {
    Guesses first;
    Guesses second;
    first.add(0.0, std::vector<Tracked>{shownThing(1, 0)});
    second.add(0.0, std::vector<Tracked>{shownThing(7, 0)}); // Another id only: drawn the same.
    first.add(10.0, std::vector<Tracked>{shownThing(1, 0), shownThing(2, 100)});
    Tracked unsure = shownThing(8, 0);
    unsure.sure = 0.5f; // "50%" against "90%".
    second.add(10.0, std::vector<Tracked>{unsure, shownThing(9, 300)});
    first.add(20.0, std::vector<Tracked>{shownThing(1, 0)});
    second.add(20.0, std::vector<Tracked>{shownThing(7, 2)});

    const GuessesDifference difference = compareGuesses(first, second, -1, 0.0);
    CHECK(difference.sets == 3);
    CHECK(difference.same == 1);
    CHECK(difference.matched == 3);
    CHECK(difference.onlyFirst == 1);  // 2 at 100,
    CHECK(difference.onlySecond == 1); // 9 at 300,
    CHECK(difference.tagDiffers == 1); // the 50%,
    CHECK(difference.moved == 1);      // and 2 pixels.
    REQUIRE(difference.firstDiffering.size() == 2);
    CHECK(difference.firstDiffering[0] == 10.0);
    CHECK(compareGuesses(first, second, -1, 15.0).sets == 1); // From 15 ms on.
}

TEST_CASE("box precision: a guesser's boxes against what the model found on each frame's own image") {
    const auto drawn = [](float x, float speedX) {
        Tracked thing;
        static_cast<Detection&>(thing) = character(x, 200, 0.9f);
        thing.speedX = speedX;
        return thing;
    };
    std::string text;
    char look[kLookChars];
    // Frame 1, image 70: the model found a 40 x 80 character at 100, 200 (and one too unsure to count at 500).
    appendFrame(text, 1, 70, 0.0);
    text.append(look, writeLook(look, 70, 0.0, std::vector<Detection>{character(100, 200, 0.9f),
                                                                      character(500, 200, 0.3f)}, true));
    appendFrame(text, 2, 71, 9.0); // Not looked at: not counted.
    // Frame 3, image 72, written a moment after its image was shown (engine/ClipRecorder): paired by image.
    appendFrame(text, 3, 72, 20.0);
    text.append(look, writeLook(look, 72, 19.0, std::vector<Detection>{character(130, 200, 0.9f)}, true));
    const ClipText clip = readClipText(text);

    Guesses guesses;
    guesses.add(0.0, std::vector<Tracked>{drawn(106, 0.0f), drawn(900, 0.0f)}); // 6 off; on nothing.
    guesses.add(15.0, std::vector<Tracked>{drawn(160, 2.0f)});                  // 30 off, moving.
    const BoxPrecision precision = boxPrecision(clip, guesses, 0.0);
    CHECK(precision.frames == 2);
    CHECK(precision.unpaired == 1);
    CHECK(precision.all.boxes == 2);
    CHECK(precision.all.meanOff == doctest::Approx(18.0));
    CHECK(precision.all.mostOff == doctest::Approx(30.0));
    CHECK(precision.all.inMiddle == 1); // 6 off: within a quarter of 40.
    CHECK(precision.all.outside == 1);  // 30 off: more than half of 40.
    CHECK(precision.moving.boxes == 1);
    CHECK(precision.moving.outside == 1);
    CHECK(boxPrecision(clip, guesses, 10.0).frames == 1); // From 10 ms on.
}

TEST_CASE("looks before the first frame warm the replay up and show at the first frame") {
    std::string text;
    char look[kLookChars];
    const std::vector<Detection> found{character(100, 200, 0.9f)};
    text.append(look, writeLook(look, 70, -4000.0, found, true));
    appendShown(text, 70, -3990.5, std::vector<Tracked>{shownThing(6, 100)});
    appendFrame(text, 1, 71, 0.0);
    const ClipText clip = readClipText(text);
    REQUIRE(clip.looks.size() == 1);
    CHECK(clip.looks[0].showed);
    CHECK(clip.looks[0].doneMs == doctest::Approx(-3990.5));
    CHECK(recordedGuesses(clip).at(0.0).size() == 1); // What the app showed then, at the first frame.
    Rules rules;
    CHECK(heuristicGuesses(clip, rules).sets() == 1);
    CHECK(settledMs(clip) == 0.0); // Compared from the first frame on.
}

TEST_CASE("a shown line without its look is shown but not replayed") {
    std::string text;
    appendShown(text, 5, 12.0, std::vector<Tracked>{shownThing(1, 0)});
    const ClipText clip = readClipText(text);
    REQUIRE(clip.looks.size() == 1);
    CHECK_FALSE(clip.looks[0].looked);
    CHECK(recordedGuesses(clip).sets() == 1);
    Rules rules;
    CHECK(heuristicGuesses(clip, rules).sets() == 0);
}

TEST_CASE("from the first snapped line on, Recorded shows the snapped boxes") {
    std::string text;
    char look[kLookChars];
    const std::vector<Detection> found{character(100, 200, 0.9f)};
    appendFrame(text, 1, 10, 0.0);
    text.append(look, writeLook(look, 10, 0.0, found, true));
    appendSnapped(text, 11, 4.2, 6.5, 6.0, std::vector<Tracked>{shownThing(6, 130)});
    appendShown(text, 10, 9.5, std::vector<Tracked>{shownThing(6, 100)}); // The look, done after the snap.
    appendSnapped(text, 12, 8.4, 10.75, 6.0, std::vector<Tracked>{shownThing(6, 140)});
    const ClipText clip = readClipText(text);
    REQUIRE(clip.snaps.size() == 2);
    CHECK(clip.snaps[1].image == 12);
    CHECK(clip.snaps[1].timeMs == 8.4);
    CHECK(clip.snaps[1].doneMs == 10.75);
    CHECK(clip.snaps[1].leadMs == 6.0);
    REQUIRE(clip.looks.size() == 1);
    CHECK(clip.looks[0].showed); // Its shown line still belongs to it.
    const Guesses recorded = recordedGuesses(clip);
    CHECK(recorded.sets() == 2); // The app showed the snapped boxes, not the look's.
    CHECK(recorded.at(6.0).empty());
    REQUIRE(recorded.at(7.0).size() == 1);
    CHECK(recorded.at(7.0)[0].x == 130.0f);
    CHECK(recorded.at(11.0)[0].x == 140.0f);
}

namespace {

constexpr int kSceneWidth = 320;
constexpr int kSceneHeight = 200;

// Grass (noise), and a striped thing 30 x 28 at `thingX`, 100 (none if `thingX` is negative).
GreyImage grassScene(int thingX) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSceneWidth * kSceneHeight));
    for (int y = 0; y < kSceneHeight; ++y) {
        for (int x = 0; x < kSceneWidth; ++x) {
            const std::uint32_t hash =
                (static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u) * 2654435761u;
            pixels[static_cast<std::size_t>(y * kSceneWidth + x)] = static_cast<std::uint8_t>(70 + (hash >> 24) % 60);
        }
    }
    for (int y = 0; y < 28 && thingX >= 0; ++y) {
        for (int x = 0; x < 30; ++x) {
            pixels[static_cast<std::size_t>((100 + y) * kSceneWidth + thingX + x)] =
                static_cast<std::uint8_t>((x / 4 + y / 5) % 2 == 0 ? 20 + 4 * y : 230 - 3 * x);
        }
    }
    GreyImage image;
    image.fromGrey(pixels.data(), kSceneWidth, kSceneHeight, kSceneWidth);
    return image;
}

// A replay's images, all grass with nothing on it: kept as engine/GreyMaker keeps them.
class GrassImages final : public SnapImages {
public:
    void make(unsigned long long image) override {
        kept_.push_back({image, grassScene(-1)});
        if (kept_.size() > static_cast<std::size_t>(kKept)) {
            kept_.pop_front();
        }
        views_.clear();
        for (const Kept& kept : kept_) {
            views_.push_back(kept.grey.view(kept.image));
        }
    }
    void read(unsigned long long, std::span<const GreyArea>) override {}
    void wait() override {}
    const GreyView* find(unsigned long long image) const override {
        for (const GreyView& view : views_) {
            if (view.image == image) {
                return &view;
            }
        }
        return nullptr;
    }

private:
    struct Kept {
        unsigned long long image;
        GreyImage grey;
    };
    std::deque<Kept> kept_;
    std::vector<GreyView> views_;
};

} // namespace

TEST_CASE("a new clip's Today snapped on other pixels is exactly what was shown live, by the searches recorded") {
    // Live: a thing moving 1 pixel a millisecond, an image every 5 ms, a look at every other one done 8 ms after it,
    // the snapper on every image taking in the newest look done by then, all written down as the clip buffer does.
    Rules tracker;
    SnapRules snapper;
    std::string text;
    char lookLine[kExactLookChars];
    struct Look {
        unsigned long long image;
        double timeMs;
        double doneMs;
        std::vector<Tracked> shown;
    };
    std::vector<Look> looks;
    std::size_t nextLook = 0;
    std::vector<GreyImage> images(41);
    std::array<Tracked, 100> out;
    for (int i = 1; i <= 40; ++i) {
        const double timeMs = 5.0 * i;
        const auto image = static_cast<unsigned long long>(i);
        const int thingX = 20 + static_cast<int>(timeMs);
        images[static_cast<std::size_t>(i)] = grassScene(thingX);
        appendFrame(text, static_cast<unsigned>(i), image, timeMs);
        if (i % 2 == 1) {
            Detection found;
            found.x = static_cast<float>(thingX);
            found.y = 100.0f;
            found.width = 30.0f;
            found.height = 28.0f;
            found.sure = 0.9f;
            const std::vector<Detection> founds{found};
            text.append(lookLine, writeLook(lookLine, image, timeMs, founds, true));
            const int count = tracker.update(timeMs, founds, out.data(), static_cast<int>(out.size()));
            looks.push_back({image, timeMs, timeMs + 8.0, {out.begin(), out.begin() + count}});
            appendShown(text, image, timeMs + 8.0, looks.back().shown);
        }
        const Look* taken = nullptr;
        for (; nextLook < looks.size() && looks[nextLook].doneMs <= timeMs; ++nextLook) {
            taken = &looks[nextLook];
        }
        if (taken != nullptr) {
            const GreyView view = images[static_cast<std::size_t>(taken->image)].view(taken->image);
            snapper.looked(taken->timeMs, &view, taken->shown);
        }
        const int count = snapper.snap(timeMs, images[static_cast<std::size_t>(i)].view(image), out.data(),
                                       static_cast<int>(out.size()));
        appendSnapped(text, image, timeMs, timeMs + 1.0, SnapRules::kLeadMs,
                      {out.data(), static_cast<std::size_t>(count)});
        appendFinds(text, image, taken != nullptr ? taken->image : 0, snapper.finds());
    }

    const ClipText clip = readClipText(text);
    REQUIRE(clip.snaps.size() == 40);
    CHECK(clip.snaps[5].hasFinds);
    const Guesses recorded = recordedGuesses(clip);
    std::size_t snappedBoxes = 0;
    for (const ClipSnap& snap : clip.snaps) {
        for (const Tracked& thing : snap.things) {
            snappedBoxes += thing.held ? 0 : 1;
        }
    }
    CHECK(snappedBoxes > 25); // Live found it on the images.

    // The replay, on grass alone: through the recorded searches, exactly live's boxes.
    Rules todayTracker;
    const Guesses today = heuristicGuesses(clip, todayTracker);
    SnapRules todaySnapper;
    GrassImages grass;
    const Guesses snapped = snapGuesses(clip, today, todaySnapper, grass);
    REQUIRE(snapped.sets() == recorded.sets());
    for (std::size_t set = 0; set < recorded.sets(); ++set) {
        CHECK(snapped.fromMs(set) == recorded.fromMs(set));
        const std::span<const Tracked> was = recorded.set(set);
        const std::span<const Tracked> now = snapped.set(set);
        REQUIRE(now.size() == was.size());
        for (std::size_t t = 0; t < was.size(); ++t) {
            CHECK(now[t].id == was[t].id);
            CHECK(now[t].x == doctest::Approx(was[t].x).epsilon(1e-6));
            CHECK(now[t].y == doctest::Approx(was[t].y).epsilon(1e-6));
            CHECK(now[t].held == was[t].held);
        }
    }
    const GuessesDifference difference = compareGuesses(recorded, snapped, -1, 0.0);
    CHECK(difference.same == difference.sets);

    // Without the finds lines (a clip from before) it searches the grass, and the boxes are not live's.
    std::string old;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = text.find('\n', at) + 1;
        if (text.compare(at, 6, "finds ") != 0) {
            old.append(text, at, end - at);
        }
        at = end;
    }
    const ClipText oldClip = readClipText(old);
    CHECK_FALSE(oldClip.snaps[5].hasFinds);
    const std::vector<SnapStep> steps = snapSteps(oldClip);
    REQUIRE(steps.size() == 40);
    CHECK(steps[2].lookImage == 1); // Look 1, done at 13 ms, taken in at image 3 (15 ms),
    CHECK(steps[3].lookImage == 0); // nothing new at image 4,
    CHECK(steps[4].lookImage == 3); // look 3 at image 5.
    Rules oldTracker;
    SnapRules oldSnapper;
    GrassImages oldGrass;
    const Guesses oldSnapped = snapGuesses(oldClip, heuristicGuesses(oldClip, oldTracker), oldSnapper, oldGrass);
    CHECK(compareGuesses(recorded, oldSnapped, -1, 0.0).same < difference.sets);
}

TEST_CASE("a clip without snapped lines is snapped on every new image of its video") {
    std::string text;
    char lookLine[kExactLookChars];
    const std::vector<Detection> found{character(100, 50, 0.9f)};
    appendFrame(text, 1, 10, 0.0);
    text.append(lookLine, writeLook(lookLine, 10, 0.0, found, true));
    appendShown(text, 10, 9.0, std::vector<Tracked>{shownThing(1, 100)});
    appendFrame(text, 2, 11, 4.0);
    appendFrame(text, 3, 11, 504.0); // A repeat: the screen did not change.
    appendFrame(text, 4, 13, 510.0);
    const std::vector<SnapStep> steps = snapSteps(readClipText(text));
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].image == 10);
    CHECK(steps[0].lookImage == 0); // Not done yet.
    CHECK(steps[1].image == 11);
    CHECK(steps[1].doneMs == 4.0 + kReplayLateMs);
    CHECK(steps[1].lookImage == 0);
    CHECK(steps[2].image == 13);
    CHECK(steps[2].lookImage == 10);
    CHECK(steps[2].finds == nullptr);
}
