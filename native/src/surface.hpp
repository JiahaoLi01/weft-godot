#pragma once

#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace weft_godot {
    // What a View draws into at one size, made and drawn on Godot's render thread (RenderBridge) and shown by the main
    // thread. Two RenderingDevice textures of 8-bit BGRA UNORM:
    //   - the image, which weft's backend draws into (color attachment) on Godot's device and queue, and which Godot only
    //     ever copies from: its first use in Godot's frame graph (a read back at creation) puts it in the copy-source
    //     layout, and every draw leaves it there again, so the graph and weft agree on its layout without either knowing
    //     the other;
    //   - the texture, a copy of the image after every draw, which Godot samples (a Texture2DRD of the View), shown from
    //     its first draw on.
    // The image is drawable once the frame of its first use has been submitted (the render frame after its creation):
    // before that, the graph's first use would discard what weft drew.
    struct Surface {
        Surface(std::uint32_t w, std::uint32_t h) : width(w), height(h) {
        }

        const std::uint32_t width;
        const std::uint32_t height;

        // Render thread.
        godot::RID image;
        godot::RID texture;
        std::uint64_t created_frame = 0;

        // Set by the render thread, read by the main thread.
        std::atomic<bool> drawable{false};
        std::atomic<bool> failed{false};
        std::atomic<std::uint64_t> draws{0};  // draws copied into the texture
        std::atomic<std::uint32_t> queued{0}; // draws queued and not run yet

        // The texture's RID for the main thread, once made.
        godot::RID shown() {
            std::lock_guard lock(lock_);
            return shown_;
        }
        void publish(const godot::RID& rid) {
            std::lock_guard lock(lock_);
            shown_ = rid;
        }

    private:
        std::mutex lock_;
        godot::RID shown_;
    };
} // namespace weft_godot
