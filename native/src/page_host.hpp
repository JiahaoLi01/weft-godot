#pragma once

#include "owner_thread.hpp"
#include "weft/weft.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace weft_godot {
    // The runtime and the Views the extension made, on the owner thread: start makes a runtime, and every View keeps the
    // messages (their values as MessagePack) and diagnostics of its last advance for the Godot side to read. Each runtime
    // has an epoch, so that the render thread knows to make its renderer again for a new one. Nothing here knows Godot:
    // the files pages name are read by the reader the extension gives.
    class PageHost {
    public:
        // Reads a file a page names (a url() image) by the path weft resolved against the page's base; false when there is
        // none. Runs on the owner thread.
        using Reader = std::function<bool(std::string_view path, std::string& bytes)>;

        static PageHost& get();

        // Runs work on the owner thread and returns once it has run.
        void call(const std::function<void()>& work) {
            owner_->call(work);
        }
        weft_runtime* runtime() const {
            return runtime_.load();
        }
        std::uint64_t epoch() const {
            return epoch_.load();
        }
        // Before the first View; any thread.
        void set_reader(Reader reader);

        // On the owner thread.
        weft_result start(std::uint32_t flags, std::uint32_t max_views);
        // Stopping is two steps, so that the render thread can finish what it does with the runtime between them: detach
        // (any thread) takes the runtime from the host (runtime() is null after it) with the Views it made, and destroy
        // (owner thread) destroys them.
        struct Detached {
            weft_runtime* runtime = nullptr;
            std::vector<weft_view_handle> views;
        };
        Detached detach();
        static void destroy(const Detached& detached);
        weft_result create_view(const weft_view_desc& desc, weft_view_handle& out);
        weft_result destroy_view(weft_view_handle view);
        weft_result advance(weft_view_handle view, double time_seconds);

        struct Message {
            std::uint32_t kind = 0;
            std::uint64_t call_id = 0;
            std::string name;
            std::string payload; // MessagePack
        };
        struct Diagnostic {
            std::uint32_t kind = 0;
            std::string text;
        };
        // What the View's advances kept since the last take (any thread).
        void take(weft_view_handle view, std::vector<Message>& messages, std::vector<Diagnostic>& diagnostics);

    private:
        PageHost();

        struct Kept {
            weft_view_handle view{};
            std::vector<Message> messages;
            std::vector<Diagnostic> diagnostics;
        };
        Kept* find(weft_view_handle view);
        static weft_result read(void* user, const char* path, size_t path_size, weft_resource_writer* file);

        // Lives as long as the process: never joined from a static destructor under the loader's lock.
        OwnerThread* owner_;
        std::atomic<weft_runtime*> runtime_{nullptr};
        std::atomic<std::uint64_t> epoch_{0};
        std::mutex lock_;
        std::vector<Kept> views_;
        Reader reader_;
    };

    bool same_view(weft_view_handle a, weft_view_handle b);
} // namespace weft_godot
