#pragma once

#include <godot_cpp/variant/callable.hpp>

#include <functional>

namespace weft_godot {
    // A Callable that runs a C++ function, whatever arguments it is called with.
    godot::Callable make_callable(std::function<void()> work);

    // Runs work on Godot's render thread (RenderingServer::call_on_render_thread): at once when the caller is that thread
    // (Godot's default, single-threaded rendering, where it is the main thread), else in order with the main thread's
    // other rendering commands of the frame. Work is a C++ function in a Callable of its own.
    void on_render_thread(std::function<void()> work);
} // namespace weft_godot
