#include "core/ModelInfo.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace observex;

namespace {

// Two kinds over three numbers, with numbers that need every digit to read back exactly.
KindHead someHead() {
    KindHead head;
    head.biases = {-4.5f, 1.0f / 3.0f};
    head.weights = {0.1f, -1.1920929e-07f, 123456.78f, 2.0f / 3.0f, 0.0f, -3.4028235e+38f};
    return head;
}

} // namespace

TEST_CASE("parseModelKinds reads the kinds train.py writes, in order") {
    const std::vector<std::string> kinds = parseModelKinds("{\n  \"category\": \"character\",\n  \"kinds\": [\n"
                                                           "    \"monster\",\n    \"player\",\n    \"npc\",\n"
                                                           "    \"own\"\n  ],\n  \"epochs\": 40\n}");
    REQUIRE(kinds.size() == 4);
    CHECK(kinds[0] == "monster");
    CHECK(kinds[1] == "player");
    CHECK(kinds[2] == "npc");
    CHECK(kinds[3] == "own");
    CHECK(parseModelKinds("{\"kinds\":[\"a\",\"b\"]}") == std::vector<std::string>{"a", "b"});
}

TEST_CASE("parseModelKinds is empty for a model of one category or a list it cannot read") {
    CHECK(parseModelKinds("{\"category\": \"character\", \"epochs\": 40}").empty());
    CHECK(parseModelKinds("").empty());
    CHECK(parseModelKinds("{\"kinds\": \"monster\"}").empty());
    CHECK(parseModelKinds("{\"kinds\": [\"monster\", 3]}").empty());
    CHECK(parseModelKinds("{\"kinds\": [\"monster\"").empty());
    CHECK(parseModelKinds("{\"kinds\": [\"\"]}").empty());
    CHECK(parseModelKinds("{\"kinds\": []}").empty());
}

TEST_CASE("a model fits a category it knows the first kinds of, in order") {
    constexpr std::string_view category[] = {"monster", "player", "npc", "own", "unknown", "dead"};
    CHECK(kindsFit({"monster", "player", "npc", "own", "unknown", "dead"}, category));
    CHECK(kindsFit({"monster", "player", "npc", "own"}, category)); // Trained before "can't tell" and "dead" came.
    CHECK(kindsFit({"monster", "player", "npc", "own", "unknown"}, category));
    CHECK_FALSE(kindsFit({"player", "monster", "npc", "own"}, category));
    CHECK_FALSE(kindsFit({"monster", "player", "npc", "own", "dead"}, category)); // Its fifth is not the fifth.
    CHECK_FALSE(kindsFit({"monster", "player", "npc", "own", "unknown", "dead", "pet"}, category));
    CHECK_FALSE(kindsFit({"monster", "npc"}, category));
    CHECK_FALSE(kindsFit({}, category));
    CHECK_FALSE(kindsFit({"monster"}, {}));
}

TEST_CASE("parseKindHead reads the kind classifier export.py writes") {
    const KindHead head = parseKindHead("{\n  \"kinds\": [\"monster\", \"player\"],\n  \"kind_head\": {\n"
                                        "    \"biases\": [\n      -4.5,\n      0.25\n    ],\n"
                                        "    \"weights\": [\n      [\n        0.5,\n        -1e-05,\n        2\n      ],\n"
                                        "      [1.1920928955078125e-07, 0, -3.0]\n    ]\n  }\n}");
    CHECK(head.kinds() == 2);
    CHECK(head.features() == 3);
    CHECK(head.biases == std::vector<float>{-4.5f, 0.25f});
    CHECK(head.weights == std::vector<float>{0.5f, -1e-05f, 2.0f, 1.1920928955078125e-07f, 0.0f, -3.0f});
    CHECK(parseModelKinds("{\"kinds\": [\"monster\"], \"kind_head\": {\"biases\": [1], \"weights\": [[2]]}}") ==
          std::vector<std::string>{"monster"});
}

TEST_CASE("parseKindHead is empty without one, or one it cannot read") {
    CHECK(parseKindHead("{\"kinds\": [\"monster\", \"player\"]}").kinds() == 0);
    CHECK(parseKindHead("").kinds() == 0);
    // Rows of different lengths, a row too few or too many, a word for a number, cut short.
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1, 2], \"weights\": [[1, 2], [3]]}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1, 2], \"weights\": [[1, 2]]}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1], \"weights\": [[1, 2], [3, 4]]}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1], \"weights\": [[1, NaN]]}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [], \"weights\": []}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1], \"weights\": [[]]}}").kinds() == 0);
    CHECK(parseKindHead("{\"kind_head\": {\"biases\": [1], \"weights\": [[1, 2").kinds() == 0);
}

TEST_CASE("the kind update file reads back exactly what was written") {
    KindUpdate update;
    update.head = someHead();
    update.answers = 10234;
    update.recordings = 27;
    const std::string text = formatKindUpdate(update);
    const KindUpdate read = parseKindUpdate(text);
    CHECK(read.head == update.head); // Every float bit for bit.
    CHECK(read.answers == 10234u);
    CHECK(read.recordings == 27u);
    CHECK(parseKindHead(text) == update.head); // The same head format as model.json's.
    CHECK(text.find("\"answers\": 10234") != std::string::npos);
}

TEST_CASE("the kind update file without counts or with a head it cannot read") {
    const KindUpdate counts = parseKindUpdate("{\"kind_head\": {\"biases\": [1], \"weights\": [[2, 3]]}}");
    CHECK(counts.head.kinds() == 1);
    CHECK(counts.answers == 0u);
    CHECK(counts.recordings == 0u);
    const KindUpdate broken =
        parseKindUpdate("{\"answers\": 12, \"kind_head\": {\"biases\": [1], \"weights\": [[2, \"x\"]]}}");
    CHECK(broken.head.kinds() == 0);
    CHECK(broken.answers == 12u);
}

TEST_CASE("a kind update is used only when it is as new as the model and of its head's shape") {
    namespace fs = std::filesystem;
    const fs::path model = fs::temp_directory_path() /
                           ("observex-tests-update-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(model);
    KindUpdate own;
    own.head = someHead();
    own.head.biases = {0.5f, 0.25f}; // The model's own differs from the update's.
    std::ofstream(model / "model.json", std::ios::binary)
        << "{\"kinds\": [\"monster\", \"player\"], " << formatKindUpdate(own).substr(1);
    std::ofstream(model / "model.onnx", std::ios::binary) << "onnx";
    KindUpdate update;
    update.head = someHead();
    update.answers = 512;
    update.recordings = 3;
    std::ofstream(model / kKindUpdateFile, std::ios::binary) << formatKindUpdate(update);
    const fs::file_time_type written = fs::last_write_time(model / kKindUpdateFile);

    fs::last_write_time(model / "model.onnx", written - std::chrono::hours(1));
    CHECK(loadKindUpdate(model).head == update.head);
    CHECK(loadKindUpdate(model).answers == 512u);
    CHECK(kindHeadToUse(model) == update.head);
    fs::last_write_time(model / "model.onnx", written); // Exported in the same instant: still as new.
    CHECK(loadKindUpdate(model).head == update.head);

    fs::last_write_time(model / "model.onnx", written + std::chrono::hours(1)); // Trained again since.
    CHECK(loadKindUpdate(model).head.kinds() == 0);
    CHECK(kindHeadToUse(model) == own.head);

    fs::last_write_time(model / "model.onnx", written - std::chrono::hours(1));
    KindUpdate wider = update;
    wider.head.weights.insert(wider.head.weights.end(), {1.0f, 2.0f}); // Four numbers per kind, not three.
    std::ofstream(model / kKindUpdateFile, std::ios::binary) << formatKindUpdate(wider);
    fs::last_write_time(model / kKindUpdateFile, written);
    CHECK(loadKindUpdate(model).head.kinds() == 0);
    CHECK(kindHeadToUse(model) == own.head);

    fs::remove(model / kKindUpdateFile);
    CHECK(loadKindUpdate(model).head.kinds() == 0);
    CHECK(kindHeadToUse(model) == own.head);
    fs::remove(model / "model.json");
    CHECK(kindHeadToUse(model).kinds() == 0);
    fs::remove_all(model);
}
