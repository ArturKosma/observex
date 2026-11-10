#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// The kinds a trained model tells apart, from its model.json (tools/train/train.py --kinds): the "kinds" list, e.g.
// ["monster", "player", "npc", "own"], in the order of the model's scores. Empty for a model trained on one category
// (no such list), or if the list cannot be read.
std::vector<std::string> parseModelKinds(std::string_view modelJson);

// Whether a model of `modelKinds` (parseModelKinds) fits a category of `categoryKinds`: it knows the category's first
// kinds, in their order, and no others. A model trained before a kind was added to the category (2026-11-08: "can't
// tell") still fits; the kinds it does not know yet are only the user's to answer. Never for a model without kinds.
bool kindsFit(const std::vector<std::string>& modelKinds, std::span<const std::string_view> categoryKinds);

// A model's kind classifier: how it turns a candidate's features (Detector::features) into one score per kind,
// before the sigmoid: features . weights of the kind + its bias. RF-DETR's class_embed, its kind rows only.
struct KindHead {
    std::vector<float> biases;  // One per kind (one for a model of one category).
    std::vector<float> weights; // One row of features() numbers per kind, row after row.

    int kinds() const { return static_cast<int>(biases.size()); }
    int features() const { return biases.empty() ? 0 : static_cast<int>(weights.size() / biases.size()); }
    bool operator==(const KindHead&) const = default;
};

// The kind head in model.json's "kind_head" (tools/train/export.py, since 2026-11-08): {"biases": [...], "weights":
// [[...], ...]}, a row of weights per bias, all as long. Empty if there is none or it cannot be read.
KindHead parseKindHead(std::string_view modelJson);

// The kind update (engine/KindUpdater, the user asked, 2026-11-08): the model's kind head re-fit to every reviewed
// answer of every recording, made after a review when there are answers newer than the model. Saved next to the model
// as kinds-update.json; live recognition and the finder pick kinds with it (core/KindLearner's pickKinds) and the
// review learns from it, until the next training makes a newer model.onnx.
struct KindUpdate {
    KindHead head;              // Empty: there is none.
    std::size_t answers = 0;    // How many answers it learned from (guesses answered yes of a kind, or no),
    std::size_t recordings = 0; // in how many recordings.
};

inline constexpr char kKindUpdateFile[] = "kinds-update.json";

// The file: {"answers": 10234, "recordings": 27, "kind_head": {"biases": [...], "weights": [[...], ...]}}, the head as
// in model.json (parseKindHead reads it), every number in full float precision (it reads back exactly).
std::string formatKindUpdate(const KindUpdate& update);
// Reads what formatKindUpdate wrote: the head as parseKindHead reads it, and the counts (0 if missing).
KindUpdate parseKindUpdate(std::string_view json);

// The kind update in `modelFolder` (models/<category>) if it is valid for the model there: at least as new as its
// model.onnx (an older one learned on an older model's features) and its head the shape of model.json's kind head.
// Else an empty one.
KindUpdate loadKindUpdate(const std::filesystem::path& modelFolder);
// The kind head to use with the model in `modelFolder`: the valid kind update's, else model.json's (empty if neither).
KindHead kindHeadToUse(const std::filesystem::path& modelFolder);

} // namespace observex
