#pragma once

#include "page_host.hpp"
#include "page_source.hpp"
#include "surface.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <memory>
#include <optional>

namespace weft_godot {
    // A weft page in the game: a View of the runtime as big as the Control (one CSS px a unit of it), run every frame,
    // drawn on Godot's render thread into a texture (get_texture) that the Control shows. The page's messages are
    // signals; the game's events, view models, call results and anchors go to the page. The mouse goes to the page where
    // an element of it is (hit_test_input), and past the View to what is under it elsewhere.
    class WeftView : public godot::Control {
        GDCLASS(WeftView, godot::Control)

    public:
        WeftView();
        ~WeftView() override;

        // The page's folder ("res://ui/hud": its page.json, or page.js and page.css), loaded when the View enters the tree.
        void set_page(const godot::String& folder);
        godot::String get_page() const;
        // Device px per CSS px: 2 draws the page twice as sharp (the texture is the Control's size times it).
        void set_device_scale(double scale);
        double get_device_scale() const;
        void set_hit_test_input(bool enabled);
        bool get_hit_test_input() const;
        void set_log_diagnostics(bool enabled);
        bool get_log_diagnostics() const;

        godot::Error load_page(const godot::String& folder);
        // A page of one script and a stylesheet; its url() images resolve from folder (empty: none).
        godot::Error load_source(const godot::String& script, const godot::String& stylesheet = "", const godot::String& folder = "");

        // An event for the page's weft.engine.on(name) handlers at its next advance; latest replaces an undelivered one of
        // the name. false when the View does not run or its queue is full.
        bool emit_event(const godot::String& name, const godot::Variant& value = godot::Variant(), bool latest = false);
        // Merges fields into the page's weft.engine.model(model).
        bool set_model(const godot::String& model, const godot::Dictionary& fields);
        // Settles the page's weft.engine.call (call_received): resolves its promise with the value, or rejects it.
        bool complete_call(std::int64_t call_id, bool succeeded, const godot::Variant& value = godot::Variant());
        // Replaces the View's anchors (elements with data-anchor="<key>"), in CSS px; scales, depths and flags may be
        // empty (1, 0, visible). The next draw places the anchored elements, without running the page.
        bool write_anchors(const godot::PackedInt64Array& keys, const godot::PackedVector2Array& positions,
                           const godot::PackedFloat32Array& scales = {}, const godot::PackedFloat32Array& depths = {},
                           const godot::PackedInt32Array& flags = {});
        // A pointer event the game gives (a View shown on a 3D surface): kind 0 move, 1 down, 2 up, 3 wheel, 4 leave;
        // buttons as MouseEvent.buttons after it; wheel in CSS px. true when a listener prevented its default action.
        bool send_pointer(int kind, const godot::Vector2& position, int buttons = 0, const godot::Vector2& wheel = godot::Vector2());
        // {"element": bool, "cursor": String}: whether an element of the page (not its root) is at the point, and its CSS cursor.
        godot::Dictionary hit_test(const godot::Vector2& position) const;

        godot::Ref<godot::Texture2D> get_texture() const;
        bool is_pointer_over_page() const;

        void _notification(int what);
        void _process(double delta) override;
        void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
        bool _has_point(const godot::Vector2& point) const override;

    protected:
        static void _bind_methods();

    private:
        struct Hit {
            bool element = false;
            godot::String cursor = "auto";
        };

        bool running() const;
        // A View of the runtime, started first if it does not run (as the View enters the tree).
        void create();
        // A View of the runtime that runs, and its page.
        void create_view();
        void destroy();
        godot::Error load(const PageSource& page);
        void fit();
        void advance();
        void draw();
        void show();
        void refresh();
        // Shows nothing and frees both surfaces.
        void drop_surfaces();
        void release(std::shared_ptr<Surface>& surface);
        Hit hit(const godot::Vector2& position) const;
        bool pointer(std::uint32_t kind, const godot::Vector2& position, std::uint32_t buttons, const godot::Vector2& wheel);

        godot::String page_;
        double device_scale_ = 1;
        bool hit_test_input_ = true;
        bool log_diagnostics_ = true;

        weft_view_handle view_{};
        bool created_ = false;
        std::uint64_t epoch_ = 0;       // of the runtime the View is of
        std::uint64_t tried_epoch_ = 0; // of the runtime the View was last made for (or tried)
        std::optional<PageSource> loaded_;
        double time_ = 0;
        godot::Vector2 logical_;
        double applied_scale_ = 0;
        bool faulted_ = false;
        std::vector<PageHost::Message> messages_;
        std::vector<PageHost::Diagnostic> diagnostics_;

        std::shared_ptr<Surface> surface_; // drawn into
        std::shared_ptr<Surface> shown_;   // what the texture shows
        std::uint64_t drawn_revision_ = 0;
        bool redraw_ = true; // anchors moved, or a new surface
        godot::Ref<godot::Texture2DRD> texture_;
        godot::Ref<godot::CanvasItemMaterial> material_;
        godot::RID item_; // a canvas item of the View's own, blending premultiplied colors

        godot::Vector2 pointer_;
        std::uint32_t buttons_ = 0;
        bool hovered_ = false;
        mutable godot::Vector2 hit_at_{-1, -1};
        mutable Hit hit_;
    };
} // namespace weft_godot
