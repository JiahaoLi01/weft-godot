#include "weft_view.hpp"

#include "render_bridge.hpp"
#include "render_thread.hpp"
#include "results.hpp"
#include "variant_pack.hpp"
#include "weft_server.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace godot;

namespace weft_godot {
    namespace {
        // The texture's side at most (weft's and the GPUs' bound).
        constexpr std::uint32_t max_side = 16384;
        // A wheel notch in CSS px, positive down and right (a browser's deltaY).
        constexpr float wheel_notch = 100;

        template <typename Work>
        weft_result on_owner(Work&& work) {
            weft_result result = WEFT_INTERNAL_ERROR;
            PageHost::get().call([&] { result = work(); });
            return result;
        }

        std::uint32_t side(float logical, double scale) {
            return std::clamp(std::uint32_t(std::ceil(double(std::max(logical, 1.0f)) * scale)), 1u, max_side);
        }

        // Godot's cursor for a CSS cursor keyword.
        Control::CursorShape cursor_shape(const String& css) {
            static const struct {
                const char* name;
                Control::CursorShape shape;
            } shapes[]{{"pointer", Control::CURSOR_POINTING_HAND}, {"text", Control::CURSOR_IBEAM},
                       {"vertical-text", Control::CURSOR_IBEAM},   {"crosshair", Control::CURSOR_CROSS},
                       {"wait", Control::CURSOR_WAIT},             {"progress", Control::CURSOR_BUSY},
                       {"grab", Control::CURSOR_DRAG},             {"grabbing", Control::CURSOR_DRAG},
                       {"copy", Control::CURSOR_CAN_DROP},         {"alias", Control::CURSOR_CAN_DROP},
                       {"not-allowed", Control::CURSOR_FORBIDDEN}, {"no-drop", Control::CURSOR_FORBIDDEN},
                       {"ns-resize", Control::CURSOR_VSIZE},       {"n-resize", Control::CURSOR_VSIZE},
                       {"s-resize", Control::CURSOR_VSIZE},        {"row-resize", Control::CURSOR_VSPLIT},
                       {"ew-resize", Control::CURSOR_HSIZE},       {"e-resize", Control::CURSOR_HSIZE},
                       {"w-resize", Control::CURSOR_HSIZE},        {"col-resize", Control::CURSOR_HSPLIT},
                       {"nesw-resize", Control::CURSOR_BDIAGSIZE}, {"ne-resize", Control::CURSOR_BDIAGSIZE},
                       {"sw-resize", Control::CURSOR_BDIAGSIZE},   {"nwse-resize", Control::CURSOR_FDIAGSIZE},
                       {"nw-resize", Control::CURSOR_FDIAGSIZE},   {"se-resize", Control::CURSOR_FDIAGSIZE},
                       {"move", Control::CURSOR_MOVE},             {"all-scroll", Control::CURSOR_MOVE},
                       {"help", Control::CURSOR_HELP}};
            for (const auto& entry : shapes) {
                if (css == entry.name) {
                    return entry.shape;
                }
            }
            return Control::CURSOR_ARROW;
        }

        // MouseEvent.buttons of Godot's mask: left 1, right 2, middle 4, as Godot's.
        std::uint32_t dom_buttons(BitField<MouseButtonMask> mask) {
            return std::uint32_t(std::int64_t(mask)) & 7u;
        }
    } // namespace

    WeftView::WeftView() {
        texture_.instantiate();
        material_.instantiate();
        material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_PREMULT_ALPHA);
        RenderingServer* rs = RenderingServer::get_singleton();
        item_ = rs->canvas_item_create();
        rs->canvas_item_set_parent(item_, get_canvas_item());
        rs->canvas_item_set_material(item_, material_->get_rid());
    }

    WeftView::~WeftView() {
        destroy();
        if (item_.is_valid()) {
            RenderingServer::get_singleton()->free_rid(item_);
        }
    }

    void WeftView::_bind_methods() {
        ClassDB::bind_method(D_METHOD("set_page", "folder"), &WeftView::set_page);
        ClassDB::bind_method(D_METHOD("get_page"), &WeftView::get_page);
        ClassDB::bind_method(D_METHOD("set_device_scale", "scale"), &WeftView::set_device_scale);
        ClassDB::bind_method(D_METHOD("get_device_scale"), &WeftView::get_device_scale);
        ClassDB::bind_method(D_METHOD("set_hit_test_input", "enabled"), &WeftView::set_hit_test_input);
        ClassDB::bind_method(D_METHOD("get_hit_test_input"), &WeftView::get_hit_test_input);
        ClassDB::bind_method(D_METHOD("set_log_diagnostics", "enabled"), &WeftView::set_log_diagnostics);
        ClassDB::bind_method(D_METHOD("get_log_diagnostics"), &WeftView::get_log_diagnostics);
        ClassDB::bind_method(D_METHOD("load_page", "folder"), &WeftView::load_page);
        ClassDB::bind_method(D_METHOD("load_source", "script", "stylesheet", "folder"), &WeftView::load_source, DEFVAL(""), DEFVAL(""));
        ClassDB::bind_method(D_METHOD("emit_event", "name", "value", "latest"), &WeftView::emit_event, DEFVAL(Variant()), DEFVAL(false));
        ClassDB::bind_method(D_METHOD("set_model", "model", "fields"), &WeftView::set_model);
        ClassDB::bind_method(D_METHOD("complete_call", "call_id", "succeeded", "value"), &WeftView::complete_call, DEFVAL(Variant()));
        ClassDB::bind_method(D_METHOD("write_anchors", "keys", "positions", "scales", "depths", "flags"), &WeftView::write_anchors,
                             DEFVAL(PackedFloat32Array()), DEFVAL(PackedFloat32Array()), DEFVAL(PackedInt32Array()));
        ClassDB::bind_method(D_METHOD("send_pointer", "kind", "position", "buttons", "wheel"), &WeftView::send_pointer, DEFVAL(0),
                             DEFVAL(Vector2()));
        ClassDB::bind_method(D_METHOD("hit_test", "position"), &WeftView::hit_test);
        ClassDB::bind_method(D_METHOD("get_texture"), &WeftView::get_texture);
        ClassDB::bind_method(D_METHOD("is_pointer_over_page"), &WeftView::is_pointer_over_page);

        ADD_PROPERTY(PropertyInfo(Variant::STRING, "page", PROPERTY_HINT_DIR), "set_page", "get_page");
        ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "device_scale", PROPERTY_HINT_RANGE, "0.25,4,0.05,or_greater"), "set_device_scale",
                     "get_device_scale");
        ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hit_test_input"), "set_hit_test_input", "get_hit_test_input");
        ADD_PROPERTY(PropertyInfo(Variant::BOOL, "log_diagnostics"), "set_log_diagnostics", "get_log_diagnostics");

        const PropertyInfo value(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
        ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::STRING, "name"), value));
        ADD_SIGNAL(MethodInfo("call_received", PropertyInfo(Variant::INT, "call_id"), PropertyInfo(Variant::STRING, "name"), value));
        ADD_SIGNAL(MethodInfo("diagnostic_received", PropertyInfo(Variant::INT, "kind"), PropertyInfo(Variant::STRING, "text")));
    }

    // ---- Properties ----

    void WeftView::set_page(const String& folder) {
        page_ = folder;
        if (is_inside_tree() && !folder.is_empty()) {
            load_page(folder);
        }
    }

    String WeftView::get_page() const {
        return page_;
    }

    void WeftView::set_device_scale(double scale) {
        device_scale_ = std::max(scale, 0.25);
    }

    double WeftView::get_device_scale() const {
        return device_scale_;
    }

    void WeftView::set_hit_test_input(bool enabled) {
        hit_test_input_ = enabled;
    }

    bool WeftView::get_hit_test_input() const {
        return hit_test_input_;
    }

    void WeftView::set_log_diagnostics(bool enabled) {
        log_diagnostics_ = enabled;
    }

    bool WeftView::get_log_diagnostics() const {
        return log_diagnostics_;
    }

    // ---- Pages and the bridge ----

    Error WeftView::load_page(const String& folder) {
        PageSource page;
        String why;
        if (!read_page(folder, page, why)) {
            UtilityFunctions::push_error("weft: " + why);
            return ERR_FILE_NOT_FOUND;
        }
        page_ = folder;
        return load(page);
    }

    Error WeftView::load_source(const String& script, const String& stylesheet, const String& folder) {
        return load(page_from_source(script, stylesheet, folder));
    }

    Error WeftView::load(const PageSource& page) {
        loaded_ = page;
        if (!running()) {
            // Loaded when the View is made.
            return OK;
        }
        std::vector<weft_script> scripts;
        for (const auto& [name, source] : page.scripts) {
            scripts.push_back({name.data(), name.size(), source.data(), source.size()});
        }
        weft_page_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.flags = page.crpg_bridge ? WEFT_PAGE_CRPG_BRIDGE : 0u;
        desc.scripts = scripts.data();
        desc.script_count = std::uint32_t(scripts.size());
        desc.stylesheet = page.stylesheet.data();
        desc.stylesheet_size = page.stylesheet.size();
        desc.keyframes = page.keyframes.data();
        desc.keyframes_size = page.keyframes.size();
        desc.root = page.root.empty() ? nullptr : page.root.data();
        desc.root_size = page.root.size();
        desc.base = page.base.data();
        desc.base_size = page.base.size();
        const auto view = view_;
        const auto result = on_owner([&] { return weft_view_load(PageHost::get().runtime(), view, &desc); });
        faulted_ = false;
        redraw_ = true;
        // A page that throws loads empty; its error is a diagnostic of the next advance.
        if (result != WEFT_OK && result != WEFT_SCRIPT_ERROR) {
            UtilityFunctions::push_error(failure("loading the page", result));
        }
        return godot_error(result);
    }

    bool WeftView::emit_event(const String& name, const Variant& value, bool latest) {
        if (!running()) {
            return false;
        }
        const CharString n = name.utf8();
        const std::string payload = pack(value);
        return weft_view_emit(PageHost::get().runtime(), view_, n.get_data(), std::size_t(n.length()), WEFT_VALUE_MSGPACK, payload.data(),
                              payload.size(), latest ? WEFT_EMIT_LATEST : 0u) == WEFT_OK;
    }

    bool WeftView::set_model(const String& model, const Dictionary& fields) {
        if (!running()) {
            return false;
        }
        const CharString m = model.utf8();
        const std::string payload = pack(fields);
        return weft_view_set_model(PageHost::get().runtime(), view_, m.get_data(), std::size_t(m.length()), WEFT_VALUE_MSGPACK,
                                   payload.data(), payload.size()) == WEFT_OK;
    }

    bool WeftView::complete_call(std::int64_t call_id, bool succeeded, const Variant& value) {
        if (!running()) {
            return false;
        }
        const std::string payload = pack(value);
        return weft_view_complete_call(PageHost::get().runtime(), view_, std::uint64_t(call_id), succeeded ? 1u : 0u, WEFT_VALUE_MSGPACK,
                                       payload.data(), payload.size()) == WEFT_OK;
    }

    bool WeftView::write_anchors(const PackedInt64Array& keys, const PackedVector2Array& positions, const PackedFloat32Array& scales,
                                 const PackedFloat32Array& depths, const PackedInt32Array& flags) {
        if (!running() || keys.size() != positions.size()) {
            return false;
        }
        std::vector<weft_anchor> anchors(std::size_t(keys.size()));
        for (std::int64_t i = 0; i < keys.size(); ++i) {
            auto& anchor = anchors[std::size_t(i)];
            anchor.key = std::uint64_t(keys[i]);
            anchor.x = positions[i].x;
            anchor.y = positions[i].y;
            anchor.scale = i < scales.size() ? scales[i] : 1.0f;
            anchor.depth = i < depths.size() ? depths[i] : 0.0f;
            anchor.flags = i < flags.size() ? std::uint32_t(flags[i]) : 0u;
        }
        redraw_ = true;
        return weft_view_write_anchors(PageHost::get().runtime(), view_, anchors.data(), anchors.size()) == WEFT_OK;
    }

    bool WeftView::send_pointer(int kind, const Vector2& position, int buttons, const Vector2& wheel) {
        return pointer(std::uint32_t(kind), position, std::uint32_t(buttons), wheel);
    }

    Dictionary WeftView::hit_test(const Vector2& position) const {
        const Hit found = hit(position);
        Dictionary out;
        out["element"] = found.element;
        out["cursor"] = found.cursor;
        return out;
    }

    Ref<Texture2D> WeftView::get_texture() const {
        return texture_;
    }

    bool WeftView::is_pointer_over_page() const {
        return hovered_ && hit_.element;
    }

    // ---- The View's life ----

    void WeftView::_notification(int what) {
        switch (what) {
            case NOTIFICATION_ENTER_TREE:
                create();
                set_process(true);
                break;
            case NOTIFICATION_EXIT_TREE:
                set_process(false);
                destroy();
                break;
            case NOTIFICATION_MOUSE_EXIT:
                if (hovered_) {
                    pointer(WEFT_POINTER_LEAVE, pointer_, buttons_, Vector2());
                }
                hovered_ = false;
                break;
            case NOTIFICATION_RESIZED:
                refresh();
                break;
            default:
                break;
        }
    }

    bool WeftView::running() const {
        return created_ && PageHost::get().runtime() && PageHost::get().epoch() == epoch_;
    }

    void WeftView::create() {
        Weft* weft = Weft::get_singleton();
        if (!created_ && weft && weft->ensure_started()) {
            create_view();
        }
    }

    void WeftView::create_view() {
        // Once a runtime: a View that does not make it waits for the next one.
        tried_epoch_ = PageHost::get().epoch();
        const Vector2 size = get_size();
        logical_ = Vector2(std::max(size.x, real_t(1)), std::max(size.y, real_t(1)));
        applied_scale_ = device_scale_;
        weft_view_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.logical_width = float(logical_.x);
        desc.logical_height = float(logical_.y);
        desc.device_scale = float(device_scale_);
        weft_view_handle view{};
        const auto result = on_owner([&] { return PageHost::get().create_view(desc, view); });
        if (result != WEFT_OK) {
            UtilityFunctions::push_error(failure("creating a View", result));
            return;
        }
        view_ = view;
        created_ = true;
        epoch_ = PageHost::get().epoch();
        time_ = 0;
        faulted_ = false;
        redraw_ = true;
        if (loaded_) {
            const PageSource page = *loaded_;
            load(page);
        } else if (!page_.is_empty()) {
            load_page(page_);
        }
    }

    void WeftView::destroy() {
        if (created_ && running()) {
            const auto view = view_;
            on_owner([&] { return PageHost::get().destroy_view(view); });
        }
        created_ = false;
        hovered_ = false;
        drop_surfaces();
    }

    void WeftView::drop_surfaces() {
        std::shared_ptr<Surface> drawn = std::move(surface_);
        std::shared_ptr<Surface> shown = std::move(shown_);
        // The View's canvas item first lets go of the texture, then the texture of the surface.
        refresh();
        if (texture_.is_valid() && RenderingServer::get_singleton()) {
            texture_->set_texture_rd_rid(RID());
        }
        release(drawn);
        release(shown);
    }

    void WeftView::_process(double) {
        if (created_ && !running()) {
            // The runtime stopped under the View (Weft.stop), or a new one started.
            created_ = false;
            drop_surfaces();
        }
        if (!created_) {
            // A View of the runtime that runs now, with the page it had.
            if (!PageHost::get().runtime() || PageHost::get().epoch() == tried_epoch_) {
                return;
            }
            create_view();
            if (!created_) {
                return;
            }
        }
        fit();
        advance();
        // What is under the mouse may have changed with the page.
        hit_at_ = Vector2(-1, -1);
        draw();
        show();
    }

    // The page at the Control's size and the View's scale, and a surface of that size.
    void WeftView::fit() {
        const Vector2 size = get_size();
        const Vector2 logical(std::max(size.x, real_t(1)), std::max(size.y, real_t(1)));
        if (logical != logical_ || applied_scale_ != device_scale_) {
            logical_ = logical;
            applied_scale_ = device_scale_;
            const auto view = view_;
            const float scale = float(device_scale_);
            on_owner([&] { return weft_view_resize(PageHost::get().runtime(), view, float(logical.x), float(logical.y), scale); });
        }
        const std::uint32_t width = side(float(logical_.x), device_scale_);
        const std::uint32_t height = side(float(logical_.y), device_scale_);
        if (surface_ && surface_->width == width && surface_->height == height) {
            return;
        }
        if (surface_ != shown_) {
            // Made and never shown: the next one replaces it.
            release(surface_);
        }
        surface_ = std::make_shared<Surface>(width, height);
        redraw_ = true;
        on_render_thread([surface = surface_] { RenderBridge::get().create(surface); });
    }

    void WeftView::advance() {
        // weft's clock only goes forward.
        time_ = std::max(time_, double(Time::get_singleton()->get_ticks_usec()) / 1e6);
        const auto view = view_;
        const double time = time_;
        const auto result = on_owner([&] { return PageHost::get().advance(view, time); });
        PageHost::get().take(view_, messages_, diagnostics_);
        for (const auto& diagnostic : diagnostics_) {
            const String text = String::utf8(diagnostic.text.data(), std::int64_t(diagnostic.text.size()));
            if (log_diagnostics_) {
                UtilityFunctions::push_warning("weft (" + String(get_name()) + "): " + text);
            }
            emit_signal("diagnostic_received", std::int64_t(diagnostic.kind), text);
        }
        if (result == WEFT_FAULTED && !faulted_) {
            faulted_ = true;
            UtilityFunctions::push_error("weft (" + String(get_name()) + "): a resource limit ended the page; load a page again");
        }
        for (const auto& message : messages_) {
            const String name = String::utf8(message.name.data(), std::int64_t(message.name.size()));
            const Variant value = unpack(message.payload.data(), message.payload.size());
            if (message.kind == WEFT_MESSAGE_CALL) {
                emit_signal("call_received", std::int64_t(message.call_id), name, value);
            } else {
                emit_signal("message_received", name, value);
            }
            // A handler may have freed or stopped the View.
            if (!running()) {
                return;
            }
        }
    }

    // Queues a draw of the View's latest frame when it shows something the surface does not yet.
    void WeftView::draw() {
        if (!surface_ || surface_->failed || !surface_->drawable || !is_visible_in_tree() || surface_->queued >= 2) {
            return;
        }
        weft_view_frame* frame = nullptr;
        if (weft_view_acquire_frame(PageHost::get().runtime(), view_, &frame) != WEFT_OK || !frame) {
            return;
        }
        weft_view_frame_info info{sizeof(info)};
        weft_view_frame_get_info(frame, &info);
        weft_view_frame_release(frame);
        if (!redraw_ && info.revision && !info.animated && info.revision == drawn_revision_) {
            return;
        }
        drawn_revision_ = info.revision;
        redraw_ = false;
        ++surface_->queued;
        on_render_thread([surface = surface_, view = view_, epoch = epoch_, scale = float(device_scale_)] {
            RenderBridge::get().draw(surface, view, epoch, scale);
        });
        // Godot draws a frame (the editor only draws on changes), which copies the draw into the texture.
        refresh();
    }

    // Shows the newest surface once a draw is in it.
    void WeftView::show() {
        if (!surface_ || surface_ == shown_ || surface_->draws == 0) {
            return;
        }
        texture_->set_texture_rd_rid(surface_->shown());
        release(shown_);
        shown_ = surface_;
        refresh();
    }

    void WeftView::refresh() {
        RenderingServer* rs = RenderingServer::get_singleton();
        if (!rs || !item_.is_valid()) {
            return;
        }
        rs->canvas_item_clear(item_);
        if (shown_ && created_) {
            rs->canvas_item_add_texture_rect(item_, Rect2(Vector2(), get_size()), texture_->get_rid());
        }
    }

    void WeftView::release(std::shared_ptr<Surface>& surface) {
        if (surface && RenderingServer::get_singleton()) {
            on_render_thread([surface] { RenderBridge::get().release(surface); });
        }
        surface.reset();
    }

    // ---- Input ----

    WeftView::Hit WeftView::hit(const Vector2& position) const {
        if (!running()) {
            return {};
        }
        if (position == hit_at_) {
            return hit_;
        }
        weft_hit found{};
        found.struct_size = sizeof(found);
        const auto view = view_;
        const float x = float(position.x);
        const float y = float(position.y);
        Hit result;
        if (on_owner([&] { return weft_view_hit_test(PageHost::get().runtime(), view, x, y, &found); }) == WEFT_OK) {
            found.cursor[sizeof(found.cursor) - 1] = 0;
            result.element = found.element != 0;
            result.cursor = String::utf8(found.cursor);
        }
        hit_at_ = position;
        hit_ = result;
        return result;
    }

    bool WeftView::_has_point(const Vector2& point) const {
        if (!Rect2(Vector2(), get_size()).has_point(point)) {
            return false;
        }
        return !hit_test_input_ || hit(point).element;
    }

    bool WeftView::pointer(std::uint32_t kind, const Vector2& position, std::uint32_t buttons, const Vector2& wheel) {
        if (!running()) {
            return false;
        }
        weft_pointer_event event{};
        event.struct_size = sizeof(event);
        event.kind = kind;
        event.x = float(position.x);
        event.y = float(position.y);
        event.buttons = buttons;
        event.wheel_x = float(wheel.x);
        event.wheel_y = float(wheel.y);
        std::uint32_t prevented = 0;
        const auto view = view_;
        on_owner([&] { return weft_view_pointer(PageHost::get().runtime(), view, &event, &prevented); });
        return prevented != 0;
    }

    void WeftView::_gui_input(const Ref<InputEvent>& event) {
        if (!running()) {
            return;
        }
        if (const Ref<InputEventMouseMotion> motion = event; motion.is_valid()) {
            pointer_ = motion->get_position();
            buttons_ = dom_buttons(motion->get_button_mask());
            hovered_ = true;
            pointer(WEFT_POINTER_MOVE, pointer_, buttons_, Vector2());
            set_default_cursor_shape(cursor_shape(hit(pointer_).cursor));
            accept_event();
            return;
        }
        const Ref<InputEventMouseButton> button = event;
        if (button.is_null()) {
            return;
        }
        const Vector2 at = button->get_position();
        if (!hovered_ || at != pointer_) {
            hovered_ = true;
            pointer_ = at;
            pointer(WEFT_POINTER_MOVE, at, buttons_, Vector2());
        }
        const MouseButton index = button->get_button_index();
        if (index == MOUSE_BUTTON_LEFT || index == MOUSE_BUTTON_RIGHT || index == MOUSE_BUTTON_MIDDLE) {
            buttons_ = dom_buttons(button->get_button_mask());
            pointer(button->is_pressed() ? WEFT_POINTER_DOWN : WEFT_POINTER_UP, at, buttons_, Vector2());
            accept_event();
            return;
        }
        if (!button->is_pressed()) {
            return;
        }
        // Precise wheels (touchpads) give a fraction of a notch.
        const float notches = button->get_factor() > 0 ? float(button->get_factor()) : 1.0f;
        Vector2 wheel;
        switch (index) {
            case MOUSE_BUTTON_WHEEL_UP:
                wheel.y = -notches * wheel_notch;
                break;
            case MOUSE_BUTTON_WHEEL_DOWN:
                wheel.y = notches * wheel_notch;
                break;
            case MOUSE_BUTTON_WHEEL_LEFT:
                wheel.x = -notches * wheel_notch;
                break;
            case MOUSE_BUTTON_WHEEL_RIGHT:
                wheel.x = notches * wheel_notch;
                break;
            default:
                return;
        }
        pointer(WEFT_POINTER_WHEEL, at, buttons_, wheel);
        accept_event();
    }
} // namespace weft_godot
