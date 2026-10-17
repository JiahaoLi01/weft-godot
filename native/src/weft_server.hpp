#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace weft_godot {
    // The engine singleton Weft: the runtime the Views of the game share (started by the first WeftView, or by start for
    // fonts first), its fonts, and what the render thread has done. It advances the render thread's frame count with
    // RenderingServer's frame_pre_draw, and destroys the renderers and the runtime when the extension goes.
    class Weft : public godot::Object {
        GDCLASS(Weft, godot::Object)

    public:
        static Weft* get_singleton();

        Weft();
        ~Weft() override;

        // Starts the runtime: system_fonts gives the pages Windows' installed fonts and their generic families;
        // max_views 0 is weft's default (64). ERR_ALREADY_IN_USE when it runs.
        godot::Error start(bool system_fonts = true, int max_views = 0);
        // Destroys the Views and the runtime; the WeftViews in the tree stop drawing until they enter it again.
        void stop();
        bool is_running() const;
        // Starts the runtime with the defaults when it is not running, and follows Godot's frames; false when it does not
        // start. A WeftView calls it as it enters the tree.
        bool ensure_started();

        // Fonts, after start and before the runtime's first page. path is a font file (res://, user:// or absolute; one
        // in an exported game's pack is copied to user://weft/fonts first, weft reads files itself).
        godot::Error add_font(const godot::String& family, const godot::String& path, int weight = 400, bool italic = false,
                              int face_index = 0);
        godot::Error set_generic_family(const godot::String& generic, const godot::String& family);
        godot::Error add_fallback_family(const godot::String& family);

        // The weft commit the extension was built from ("+changes" for a checkout with changes).
        godot::String get_build() const;
        // {"api": "vulkan" | "d3d12" | "", "draws": int, "failures": int, "last_failure": String}.
        godot::Dictionary get_render_stats() const;

        // When the extension goes (its scene level ends).
        void shutdown();

    protected:
        static void _bind_methods();

    private:
        // Connects to RenderingServer's frame_pre_draw, once there is a RenderingServer.
        void attach();
        void on_frame_pre_draw();

        static Weft* singleton_;
        bool connected_ = false;
    };
} // namespace weft_godot
