#include "core/Text.h"

#include <doctest/doctest.h>

using namespace observex;

TEST_CASE("toLowerAscii only changes A-Z") {
    CHECK(toLowerAscii('A') == 'a');
    CHECK(toLowerAscii('Z') == 'z');
    CHECK(toLowerAscii('a') == 'a');
    CHECK(toLowerAscii('5') == '5');
    CHECK(toLowerAscii('\xC3') == '\xC3'); // first byte of a UTF-8 character
}

TEST_CASE("equalsIgnoreCase") {
    CHECK(equalsIgnoreCase("Warning", "WARNING"));
    CHECK(equalsIgnoreCase("", ""));
    CHECK_FALSE(equalsIgnoreCase("warn", "warning"));
}

TEST_CASE("containsIgnoreCase") {
    CHECK(containsIgnoreCase("Peer connected", "CONNECT"));
    CHECK(containsIgnoreCase("anything", ""));
    CHECK_FALSE(containsIgnoreCase("ab", "abc"));
    CHECK_FALSE(containsIgnoreCase("", "a"));
}
