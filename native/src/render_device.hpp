#pragma once

#include "weft/render_backend.h"
#include "weft/weft.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace godot {
    class RenderingDevice;
}

namespace weft_godot {
    // A target of a draw: a RenderingDevice texture's native handles (RenderingDevice::get_driver_resource): its image
    // (VkImage, ID3D12Resource) and view (VkImageView; the D3D12 adapter makes its own), in the layout or state the API's
    // device says before and after the draw.
    struct Target {
        std::uint64_t image = 0;
        std::uint64_t view = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Draws View frames on Godot's render thread with one graphics API: a weft renderer over the API's built-in weft
    // backend on Godot's device, made on the render thread (its owner thread) for a runtime and made again for the next
    // one. Godot gives an extension no command buffer of its frames, so each draw is a command buffer of the device's own,
    // submitted at once on Godot's main queue (before the frame that copies from the target, which Godot submits later). A
    // renderer's recordings retire when the GPU is past them (the device's fences); a renderer of an earlier runtime is
    // destroyed once all of its recordings have.
    class RenderDevice {
    public:
        virtual ~RenderDevice();

        // The driver's name ("vulkan", "d3d12").
        virtual const char* api() const = 0;
        // Draws a frame of a runtime (the frame reference stays the caller's) into the target, cleared first; false and
        // why.
        bool draw(weft_view_frame* frame, std::uint64_t epoch, float scale, const Target& target, std::string& why);
        // Reports the recordings the GPU has finished and destroys drained renderers of earlier runtimes.
        void retire();
        // When Godot's device goes: waits (a while at most) for the device's recordings, then destroys every renderer.
        void shutdown();

    protected:
        // The queue name the renderers report submissions on.
        static constexpr std::uint64_t queue = 1;

        struct Slot {
            weft_renderer* renderer = nullptr;
            void* backend = nullptr; // the API's weft backend
            weft_render_identity identity{};
            std::uint64_t epoch = 0;
            std::uint64_t serial = 0;    // the last submitted
            std::uint64_t completed = 0; // the last reported complete
            std::deque<std::pair<std::uint64_t, std::uint64_t>> pending; // serial, the device's mark of it
        };

        // The API's weft backend for a new renderer, and its table; false and why.
        virtual bool open(Slot& slot, weft_render_backend& table, std::string& why) = 0;
        // After its renderer is destroyed.
        virtual void close(Slot& slot) = 0;
        // Records a prepared frame into a command buffer of the device's, clearing the target, and submits it
        // (submit(slot, use) once it is submitted); false and why.
        virtual bool encode(Slot& slot, weft_render_handle prepared, const Target& target, std::string& why) = 0;
        // The device's mark of the last submission, and of the last the GPU has finished.
        virtual std::uint64_t mark() = 0;
        virtual std::uint64_t finished() = 0;
        // Waits (2 s at most) until the GPU has finished every submission.
        virtual void idle() = 0;

        // Reports a recording submitted at the device's current mark.
        void submit(Slot& slot, weft_render_handle use);

    private:
        bool start(std::uint64_t epoch, std::string& why);
        void retire(Slot& slot);
        // false keeps the slot: its renderer still drains.
        bool destroy(Slot& slot);

        std::unique_ptr<Slot> current_;
        std::vector<std::unique_ptr<Slot>> retiring_;
    };

    // A weft renderer result as text: its name and number ("WRONG_THREAD (4)").
    std::string describe(weft_render_result result);

    // The device of Godot's RenderingDevice for its driver; nullptr and why when it has none weft draws with.
    std::unique_ptr<RenderDevice> make_vulkan_device(godot::RenderingDevice* rd, std::string& why);
    std::unique_ptr<RenderDevice> make_d3d12_device(godot::RenderingDevice* rd, std::string& why);
} // namespace weft_godot
