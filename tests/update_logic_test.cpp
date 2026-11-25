// Host test for src/core/update_logic.cpp (GitHub release check).
// Build: c++ -std=c++17 -fsanitize=address,undefined -I.pio/libdeps/linkra-s3-breadboard/ArduinoJson/src \
//        tests/update_logic_test.cpp src/core/update_logic.cpp -o /tmp/upd_test && /tmp/upd_test
#include "../src/core/update_logic.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int cmp(const char* a, const char* b) {
    return upd::compareVersions(upd::parseVersion(a), upd::parseVersion(b));
}

static void testParse() {
    auto v = upd::parseVersion("v0.7.0-beta");
    CHECK(v.ok && v.major == 0 && v.minor == 7 && v.patch == 0 && v.pre == "beta");
    v = upd::parseVersion("1.2.3+build.5");
    CHECK(v.ok && v.patch == 3 && v.pre.empty());
    CHECK(upd::parseVersion("10.20.30-rc.1").ok);
    CHECK(!upd::parseVersion("").ok);
    CHECK(!upd::parseVersion("1.2").ok);
    CHECK(!upd::parseVersion("1.2.3.4").ok);
    CHECK(!upd::parseVersion("01.2.3").ok);
    CHECK(!upd::parseVersion("1.2.x").ok);
    CHECK(!upd::parseVersion("1.2.3-").ok);
    CHECK(!upd::parseVersion("1.2.3-be ta").ok);
    CHECK(!upd::parseVersion("latest").ok);
}

static void testCompare() {
    // SemVer 2.0 §11 example chain
    const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                           "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (size_t i = 0; i + 1 < sizeof(chain) / sizeof(*chain); i++) {
        CHECK(cmp(chain[i], chain[i + 1]) < 0);
        CHECK(cmp(chain[i + 1], chain[i]) > 0);
    }
    CHECK(cmp("0.7.0-beta", "v0.7.0-beta") == 0);
    CHECK(cmp("0.7.0-beta", "0.7.1-beta") < 0);
    CHECK(cmp("0.7.0", "0.7.1-beta") < 0);
    CHECK(cmp("0.10.0", "0.9.9") > 0);
    CHECK(cmp("1.0.0+a", "1.0.0+b") == 0);
}

static const char* kReleases = R"([
  {"tag_name":"v0.9.0-beta","name":"Entwurf","draft":true,"prerelease":true,"body":"",
   "html_url":"https://example/draft","assets":[{"name":"firmware.bin","browser_download_url":"https://example/d.bin","size":1},
   {"name":"firmware.bin.sha256","browser_download_url":"https://example/d.sha","size":80}]},
  {"tag_name":"v0.8.0-beta","name":"Ohne Prüfsumme","draft":false,"prerelease":true,"body":"",
   "html_url":"https://example/nosha","assets":[{"name":"firmware.bin","browser_download_url":"https://example/n.bin","size":1}]},
  {"tag_name":"v0.7.2-beta","name":"Linkra Duo 0.7.2-beta","draft":false,"prerelease":true,"body":"Änderungen: viele",
   "html_url":"https://example/072","uploader":{"login":"x","id":1},
   "assets":[{"name":"firmware.bin","browser_download_url":"https://example/072.bin","size":2434048,"uploader":{"login":"x"}},
             {"name":"firmware.bin.sha256","browser_download_url":"https://example/072.sha","size":80}]},
  {"tag_name":"v0.7.1","name":"Stabil","draft":false,"prerelease":false,"body":"stabil",
   "html_url":"https://example/071","assets":[{"name":"firmware.bin","browser_download_url":"https://example/071.bin","size":2000000},
   {"name":"firmware.bin.sha256","browser_download_url":"https://example/071.sha","size":80}]},
  {"tag_name":"nightly","name":"kein Versions-Tag","draft":false,"prerelease":false,"body":"",
   "html_url":"https://example/n","assets":[{"name":"firmware.bin","browser_download_url":"https://example/x.bin","size":1},
   {"name":"firmware.bin.sha256","browser_download_url":"https://example/x.sha","size":80}]}
])";

static void testPick() {
    upd::Release r;
    std::string err;
    CHECK(upd::pickRelease(kReleases, strlen(kReleases), true, r, err));
    CHECK(r.tag == "v0.7.2-beta");
    CHECK(r.version == "0.7.2-beta");
    CHECK(r.binUrl == "https://example/072.bin");
    CHECK(r.shaUrl == "https://example/072.sha");
    CHECK(r.binSize == 2434048);
    CHECK(r.prerelease);
    CHECK(r.notes == "Änderungen: viele");

    CHECK(upd::pickRelease(kReleases, strlen(kReleases), false, r, err));
    CHECK(r.tag == "v0.7.1");
    CHECK(!r.prerelease);

    // notes are shortened without cutting a UTF-8 sequence
    CHECK(upd::pickRelease(kReleases, strlen(kReleases), true, r, err, 1));
    CHECK(r.notes == "…");

    const char* empty = "[]";
    CHECK(upd::pickRelease(empty, 2, true, r, err));
    CHECK(r.tag.empty());

    const char* bad = "{\"message\":\"API rate limit exceeded\"}";
    CHECK(!upd::pickRelease(bad, strlen(bad), true, r, err));
    CHECK(!err.empty());
    CHECK(!upd::pickRelease("[{", 2, true, r, err));
}

static void testSha() {
    uint8_t h[32];
    const std::string hex = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    CHECK(upd::parseSha256(hex + "  firmware.bin\n", h));
    CHECK(h[0] == 0x9f && h[31] == 0x08);
    CHECK(upd::parseSha256(hex, h));
    CHECK(upd::parseSha256("  " + hex + "\n", h));
    std::string upper = hex;
    for (auto& c : upper) c = (char)toupper(c);
    CHECK(upd::parseSha256(upper, h) && h[0] == 0x9f);
    CHECK(!upd::parseSha256(hex.substr(0, 63), h));
    CHECK(!upd::parseSha256(hex + "0", h));
    CHECK(!upd::parseSha256("zz" + hex.substr(2), h));
    CHECK(!upd::parseSha256("", h));
}

int main() {
    testParse();
    testCompare();
    testPick();
    testSha();
    if (failures) { std::printf("%d Fehler\n", failures); return 1; }
    std::printf("update_logic: alle Tests ok\n");
    return 0;
}
