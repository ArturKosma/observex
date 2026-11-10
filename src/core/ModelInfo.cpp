#include "core/ModelInfo.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace observex {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSpaces = " \t\r\n";

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Appends `value` in as few digits as read back to exactly the same float ("0.25", "-1.1920929e-07").
void appendNumber(std::string& text, float value) {
    char number[32];
    const auto [end, error] = std::to_chars(number, number + sizeof(number), value);
    text.append(number, error == std::errc() ? end : number);
}

void appendCount(std::string& text, std::size_t count) {
    char number[24];
    const auto [end, error] = std::to_chars(number, number + sizeof(number), count);
    text.append(number, error == std::errc() ? end : number);
}

// Where the value of `key` (quotes included) starts, searching from `from`: just after its ':'. Or npos.
std::size_t valueOf(std::string_view json, std::string_view key, std::size_t from) {
    std::size_t at = json.find(key, from);
    if (at == std::string_view::npos) {
        return at;
    }
    at = json.find_first_not_of(kSpaces, at + key.size());
    return at != std::string_view::npos && json[at] == ':' ? at + 1 : std::string_view::npos;
}

// Reads a list of finite numbers ("[0.5, -1e-05]") from `at` into `numbers`. Returns where it ends (after its ']'),
// or npos.
std::size_t readNumbers(std::string_view json, std::size_t at, std::vector<float>& numbers) {
    at = json.find_first_not_of(kSpaces, at);
    if (at == std::string_view::npos || json[at] != '[') {
        return std::string_view::npos;
    }
    for (bool first = true;; first = false) {
        at = json.find_first_not_of(kSpaces, at + (first ? 1 : 0));
        if (at != std::string_view::npos && json[at] == ']') {
            return at + 1;
        }
        if (!first) {
            at = at != std::string_view::npos && json[at] == ',' ? json.find_first_not_of(kSpaces, at + 1)
                                                                 : std::string_view::npos;
        }
        if (at == std::string_view::npos) {
            return at;
        }
        float value = 0.0f;
        const auto [end, error] = std::from_chars(json.data() + at, json.data() + json.size(), value);
        if (error != std::errc() || !std::isfinite(value)) {
            return std::string_view::npos;
        }
        numbers.push_back(value);
        at = static_cast<std::size_t>(end - json.data());
    }
}

} // namespace

bool kindsFit(const std::vector<std::string>& modelKinds, std::span<const std::string_view> categoryKinds) {
    return !modelKinds.empty() && modelKinds.size() <= categoryKinds.size() &&
           std::equal(modelKinds.begin(), modelKinds.end(), categoryKinds.begin());
}

std::vector<std::string> parseModelKinds(std::string_view modelJson) {
    // Enough JSON for a list of plain words: train.py writes kinds without quotes or backslashes in them.
    std::size_t at = modelJson.find("\"kinds\"");
    if (at == std::string_view::npos) {
        return {};
    }
    at = modelJson.find_first_not_of(" \t\r\n", at + 7);
    if (at == std::string_view::npos || modelJson[at] != ':') {
        return {};
    }
    at = modelJson.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string_view::npos || modelJson[at] != '[') {
        return {};
    }
    std::vector<std::string> kinds;
    for (;;) {
        at = modelJson.find_first_not_of(" \t\r\n,", at + 1);
        if (at == std::string_view::npos) {
            return {};
        }
        if (modelJson[at] == ']') {
            return kinds;
        }
        const std::size_t end = modelJson[at] == '"' ? modelJson.find('"', at + 1) : std::string_view::npos;
        if (end == std::string_view::npos || end == at + 1) {
            return {};
        }
        kinds.emplace_back(modelJson.substr(at + 1, end - at - 1));
        at = end;
    }
}

KindHead parseKindHead(std::string_view modelJson) {
    // Enough JSON for what export.py writes: two lists of numbers inside "kind_head".
    const std::size_t head = modelJson.find("\"kind_head\"");
    if (head == std::string_view::npos) {
        return {};
    }
    KindHead parsed;
    std::size_t at = valueOf(modelJson, "\"biases\"", head);
    if (at == std::string_view::npos || readNumbers(modelJson, at, parsed.biases) == std::string_view::npos ||
        parsed.biases.empty()) {
        return {};
    }
    // The weights: a list of rows, each a list of numbers, as many rows as biases, all as long.
    at = valueOf(modelJson, "\"weights\"", head);
    at = at == std::string_view::npos ? at : modelJson.find_first_not_of(kSpaces, at);
    if (at == std::string_view::npos || modelJson[at] != '[') {
        return {};
    }
    ++at;
    std::size_t rowLength = 0;
    for (std::size_t row = 0; row < parsed.biases.size(); ++row) {
        if (row > 0) {
            at = modelJson.find_first_not_of(kSpaces, at);
            if (at == std::string_view::npos || modelJson[at] != ',') {
                return {};
            }
            ++at;
        }
        at = readNumbers(modelJson, at, parsed.weights);
        rowLength = row == 0 ? parsed.weights.size() : rowLength;
        if (at == std::string_view::npos || rowLength == 0 || parsed.weights.size() != (row + 1) * rowLength) {
            return {};
        }
    }
    at = modelJson.find_first_not_of(kSpaces, at);
    if (at == std::string_view::npos || modelJson[at] != ']') {
        return {};
    }
    return parsed;
}

std::string formatKindUpdate(const KindUpdate& update) {
    const KindHead& head = update.head;
    const std::size_t size = static_cast<std::size_t>(head.features());
    std::string text = "{\n  \"answers\": ";
    appendCount(text, update.answers);
    text += ",\n  \"recordings\": ";
    appendCount(text, update.recordings);
    text += ",\n  \"kind_head\": {\n    \"biases\": [";
    for (std::size_t kind = 0; kind < head.biases.size(); ++kind) {
        text += kind > 0 ? ", " : "";
        appendNumber(text, head.biases[kind]);
    }
    text += "],\n    \"weights\": [";
    for (std::size_t kind = 0; kind < head.biases.size(); ++kind) {
        text += kind > 0 ? ",\n      [" : "\n      [";
        for (std::size_t i = 0; i < size; ++i) {
            text += i > 0 ? ", " : "";
            appendNumber(text, head.weights[kind * size + i]);
        }
        text += ']';
    }
    text += "\n    ]\n  }\n}\n";
    return text;
}

KindUpdate parseKindUpdate(std::string_view json) {
    KindUpdate update;
    update.head = parseKindHead(json);
    const auto count = [&](std::string_view key) {
        std::size_t value = 0;
        std::size_t at = valueOf(json, key, 0);
        at = at == std::string_view::npos ? at : json.find_first_not_of(kSpaces, at);
        if (at != std::string_view::npos) {
            std::from_chars(json.data() + at, json.data() + json.size(), value);
        }
        return value;
    };
    update.answers = count("\"answers\"");
    update.recordings = count("\"recordings\"");
    return update;
}

KindUpdate loadKindUpdate(const fs::path& modelFolder) {
    const fs::path file = modelFolder / kKindUpdateFile;
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(file, error);
    if (error) {
        return {};
    }
    const fs::file_time_type trained = fs::last_write_time(modelFolder / L"model.onnx", error);
    if (error || written < trained) {
        return {};
    }
    KindUpdate update = parseKindUpdate(readFile(file));
    const KindHead own = parseKindHead(readFile(modelFolder / L"model.json"));
    if (own.kinds() == 0 || update.head.kinds() != own.kinds() || update.head.features() != own.features()) {
        return {};
    }
    return update;
}

KindHead kindHeadToUse(const fs::path& modelFolder) {
    KindUpdate update = loadKindUpdate(modelFolder);
    return update.head.kinds() > 0 ? std::move(update.head) : parseKindHead(readFile(modelFolder / L"model.json"));
}

} // namespace observex
