#include "wt/session/Workspace.h"

#include "wt/platform/Files.h"

#include <algorithm>
#include <filesystem>

namespace wt::session {
namespace {

core::Json paneToJson(const PaneState& pane) {
    core::Json json;
    json["profileId"] = pane.profileId;
    json["profile"] = pane.profileJson;
    json["program"] = pane.program;
    core::Json args = core::Json::array();
    for (const auto& arg : pane.arguments) {
        args.push(arg);
    }
    json["arguments"] = std::move(args);
    json["workingDirectory"] = pane.workingDirectory;
    json["title"] = pane.title;
    return json;
}

PaneState paneFromJson(const core::Json& json) {
    PaneState pane;
    pane.profileId = json["profileId"].toString();
    pane.profileJson = json["profile"].toString();
    pane.program = json["program"].toString();
    for (const auto& arg : json["arguments"].items()) {
        pane.arguments.push_back(arg.toString());
    }
    pane.workingDirectory = json["workingDirectory"].toString();
    pane.title = json["title"].toString();
    return pane;
}

core::Json nodeToJson(const LayoutNode& node) {
    core::Json json;
    if (!node.split) {
        json["pane"] = paneToJson(node.pane);
        return json;
    }
    json["orientation"] = node.vertical ? "vertical" : "horizontal";
    core::Json sizes = core::Json::array();
    for (const int size : node.sizes) {
        sizes.push(size);
    }
    json["sizes"] = std::move(sizes);
    core::Json children = core::Json::array();
    for (const auto& child : node.children) {
        children.push(nodeToJson(child));
    }
    json["children"] = std::move(children);
    return json;
}

bool nodeFromJson(const core::Json& json, int depth, LayoutNode& out, std::string& error) {
    if (depth > WorkspaceState::kMaxDepth) {
        error = "layout too deep";
        return false;
    }
    if (!json["pane"].isNull()) {
        out.split = false;
        out.pane = paneFromJson(json["pane"]);
        return true;
    }
    out.split = true;
    out.vertical = json["orientation"].toString() == "vertical";
    const auto& children = json["children"].items();
    if (children.size() < 2 || children.size() > WorkspaceState::kMaxChildren) {
        error = "a split needs 2 to 64 children";
        return false;
    }
    for (const auto& child : children) {
        LayoutNode node;
        if (!nodeFromJson(child, depth + 1, node, error)) {
            return false;
        }
        out.children.push_back(std::move(node));
    }
    for (const auto& size : json["sizes"].items()) {
        out.sizes.push_back(static_cast<int>(std::clamp<std::int64_t>(size.toInt(1), 0, 1000000)));
    }
    if (out.sizes.size() != out.children.size()) {
        out.sizes.assign(out.children.size(), 1); // sizes are a nicety: share the space evenly
    }
    return true;
}

} // namespace

int LayoutNode::paneCount() const {
    if (!split) {
        return 1;
    }
    int count = 0;
    for (const auto& child : children) {
        count += child.paneCount();
    }
    return count;
}

core::Json WorkspaceState::toJson() const {
    core::Json json;
    json["version"] = kVersion;
    core::Json list = core::Json::array();
    for (const WindowState& window : windows) {
        core::Json w;
        w["geometry"] = window.geometry;
        w["currentTab"] = window.currentTab;
        core::Json tabs = core::Json::array();
        for (const TabState& tab : window.tabs) {
            core::Json t;
            t["layout"] = nodeToJson(tab.root);
            t["activePane"] = tab.activePane;
            tabs.push(std::move(t));
        }
        w["tabs"] = std::move(tabs);
        list.push(std::move(w));
    }
    json["windows"] = std::move(list);
    return json;
}

std::optional<WorkspaceState> WorkspaceState::fromJson(const core::Json& json, std::string* error) {
    WorkspaceState state;
    std::string problem;
    for (const auto& w : json["windows"].items()) {
        WindowState window;
        window.geometry = w["geometry"].toString();
        for (const auto& t : w["tabs"].items()) {
            TabState tab;
            if (!nodeFromJson(t["layout"], 0, tab.root, problem)) {
                if (error) {
                    *error = problem;
                }
                return std::nullopt;
            }
            tab.activePane = static_cast<int>(std::clamp<std::int64_t>(t["activePane"].toInt(0), 0, tab.root.paneCount() - 1));
            window.tabs.push_back(std::move(tab));
        }
        const int tabCount = static_cast<int>(window.tabs.size());
        window.currentTab = tabCount == 0 ? 0
                                          : static_cast<int>(std::clamp<std::int64_t>(w["currentTab"].toInt(0), 0, tabCount - 1));
        state.windows.push_back(std::move(window));
    }
    return state;
}

core::Status WorkspaceState::save(std::string_view path) const {
    return platform::writeFileAtomic(path, toJson().dump(1) + "\n", true);
}

std::optional<WorkspaceState> WorkspaceState::load(std::string_view path, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
        if (error) {
            error->clear();
        }
        return std::nullopt;
    }
    std::string problem;
    const auto text = platform::readFile(path, &problem);
    if (!text) {
        if (error) {
            *error = problem;
        }
        return std::nullopt;
    }
    const auto json = core::Json::parse(*text, &problem);
    if (!json) {
        if (error) {
            *error = std::string(path) + ": " + problem;
        }
        return std::nullopt;
    }
    return fromJson(*json, error);
}

} // namespace wt::session
