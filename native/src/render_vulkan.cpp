#include "render_device.hpp"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <windows.h>

#include <vulkan/vulkan.h>

#include "weft/render_backend_vulkan.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using godot::RenderingDevice;

namespace weft_godot {
    namespace {
        template <typename T>
        std::uint64_t handle(T value) {
            return std::uint64_t(reinterpret_cast<std::uintptr_t>(value));
        }

        // Vulkan: each draw records into a command buffer of the device's own pool on Godot's main queue family and is
        // submitted at once on Godot's main queue, with a fence that says when the GPU is past it. The target is
        // Godot's texture, which Godot's frame graph keeps in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL (it only copies from
        // it): the commands take it to the color attachment layout for weft's pass and back, so that Godot's copy in the
        // frame it submits next (later on the same queue) reads what weft drew.
        class VulkanDevice final : public RenderDevice {
        public:
            static std::unique_ptr<RenderDevice> make(RenderingDevice* rd, std::string& why) {
                auto device = std::unique_ptr<VulkanDevice>(new VulkanDevice());
                return device->load(rd, why) ? std::move(device) : nullptr;
            }

            ~VulkanDevice() override {
                idle();
                for (const auto& record : records_) {
                    destroy_fence_(device_, record.fence, nullptr);
                }
                if (pool_) {
                    destroy_pool_(device_, pool_, nullptr);
                }
            }

            const char* api() const override {
                return "vulkan";
            }

        protected:
            bool open(Slot& slot, weft_render_backend& table, std::string& why) override {
                const weft_render_vulkan_desc desc{sizeof(desc),        WEFT_RENDER_ABI_VERSION, handle(instance_), handle(physical_),
                                                   handle(device_),     handle(queue_),          family_,           0,
                                                   reinterpret_cast<void*>(get_instance_), RenderDevice::queue};
                weft_render_vulkan* backend = nullptr;
                if (const auto result = weft_render_vulkan_create(&desc, &backend, &table); result != WEFT_RENDER_OK) {
                    why = "no Vulkan backend: " + describe(result);
                    return false;
                }
                slot.backend = backend;
                return true;
            }

            void close(Slot& slot) override {
                if (slot.backend) {
                    weft_render_vulkan_destroy(static_cast<weft_render_vulkan*>(slot.backend));
                    slot.backend = nullptr;
                }
            }

            bool encode(Slot& slot, weft_render_handle prepared, const Target& target, std::string& why) override {
                Record* record = take(why);
                if (!record) {
                    return false;
                }
                const auto image = reinterpret_cast<VkImage>(std::uintptr_t(target.image));
                const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
                if (begin_(record->commands, &begin) != VK_SUCCESS) {
                    why = "vkBeginCommandBuffer failed";
                    return false;
                }
                // From Godot's copy source (its copies of earlier frames read it: an execution dependency) to weft's pass.
                barrier(record->commands, image, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                const weft_render_vulkan_pass native{sizeof(native),
                                                     WEFT_RENDER_ABI_VERSION,
                                                     handle(device_),
                                                     handle(record->commands),
                                                     target.view,
                                                     1,
                                                     0,
                                                     {0, 0, 0, 0},
                                                     target.image};
                const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, slot.identity, target.width, target.height,
                                            WEFT_RENDER_TARGET_LINEAR_BGRA8, 1, &native, sizeof(native)};
                weft_render_handle use{};
                if (const auto result = weft_renderer_encode(slot.renderer, prepared, &pass, &use); result != WEFT_RENDER_OK) {
                    // Nothing of the frame is recorded; the command buffer is begun again at its next use.
                    end_(record->commands);
                    why = "encode failed: " + describe(result);
                    return false;
                }
                // Weft's writes, then Godot's copy (in a later submission on the queue).
                barrier(record->commands, image, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkResult result = end_(record->commands);
                if (result == VK_SUCCESS) {
                    reset_fences_(device_, 1, &record->fence);
                    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    submit_info.commandBufferCount = 1;
                    submit_info.pCommandBuffers = &record->commands;
                    result = queue_submit_(queue_, 1, &submit_info, record->fence);
                }
                if (result != VK_SUCCESS) {
                    // Recorded but never submitted: weft's use is discarded.
                    weft_renderer_discard_encoded(slot.renderer, use);
                    why = "vkQueueSubmit failed (VkResult " + std::to_string(int(result)) + ")";
                    return false;
                }
                record->mark = ++submitted_;
                record->busy = true;
                submit(slot, use);
                return true;
            }

            std::uint64_t mark() override {
                return submitted_;
            }

            std::uint64_t finished() override {
                std::uint64_t done = submitted_;
                for (auto& record : records_) {
                    if (!record.busy) {
                        continue;
                    }
                    if (fence_status_(device_, record.fence) == VK_SUCCESS) {
                        record.busy = false;
                    } else {
                        done = std::min(done, record.mark - 1);
                    }
                }
                return done;
            }

            void idle() override {
                std::vector<VkFence> fences;
                for (const auto& record : records_) {
                    if (record.busy) {
                        fences.push_back(record.fence);
                    }
                }
                if (!fences.empty()) {
                    wait_fences_(device_, std::uint32_t(fences.size()), fences.data(), VK_TRUE, 2'000'000'000ull);
                }
                finished();
            }

        private:
            struct Record {
                VkCommandBuffer commands = VK_NULL_HANDLE;
                VkFence fence = VK_NULL_HANDLE;
                std::uint64_t mark = 0;
                bool busy = false;
            };

            // At most this many recordings in flight; a draw past them waits for the oldest.
            static constexpr std::size_t max_records = 8;

            VulkanDevice() = default;

            bool load(RenderingDevice* rd, std::string& why) {
                instance_ = reinterpret_cast<VkInstance>(std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT, {}, 0)));
                physical_ = reinterpret_cast<VkPhysicalDevice>(
                    std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE, {}, 0)));
                device_ = reinterpret_cast<VkDevice>(std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, {}, 0)));
                queue_ = reinterpret_cast<VkQueue>(std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, {}, 0)));
                family_ = std::uint32_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY, {}, 0));
                if (!instance_ || !physical_ || !device_ || !queue_) {
                    why = "Godot's RenderingDevice gives no Vulkan instance, device or queue";
                    return false;
                }
                // Godot loads the system's Vulkan loader; its vkGetInstanceProcAddr reaches Godot's instance and device.
                HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
                get_instance_ = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
                if (!get_instance_) {
                    why = "no vulkan-1.dll in the process";
                    return false;
                }
                auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_(instance_, "vkGetDeviceProcAddr"));
                auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                    get_instance_(instance_, "vkGetPhysicalDeviceQueueFamilyProperties"));
                if (!get_device || !families) {
                    why = "no vkGetDeviceProcAddr";
                    return false;
                }
                bool all = true;
                const auto load_device = [&](auto& function, const char* name) {
                    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(get_device(device_, name));
                    all = all && function;
                };
                load_device(create_pool_, "vkCreateCommandPool");
                load_device(destroy_pool_, "vkDestroyCommandPool");
                load_device(allocate_, "vkAllocateCommandBuffers");
                load_device(begin_, "vkBeginCommandBuffer");
                load_device(end_, "vkEndCommandBuffer");
                load_device(pipeline_barrier_, "vkCmdPipelineBarrier");
                load_device(queue_submit_, "vkQueueSubmit");
                load_device(create_fence_, "vkCreateFence");
                load_device(destroy_fence_, "vkDestroyFence");
                load_device(fence_status_, "vkGetFenceStatus");
                load_device(reset_fences_, "vkResetFences");
                load_device(wait_fences_, "vkWaitForFences");
                if (!all) {
                    why = "Godot's Vulkan device lacks a core function";
                    return false;
                }
                const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                   family_};
                if (create_pool_(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
                    why = "vkCreateCommandPool failed";
                    return false;
                }
                warn_shared_queue(families);
                godot::UtilityFunctions::print_verbose("weft: drawing on Vulkan, queue family " + godot::String::num_int64(family_));
                return true;
            }

            // Godot submits on its main queue from its render thread, as the device does, under a lock of its own. Its
            // transfer workers, which may submit from other threads, use a queue of the family with the fewest
            // capabilities that can transfer: when that is the main family, it is the same queue, and the device's
            // submissions are not ordered with theirs.
            void warn_shared_queue(PFN_vkGetPhysicalDeviceQueueFamilyProperties families) {
                std::uint32_t count = 0;
                families(physical_, &count, nullptr);
                std::vector<VkQueueFamilyProperties> properties(count);
                families(physical_, &count, properties.data());
                std::uint32_t picked = UINT32_MAX;
                VkQueueFlags fewest = VK_QUEUE_FLAG_BITS_MAX_ENUM;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const VkQueueFlags flags = properties[i].queueFlags;
                    if (properties[i].queueCount && (flags & VK_QUEUE_TRANSFER_BIT) && flags < fewest) {
                        picked = i;
                        fewest = flags;
                    }
                }
                if (picked == family_) {
                    godot::UtilityFunctions::push_warning(
                        "weft: this GPU has no transfer queue family apart from the graphics one, so Godot's resource uploads "
                        "from other threads share the queue weft submits on; loading resources in threads while pages draw "
                        "is unsafe on it");
                }
            }

            Record* take(std::string& why) {
                finished();
                for (auto& record : records_) {
                    if (!record.busy) {
                        return &record;
                    }
                }
                if (records_.size() < max_records) {
                    Record record;
                    const VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
                    const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    if (allocate_(device_, &info, &record.commands) != VK_SUCCESS ||
                        create_fence_(device_, &fence, nullptr, &record.fence) != VK_SUCCESS) {
                        why = "no command buffer or fence";
                        return nullptr;
                    }
                    records_.push_back(record);
                    return &records_.back();
                }
                auto oldest = std::min_element(records_.begin(), records_.end(),
                                               [](const Record& a, const Record& b) { return a.mark < b.mark; });
                if (wait_fences_(device_, 1, &oldest->fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
                    why = "the GPU is 2 s behind";
                    return nullptr;
                }
                oldest->busy = false;
                return &*oldest;
            }

            void barrier(VkCommandBuffer commands, VkImage image, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                         VkImageLayout from, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access, VkImageLayout to) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask = src_access;
                b.dstAccessMask = dst_access;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                pipeline_barrier_(commands, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
            }

            VkInstance instance_ = VK_NULL_HANDLE;
            VkPhysicalDevice physical_ = VK_NULL_HANDLE;
            VkDevice device_ = VK_NULL_HANDLE;
            VkQueue queue_ = VK_NULL_HANDLE;
            std::uint32_t family_ = 0;
            PFN_vkGetInstanceProcAddr get_instance_ = nullptr;
            PFN_vkCreateCommandPool create_pool_ = nullptr;
            PFN_vkDestroyCommandPool destroy_pool_ = nullptr;
            PFN_vkAllocateCommandBuffers allocate_ = nullptr;
            PFN_vkBeginCommandBuffer begin_ = nullptr;
            PFN_vkEndCommandBuffer end_ = nullptr;
            PFN_vkCmdPipelineBarrier pipeline_barrier_ = nullptr;
            PFN_vkQueueSubmit queue_submit_ = nullptr;
            PFN_vkCreateFence create_fence_ = nullptr;
            PFN_vkDestroyFence destroy_fence_ = nullptr;
            PFN_vkGetFenceStatus fence_status_ = nullptr;
            PFN_vkResetFences reset_fences_ = nullptr;
            PFN_vkWaitForFences wait_fences_ = nullptr;
            VkCommandPool pool_ = VK_NULL_HANDLE;
            std::vector<Record> records_;
            std::uint64_t submitted_ = 0;
        };
    } // namespace

    std::unique_ptr<RenderDevice> make_vulkan_device(RenderingDevice* rd, std::string& why) {
        return VulkanDevice::make(rd, why);
    }
} // namespace weft_godot
