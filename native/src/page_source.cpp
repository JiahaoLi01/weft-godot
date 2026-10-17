#include "page_source.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

namespace weft_godot {
    namespace {
        std::string utf8(const String& text) {
            const CharString chars = text.utf8();
            return std::string(chars.get_data(), std::size_t(chars.length()));
        }

        String join(const String& folder, const String& name) {
            return folder.ends_with("/") ? folder + name : folder + String("/") + name;
        }

        bool read_text(const String& path, std::string& out, String& why) {
            if (!FileAccess::file_exists(path)) {
                why = path + String(" is missing");
                return false;
            }
            const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
            out.assign(reinterpret_cast<const char*>(bytes.ptr()), std::size_t(bytes.size()));
            // A UTF-8 byte order mark is no part of the text.
            if (out.starts_with("\xEF\xBB\xBF")) {
                out.erase(0, 3);
            }
            return true;
        }

        String text_field(const Dictionary& manifest, const char* name) {
            const Variant value = manifest.get(name, Variant());
            return value.get_type() == Variant::STRING ? String(value) : String();
        }
    } // namespace

    bool read_page(const String& folder, PageSource& out, String& why) {
        out = {};
        if (folder.is_empty()) {
            why = "a page needs a folder";
            return false;
        }
        const String manifest_path = join(folder, "page.json");
        String stylesheet = "page.css";
        if (FileAccess::file_exists(manifest_path)) {
            std::string text;
            if (!read_text(manifest_path, text, why)) {
                return false;
            }
            const Variant parsed = JSON::parse_string(String::utf8(text.data(), std::int64_t(text.size())));
            if (parsed.get_type() != Variant::DICTIONARY) {
                why = manifest_path + String(" is not a JSON object");
                return false;
            }
            const Dictionary manifest = parsed;
            const Variant scripts = manifest.get("scripts", Variant());
            if (scripts.get_type() == Variant::ARRAY) {
                const Array names = scripts;
                for (std::int64_t i = 0; i < names.size(); ++i) {
                    const String name = names[i];
                    std::string source;
                    if (!read_text(join(folder, name), source, why)) {
                        return false;
                    }
                    out.scripts.emplace_back(utf8(name), std::move(source));
                }
            }
            if (const String file = text_field(manifest, "stylesheet"); !file.is_empty()) {
                stylesheet = file;
                if (!read_text(join(folder, file), out.stylesheet, why)) {
                    return false;
                }
            }
            if (const String file = text_field(manifest, "keyframes"); !file.is_empty()) {
                if (!read_text(join(folder, file), out.keyframes, why)) {
                    return false;
                }
            }
            out.root = utf8(text_field(manifest, "root"));
            out.crpg_bridge = bool(manifest.get("crpgBridge", false));
            if (out.scripts.empty()) {
                why = manifest_path + String(" names no script");
                return false;
            }
        } else {
            std::string source;
            if (!read_text(join(folder, "page.js"), source, why)) {
                why = folder + String(" has neither page.json nor page.js");
                return false;
            }
            out.scripts.emplace_back("page.js", std::move(source));
            String ignored;
            if (FileAccess::file_exists(join(folder, "page.css"))) {
                read_text(join(folder, "page.css"), out.stylesheet, ignored);
            }
            if (FileAccess::file_exists(join(folder, "keyframes.css"))) {
                read_text(join(folder, "keyframes.css"), out.keyframes, ignored);
            }
        }
        out.base = utf8(join(folder, stylesheet));
        return true;
    }

    PageSource page_from_source(const String& script, const String& stylesheet, const String& folder) {
        PageSource page;
        page.scripts.emplace_back("page.js", utf8(script));
        page.stylesheet = utf8(stylesheet);
        if (!folder.is_empty()) {
            page.base = utf8(join(folder, "page.css"));
        }
        return page;
    }

    bool read_page_file(std::string_view path, std::string& bytes) {
        String file = String::utf8(path.data(), std::int64_t(path.size()));
        if (file.begins_with("/") && !file.begins_with("//")) {
            file = "res:/" + file;
        } else if (file.find("://") < 0) {
            file = "res://" + file;
        }
        if (!FileAccess::file_exists(file)) {
            return false;
        }
        const PackedByteArray read = FileAccess::get_file_as_bytes(file);
        bytes.assign(reinterpret_cast<const char*>(read.ptr()), std::size_t(read.size()));
        return true;
    }
} // namespace weft_godot
