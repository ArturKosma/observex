#include "heuristics/TurnPitch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace observex {

namespace {

constexpr int kScale = 1 << TurnPitch::kLevel; // Full-size pixels a kLevel pixel.

// The camera on the ground's map seen from world to camera (rows: right, down, ahead), as core/Ground's worldToScreen
// and mapToGround have it: `yaw` as GroundCamera's, `pitch` > 0 looking down; the world's axes the map's x, y and up.
struct Rotation {
    double m[3][3];
};

Rotation cameraRotation(double yaw, double pitch) {
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    return {{{cy, -sy, 0.0}, {-sp * sy, -sp * cy, -cp}, {cp * sy, cp * cy, -sp}}};
}

// a * b transposed.
Rotation timesTransposed(const Rotation& a, const Rotation& b) {
    Rotation out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.m[i][j] = a.m[i][0] * b.m[j][0] + a.m[i][1] * b.m[j][1] + a.m[i][2] * b.m[j][2];
        }
    }
    return out;
}

// The first camera's view to the second's after turning `turn` and tilting from `pitch` to `pitch + tilt`.
Rotation between(double pitch, double turn, double tilt) {
    return timesTransposed(cameraRotation(turn, pitch + tilt), cameraRotation(0.0, pitch));
}

// Two cameras as the fit has them: the first's pitch, how far it turned and tilted to the second, and which way it
// went (a direction in the second's view: its length says nothing, the points' distances unknown).
struct Model {
    double pitch = 0.0;
    double turn = 0.0;
    double tilt = 0.0;
    double way[3] = {1.0, 0.0, 0.0};
};

constexpr int kParameters = 5; // A step: the pitch, the turn, the tilt, and the way across two directions square to it.

// The essential matrix of `model`: a point seen at x1 on the first image and x2 on the second has x2' E x1 = 0.
struct Essential {
    double e[3][3];
};

Essential essentialOf(const Model& model) {
    const Rotation r = between(model.pitch, model.turn, model.tilt);
    const double* t = model.way;
    const double tx[3][3] = {{0.0, -t[2], t[1]}, {t[2], 0.0, -t[0]}, {-t[1], t[0], 0.0}};
    Essential out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.e[i][j] = tx[i][0] * r.m[0][j] + tx[i][1] * r.m[1][j] + tx[i][2] * r.m[2][j];
        }
    }
    return out;
}

// `model` moved by `step`: the way along two directions square to it, then made a unit again; the rest held to what
// the camera can do between two follows.
Model moved(const Model& model, const double step[kParameters]) {
    const double* t = model.way;
    // A direction not along the way, then the two square to it.
    const bool across = std::fabs(t[0]) < 0.9;
    const double helper[3] = {across ? 1.0 : 0.0, across ? 0.0 : 1.0, 0.0};
    double u[3] = {t[1] * helper[2] - t[2] * helper[1], t[2] * helper[0] - t[0] * helper[2],
                   t[0] * helper[1] - t[1] * helper[0]};
    const double uLength = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (double& value : u) {
        value /= uLength;
    }
    const double v[3] = {t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0]};
    Model out = model;
    out.pitch = std::clamp(model.pitch + step[0], -1.55, 1.55);
    out.turn = std::clamp(model.turn + step[1], -0.8, 0.8);
    out.tilt = std::clamp(model.tilt + step[2], -0.4, 0.4);
    double length = 0.0;
    for (int i = 0; i < 3; ++i) {
        out.way[i] = t[i] + step[3] * u[i] + step[4] * v[i];
        length += out.way[i] * out.way[i];
    }
    length = std::sqrt(length);
    for (double& value : out.way) {
        value /= length;
    }
    return out;
}

// How far (in rays: full-size pixels over the focal) a point is off the line the essential matrix says it must be
// on (Sampson's first-order distance, signed).
double sampson(const Essential& essential, const TurnPitch::Ray2& ray) {
    const auto& e = essential.e;
    const double a0 = e[0][0] * ray.x1 + e[0][1] * ray.y1 + e[0][2];
    const double a1 = e[1][0] * ray.x1 + e[1][1] * ray.y1 + e[1][2];
    const double a2 = e[2][0] * ray.x1 + e[2][1] * ray.y1 + e[2][2];
    const double b0 = e[0][0] * ray.x2 + e[1][0] * ray.y2 + e[2][0];
    const double b1 = e[0][1] * ray.x2 + e[1][1] * ray.y2 + e[2][1];
    const double along = ray.x2 * a0 + ray.y2 * a1 + a2;
    const double spread = a0 * a0 + a1 * a1 + b0 * b0 + b1 * b1;
    return spread > 1e-30 ? along / std::sqrt(spread) : 0.0;
}

// Solves a * x = b (kParameters x kParameters) in place by Gaussian elimination; false if singular.
bool solve(double a[kParameters][kParameters], double b[kParameters]) {
    for (int k = 0; k < kParameters; ++k) {
        int pivot = k;
        for (int i = k + 1; i < kParameters; ++i) {
            if (std::fabs(a[i][k]) > std::fabs(a[pivot][k])) {
                pivot = i;
            }
        }
        if (std::fabs(a[pivot][k]) < 1e-300) {
            return false;
        }
        if (pivot != k) {
            for (int j = 0; j < kParameters; ++j) {
                std::swap(a[k][j], a[pivot][j]);
            }
            std::swap(b[k], b[pivot]);
        }
        for (int i = k + 1; i < kParameters; ++i) {
            const double f = a[i][k] / a[k][k];
            for (int j = k; j < kParameters; ++j) {
                a[i][j] -= f * a[k][j];
            }
            b[i] -= f * b[k];
        }
    }
    for (int k = kParameters - 1; k >= 0; --k) {
        double sum = b[k];
        for (int j = k + 1; j < kParameters; ++j) {
            sum -= a[k][j] * b[j];
        }
        b[k] = sum / a[k][k];
    }
    return true;
}

// Robust cost of `model` over `rays` (Cauchy, scale `scale` in rays).
double costOf(const Model& model, std::span<const TurnPitch::Ray2> rays, double scale) {
    const Essential e = essentialOf(model);
    double cost = 0.0;
    for (const TurnPitch::Ray2& ray : rays) {
        const double r = sampson(e, ray) / scale;
        cost += std::log1p(r * r);
    }
    return cost;
}

// Each ray's distance off its line under `model`, and how it changes with each parameter of a step (numerically).
constexpr double kStep = 1e-6;
void differentiate(const Model& model, std::span<const TurnPitch::Ray2> rays, std::vector<double>& residuals,
                   std::vector<double>& jacobian) {
    residuals.resize(rays.size());
    jacobian.resize(rays.size() * kParameters);
    const Essential e = essentialOf(model);
    for (std::size_t i = 0; i < rays.size(); ++i) {
        residuals[i] = sampson(e, rays[i]);
    }
    for (int k = 0; k < kParameters; ++k) {
        double step[kParameters] = {};
        step[k] = kStep;
        const Essential changed = essentialOf(moved(model, step));
        for (std::size_t i = 0; i < rays.size(); ++i) {
            jacobian[i * kParameters + static_cast<std::size_t>(k)] = (sampson(changed, rays[i]) - residuals[i]) / kStep;
        }
    }
}

// The robust normal equations: h = J' W J and g = J' W r, W the Cauchy weights.
void normals(const std::vector<double>& residuals, const std::vector<double>& jacobian, double scale,
             double h[kParameters][kParameters], double g[kParameters]) {
    for (int a = 0; a < kParameters; ++a) {
        g[a] = 0.0;
        for (int b = 0; b < kParameters; ++b) {
            h[a][b] = 0.0;
        }
    }
    for (std::size_t i = 0; i < residuals.size(); ++i) {
        const double r = residuals[i] / scale;
        const double w = 1.0 / (1.0 + r * r);
        const double* j = &jacobian[i * kParameters];
        for (int a = 0; a < kParameters; ++a) {
            g[a] += w * j[a] * residuals[i];
            for (int b = a; b < kParameters; ++b) {
                h[a][b] += w * j[a] * j[b];
            }
        }
    }
    for (int a = 0; a < kParameters; ++a) {
        for (int b = 0; b < a; ++b) {
            h[a][b] = h[b][a];
        }
    }
}

// Levenberg-Marquardt on the robust cost from `model` (iteratively reweighted), `iterations` at most: the cost there.
double refine(Model& model, std::span<const TurnPitch::Ray2> rays, double scale, int iterations) {
    std::vector<double> residuals;
    std::vector<double> jacobian;
    double cost = costOf(model, rays, scale);
    double lambda = 1e-3;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        differentiate(model, rays, residuals, jacobian);
        double h[kParameters][kParameters];
        double g[kParameters];
        normals(residuals, jacobian, scale, h, g);
        bool better = false;
        for (int attempt = 0; attempt < 6 && !better; ++attempt) {
            double damped[kParameters][kParameters];
            double step[kParameters];
            for (int a = 0; a < kParameters; ++a) {
                for (int b = 0; b < kParameters; ++b) {
                    damped[a][b] = h[a][b];
                }
                damped[a][a] += lambda * (h[a][a] + 1e-12);
                step[a] = -g[a];
            }
            if (!solve(damped, step)) {
                lambda *= 10.0;
                continue;
            }
            const Model tried = moved(model, step);
            const double triedCost = costOf(tried, rays, scale);
            if (triedCost < cost) {
                const double gain = cost - triedCost;
                model = tried;
                cost = triedCost;
                lambda = std::max(lambda * 0.3, 1e-9);
                better = true;
                if (gain < 1e-9 * (1.0 + cost)) {
                    return cost;
                }
            } else {
                lambda *= 10.0;
            }
        }
        if (!better) {
            break;
        }
    }
    return cost;
}

} // namespace

bool TurnPitch::build(const GreyView& image, Pyramid& out) {
    const GreyLevel& source = image.levels[kLevel];
    if (source.pixels == nullptr || source.width < 64 || source.height < 64) {
        return false;
    }
    Level& first = out[0];
    first.width = source.width;
    first.height = source.height;
    first.pixels.resize(static_cast<std::size_t>(first.width) * static_cast<std::size_t>(first.height));
    for (int y = 0; y < first.height; ++y) {
        const std::uint8_t* row = image.row(kLevel, y);
        float* to = first.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(first.width);
        for (int x = 0; x < first.width; ++x) {
            to[x] = static_cast<float>(row[x]);
        }
    }
    for (int level = 1; level < kPyramid; ++level) {
        const Level& above = out[static_cast<std::size_t>(level - 1)];
        Level& here = out[static_cast<std::size_t>(level)];
        here.width = above.width / 2;
        here.height = above.height / 2;
        here.pixels.resize(static_cast<std::size_t>(here.width) * static_cast<std::size_t>(here.height));
        for (int y = 0; y < here.height; ++y) {
            const float* a = above.pixels.data() + static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(above.width);
            const float* b = a + above.width;
            float* to = here.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(here.width);
            for (int x = 0; x < here.width; ++x) {
                to[x] = 0.25f * (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1]);
            }
        }
    }
    return true;
}

void TurnPitch::learn(const GreyView& image, std::span<const GreyArea> avoid) {
    points_.clear();
    if (image.image != 0 && image.image == seenImage_ && !seen_[0].pixels.empty()) {
        std::swap(learned_, seen_); // Just followed onto: built already.
    } else if (!build(image, learned_)) {
        return;
    }
    seenImage_ = 0;
    const Level& level = learned_[0];
    const int w = level.width;
    const int h = level.height;
    // Left out: the areas given, kHalfWindow + 2 pixels wider (the window must not reach into them), and the edges.
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    const int margin = kHalfWindow + 2;
    for (const GreyArea& area : avoid) {
        const int x0 = std::max(area.x0 / kScale - margin, 0);
        const int y0 = std::max(area.y0 / kScale - margin, 0);
        const int x1 = std::min((area.x1 + kScale - 1) / kScale + margin, w);
        const int y1 = std::min((area.y1 + kScale - 1) / kScale + margin, h);
        for (int y = y0; y < y1; ++y) {
            std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(y) * w + x0, std::max(x1 - x0, 0), std::uint8_t{1});
        }
    }
    // The corners' strength: the structure tensor's smaller eigenvalue over 5 x 5 pixels (gradients by central
    // differences), summed by rows then columns.
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> xx(n, 0.0f), xy(n, 0.0f), yy(n, 0.0f);
    for (int y = 1; y < h - 1; ++y) {
        const float* row = level.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 1; x < w - 1; ++x) {
            const float gx = 0.5f * (row[x + 1] - row[x - 1]);
            const float gy = 0.5f * (row[x + w] - row[x - w]);
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            xx[i] = gx * gx;
            xy[i] = gx * gy;
            yy[i] = gy * gy;
        }
    }
    const auto boxSum = [w, h](std::vector<float>& values) {
        std::vector<float> rows(values.size(), 0.0f);
        for (int y = 0; y < h; ++y) {
            const float* in = values.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            float* to = rows.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            for (int x = 2; x < w - 2; ++x) {
                to[x] = in[x - 2] + in[x - 1] + in[x] + in[x + 1] + in[x + 2];
            }
        }
        std::fill(values.begin(), values.end(), 0.0f);
        for (int y = 2; y < h - 2; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                const std::size_t up = static_cast<std::size_t>(w);
                values[i] = rows[i - 2 * up] + rows[i - up] + rows[i] + rows[i + up] + rows[i + 2 * up];
            }
        }
    };
    boxSum(xx);
    boxSum(xy);
    boxSum(yy);
    const int edge = kHalfWindow + 3; // Room for the window and its gradients.
    const float cellW = static_cast<float>(w) / kCellsX;
    const float cellH = static_cast<float>(h) / kCellsY;
    for (int cy = 0; cy < kCellsY; ++cy) {
        for (int cx = 0; cx < kCellsX; ++cx) {
            const int x0 = std::max(static_cast<int>(static_cast<float>(cx) * cellW), edge);
            const int x1 = std::min(static_cast<int>(static_cast<float>(cx + 1) * cellW), w - edge);
            const int y0 = std::max(static_cast<int>(static_cast<float>(cy) * cellH), edge);
            const int y1 = std::min(static_cast<int>(static_cast<float>(cy + 1) * cellH), h - edge);
            float best = kLeastCorner;
            int bestX = -1;
            int bestY = -1;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                    if (out[i] != 0) {
                        continue;
                    }
                    const float half = 0.5f * (xx[i] + yy[i]);
                    const float diff = 0.5f * (xx[i] - yy[i]);
                    const float least = half - std::sqrt(diff * diff + xy[i] * xy[i]);
                    if (least > best) {
                        best = least;
                        bestX = x;
                        bestY = y;
                    }
                }
            }
            if (bestX >= 0) {
                points_.push_back({static_cast<float>(bestX), static_cast<float>(bestY)});
            }
        }
    }
}

bool TurnPitch::track(const Pyramid& from, const Pyramid& to, float x, float y, float guessX, float guessY,
                      float& outX, float& outY) {
    constexpr int kSide = 2 * kHalfWindow + 1;
    constexpr int kArea = kSide * kSide;
    std::array<float, kArea> learnedGrey{};
    std::array<float, kArea> gradX{};
    std::array<float, kArea> gradY{};
    // Level l's pixel of kLevel's (x, y): (x + 0.5) / 2^l - 0.5; a motion there is the motion here over 2^l.
    float moveX = guessX / static_cast<float>(1 << (kPyramid - 1));
    float moveY = guessY / static_cast<float>(1 << (kPyramid - 1));
    const float window = static_cast<float>(kHalfWindow) + 1.0f;
    for (int l = kPyramid - 1; l >= 0; --l) {
        const Level& a = from[static_cast<std::size_t>(l)];
        const Level& b = to[static_cast<std::size_t>(l)];
        const float scale = 1.0f / static_cast<float>(1 << l);
        const float px = (x + 0.5f) * scale - 0.5f;
        const float py = (y + 0.5f) * scale - 0.5f;
        if (!a.inside(px, py, window + 1.0f)) {
            if (l == 0) {
                return false;
            }
            moveX *= 2.0f; // Too near the edge at this size: from the next.
            moveY *= 2.0f;
            continue;
        }
        // The learned window and its gradients (the bilinear weights are the same for all its pixels).
        float gxx = 0.0f;
        float gxy = 0.0f;
        float gyy = 0.0f;
        {
            const int ix = static_cast<int>(px);
            const int iy = static_cast<int>(py);
            const float fx = px - static_cast<float>(ix);
            const float fy = py - static_cast<float>(iy);
            const float w00 = (1.0f - fx) * (1.0f - fy);
            const float w01 = fx * (1.0f - fy);
            const float w10 = (1.0f - fx) * fy;
            const float w11 = fx * fy;
            const auto grey = [&](int gx, int gy) {
                const float* p = a.pixels.data() + static_cast<std::ptrdiff_t>(gy) * a.width + gx;
                return w00 * p[0] + w01 * p[1] + w10 * p[a.width] + w11 * p[a.width + 1];
            };
            int k = 0;
            for (int dy = -kHalfWindow; dy <= kHalfWindow; ++dy) {
                for (int dx = -kHalfWindow; dx <= kHalfWindow; ++dx, ++k) {
                    const int sx = ix + dx;
                    const int sy = iy + dy;
                    learnedGrey[static_cast<std::size_t>(k)] = grey(sx, sy);
                    const float ex = 0.5f * (grey(sx + 1, sy) - grey(sx - 1, sy));
                    const float ey = 0.5f * (grey(sx, sy + 1) - grey(sx, sy - 1));
                    gradX[static_cast<std::size_t>(k)] = ex;
                    gradY[static_cast<std::size_t>(k)] = ey;
                    gxx += ex * ex;
                    gxy += ex * ey;
                    gyy += ey * ey;
                }
            }
        }
        const float det = gxx * gyy - gxy * gxy;
        if (det < 1e-3f * (gxx + gyy) * (gxx + gyy) || gxx + gyy < 1e-3f) {
            return false; // No corner at this size.
        }
        float vx = 0.0f;
        float vy = 0.0f;
        for (int iteration = 0; iteration < kIterations; ++iteration) {
            const float qx = px + moveX + vx;
            const float qy = py + moveY + vy;
            if (!b.inside(qx, qy, window)) {
                if (l == 0) {
                    return false;
                }
                break; // Off the edge at this size: on from the next as far as it got.
            }
            const int ix = static_cast<int>(qx);
            const int iy = static_cast<int>(qy);
            const float fx = qx - static_cast<float>(ix);
            const float fy = qy - static_cast<float>(iy);
            const float w00 = (1.0f - fx) * (1.0f - fy);
            const float w01 = fx * (1.0f - fy);
            const float w10 = (1.0f - fx) * fy;
            const float w11 = fx * fy;
            float bx = 0.0f;
            float by = 0.0f;
            int k = 0;
            for (int dy = -kHalfWindow; dy <= kHalfWindow; ++dy) {
                const float* p = b.pixels.data() + static_cast<std::ptrdiff_t>(iy + dy) * b.width + ix - kHalfWindow;
                for (int dx = 0; dx < kSide; ++dx, ++k) {
                    const float seen = w00 * p[dx] + w01 * p[dx + 1] + w10 * p[dx + b.width] + w11 * p[dx + b.width + 1];
                    const float off = learnedGrey[static_cast<std::size_t>(k)] - seen;
                    bx += off * gradX[static_cast<std::size_t>(k)];
                    by += off * gradY[static_cast<std::size_t>(k)];
                }
            }
            const float stepX = (gyy * bx - gxy * by) / det;
            const float stepY = (gxx * by - gxy * bx) / det;
            vx += stepX;
            vy += stepY;
            if (stepX * stepX + stepY * stepY < 1e-4f) {
                break;
            }
        }
        moveX += vx;
        moveY += vy;
        if (l > 0) {
            moveX *= 2.0f;
            moveY *= 2.0f;
        }
    }
    outX = x + moveX;
    outY = y + moveY;
    const Level& last = to[0];
    return last.inside(outX, outY, window);
}

TurnPitch::Pair TurnPitch::follow(const GreyView& image, const GroundCamera& camera, float pitch, float turn,
                                  float tilt) {
    Pair pair;
    seenImage_ = 0;
    if (points_.empty() || camera.focal <= 0.0f || !build(image, seen_) || seen_[0].width != learned_[0].width ||
        seen_[0].height != learned_[0].height) {
        return pair;
    }
    seenImage_ = image.image;
    // Where the camera's motion as the follower has it takes each point: on the flat ground (turned round the feet,
    // tilted) below the horizon, else as if infinitely far (turned and tilted only).
    GroundCamera first = camera;
    first.pitch = pitch;
    GroundCamera second = camera;
    second.pitch = pitch + tilt;
    const Rotation far = between(pitch, turn, tilt);
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    const float scale = static_cast<float>(kScale);
    rays_.clear();
    for (const Point& point : points_) {
        const float fullX = (point.x + 0.5f) * scale;
        const float fullY = (point.y + 0.5f) * scale;
        float toX = fullX;
        float toY = fullY;
        bool foretold = false;
        if (const std::optional<GroundPoint> ground = screenToGround(first, fullX, fullY); ground && ground->y < 40.0f) {
            const float right = c * ground->x - s * ground->y;
            const float away = s * ground->x + c * ground->y;
            if (const GroundPoint moved = groundToScreen(second, right, away); moved.ahead) {
                toX = moved.x;
                toY = moved.y;
                foretold = true;
            }
        }
        if (!foretold) {
            const double rx = (fullX - camera.middleX) / camera.focal;
            const double ry = (fullY - camera.middleY) / camera.focal;
            const double ax = far.m[0][0] * rx + far.m[0][1] * ry + far.m[0][2];
            const double ay = far.m[1][0] * rx + far.m[1][1] * ry + far.m[1][2];
            const double az = far.m[2][0] * rx + far.m[2][1] * ry + far.m[2][2];
            if (az > 0.1) {
                toX = camera.middleX + camera.focal * static_cast<float>(ax / az);
                toY = camera.middleY + camera.focal * static_cast<float>(ay / az);
            }
        }
        const float guessX = (toX - fullX) / scale;
        const float guessY = (toY - fullY) / scale;
        float x2 = 0.0f;
        float y2 = 0.0f;
        if (!track(learned_, seen_, point.x, point.y, guessX, guessY, x2, y2)) {
            continue;
        }
        float backX = 0.0f;
        float backY = 0.0f;
        if (!track(seen_, learned_, x2, y2, point.x - x2, point.y - y2, backX, backY) ||
            std::hypot(backX - point.x, backY - point.y) > kBackMiss) {
            continue;
        }
        Ray2 ray;
        ray.x1 = (fullX - camera.middleX) / camera.focal;
        ray.y1 = (fullY - camera.middleY) / camera.focal;
        ray.x2 = ((x2 + 0.5f) * scale - camera.middleX) / camera.focal;
        ray.y2 = ((y2 + 0.5f) * scale - camera.middleY) / camera.focal;
        rays_.push_back(ray);
    }
    pair.points = static_cast<int>(rays_.size());
    if (pair.points < kLeastPoints) {
        return pair;
    }
    const Pair fitted = fit(rays_, camera.focal, pitch, turn, tilt);
    if (fitted.fitted) {
        pair = fitted;
        pair.points = static_cast<int>(rays_.size());
    }
    return pair;
}

TurnPitch::Pair TurnPitch::fit(std::span<const Ray2> rays, float focal, float pitch, float turn, float tilt) {
    Pair pair;
    pair.points = static_cast<int>(rays.size());
    if (rays.size() < 8 || focal <= 0.0f) {
        return pair;
    }
    const double scale = kMissScale / focal;
    // Starts: the pitch given and 11 degrees either way, the camera going sideways (as when it turns round the
    // character), up or down (tilting round it) or ahead (walking); each tried on a part of the points, the best then
    // on all of them.
    std::vector<Ray2> few;
    const std::size_t every = std::max<std::size_t>(rays.size() / 120, 1);
    for (std::size_t i = 0; i < rays.size(); i += every) {
        few.push_back(rays[i]);
    }
    Model best;
    double bestCost = std::numeric_limits<double>::infinity();
    for (const double pitchOff : {0.0, -0.2, 0.2}) {
        for (int way = 0; way < 3; ++way) {
            Model model;
            model.pitch = pitch;
            model.turn = turn;
            model.tilt = tilt;
            model.way[0] = way == 0 ? 1.0 : 0.0;
            model.way[1] = way == 1 ? 1.0 : 0.0;
            model.way[2] = way == 2 ? 1.0 : 0.0;
            const double off[kParameters] = {pitchOff, 0.0, 0.0, 0.0, 0.0};
            model = moved(model, off);
            const double cost = refine(model, few, scale, 15);
            if (cost < bestCost) {
                bestCost = cost;
                best = model;
            }
        }
    }
    refine(best, rays, scale, 40);
    // How sure the pitch is: the robust fit's covariance.
    std::vector<double> residuals;
    std::vector<double> jacobian;
    differentiate(best, rays, residuals, jacobian);
    double h[kParameters][kParameters];
    double g[kParameters];
    normals(residuals, jacobian, scale, h, g);
    double weighted = 0.0;
    double weights = 0.0;
    double agreeingSquares = 0.0;
    int agreeing = 0;
    for (const double r : residuals) {
        const double u = r / scale;
        const double w = 1.0 / (1.0 + u * u);
        weighted += w * r * r;
        weights += w;
        if (std::fabs(u) < 3.0) {
            ++agreeing;
            agreeingSquares += r * r;
        }
    }
    const double variance = weights > kParameters ? weighted / (weights - kParameters) : weighted;
    double unit[kParameters] = {1.0, 0.0, 0.0, 0.0, 0.0};
    for (int a = 0; a < kParameters; ++a) {
        h[a][a] += 1e-12;
    }
    double spread = 1.0;
    if (solve(h, unit) && unit[0] > 0.0) {
        spread = std::min(std::sqrt(unit[0] * variance), 1.0);
    }
    pair.fitted = true;
    pair.agreeing = agreeing;
    pair.pitch = static_cast<float>(best.pitch);
    pair.turn = static_cast<float>(best.turn);
    pair.tilt = static_cast<float>(best.tilt);
    pair.spread = static_cast<float>(spread);
    pair.miss = agreeing > 0 ? static_cast<float>(std::sqrt(agreeingSquares / agreeing) * focal) : 0.0f;
    return pair;
}

bool TurnHold::pair(const TurnPitch::Pair& pair, float before, float followerTilt) {
    const bool held = holds();
    if (!held) { // Not held: each pair starts from the follower's pitch.
        known_ = true;
        pitch_ = before;
        variance_ = std::max(variance_, kOtherSpread * kOtherSpread);
    }
    if (!pair.fitted || pair.agreeing < kLeastAgreeing) {
        pitch_ += followerTilt;
        outlierPitch_ += followerTilt;
        const float lost = kLostSpread + std::fabs(followerTilt) / 3.0f;
        variance_ += lost * lost;
        return false;
    }
    pitch_ += pair.tilt;
    outlierPitch_ += pair.tilt;
    const float tilted = kTiltSpread + kTiltShare * std::fabs(pair.tilt);
    variance_ += tilted * tilted;
    if (std::fabs(pair.turn) < kLeastTurn || pair.spread > kSaySpread) {
        return false;
    }
    const float said = pair.pitch + pair.tilt; // On the second image.
    const float spread = std::max(pair.spread, kLeastSpread);
    const float measured = spread * spread;
    const float off = said - pitch_;
    const float both = variance_ + measured;
    if (held ? off * off > kOutlier * kOutlier * both : std::fabs(off) > kFirstReach) {
        const float apart = said - outlierPitch_;
        const bool agree = outliers_ > 0 && apart * apart <= kOutlier * kOutlier * (measured + outlierVariance_);
        outliers_ = agree ? outliers_ + 1 : 1;
        outlierPitch_ = said;
        outlierVariance_ = measured;
        if (outliers_ < kOutliers) {
            return false;
        }
        pitch_ = said; // Turns keep saying otherwise: the pitch held was lost (a tilt missed in a blur).
        variance_ = measured;
        outliers_ = 0;
        return true;
    }
    outliers_ = 0;
    const float gain = variance_ / both;
    pitch_ += gain * off;
    variance_ *= 1.0f - gain;
    return true;
}

float TurnHold::spread() const {
    return known_ ? std::sqrt(variance_) : std::numeric_limits<float>::infinity();
}

} // namespace observex
