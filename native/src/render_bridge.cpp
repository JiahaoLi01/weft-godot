#include "render_bridge.hpp"

#include "page_host.hpp"
#include "render_thread.hpp"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace weft_godot {
    namespace {
        RenderingDevice* rendering_device() {
            return RenderingServer::get_singleton()->get_rendering_device();
        }

        // A texture of 8-bit BGRA UNORM; shared adds its sRGB view, which Godot's texture_rd_create makes (it samples
        // through it where it works in linear color: HDR 2D, 3D materials).
        RID make_texture(RenderingDevice* rd, const Surface& surface, std::int64_t usage, bool shared) {
            Ref<RDTextureFormat> format;
            format.instantiate();
            format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
            format->set_format(RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM);
            if (shared) {
                format->add_shareable_format(RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM);
                format->add_shareable_format(RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB);
            }
            format->set_width(surface.width);
            format->set_height(surface.height);
            format->set_usage_bits(BitField<RenderingDevice::TextureUsageBits>(usage));
            Ref<RDTextureView> view;
            view.instantiate();
            return rd->texture_create(format, view);
        }

        Vector3 extent(const Surface& surface) {
            return Vector3(real_t(surface.width), real_t(surface.height), 1);
        }
    } // namespace

    RenderBridge& RenderBridge::get() {
        static RenderBridge* bridge = new RenderBridge();
        return *bridge;
    }

    void RenderBridge::frame() {
        ++frames_;
        std::erase_if(young_, [this](const std::weak_ptr<Surface>& weak) {
            const auto surface = weak.lock();
            if (!surface) {
                return true;
            }
            if (frames_ > surface->created_frame) {
                surface->drawable = true;
                return true;
            }
            return false;
        });
        if (device_) {
            device_->retire();
        }
    }

    void RenderBridge::create(const std::shared_ptr<Surface>& surface) {
        std::string why;
        if (!ready(why)) {
            surface->failed = true;
            fail(why);
            return;
        }
        RenderingDevice* rd = rendering_device();
        surface->image = make_texture(rd, *surface,
                                      RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false);
        // The texture can be read back (RenderingDevice.texture_get_data, as the tests do) and copied from.
        surface->texture = make_texture(rd, *surface,
                                        RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
                                            RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT,
                                        true);
        if (!surface->image.is_valid() || !surface->texture.is_valid()) {
            surface->failed = true;
            fail("no RenderingDevice texture of " + std::to_string(surface->width) + " x " + std::to_string(surface->height));
            release(surface);
            return;
        }
        // The image's first use in Godot's graph, which starts it from an undefined layout: a read (its bytes are not
        // wanted) that leaves it in the copy-source layout and writes nothing of the surface's. The texture is not
        // shown before a draw is copied into it.
        rd->texture_get_data_async(surface->image, 0, make_callable([] {}));
        surface->created_frame = frames_;
        young_.push_back(surface);
        surface->publish(surface->texture);
    }

    void RenderBridge::draw(const std::shared_ptr<Surface>& surface, weft_view_handle view, std::uint64_t epoch, float scale) {
        --surface->queued;
        weft_runtime* runtime = PageHost::get().runtime();
        if (surface->failed || !surface->drawable || !device_ || !runtime || PageHost::get().epoch() != epoch) {
            return;
        }
        weft_view_frame* frame = nullptr;
        if (weft_view_acquire_frame(runtime, view, &frame) != WEFT_OK || !frame) {
            return;
        }
        RenderingDevice* rd = rendering_device();
        Target target;
        target.image = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, surface->image, 0);
        target.view = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_VIEW, surface->image, 0);
        target.width = surface->width;
        target.height = surface->height;
        std::string why;
        const bool drawn = target.image && device_->draw(frame, epoch, scale, target, why);
        weft_view_frame_release(frame);
        if (!drawn) {
            fail(target.image ? why : "the surface's image has no native handle");
            return;
        }
        // Into the texture Godot samples, in the frame Godot submits next (after the draw's submission).
        rd->texture_copy(surface->image, surface->texture, Vector3(), Vector3(), extent(*surface), 0, 0, 0, 0);
        ++surface->draws;
        std::lock_guard lock(lock_);
        ++stats_.draws;
    }

    void RenderBridge::release(const std::shared_ptr<Surface>& surface) {
        // Godot frees them once its frames that use them are done; the device's submissions that drew into the image
        // came before those frames on the same queue.
        RenderingDevice* rd = rendering_device();
        if (rd) {
            if (surface->image.is_valid()) {
                rd->free_rid(surface->image);
            }
            if (surface->texture.is_valid()) {
                rd->free_rid(surface->texture);
            }
        }
        surface->image = RID();
        surface->texture = RID();
        surface->publish(RID());
    }

    void RenderBridge::shutdown() {
        if (device_) {
            device_->shutdown();
            device_.reset();
        }
        young_.clear();
    }

    RenderStats RenderBridge::stats() {
        std::lock_guard lock(lock_);
        return stats_;
    }

    bool RenderBridge::ready(std::string& why) {
        if (device_) {
            return true;
        }
        if (unavailable_) {
            why = unavailable_why_;
            return false;
        }
        RenderingDevice* rd = rendering_device();
        const String driver = RenderingServer::get_singleton()->get_current_rendering_driver_name();
        if (!rd) {
            why = "the Compatibility renderer (" + std::string(driver.utf8().get_data()) +
                  ") has no RenderingDevice: weft draws with the Forward+ or Mobile renderer";
        } else if (driver == "vulkan") {
            device_ = make_vulkan_device(rd, why);
        } else if (driver == "d3d12") {
            device_ = make_d3d12_device(rd, why);
        } else {
            why = "weft does not draw with Godot's " + std::string(driver.utf8().get_data()) + " driver";
        }
        if (!device_) {
            // Said once, by the failure of the first surface.
            unavailable_ = true;
            unavailable_why_ = why;
            return false;
        }
        std::lock_guard lock(lock_);
        stats_.api = device_->api();
        return true;
    }

    void RenderBridge::fail(const std::string& why) {
        bool first = false;
        {
            std::lock_guard lock(lock_);
            first = stats_.failures == 0;
            ++stats_.failures;
            stats_.last_failure = why;
        }
        if (first) {
            UtilityFunctions::push_error(String::utf8(("weft: a draw failed: " + why).c_str()));
        }
    }
} // namespace weft_godot
