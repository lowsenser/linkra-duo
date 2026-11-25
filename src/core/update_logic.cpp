#include "update_logic.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace upd {

static bool parseNum(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    out = atoi(s.c_str());
    return true;
}

Version parseVersion(const std::string& in) {
    Version v;
    std::string s = in;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    const size_t plus = s.find('+');
    if (plus != std::string::npos) s.erase(plus);
    const size_t dash = s.find('-');
    std::string core = s.substr(0, dash);
    if (dash != std::string::npos) {
        v.pre = s.substr(dash + 1);
        if (v.pre.empty()) return v;
        for (char c : v.pre)
            if (!isalnum((unsigned char)c) && c != '.' && c != '-') return v;
    }
    int parts[3];
    size_t pos = 0;
    for (int i = 0; i < 3; i++) {
        const size_t dot = core.find('.', pos);
        if ((i < 2) == (dot == std::string::npos)) return v;   // exactly two dots
        if (!parseNum(core.substr(pos, i < 2 ? dot - pos : std::string::npos), parts[i])) return v;
        pos = dot + 1;
    }
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.ok = true;
    return v;
}

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t dot = s.find('.', pos);
        out.push_back(s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos));
        if (dot == std::string::npos) return out;
        pos = dot + 1;
    }
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;    // final release > any pre-release
    if (b.pre.empty()) return -1;
    const auto pa = split(a.pre), pb = split(b.pre);
    for (size_t i = 0; i < pa.size() && i < pb.size(); i++) {
        int na, nb;
        const bool da = parseNum(pa[i], na), db = parseNum(pb[i], nb);
        if (da && db) { if (na != nb) return na < nb ? -1 : 1; continue; }
        if (da != db) return da ? -1 : 1;   // numeric identifiers sort first
        const int c = pa[i].compare(pb[i]);
        if (c) return c < 0 ? -1 : 1;
    }
    if (pa.size() == pb.size()) return 0;
    return pa.size() < pb.size() ? -1 : 1;
}

bool pickRelease(const char* json, size_t len, bool allowPrerelease, Release& out, std::string& err,
                 size_t maxNotes) {
    out = Release();
    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    f["tag_name"] = true;
    f["name"] = true;
    f["body"] = true;
    f["draft"] = true;
    f["prerelease"] = true;
    f["html_url"] = true;
    JsonObject fa = f["assets"][0].to<JsonObject>();
    fa["name"] = true;
    fa["browser_download_url"] = true;
    fa["size"] = true;

    JsonDocument doc;
    const auto e = deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
    if (e) { err = std::string("JSON: ") + e.c_str(); return false; }
    if (!doc.is<JsonArray>()) { err = "Antwort ist keine Release-Liste"; return false; }

    Version best;
    for (JsonObject r : doc.as<JsonArray>()) {
        if (r["draft"] | false) continue;
        const bool pre = r["prerelease"] | false;
        if (pre && !allowPrerelease) continue;
        const std::string tag = r["tag_name"] | "";
        const Version v = parseVersion(tag);
        if (!v.ok) continue;
        std::string bin, sha;
        uint32_t size = 0;
        for (JsonObject a : r["assets"].as<JsonArray>()) {
            const std::string n = a["name"] | "";
            if (n == "firmware.bin") { bin = a["browser_download_url"] | ""; size = a["size"] | 0u; }
            else if (n == "firmware.bin.sha256") sha = a["browser_download_url"] | "";
        }
        if (bin.empty() || sha.empty()) continue;
        if (best.ok && compareVersions(v, best) <= 0) continue;
        best = v;
        out.tag = tag;
        out.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
        out.name = r["name"] | "";
        out.notes = r["body"] | "";
        if (out.notes.size() > maxNotes) {
            size_t cut = maxNotes;
            while (cut > 0 && (out.notes[cut] & 0xC0) == 0x80) cut--;   // keep UTF-8 intact
            out.notes = out.notes.substr(0, cut) + "…";
        }
        out.htmlUrl = r["html_url"] | "";
        out.binUrl = bin;
        out.shaUrl = sha;
        out.binSize = size;
        out.prerelease = pre;
    }
    return true;
}

bool parseSha256(const std::string& text, uint8_t out[32]) {
    size_t i = 0;
    while (i < text.size() && isspace((unsigned char)text[i])) i++;
    if (text.size() - i < 64) return false;
    for (int k = 0; k < 32; k++) {
        int b = 0;
        for (int h = 0; h < 2; h++) {
            const char c = (char)tolower((unsigned char)text[i + k * 2 + h]);
            if (c >= '0' && c <= '9') b = b * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') b = b * 16 + (c - 'a' + 10);
            else return false;
        }
        out[k] = (uint8_t)b;
    }
    const size_t end = i + 64;
    return end == text.size() || isspace((unsigned char)text[end]);
}

}  // namespace upd
