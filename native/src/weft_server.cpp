#include "weft_server.hpp"

#include "page_host.hpp"
#include "page_source.hpp"
#include "render_bridge.hpp"
#include "render_thread.hpp"
#include "results.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace weft_godot {
    namespace {
        // Runs work on the owner thread and returns its result.
        template <typename Work>
        weft_result on_owner(Work&& work) {
            weft_result result = WEFT_INTERNAL_ERROR;
            PageHost::get().call([&] { result = work(); });
            return result;
        }

        CharString utf8(const String& text) {
            return text.utf8();
        }

        // A font file weft can open by its path: a file on disk as it is, one in the game's pack (or imported, as
        // Godot imports fonts) written out to user://weft/fonts.
        Error font_file(const String& path, String& out) {
            const String global = ProjectSettings::get_singleton()->globalize_path(path);
            if (!global.contains("://") && FileAccess::file_exists(global)) {
                out = global;
                return OK;
            }
            PackedByteArray bytes;
            if (FileAccess::file_exists(path)) {
                bytes = FileAccess::get_file_as_bytes(path);
            }
            if (bytes.is_empty() && ResourceLoader::get_singleton()->exists(path)) {
                const Ref<FontFile> font = ResourceLoader::get_singleton()->load(path);
                if (font.is_valid()) {
                    bytes = font->get_data();
                }
            }
            if (bytes.is_empty()) {
                return ERR_FILE_NOT_FOUND;
            }
            const String folder = "user://weft/fonts";
            DirAccess::make_dir_recursive_absolute(folder);
            const String copy = folder + String("/") + String::num_uint64(uint64_t(path.hash())) + String("-") + path.get_file();
            const Ref<FileAccess> file = FileAccess::open(copy, FileAccess::WRITE);
            if (file.is_null() || !file->store_buffer(bytes)) {
                return ERR_FILE_CANT_WRITE;
            }
            out = ProjectSettings::get_singleton()->globalize_path(copy);
            return OK;
        }

        Error report(const char* what, weft_result result) {
            if (result != WEFT_OK) {
                UtilityFunctions::push_error(failure(what, result));
            }
            return godot_error(result);
        }
    } // namespace

    Weft* Weft::singleton_ = nullptr;

    Weft* Weft::get_singleton() {
        return singleton_;
    }

    Weft::Weft() {
        singleton_ = this;
        PageHost::get().set_reader(&read_page_file);
    }

    Weft::~Weft() {
        if (singleton_ == this) {
            singleton_ = nullptr;
        }
    }

    void Weft::_bind_methods() {
        ClassDB::bind_method(D_METHOD("start", "system_fonts", "max_views"), &Weft::start, DEFVAL(true), DEFVAL(0));
        ClassDB::bind_method(D_METHOD("stop"), &Weft::stop);
        ClassDB::bind_method(D_METHOD("is_running"), &Weft::is_running);
        ClassDB::bind_method(D_METHOD("add_font", "family", "path", "weight", "italic", "face_index"), &Weft::add_font, DEFVAL(400),
                             DEFVAL(false), DEFVAL(0));
        ClassDB::bind_method(D_METHOD("set_generic_family", "generic", "family"), &Weft::set_generic_family);
        ClassDB::bind_method(D_METHOD("add_fallback_family", "family"), &Weft::add_fallback_family);
        ClassDB::bind_method(D_METHOD("get_build"), &Weft::get_build);
        ClassDB::bind_method(D_METHOD("get_render_stats"), &Weft::get_render_stats);
    }

    Error Weft::start(bool system_fonts, int max_views) {
        const std::uint32_t flags = system_fonts ? WEFT_RUNTIME_SYSTEM_FONTS : 0u;
        const auto views = std::uint32_t(max_views > 0 ? max_views : 0);
        const auto result = on_owner([&] { return PageHost::get().start(flags, views); });
        if (result == WEFT_OK) {
            UtilityFunctions::print_verbose(String("weft: runtime started (weft ") + get_build() + ")");
        }
        return result == WEFT_BUSY ? ERR_ALREADY_IN_USE : report("starting the runtime", result);
    }

    void Weft::stop() {
        const PageHost::Detached detached = PageHost::get().detach();
        if (!detached.runtime) {
            return;
        }
        // Godot's separate render thread may be drawing a View of the runtime: its queued work is done first (draws
        // queued after the detach find no runtime).
        RenderingServer* rs = RenderingServer::get_singleton();
        if (rs && !rs->is_on_render_thread()) {
            rs->force_sync();
        }
        on_owner([&] {
            PageHost::destroy(detached);
            return WEFT_OK;
        });
    }

    bool Weft::is_running() const {
        return PageHost::get().runtime() != nullptr;
    }

    bool Weft::ensure_started() {
        attach();
        return is_running() || start() == OK || is_running();
    }

    void Weft::attach() {
        // The RenderingServer is made after the extension's scene level starts.
        RenderingServer* rs = connected_ ? nullptr : RenderingServer::get_singleton();
        if (rs) {
            rs->connect("frame_pre_draw", callable_mp(this, &Weft::on_frame_pre_draw));
            connected_ = true;
        }
    }

    Error Weft::add_font(const String& family, const String& path, int weight, bool italic, int face_index) {
        String file;
        if (const Error error = font_file(path, file); error != OK) {
            UtilityFunctions::push_error("weft: no font file at " + path);
            return error;
        }
        const CharString name = utf8(family);
        const CharString where = utf8(file);
        const weft_font_face face{sizeof(face),
                                  std::uint32_t(weight),
                                  italic ? 1u : 0u,
                                  std::uint32_t(face_index),
                                  name.get_data(),
                                  std::size_t(name.length()),
                                  where.get_data(),
                                  std::size_t(where.length())};
        return report("adding a font", on_owner([&] {
                          weft_runtime* runtime = PageHost::get().runtime();
                          return runtime ? weft_runtime_add_font(runtime, &face) : WEFT_STALE_HANDLE;
                      }));
    }

    Error Weft::set_generic_family(const String& generic, const String& family) {
        const CharString g = utf8(generic);
        const CharString f = utf8(family);
        return report("setting a generic family", on_owner([&] {
                          weft_runtime* runtime = PageHost::get().runtime();
                          return runtime ? weft_runtime_set_generic_family(runtime, g.get_data(), std::size_t(g.length()), f.get_data(),
                                                                           std::size_t(f.length()))
                                         : WEFT_STALE_HANDLE;
                      }));
    }

    Error Weft::add_fallback_family(const String& family) {
        const CharString f = utf8(family);
        return report("adding a fallback family", on_owner([&] {
                          weft_runtime* runtime = PageHost::get().runtime();
                          return runtime ? weft_runtime_add_fallback_family(runtime, f.get_data(), std::size_t(f.length()))
                                         : WEFT_STALE_HANDLE;
                      }));
    }

    String Weft::get_build() const {
        return WEFT_GODOT_COMMIT;
    }

    Dictionary Weft::get_render_stats() const {
        const RenderStats stats = RenderBridge::get().stats();
        Dictionary out;
        out["api"] = String::utf8(stats.api.c_str());
        out["draws"] = std::int64_t(stats.draws);
        out["failures"] = std::int64_t(stats.failures);
        out["last_failure"] = String::utf8(stats.last_failure.c_str());
        return out;
    }

    void Weft::shutdown() {
        RenderingServer* rs = connected_ ? RenderingServer::get_singleton() : nullptr;
        if (rs) {
            rs->disconnect("frame_pre_draw", callable_mp(this, &Weft::on_frame_pre_draw));
            connected_ = false;
            on_render_thread([] { RenderBridge::get().shutdown(); });
            if (!rs->is_on_render_thread()) {
                rs->force_sync();
            }
        }
        stop();
    }

    void Weft::on_frame_pre_draw() {
        on_render_thread([] { RenderBridge::get().frame(); });
    }
} // namespace weft_godot
