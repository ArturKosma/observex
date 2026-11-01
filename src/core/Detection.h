#pragma once

namespace observex {

// Something a model found on a screen image: a box in image pixels, and how sure the model is (0 to 1).
struct Detection {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float sure = 0.0f;
};

} // namespace observex
