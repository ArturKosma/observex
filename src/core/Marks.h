#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A rectangle in image pixels: left, top, width and height.
struct Box {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool contains(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    bool operator==(const Box&) const = default;
};

// How much two boxes cover the same area: 0 = not at all, 1 = the same box (intersection over union).
float overlap(const Box& a, const Box& b);

// How much of box `a` lies inside box `b`: 0 = none of it, 1 = all of it.
float shareInside(const Box& a, const Box& b);

enum class Answer : std::uint8_t {
    None, // Not answered yet.
    Yes,  // It belongs to the category, e.g. it is a character.
    No,   // It does not.
};

// The kinds a category's yes marks come in, each by the word that stands for it in a marks file, e.g. "monster",
// "player"; the first is the default. Empty for a category without kinds.
using KindIds = std::span<const std::string_view>;

// The kinds of characters, by the words the answers file names them with, in the review's order (app/Review.h's
// kCharacterKinds gives each its name and colour; tools/train/train.py's KINDS trains them in this order). New kinds go
// last: a model trained before one came knows the ones before it (kindsFit), and its guesses and classifier keep their
// places; the answers of a kind it does not know yet teach it nothing until the next training.
// "unknown" (the user asked, 2026-11-08): a character too far off to tell which kind (too few pixels).
// "dead" (the user asked, 2026-11-08): a dead character, e.g. a monster's corpse on the ground. Not no: the model sees
// it as a character anyway (70-97% sure), and the bot is to know it is dead (no target; a kill, loot). Measured on
// the eleventh's own features: 34 corpses tell apart from 5296 living monsters well enough to catch 80% of the corpses
// of a recording it never saw, calling 1.8% of living monsters dead (the box's shape alone: 19%).
inline constexpr std::string_view kCharacterKindIds[] = {"monster", "player", "npc", "own", "unknown", "dead"};
inline constexpr std::string_view kUnknownKind = kCharacterKindIds[4]; // "Can't tell".

// Something marked on an image for one category (e.g. a character): a guess, the user's answer, or both.
struct Mark {
    Box box;
    Answer answer = Answer::None;
    float guess = -1.0f;    // How sure the finder was that it belongs to the category (0 to 1); -1 if the user drew it.
    bool carried = false;   // The answer (or kind) was copied from the image before, not given on this one.
    bool presumed = false;  // Answered yes because the finder was sure enough (presumeKinds), not by the user.
    std::uint8_t kind = 0;  // For a yes in a category with kinds: which one, as an index into its KindIds. For a
                            // guess of a model with kinds, the kind the finder was surest of.
    bool unchecked = false; // A yes whose kind the user has not checked yet: it was answered before the category had
                            // kinds, so its kind is only a suggestion (the first kind unless the file names one).
    std::uint8_t secondKind = 0; // For a guess of a model with kinds: the kind the finder was next surest of,
    float secondGuess = -1.0f;   // and how sure (0 to 1); -1 if it named no kinds.
    float kindGuess = -1.0f;     // For a guess of a model with kinds: how sure the finder was of `kind`; -1: `guess`.

    bool operator==(const Mark&) const = default;
};

// How sure the finder was that `mark` is of its kind.
inline float kindGuessOf(const Mark& mark) {
    return mark.kindGuess >= 0.0f ? mark.kindGuess : mark.guess;
}

// The marks of each image, by image file name (e.g. "000001.jpg"), in name order.
using ImageMarks = std::map<std::string, std::vector<Mark>>;

// Reads marks from text. Each image starts with an "image <file name>" line, and each of its marks follows on a
// line of its own: what it is, then x, y, width and height in image pixels. What it is: a kind (one of `kinds`,
// e.g. "monster 1012 540 80 120"), "yes" (in the category; with kinds, one whose kind is not checked yet, maybe
// with a suggested kind: "yes player 1012 540 80 120"), "no", or a guess (0 to 1). A guess of a model with kinds
// then names the kind it is surest of and the next, each with how sure of it: "0.820 monster 0.700 player 0.400
// 1012 540 80 120" (82% sure it is a character: core/Detection's eitherKind).
// An image line with no marks after it is kept, with no marks. Blank lines, lines starting with '#' and lines
// it does not understand are skipped.
ImageMarks parseMarks(std::string_view text, KindIds kinds = {});

// The text parseMarks reads: `comment` first (each line gets a "# "), then the images in name order. Each mark is
// written with its answer, or with its guess if it has no answer; marks with neither are left out. With `kinds`, a
// yes is written as its kind ("monster"), or as "yes <kind>" while unchecked, and a guess with its kinds.
std::string formatMarks(const ImageMarks& marks, std::string_view comment = {}, KindIds kinds = {});

// Whether no yes mark on an image waits for the user to check its kind.
bool kindsChecked(const std::vector<Mark>& marks);

// Whether the user is done with an image's answers: its kinds are checked and no mark waits for an answer. A mark
// without one in an answers file was added after the image was reviewed (2026-11-09, the user asked: a sweep for
// characters nobody marked, things the model was sure of that no answer covered) and is asked as a guess.
bool fullyAnswered(const std::vector<Mark>& marks);

// Gives each unanswered mark in `marks` the answer (and kind) of the answered mark in `previous` that overlaps it
// most, if they overlap at least `minOverlap`, and flags it as carried. Returns how many marks got an answer.
// This is how one answer covers the same thing on the next image, as long as it has not moved much.
// A no the user drew again tighter (boxedAgain) is not carried: the same character was there, only boxed otherwise
// (2026-11-08: replaying the user's first review boxing far-off characters tight, such nos landed on the same
// character on the next image; the user's tighter box is carried instead, onto a guess nudged to match it).
// `presumed` holds, per mark, the kind the review would start it as yes of (presumedKind; -1: none). No answer is
// carried onto a mark the review is sure is something else: a "no" onto a mark it is sure of (2026-11-08: a "no" on a
// lantern landed on a player the model was 100% sure of, and was saved as no without the user seeing it), or another
// kind than the one it is sure of (2026-11-08: the user's character stood on a monster, the monster's answer landed
// on the user's character the model was 94% sure of, and went on from image to image for 13 images). Something else
// stood there on the image before, or the boxes swapped. Its entry becomes -1 instead, so presumeKinds leaves it
// unanswered too: the review asks. (Across the reviews to then, the sure kind was right 8 times of 12 and the copied
// one 4, too often wrong to pre-fill either.)
int carryAnswers(const std::vector<Mark>& previous, std::vector<Mark>& marks, std::span<int> presumed = {},
                 float minOverlap = 0.5f);

// Whether `mark` is a no the user drew again tighter, as matchAnswers takes it: one yes in `marks` (the image's
// answers) kTightInside or more inside it, overlapping it at least kTightOverlap, and it is not around a group (two or
// more yes kGroupInside or more inside it). Not "not in the category": the same thing, boxed otherwise.
bool boxedAgain(const Mark& mark, const std::vector<Mark>& marks);

// Gives each unchecked yes in `marks` the kind of the checked yes in `previous` that overlaps it most, if they
// overlap at least `minOverlap`, and flags it as carried; it stays unchecked. Returns how many marks got one.
// This is carryAnswers for images answered before their category had kinds: once the user checked the kinds on one
// image, the next one suggests the same kinds for the same things.
int carryKinds(const std::vector<Mark>& previous, std::vector<Mark>& marks, float minOverlap = 0.5f);

// Which yes answer each of a reviewed image's `guesses` (the finder's marks, with a guess) is, by `answers` (the
// image's answers): its index in `answers`, or -1 for none (the guess is not in the category). Surest guess first, each
// takes the yes answer it overlaps most that no surer guess took: by at least `minOverlap`, or the user's tighter box
// around the same thing: a yes answer kTightInside or more inside the guess and overlapping it at least kTightOverlap,
// unless the guess is around a group (two or more yes answers kGroupInside or more inside it). So one thing is one
// guess, as models are scored. Drawn boxes are answers like any other.
// The tighter box since 2026-11-08: the user started drawing far-off characters tight around the body, about 0.7 of
// the guess's width and height. Of the 385 guesses the user said no to and drew again on 2026-11-06_20-41-55, 297
// overlapped the new box less than half (down to 0.17) and counted as no, though every one was the same character
// with its new box all inside the guess.
inline constexpr float kTightInside = 0.9f;
inline constexpr float kTightOverlap = 0.15f;
inline constexpr float kGroupInside = 0.6f;
std::vector<int> matchAnswers(const std::vector<Mark>& guesses, const std::vector<Mark>& answers,
                              float minOverlap = 0.5f);

// The kinds of matchAnswers' answers, `answers` with every kind checked: per guess, the answered kind (an index into the
// category's kinds) of the yes answer it matches, or -1 for none. What the kind update learns from
// (engine/KindUpdater), as the Python simulation did.
std::vector<int> matchGuesses(const std::vector<Mark>& guesses, const std::vector<Mark>& answers,
                              float minOverlap = 0.5f);

// Guesses whose kind the finder is at least this sure of start answered yes in the review; the user only flips the
// wrong ones. Less sure ones are what the review asks about. Since 2026-11-07 it is how sure of the kind (the user
// asked: pre-answer only when very sure, kind included); it was how sure of a character, 70%, which pre-answered
// kinds it was far less sure of. On the tenth model's 299 held-out images (2015 guesses at least 30% sure): the old
// rule pre-answered 1594 with 99 wrong (66 not characters, 33 the wrong kind); 78% of the kind (the user's pick)
// pre-answers 1308 with 56 wrong (35, 21); 85%: 899 with 30 (18, 12); 90%: 441 with 12. With a one-category model
// it is how sure of a character.
inline constexpr float kPresumeYes = 0.78f;

// A review that learns (core/KindLearner, the user asked, 2026-11-08) pre-answers a guess yes when the model and the
// classifier re-fit to the user's answers so far agree on its kind, both at least kPresumeYes sure; or when the re-fit
// alone is at least this sure, which is how a correction on earlier images reaches the next ones. Replayed in Python
// (2026-11-08, whole reviews in order with answers carried forward, the user's answers as the user), guesses pre-filled
// right / wrong / asked: the tenth model on the two 2026-11-07 recordings it never trained on, the model alone 807 /
// 38 / 279, this rule with KindLearner's kPull 880 / 38 / 206; the ninth model on 4 places it never saw 1049 / 950 /
// 1369 against 1440 / 107 / 1821. Better than the model alone on every recording but one (3 more wrong, 57 more
// right). 95% with a pull of 0.01 (the first setting, 2026-11-08) was safer but learned little: 825 / 30 / 269 and
// 872 / 62 / 2434; 90% sits between.
// 85% -> 90% on 2026-11-08 (the user's pick: fewer wrong pre-fills, more questions). Replayed in C++
// (observex_bench --replay, answers carried forward) on all 29 reviewed recordings from the eleventh model's own
// classifier: 85% 11126 / 339 / 2416, 90% 10840 / 290 / 2751, 95% 252 wrong / 3247 asked; about 7 more questions
// for each wrong pre-fill avoided.
inline constexpr float kPresumeLearned = 0.90f;

// The surest kind in `sure` (how sure of each kind, 0 to 1), if it is at least `minSure` sure and not close to the
// next (core/Detection's kindsClose); else -1.
int sureKind(std::span<const float> sure, float minSure);

// The kind to start a guess as yes of, from how sure the model is of each of its kinds (`own`) and how sure the
// re-fit is (`learned`): the one both are at least kPresumeYes sure of (sureKind), else the one the re-fit is at
// least kPresumeLearned sure of; -1: the user answers it. With nothing learned yet (learned == own) this is the rule
// for a guess below.
int presumedKind(std::span<const float> own, std::span<const float> learned);

// The same for a review that does not learn (a model without a kind head, or guesses without features), from the
// finder's guess alone: its kind, if the finder was at least kPresumeYes sure of it (kindGuessOf) and not unsure
// which kind it is (core/Detection's kindsClose); else -1, the user answers it.
int presumedKind(const Mark& guess);

// Makes a guess show the kinds of `sure` (how sure of each kind, in the category's order): its surest kind and the
// next, with how sure of each. Its tag and which kind Y answers then come from those.
void showKinds(Mark& mark, std::span<const float> sure);

// Answers yes each unanswered mark i for which kinds[i] is a kind (presumedKind; -1: none), of that kind, and flags
// it as presumed. Returns how many marks got an answer. Run it after carryAnswers, so an answer from the image before
// wins (a "no" stays no, unless carryAnswers left the mark to the user).
int presumeKinds(std::vector<Mark>& marks, std::span<const int> kinds);

} // namespace observex
