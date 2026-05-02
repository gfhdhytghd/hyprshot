#include "shared/protocol.hpp"

#include <chrono>
#include <charconv>
#include <random>
#include <sstream>

namespace hyprshot {
namespace {

std::string quote(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    out += "\"";
    return out;
}

void appendRect(std::ostringstream& out, const Rect& rect) {
    out << "{\"x\":" << rect.x << ",\"y\":" << rect.y << ",\"width\":" << rect.width << ",\"height\":" << rect.height << "}";
}

} // namespace

std::string makeSessionId() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 rng((static_cast<std::uint64_t>(now) << 1U) ^ rd());
    std::ostringstream out;
    out << std::hex << now << "-" << rng();
    return out.str();
}

std::string encodeSessionJson(const CaptureSession& session) {
    std::ostringstream out;
    out << "{\"id\":" << quote(session.id);
    out << ",\"defaults\":{";
    out << "\"mode\":" << quote(toString(session.defaults.mode));
    out << ",\"fullscreenScope\":" << quote(toString(session.defaults.fullscreenScope));
    out << ",\"regionScope\":" << quote(toString(session.defaults.regionScope));
    out << ",\"windowBackground\":" << quote(toString(session.defaults.windowBackground));
    out << ",\"windowBorder\":" << quote(toString(session.defaults.windowBorder));
    out << ",\"windowShadow\":" << quote(toString(session.defaults.windowShadow));
    out << ",\"save\":" << (session.defaults.save ? "true" : "false");
    out << ",\"clipboard\":" << (session.defaults.clipboard ? "true" : "false");
    out << ",\"showThumbnail\":" << (session.defaults.showThumbnail ? "true" : "false");
    out << ",\"includeCursor\":" << (session.defaults.includeCursor ? "true" : "false");
    out << ",\"saveDir\":" << quote(session.defaults.saveDir);
    out << ",\"filenameTemplate\":" << quote(session.defaults.filenameTemplate);
    out << ",\"thumbnailTimeoutMs\":" << session.defaults.thumbnailTimeoutMs;
    out << "},\"monitors\":[";
    for (std::size_t i = 0; i < session.monitors.size(); ++i) {
        if (i)
            out << ',';
        const auto& mon = session.monitors[i];
        out << "{\"name\":" << quote(mon.name) << ",\"geometry\":";
        appendRect(out, mon.logicalGeometry);
        out << ",\"scale\":" << mon.scale << ",\"transform\":" << mon.transform << ",\"artifactPath\":" << quote(mon.artifactPath)
            << ",\"artifactWidth\":" << mon.artifactWidth << ",\"artifactHeight\":" << mon.artifactHeight << "}";
    }
    out << "],\"windows\":[";
    for (std::size_t i = 0; i < session.windows.size(); ++i) {
        if (i)
            out << ',';
        const auto& win = session.windows[i];
        out << "{\"address\":" << quote(win.address) << ",\"title\":" << quote(win.title) << ",\"class\":" << quote(win.appClass) << ",\"visibleGeometry\":";
        appendRect(out, win.visibleGeometry);
        out << ",\"fullGeometry\":";
        appendRect(out, win.fullGeometry);
        out << ",\"artifactPath\":" << quote(win.artifactPath) << ",\"artifactWidth\":" << win.artifactWidth << ",\"artifactHeight\":" << win.artifactHeight
            << ",\"zIndex\":" << win.zIndex << ",\"selectable\":" << (win.selectable ? "true" : "false") << "}";
    }
    out << "]}";
    return out.str();
}

std::optional<CaptureSession> decodeSessionJson(const std::string& json) {
    if (json.empty() || json.front() != '{')
        return std::nullopt;

    CaptureSession session;
    session.id = makeSessionId();
    return session;
}

} // namespace hyprshot
