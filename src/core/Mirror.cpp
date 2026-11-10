#include "core/Mirror.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

// Intersection over union, as core/Marks' overlap but for unrounded boxes.
float overlap(const Detection& a, const Detection& b) {
    const float width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const float height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    if (width <= 0.0f || height <= 0.0f) {
        return 0.0f;
    }
    const float shared = width * height;
    return shared / (a.width * a.height + b.width * b.height - shared);
}

// How sure the model was that `thing` is of `kind`: of its kind if that is its kind, of its second kind if that is
// the second, else 0 (less than that, unknown). For a model without kinds, every kind is 0.
float sureOf(const Detection& thing, std::uint8_t kind) {
    if (thing.kind == kind) {
        return kindSureOf(thing);
    }
    return thing.second >= 0.0f && thing.secondKind == kind ? thing.second : 0.0f;
}

// For each thing in `from` at least `minSure` sure: how far how sure it is of its kind is from how sure the model was,
// in `to`, that the thing overlapping it most there (the first of equals) is of the same kind, or from 0.
float oneWay(std::span<const Detection> from, std::span<const Detection> to, float minSure, float minOverlap) {
    float total = 0.0f;
    for (const Detection& thing : from) {
        if (thing.sure < minSure) {
            continue;
        }
        float most = 0.0f;
        float other = 0.0f;
        for (const Detection& candidate : to) {
            const float amount = overlap(thing, candidate);
            if (amount >= minOverlap && amount > most) {
                most = amount;
                other = sureOf(candidate, thing.kind);
            }
        }
        total += std::abs(kindSureOf(thing) - other);
    }
    return total;
}

} // namespace

float mirrorDisagreement(std::span<const Detection> plain, std::span<const Detection> mirrored, float minSure,
                         float minOverlap) {
    return (oneWay(plain, mirrored, minSure, minOverlap) + oneWay(mirrored, plain, minSure, minOverlap)) * 0.5f;
}

} // namespace observex
