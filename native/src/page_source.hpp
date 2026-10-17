#pragma once

#include <godot_cpp/variant/string.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weft_godot {
    // A page for a View, as UTF-8: its scripts in order (name, source), its stylesheet and keyframes, the selector of its
    // root (empty: "#root"), whether it uses CRPG Framework's bridge, and the base its relative URLs (url() images)
    // resolve against: the path of its stylesheet ("res://ui/hud/page.css").
    struct PageSource {
        std::vector<std::pair<std::string, std::string>> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string root;
        std::string base;
        bool crpg_bridge = false;
    };

    // The page of a folder ("res://ui/hud", or any path FileAccess reads, as in an exported game's pack): its page.json
    // names the scripts, the stylesheet and keyframes files, the root and whether the page uses the CRPG bridge
    // ({"scripts": ["d3.js", "page.js"], "stylesheet": "page.css", "keyframes": "keyframes.css", "root": "#root",
    // "crpgBridge": false}); without one the folder's page.js, page.css and keyframes.css are the page. false and why.
    bool read_page(const godot::String& folder, PageSource& out, godot::String& why);

    // A page of one script and a stylesheet, its url() images from a folder (empty: none).
    PageSource page_from_source(const godot::String& script, const godot::String& stylesheet, const godot::String& folder);

    // Reads a file a page names by the path weft resolved: a path with a scheme ("res://", "user://") as it is, one from
    // the root ("/ui/x.png") from res://. false when there is none.
    bool read_page_file(std::string_view path, std::string& bytes);
} // namespace weft_godot
