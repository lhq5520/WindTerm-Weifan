#pragma once

#include "wt/core/Json.h"
#include "wt/core/Status.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::session {

// What a pane showed, enough to open it again.
struct PaneState {
    std::string profileId;   // a saved session
    std::string profileJson; // an unsaved session's profile (no secrets: they are referenced by id)
    std::string program;     // local shell (empty: the login shell)
    std::vector<std::string> arguments;
    std::string workingDirectory; // last directory reported by the shell (OSC 7), else the start one
    std::string title;

    friend bool operator==(const PaneState&, const PaneState&) = default;
};

// A tab's split layout: a pane, or a splitter with two or more children.
struct LayoutNode {
    bool split = false;
    PaneState pane;               // !split
    bool vertical = false;        // split: children stacked top to bottom (else side by side)
    std::vector<int> sizes;       // split: relative sizes, one per child
    std::vector<LayoutNode> children;

    int paneCount() const;
    friend bool operator==(const LayoutNode&, const LayoutNode&) = default;
};

struct TabState {
    LayoutNode root;
    int activePane = 0; // index in depth-first order

    friend bool operator==(const TabState&, const TabState&) = default;
};

struct WindowState {
    std::string geometry; // QWidget::saveGeometry(), base64
    std::vector<TabState> tabs;
    int currentTab = 0;

    friend bool operator==(const WindowState&, const WindowState&) = default;
};

// The open windows when the application quit (workspace.json), restored on the next start.
struct WorkspaceState {
    static constexpr int kVersion = 1;
    static constexpr int kMaxDepth = 16;
    static constexpr std::size_t kMaxChildren = 64;

    std::vector<WindowState> windows;

    core::Json toJson() const;
    // Rejects malformed layouts (too deep, sizes not matching children, splits with < 2 children).
    static std::optional<WorkspaceState> fromJson(const core::Json& json, std::string* error = nullptr);
    core::Status save(std::string_view path) const;
    // nullopt (with an empty error) when there is no file.
    static std::optional<WorkspaceState> load(std::string_view path, std::string* error = nullptr);

    friend bool operator==(const WorkspaceState&, const WorkspaceState&) = default;
};

} // namespace wt::session
