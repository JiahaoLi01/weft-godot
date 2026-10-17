#include "render_thread.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_custom.hpp>

#include <cstdint>

using namespace godot;

namespace weft_godot {
    namespace {
        class Work final : public CallableCustom {
        public:
            explicit Work(std::function<void()> work) : work_(std::move(work)) {
            }

            uint32_t hash() const override {
                return uint32_t(reinterpret_cast<std::uintptr_t>(this));
            }
            String get_as_text() const override {
                return "weft render work";
            }
            CompareEqualFunc get_compare_equal_func() const override {
                return [](const CallableCustom* a, const CallableCustom* b) { return a == b; };
            }
            CompareLessFunc get_compare_less_func() const override {
                return [](const CallableCustom* a, const CallableCustom* b) { return a < b; };
            }
            // Of no object: valid as long as it lives.
            bool is_valid() const override {
                return true;
            }
            ObjectID get_object() const override {
                return ObjectID();
            }
            void call(const Variant**, int, Variant&, GDExtensionCallError& error) const override {
                work_();
                error.error = GDEXTENSION_CALL_OK;
            }

        private:
            std::function<void()> work_;
        };
    } // namespace

    Callable make_callable(std::function<void()> work) {
        return Callable(memnew(Work(std::move(work))));
    }

    void on_render_thread(std::function<void()> work) {
        RenderingServer::get_singleton()->call_on_render_thread(make_callable(std::move(work)));
    }
} // namespace weft_godot
