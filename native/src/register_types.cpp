// The extension's entry: the Weft singleton and the WeftView class at Godot's scene level, and their end with it.
#include "weft_server.hpp"
#include "weft_view.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {
    weft_godot::Weft* weft = nullptr;

    void initialize(ModuleInitializationLevel level) {
        if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
            return;
        }
        GDREGISTER_CLASS(weft_godot::Weft);
        GDREGISTER_CLASS(weft_godot::WeftView);
        weft = memnew(weft_godot::Weft);
        Engine::get_singleton()->register_singleton("Weft", weft);
    }

    void uninitialize(ModuleInitializationLevel level) {
        if (level != MODULE_INITIALIZATION_LEVEL_SCENE || !weft) {
            return;
        }
        weft->shutdown();
        Engine::get_singleton()->unregister_singleton("Weft");
        memdelete(weft);
        weft = nullptr;
    }
} // namespace

extern "C" {
GDExtensionBool GDE_EXPORT weft_godot_init(GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
                                           GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
