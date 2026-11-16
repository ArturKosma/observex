#include "core/Flee.h"

#include "core/Ground.h"
#include "core/StateTree.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace observex;

namespace {

constexpr double kDt = 1.0 / 144.0; // A UI frame.
constexpr float kWidth = 2560.0f;
constexpr float kHeight = 1440.0f;
constexpr double kHold = 0.08; // The app holds a click this long.

// A game as Lineage 2's camera shows it (core/Ground): the own character's feet at the map's (feetX, feetY), the
// target's at (targetX, targetY) on the map, the camera turned `yaw`. A click on the ground walks the feet towards
// the point clicked at `speed` own heights a second (0: stuck).
struct Game {
    GroundCamera camera = [] {
        GroundCamera c = groundCamera(0.42f, 8.0f, kWidth, kHeight);
        c.pitchMeasured = true;
        return c;
    }();
    float targetX = 0.0f;
    float targetY = 6.0f;
    bool targetShown = true;
    bool hasTarget = true;
    float targetHp = 0.8f;
    float speed = 3.0f;
    float yawSpeed = 0.0f; // Radians a second.
    std::vector<FleeWalker::Box> avoid;
    std::vector<FleeWalker::Box> extraCharacters;
    const GroundHeights* heights = nullptr;

    double now = 0.0;
    double cursorX = kWidth * 0.5;
    double cursorY = kHeight * 0.5;
    double releaseAt = -1.0;
    bool walking = false;
    float goX = 0.0f; // Where it walks to, on the map.
    float goY = 0.0f;

    struct Click {
        double t;
        float x, y;       // Screen.
        float mapX, mapY; // The flat ground there.
    };
    std::vector<Click> clicks;
    std::vector<std::string> log;
    bool done = false;
    FleeWalker::Output last;

    FleeWalker::Box ownBox() const {
        const GroundPoint feet = groundToScreen(camera, 0.0f, 0.0f);
        return {feet.x - 40.0f, feet.y - 190.0f, feet.x + 40.0f, feet.y};
    }
    bool targetBox(FleeWalker::Box& box) const {
        float right = 0.0f;
        float away = 0.0f;
        mapToGround(camera, targetX, targetY, right, away);
        const GroundPoint feet = groundToScreen(camera, right, away);
        if (!feet.ahead || feet.x < 0.0f || feet.x > kWidth || feet.y < 0.0f || feet.y > kHeight) {
            return false;
        }
        box = {feet.x - 35.0f, feet.y - 120.0f, feet.x + 35.0f, feet.y};
        return true;
    }

    void run(FleeWalker& flee, double seconds) {
        const double end = now + seconds;
        while (now < end && !done) {
            FleeWalker::Input in;
            in.now = now;
            in.dt = kDt;
            in.width = kWidth;
            in.height = kHeight;
            in.cursorX = static_cast<int>(std::lround(cursorX));
            in.cursorY = static_cast<int>(std::lround(cursorY));
            in.canClick = releaseAt < now;
            in.hasTarget = hasTarget;
            in.targetHp = targetHp;
            in.targetName = "Goblin";
            FleeWalker::Box box;
            in.targetSeen = targetShown && targetBox(box);
            in.targetBox = box;
            in.cameraFollowed = true;
            in.camera = camera;
            in.own = true;
            in.ownBox = ownBox();
            in.heights = heights;
            std::vector<FleeWalker::Box> characters = extraCharacters;
            characters.push_back(in.ownBox);
            if (in.targetSeen) {
                characters.push_back(box);
            }
            in.characters = characters;
            in.avoid = avoid;
            last = flee.step(in);
            cursorX += last.dx;
            cursorY += last.dy;
            if (last.click) {
                releaseAt = now + kHold;
                const std::optional<GroundPoint> at = screenToGround(camera, static_cast<float>(cursorX),
                                                                     static_cast<float>(cursorY));
                REQUIRE(at.has_value());
                Click click{now, static_cast<float>(cursorX), static_cast<float>(cursorY), 0.0f, 0.0f};
                groundToMap(camera, at->x, at->y, click.mapX, click.mapY);
                clicks.push_back(click);
                walking = true;
                goX = click.mapX;
                goY = click.mapY;
            }
            if (!last.log.empty()) {
                log.push_back(last.log);
            }
            done = last.done;
            // The game: the feet walk on, the camera turns.
            if (walking && speed > 0.0f) {
                const float dx = goX - camera.feetX;
                const float dy = goY - camera.feetY;
                const float left = std::hypot(dx, dy);
                const float stepLength = speed * static_cast<float>(kDt);
                if (left <= stepLength) {
                    camera.feetX = goX;
                    camera.feetY = goY;
                    walking = false;
                } else {
                    camera.feetX += dx / left * stepLength;
                    camera.feetY += dy / left * stepLength;
                }
            }
            camera.yaw += yawSpeed * static_cast<float>(kDt);
            now += kDt;
        }
    }

    // The direction of a click from the feet then (map), and of the target from them (map), as an angle between them.
    static float angleBetween(float ax, float ay, float bx, float by) {
        return std::acos(std::clamp((ax * bx + ay * by) / (std::hypot(ax, ay) * std::hypot(bx, by)), -1.0f, 1.0f));
    }
    bool logHas(const std::string& text) const {
        for (const std::string& line : log) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

constexpr float kPi = 3.14159265f;

} // namespace

TEST_CASE("Flee runs from a target beyond the character towards the camera") {
    Game game;
    game.targetX = 0.0f;
    game.targetY = 6.0f; // Up the screen, beyond the character.
    FleeWalker flee;
    flee.start();
    game.run(flee, 0.5);
    REQUIRE_FALSE(game.clicks.empty());
    const Game::Click& first = game.clicks.front();
    const GroundPoint feet = groundToScreen(game.camera, 0.0f, 0.0f);
    CHECK(first.y > feet.y); // Below the feet on the screen: towards the camera.
    CHECK(first.mapY < 0.0f);
    CHECK(first.t < 0.4); // Onto it quickly.
    CHECK(game.logHas("Flee: from Goblin at 280"));
}

TEST_CASE("Flee runs left from a target on the right, and keeps clicking") {
    Game game;
    game.targetX = 4.0f;
    game.targetY = 0.5f;
    game.speed = 0.0f; // Measure the cadence alone (stuck detection blocks a way after 1.5 s).
    FleeWalker flee;
    flee.start();
    game.run(flee, 1.6);
    REQUIRE(game.clicks.size() >= 2);
    for (const Game::Click& click : game.clicks) {
        CHECK(click.x < kWidth * 0.5f);
        CHECK(Game::angleBetween(click.mapX - 0.0f, click.mapY - 0.0f, game.targetX, game.targetY) > 2.5f);
    }
    CHECK(game.clicks[1].t - game.clicks[0].t == doctest::Approx(FleeWalker::kClickEvery).epsilon(0.05));
}

TEST_CASE("Flee keeps moving away while walking") {
    Game game;
    game.targetX = 1.0f;
    game.targetY = 5.0f;
    FleeWalker flee;
    flee.start();
    game.run(flee, 4.0);
    CHECK(game.clicks.size() >= 5);
    const float from = std::hypot(game.targetX, game.targetY);
    const float now = std::hypot(game.targetX - game.camera.feetX, game.targetY - game.camera.feetY);
    CHECK(now > from + 6.0f); // About 3 own heights a second for 4 s, less the start.
    CHECK_FALSE(game.logHas("not moving"));
    CHECK_FALSE(game.done);
}

TEST_CASE("Flee goes away from the camera while the target was never seen") {
    Game game;
    game.targetShown = false;
    FleeWalker flee;
    flee.start();
    game.run(flee, 0.5);
    REQUIRE_FALSE(game.clicks.empty());
    CHECK(game.clicks.front().y < kHeight * 0.5f);
    CHECK(game.logHas("not seen yet, away from the camera"));
}

TEST_CASE("Flee flees from where the target was last seen while the camera turns") {
    Game game;
    game.targetX = 0.0f;
    game.targetY = 6.0f;
    game.speed = 0.0f;
    FleeWalker flee;
    flee.start();
    game.run(flee, 0.3);
    game.targetShown = false;
    game.yawSpeed = 0.3f; // Turns 20 degrees in the next 1.2 s.
    game.run(flee, 1.2);
    CHECK(game.logHas("lost sight of Goblin"));
    REQUIRE(game.clicks.size() >= 2);
    const Game::Click& later = game.clicks.back();
    // Still away from the target on the map, though it shows elsewhere on the screen now.
    CHECK(Game::angleBetween(later.mapX - game.camera.feetX, later.mapY - game.camera.feetY, game.targetX,
                             game.targetY) > kPi - 0.5f);
}

TEST_CASE("Flee turns off a way the interface covers, and never clicks a character") {
    Game game;
    game.targetX = 0.0f;
    game.targetY = 6.0f;
    // The hotbar and the chat under the feet: the way towards the camera is covered.
    const GroundPoint feet = groundToScreen(game.camera, 0.0f, 0.0f);
    game.avoid.push_back({kWidth * 0.3f, feet.y + 10.0f, kWidth * 0.7f, kHeight});
    // A monster left of the feet.
    game.extraCharacters.push_back({feet.x - 600.0f, feet.y - 150.0f, feet.x - 300.0f, feet.y + 150.0f});
    FleeWalker flee;
    flee.start();
    game.run(flee, 0.6);
    REQUIRE_FALSE(game.clicks.empty());
    for (const Game::Click& click : game.clicks) {
        for (const FleeWalker::Box& box : game.avoid) {
            CHECK_FALSE((click.x >= box.x0 && click.x <= box.x1 && click.y >= box.y0 && click.y <= box.y1));
        }
        for (const FleeWalker::Box& box : game.extraCharacters) {
            CHECK_FALSE((click.x >= box.x0 && click.x <= box.x1 && click.y >= box.y0 && click.y <= box.y1));
        }
    }
    CHECK(game.logHas("degrees"));
    // Still away from it: at least 90 degrees off its way.
    const Game::Click& first = game.clicks.front();
    CHECK(Game::angleBetween(first.mapX, first.mapY, game.targetX, game.targetY) >= kPi * 0.5f - 0.01f);
}

TEST_CASE("Flee takes another way when the character does not move") {
    Game game;
    game.targetX = 0.0f;
    game.targetY = 6.0f;
    game.speed = 0.0f;
    FleeWalker flee;
    flee.start();
    game.run(flee, 2.6); // Stuck 1.5 s after the first click (~0.37 s), then the cursor glides to another way.
    CHECK(game.logHas("not moving"));
    REQUIRE(game.clicks.size() >= 4);
    const Game::Click& first = game.clicks.front();
    const Game::Click& later = game.clicks.back();
    CHECK(Game::angleBetween(first.mapX, first.mapY, later.mapX, later.mapY) > FleeWalker::kBlockedSpread);
}

TEST_CASE("Flee stops at a wall the terrain shows") {
    // Flat ground with a wall 2 own heights towards the camera (map y below -2 rises 2 own heights at once).
    auto heights = std::make_unique<GroundHeights>();
    heights->known = true;
    heights->version = 1;
    heights->firstX = -32.0f;
    heights->firstY = -32.0f;
    for (int y = 0; y < GroundHeights::kCells; ++y) {
        const float mapY = heights->firstY + static_cast<float>(y) * GroundHeights::kCell;
        for (int x = 0; x < GroundHeights::kCells; ++x) {
            heights->height[static_cast<std::size_t>(y * GroundHeights::kCells + x)] = mapY < -2.0f ? 2.0f : 0.0f;
        }
    }
    Game game;
    game.targetX = 0.0f;
    game.targetY = 4.0f;
    game.camera.pitch = 0.75f; // Looking down steeply from far: the ground towards the camera shows far enough.
    game.camera.distance = 12.0f;
    game.heights = heights.get();
    game.speed = 0.0f;
    FleeWalker flee;
    flee.start();
    game.run(flee, 0.5);
    REQUIRE_FALSE(game.clicks.empty());
    // Not past the wall: the click's way is cut before it, or it turned aside.
    const Game::Click& first = game.clicks.front();
    CHECK(first.mapY > -2.3f);

    // Without the wall it goes farther towards the camera.
    Game flat;
    flat.targetX = 0.0f;
    flat.targetY = 4.0f;
    flat.camera.pitch = 0.75f;
    flat.camera.distance = 12.0f;
    flat.speed = 0.0f;
    FleeWalker other;
    other.start();
    flat.run(other, 0.5);
    REQUIRE_FALSE(flat.clicks.empty());
    CHECK(flat.clicks.front().mapY < -3.5f);
}

TEST_CASE("Flee is done once there is no target, or it is dead") {
    {
        Game game;
        game.hasTarget = false;
        FleeWalker flee;
        flee.start();
        game.run(flee, 0.4);
        CHECK_FALSE(game.done);
        game.run(flee, 0.3);
        CHECK(game.done);
        CHECK(game.logHas("no target to flee from"));
    }
    {
        Game game;
        game.targetHp = 0.0f;
        FleeWalker flee;
        flee.start();
        game.run(flee, 0.1);
        CHECK(game.done);
        CHECK(game.logHas("Goblin is dead"));
        CHECK(game.clicks.empty());
    }
}

TEST_CASE("Flee waits for a press to be let go before clicking again") {
    Game game;
    FleeWalker flee;
    flee.start();
    game.releaseAt = 10.0; // A press of another task held.
    game.run(flee, 1.0);
    CHECK(game.clicks.empty());
    CHECK(game.last.aiming);
}

TEST_CASE("A Flee task is written to its .brain file and read back") {
    statetree::Tree tree;
    const int fleeing = tree.add(tree.root(), "Run");
    statetree::Task task;
    task.kind = statetree::TaskKind::Flee;
    task.from = statetree::FleeFrom::Target;
    tree.find(fleeing)->tasks.push_back(task);
    const std::string text = statetree::writeTree(tree);
    CHECK(text.find(" flee target\n") != std::string::npos);
    std::string error;
    const std::optional<statetree::Tree> read = statetree::readTree(text, &error);
    REQUIRE(read.has_value());
    const statetree::State* run = nullptr;
    for (const statetree::Row& row : read->rows()) {
        if (read->find(row.state)->name == "Run") {
            run = read->find(row.state);
        }
    }
    REQUIRE(run != nullptr);
    REQUIRE(run->tasks.size() == 1);
    CHECK(run->tasks[0].kind == statetree::TaskKind::Flee);
    CHECK(run->tasks[0].from == statetree::FleeFrom::Target);
    CHECK_FALSE(statetree::canRunParallel(statetree::TaskKind::Flee));
    CHECK(std::string(statetree::nameOf(statetree::TaskKind::Flee)) == "Flee");
}
