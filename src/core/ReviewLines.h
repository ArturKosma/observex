#pragma once

#include "core/BoxLearner.h"
#include "core/KindLearner.h"
#include "core/Marks.h"

#include <span>
#include <string>
#include <vector>

namespace observex {

// The words of the review log (app/ReviewLog writes them, with the time, to <category>-review-log.txt in the
// recording's folder): what each mark is, how sure the model and the re-fit were, what the user changed. So the
// user's reviews can be read back afterwards, also by Claude, to see what the user mostly fixed and why (the user
// asked, 2026-11-08). Kinds are named by their ids, as in the answers file (monster, player, npc, own); "no" is "not
// in the category". Boxes are [x y width height] in image pixels.

// "[x y width height]".
std::string describeBox(const Box& box);

// What a mark is now, and where that came from: "monster (pre-filled)", "player (copied from the image before)",
// "no (yours)", "monster (kind not checked)", "unanswered".
std::string describeMark(const Mark& mark, KindIds kinds);

// Where a mark is: "[x y width height] guess 72%" for the finder's, just the box for one drawn by the user or read
// back from the answers file (which keeps no sureness).
std::string describeWhere(const Mark& mark);

// How sure of each kind (`sure`, 0 to 1, in the kinds' order), surest first, those at 1% and up: "monster 85%, player
// 12%". "nothing" if none.
std::string describeSure(std::span<const float> sure, KindIds kinds);

// Why a just-shown guess starts as it does (core/Marks' rules): "asked" ("asked, though sure enough to pre-fill: the
// image before had another answer here" when a "no" or another kind was not copied onto it), "copied from the image
// before", or for one
// pre-filled the rule that answered it ("the model and the re-fit agree, both 78%+ sure", "the re-fit is 90%+ sure",
// "the model is 78%+ sure"). `own` and `learned`: how sure the model and the re-fit are of each kind; both empty
// without learning (the model alone).
std::string describePrefill(const Mark& mark, std::span<const float> own, std::span<const float> learned);

// The other marks on the image that mark `index` overlaps, one each: "mark 3 [..] guess 72% monster (pre-filled):
// overlap 41%, 95% of this box inside it, 30% of it inside this box". Only real overlaps (10%+) or one box mostly
// (50%+) inside the other: one box over two characters, a box drawn inside a guess. Empty if none.
std::vector<std::string> describeOverlaps(const std::vector<Mark>& marks, std::size_t index, KindIds kinds);

// What changed on an image from `before` (as it was shown) to `after`, one line per mark: "changed mark 2 [..]:
// monster (pre-filled) -> player (yours)", "drew mark 7 [..]: player (yours)", "removed mark 7 [..]: monster
// (yours)". Marks are matched by their box (guesses keep theirs, drawn ones are new boxes). Then, if any, "kept 5
// marks as shown" (the user moved on with them as they were). Empty if nothing at all.
std::vector<std::string> describeChanges(const std::vector<Mark>& before, const std::vector<Mark>& after,
                                         KindIds kinds);

// How many marks of each kind, then no and unanswered: "monster 3, player 2, no 1".
std::string describeCounts(const std::vector<Mark>& marks, KindIds kinds);

// A fit of the review's re-fit (`learner`, just fitted to `answers` in `ms`): "learned from 215 answers (monster 120,
// player 60, no 35) in 3.1 ms; weights moved 4.2% from the model's, biases monster +0.31, player +1.20, ...".
std::string describeFit(const KindLearner& learner, std::span<const int> answers, KindIds kinds, double ms);

// A fit of the review's box nudge (`learner`, just fitted in `ms`): "learned from 640 answered guesses in 4.2 ms; the
// user's boxes near this image on average: 0.78 of the guess's width, 0.80 of its height, the middle 1% of it right,
// 2% up", or "learned from no answered guesses yet: the finder's boxes".
std::string describeBoxFit(const BoxLearner& learner, double ms);

} // namespace observex
