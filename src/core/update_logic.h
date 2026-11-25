#pragma once
// Update check against GitHub Releases: version comparison and release selection
// (pure logic, no Arduino). Host test: tests/update_logic_test.cpp.
//
// Release convention (see .github/workflows/release.yml):
//   tag "v0.7.1-beta", assets "firmware.bin" and "firmware.bin.sha256"
//   (sha256sum format: "<64 hex>  firmware.bin"). Tags with a pre-release part
//   ("-beta", "-rc.1") are GitHub pre-releases.

#include <cstddef>
#include <cstdint>
#include <string>

namespace upd {

struct Version {
    bool ok = false;
    int major = 0, minor = 0, patch = 0;
    std::string pre;             // "beta", "rc.1"; empty = final release
};

// Accepts "0.7.0-beta", "v0.7.0-beta", "0.7.0+build" (build metadata ignored).
Version parseVersion(const std::string& s);
// Semantic Versioning 2.0 precedence: <0 if a<b, 0 if equal, >0 if a>b.
int compareVersions(const Version& a, const Version& b);

struct Release {
    std::string tag;             // "v0.7.1-beta"
    std::string version;         // "0.7.1-beta"
    std::string name;            // release title
    std::string notes;           // body, shortened
    std::string htmlUrl;         // release page
    std::string binUrl;          // firmware.bin download
    std::string shaUrl;          // firmware.bin.sha256 download
    uint32_t binSize = 0;
    bool prerelease = false;
};

// Picks the newest installable release from a GitHub "/releases" JSON array:
// not a draft, has both assets, valid version tag, pre-releases only if allowed.
// Returns false (with err) if the JSON cannot be read; returns true with
// out.tag empty if there is no usable release.
bool pickRelease(const char* json, size_t len, bool allowPrerelease, Release& out, std::string& err,
                 size_t maxNotes = 600);

// Parses a sha256sum line ("<64 hex>  firmware.bin" or just the hex).
bool parseSha256(const std::string& text, uint8_t out[32]);

}  // namespace upd
