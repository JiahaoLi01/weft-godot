#pragma once

#include "render_device.hpp"
#include "surface.hpp"
#include "weft/weft.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace weft_godot {
    // What the render thread has done, for Weft.get_render_stats (any thread).
    struct RenderStats {
        std::string api; // the driver weft draws with ("vulkan", "d3d12"), empty before the first surface
        std::uint64_t draws = 0;
        std::uint64_t failures = 0;
        std::string last_failure;
    };

    // The extension's work on Godot's render thread: the surfaces of the Views (made, drawn and freed there with Godot's
    // RenderingDevice) and the RenderDevice of Godot's driver, made at the first surface. Every function but stats runs on
    // the render thread (on_render_thread). The render frame count advances with Godot's frames (frame, from
    // RenderingServer's frame_pre_draw, just before Godot draws and submits the frame): a surface made at a count is
    // drawable once the count is past it.
    class RenderBridge {
    public:
        static RenderBridge& get();

        void frame();
        void create(const std::shared_ptr<Surface>& surface);
        // Draws the View's latest frame (of the runtime of the epoch) into the surface; nothing when the surface is not
        // drawable yet or the View has no frame.
        void draw(const std::shared_ptr<Surface>& surface, weft_view_handle view, std::uint64_t epoch, float scale);
        void release(const std::shared_ptr<Surface>& surface);
        // When the extension goes: every renderer destroyed (Godot's device still lives).
        void shutdown();

        RenderStats stats();

    private:
        RenderBridge() = default;
        bool ready(std::string& why);
        void fail(const std::string& why);

        std::unique_ptr<RenderDevice> device_;
        bool unavailable_ = false; // no device for Godot's driver
        std::string unavailable_why_;
        std::uint64_t frames_ = 0;
        std::vector<std::weak_ptr<Surface>> young_;
        std::mutex lock_;
        RenderStats stats_;
    };
} // namespace weft_godot
