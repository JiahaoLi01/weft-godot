#include "render_device.hpp"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "weft/render_backend_d3d12.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using godot::RenderingDevice;
using Microsoft::WRL::ComPtr;

namespace weft_godot {
    namespace {
        // Direct3D 12: each draw records into a command list of the device's own and is executed at once on Godot's
        // command queue, then signals the device's fence. The target is Godot's texture, which Godot's frame graph keeps
        // in the copy-source state (it only copies from it); the commands take it to the render target state for weft's
        // pass and back. Godot uses enhanced barriers where the device has them, its layouts then standing for the
        // legacy states: the commands use what Godot uses.
        class D3D12Device final : public RenderDevice {
        public:
            static std::unique_ptr<RenderDevice> make(RenderingDevice* rd, std::string& why) {
                auto device = std::unique_ptr<D3D12Device>(new D3D12Device());
                return device->load(rd, why) ? std::move(device) : nullptr;
            }

            ~D3D12Device() override {
                idle();
                if (event_) {
                    CloseHandle(event_);
                }
            }

            const char* api() const override {
                return "d3d12";
            }

        protected:
            bool open(Slot& slot, weft_render_backend& table, std::string& why) override {
                const weft_render_d3d12_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, device_, RenderDevice::queue, 0};
                weft_render_d3d12* backend = nullptr;
                if (const auto result = weft_render_d3d12_create(&desc, &backend, &table); result != WEFT_RENDER_OK) {
                    why = "no D3D12 backend: " + describe(result);
                    return false;
                }
                slot.backend = backend;
                return true;
            }

            void close(Slot& slot) override {
                if (slot.backend) {
                    weft_render_d3d12_destroy(static_cast<weft_render_d3d12*>(slot.backend));
                    slot.backend = nullptr;
                }
            }

            bool encode(Slot& slot, weft_render_handle prepared, const Target& target, std::string& why) override {
                auto* resource = reinterpret_cast<ID3D12Resource*>(std::uintptr_t(target.image));
                const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                std::uint32_t format = 0;
                if (!target_format(desc.Format, format)) {
                    why = "Godot's texture is not RGBA8 or BGRA8";
                    return false;
                }
                Record* record = take(why);
                if (!record) {
                    return false;
                }
                if (FAILED(record->allocator->Reset()) || FAILED(record->list->Reset(record->allocator.Get(), nullptr))) {
                    why = "the command list does not reset";
                    return false;
                }
                transition(*record, resource, true);
                const weft_render_d3d12_pass native{sizeof(native), WEFT_RENDER_ABI_VERSION, record->list.Get(), resource, 1, 0, {0, 0, 0, 0}};
                const weft_render_pass pass{sizeof(pass), WEFT_RENDER_ABI_VERSION, slot.identity, target.width, target.height, format, 1,
                                            &native, sizeof(native)};
                weft_render_handle use{};
                if (const auto result = weft_renderer_encode(slot.renderer, prepared, &pass, &use); result != WEFT_RENDER_OK) {
                    // Nothing of the frame is recorded; the list is reset at its next use.
                    record->list->Close();
                    why = "encode failed: " + describe(result);
                    return false;
                }
                transition(*record, resource, false);
                if (FAILED(record->list->Close())) {
                    weft_renderer_discard_encoded(slot.renderer, use);
                    why = "the command list does not close";
                    return false;
                }
                ID3D12CommandList* lists[]{record->list.Get()};
                queue_->ExecuteCommandLists(1, lists);
                record->mark = ++submitted_;
                queue_->Signal(fence_.Get(), record->mark);
                submit(slot, use);
                return true;
            }

            std::uint64_t mark() override {
                return submitted_;
            }

            std::uint64_t finished() override {
                return fence_->GetCompletedValue();
            }

            void idle() override {
                if (!fence_ || fence_->GetCompletedValue() >= submitted_) {
                    return;
                }
                if (SUCCEEDED(fence_->SetEventOnCompletion(submitted_, event_))) {
                    WaitForSingleObject(event_, 2000);
                }
            }

        private:
            struct Record {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> list;
                ComPtr<ID3D12GraphicsCommandList7> enhanced;
                std::uint64_t mark = 0;
            };

            static constexpr std::size_t max_records = 8;

            D3D12Device() = default;

            // weft's name of a texture's format; false for one weft does not draw into.
            static bool target_format(DXGI_FORMAT format, std::uint32_t& weft) {
                switch (format) {
                    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                        weft = WEFT_RENDER_TARGET_LINEAR_RGBA8;
                        return true;
                    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                        weft = WEFT_RENDER_TARGET_LINEAR_BGRA8;
                        return true;
                    default:
                        return false;
                }
            }

            bool load(RenderingDevice* rd, std::string& why) {
                device_ = reinterpret_cast<ID3D12Device*>(std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, {}, 0)));
                queue_ = reinterpret_cast<ID3D12CommandQueue*>(std::uintptr_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, {}, 0)));
                if (!device_ || !queue_) {
                    why = "Godot's RenderingDevice gives no D3D12 device or queue";
                    return false;
                }
                D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
                enhanced_ = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) &&
                            options12.EnhancedBarriersSupported;
                godot::UtilityFunctions::print_verbose(enhanced_ ? "weft: drawing on Direct3D 12 with enhanced barriers"
                                                                 : "weft: drawing on Direct3D 12 with legacy barriers");
                if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
                    why = "no fence";
                    return false;
                }
                event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event_) {
                    why = "no event";
                    return false;
                }
                return true;
            }

            Record* take(std::string& why) {
                const std::uint64_t done = fence_->GetCompletedValue();
                for (auto& record : records_) {
                    if (record.mark <= done) {
                        return &record;
                    }
                }
                if (records_.size() < max_records) {
                    Record record;
                    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&record.allocator))) ||
                        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, record.allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&record.list)))) {
                        why = "no command list";
                        return nullptr;
                    }
                    // Made open; take closes it to reset it like the others.
                    record.list->Close();
                    if (enhanced_ && FAILED(record.list.As(&record.enhanced))) {
                        why = "the device has enhanced barriers but its command lists do not";
                        return nullptr;
                    }
                    records_.push_back(std::move(record));
                    return &records_.back();
                }
                auto oldest = std::min_element(records_.begin(), records_.end(),
                                               [](const Record& a, const Record& b) { return a.mark < b.mark; });
                if (FAILED(fence_->SetEventOnCompletion(oldest->mark, event_)) || WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) {
                    why = "the GPU is 2 s behind";
                    return nullptr;
                }
                return &*oldest;
            }

            // Copy source to render target (into the pass) or back (after it).
            void transition(Record& record, ID3D12Resource* resource, bool into) {
                if (record.enhanced) {
                    D3D12_TEXTURE_BARRIER b{};
                    b.SyncBefore = into ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_RENDER_TARGET;
                    b.SyncAfter = into ? D3D12_BARRIER_SYNC_RENDER_TARGET : D3D12_BARRIER_SYNC_COPY;
                    b.AccessBefore = into ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_RENDER_TARGET;
                    b.AccessAfter = into ? D3D12_BARRIER_ACCESS_RENDER_TARGET : D3D12_BARRIER_ACCESS_COPY_SOURCE;
                    b.LayoutBefore = into ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                    b.LayoutAfter = into ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
                    b.pResource = resource;
                    b.Subresources.IndexOrFirstMipLevel = 0xffffffff;
                    const D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1, {}};
                    D3D12_BARRIER_GROUP groups[]{group};
                    groups[0].pTextureBarriers = &b;
                    record.enhanced->Barrier(1, groups);
                    return;
                }
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = into ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter = into ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_SOURCE;
                record.list->ResourceBarrier(1, &b);
            }

            // Godot's; borrowed while Godot's device lives.
            ID3D12Device* device_ = nullptr;
            ID3D12CommandQueue* queue_ = nullptr;
            bool enhanced_ = false;
            ComPtr<ID3D12Fence> fence_;
            HANDLE event_ = nullptr;
            std::vector<Record> records_;
            std::uint64_t submitted_ = 0;
        };
    } // namespace

    std::unique_ptr<RenderDevice> make_d3d12_device(RenderingDevice* rd, std::string& why) {
        return D3D12Device::make(rd, why);
    }
} // namespace weft_godot
