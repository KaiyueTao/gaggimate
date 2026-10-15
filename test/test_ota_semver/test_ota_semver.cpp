// Unit tests: semver_extensions from_string() robustness.
//
// Regression suite for the muse-1.1 hardware boot loop: that build embedded
// BUILD_GIT_VERSION "v-muse-1.1", WebUIPlugin constructs GitHubOTA during
// setup, and the constructor feeds substring(1) == "muse-1.1" into
// from_string(). The unchecked vector::at(2) threw std::out_of_range on the
// two-token string -> uncaught -> abort() -> deterministic boot loop before
// WiFi/BLE. These tests pin the hardened behaviour: malformed input degrades
// to the "no version" result {0,0,0} (same as the empty-string case) instead
// of throwing, and well-formed versions parse exactly as before the guard.
//
// Host-side — pio test -e native -f test_ota_semver.

#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// semver_extensions.cpp renders versions through Arduino's String class and
// the native env has no Arduino runtime, so provide the minimal String the
// translation unit needs (construction from int / const char*,
// concatenation, c_str()). The <Arduino.h> include inside the source
// resolves to the no-op native stub, which defines nothing.
class String {
  public:
    String() = default;
    String(const char *s) : buf(s ? s : "") {}
    String(int v) : buf(std::to_string(v)) {}
    String(const std::string &s) : buf(s) {}

    String operator+(const String &rhs) const { return String(buf + rhs.buf); }
    String operator+(const char *rhs) const { return String(buf + (rhs ? rhs : "")); }
    String &operator+=(const String &rhs) {
        buf += rhs.buf;
        return *this;
    }
    const char *c_str() const { return buf.c_str(); }

  private:
    std::string buf;
};

static String operator+(const char *lhs, const String &rhs) {
    return String(std::string(lhs ? lhs : "") + rhs.c_str());
}

// semver.c itself is compiled as a separate C TU (semver_bridge.c); only the
// C++ extensions source is included here.
#include "semver_extensions.cpp"

void setUp() {}
void tearDown() {}

static void assert_version(const char *input, int major, int minor, int patch, const char *prerelease) {
    semver_t v = from_string(input);
    TEST_ASSERT_EQUAL_INT(major, v.major);
    TEST_ASSERT_EQUAL_INT(minor, v.minor);
    TEST_ASSERT_EQUAL_INT(patch, v.patch);
    TEST_ASSERT_NULL(v.metadata);
    if (prerelease == nullptr) {
        TEST_ASSERT_NULL(v.prerelease);
    } else {
        TEST_ASSERT_NOT_NULL(v.prerelease);
        TEST_ASSERT_EQUAL_STRING(prerelease, v.prerelease);
    }
    semver_free(&v);
}

// The exact incident input: GitHubOTA received "v-muse-1.1" and passed
// substring(1) == "muse-1.1" to from_string(). Two tokens -> the old code
// threw at numbers.at(2). Must now degrade to "no version".
void test_incident_input_muse_1_1() { assert_version("muse-1.1", 0, 0, 0, nullptr); }

// Fewer than three components.
void test_two_components() { assert_version("1.9", 0, 0, 0, nullptr); }

// The pre-existing empty-string guard, unchanged.
void test_empty_string() { assert_version("", 0, 0, 0, nullptr); }

// Three components, none of them numeric.
void test_non_numeric_components() { assert_version("a.b.c", 0, 0, 0, nullptr); }

// Empty components are malformed too (atoi("") would silently read 0).
void test_empty_components() {
    assert_version("1..3", 0, 0, 0, nullptr);
    assert_version("1.9.", 0, 0, 0, nullptr);
    assert_version("1.9.-rc1", 0, 0, 0, nullptr);
}

// Non-numeric patch head with a prerelease tail must not leak a version.
void test_non_numeric_patch() { assert_version("1.9.x-rc1", 0, 0, 0, nullptr); }

// Well-formed versions parse exactly as before the guard: the official
// nightly version the OTA machinery compares against.
void test_official_nightly_version() {
    assert_version("1.9.0-133-g8aebae1d", 1, 9, 0, "133-g8aebae1d");
    semver_t v = from_string("1.9.0-133-g8aebae1d");
    TEST_ASSERT_EQUAL_STRING("1.9.0-133-g8aebae1d", render_to_string(v).c_str());
    semver_free(&v);
}

// The muse-1.2 build version: BUILD_GIT_VERSION "v1.9.0-muse2" reaches
// from_string() as "1.9.0-muse2" — three tokens, same shape as the official
// prerelease form.
void test_muse_1_2_build_version() {
    assert_version("1.9.0-muse2", 1, 9, 0, "muse2");
    semver_t v = from_string("1.9.0-muse2");
    TEST_ASSERT_EQUAL_STRING("1.9.0-muse2", render_to_string(v).c_str());
    semver_free(&v);
}

// Plain three-component release version, unchanged.
void test_plain_release_version() { assert_version("1.9.0", 1, 9, 0, nullptr); }

// OTA safety of the degraded semantics: a device whose local version is
// malformed parses to {0,0,0}, so update_required(latest, local) offers the
// latest release and the local version can never claim to outrank it.
void test_malformed_local_version_never_outranks_latest() {
    semver_t local = from_string("muse-1.1");
    semver_t latest = from_string("1.9.0-133-g8aebae1d");
    TEST_ASSERT_FALSE(local > latest);
    TEST_ASSERT_TRUE(latest > local);
    semver_free(&local);
    semver_free(&latest);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_incident_input_muse_1_1);
    RUN_TEST(test_two_components);
    RUN_TEST(test_empty_string);
    RUN_TEST(test_non_numeric_components);
    RUN_TEST(test_empty_components);
    RUN_TEST(test_non_numeric_patch);
    RUN_TEST(test_official_nightly_version);
    RUN_TEST(test_muse_1_2_build_version);
    RUN_TEST(test_plain_release_version);
    RUN_TEST(test_malformed_local_version_never_outranks_latest);
    return UNITY_END();
}
