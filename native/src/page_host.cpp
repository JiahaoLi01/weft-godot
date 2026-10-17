#include "page_host.hpp"

namespace weft_godot {
    bool same_view(weft_view_handle a, weft_view_handle b) {
        return a.runtime_id == b.runtime_id && a.slot == b.slot && a.generation == b.generation;
    }

    PageHost& PageHost::get() {
        // Made once and kept for the process's life, with its owner thread.
        static PageHost* host = new PageHost();
        return *host;
    }

    PageHost::PageHost() : owner_(new OwnerThread()) {
    }

    void PageHost::set_reader(Reader reader) {
        std::lock_guard lock(lock_);
        reader_ = std::move(reader);
    }

    weft_result PageHost::start(std::uint32_t flags, std::uint32_t max_views) {
        if (runtime_.load()) {
            return WEFT_BUSY;
        }
        // 0 is weft's default (the count a runtime made without a description has).
        constexpr std::uint32_t default_max_views = 64;
        const weft_runtime_desc desc{sizeof(desc), WEFT_ABI_VERSION, max_views ? max_views : default_max_views, flags};
        weft_runtime* runtime = nullptr;
        const auto result = weft_runtime_create(&desc, &runtime);
        if (result == WEFT_OK) {
            ++epoch_;
            runtime_.store(runtime);
        }
        return result;
    }

    PageHost::Detached PageHost::detach() {
        Detached detached;
        detached.runtime = runtime_.exchange(nullptr);
        std::lock_guard lock(lock_);
        for (const auto& kept : views_) {
            detached.views.push_back(kept.view);
        }
        views_.clear();
        return detached;
    }

    void PageHost::destroy(const Detached& detached) {
        if (!detached.runtime) {
            return;
        }
        for (const auto& view : detached.views) {
            weft_view_destroy(detached.runtime, view);
        }
        weft_runtime_destroy(detached.runtime);
    }

    weft_result PageHost::create_view(const weft_view_desc& desc, weft_view_handle& out) {
        weft_runtime* runtime = runtime_.load();
        if (!runtime) {
            return WEFT_STALE_HANDLE;
        }
        const auto result = weft_view_create(runtime, &desc, &out);
        if (result != WEFT_OK) {
            return result;
        }
        weft_view_set_resource_reader(runtime, out, &PageHost::read, this);
        std::lock_guard lock(lock_);
        views_.push_back({out, {}, {}});
        return WEFT_OK;
    }

    weft_result PageHost::destroy_view(weft_view_handle view) {
        weft_runtime* runtime = runtime_.load();
        if (!runtime) {
            return WEFT_STALE_HANDLE;
        }
        {
            std::lock_guard lock(lock_);
            std::erase_if(views_, [&](const Kept& kept) { return same_view(kept.view, view); });
        }
        return weft_view_destroy(runtime, view);
    }

    weft_result PageHost::advance(weft_view_handle view, double time_seconds) {
        weft_runtime* runtime = runtime_.load();
        if (!runtime) {
            return WEFT_STALE_HANDLE;
        }
        const auto result = weft_view_advance(runtime, view, time_seconds);
        std::vector<Message> sent;
        std::vector<Diagnostic> said;
        weft_view_drain_messages(
            runtime, view,
            [](void* user, weft_view_handle, const weft_message* m) {
                static_cast<std::vector<Message>*>(user)->push_back(
                    {m->kind, m->call_id, std::string(m->name, m->name_size),
                     std::string(static_cast<const char*>(m->payload), m->payload_size)});
            },
            &sent);
        weft_view_drain_diagnostics(
            runtime, view,
            [](void* user, weft_view_handle, const weft_diagnostic* d) {
                static_cast<std::vector<Diagnostic>*>(user)->push_back({d->kind, std::string(d->message, d->message_size)});
            },
            &said, nullptr);
        std::lock_guard lock(lock_);
        if (Kept* kept = find(view)) {
            for (auto& message : sent) {
                kept->messages.push_back(std::move(message));
            }
            for (auto& diagnostic : said) {
                kept->diagnostics.push_back(std::move(diagnostic));
            }
        }
        return result;
    }

    void PageHost::take(weft_view_handle view, std::vector<Message>& messages, std::vector<Diagnostic>& diagnostics) {
        messages.clear();
        diagnostics.clear();
        std::lock_guard lock(lock_);
        if (Kept* kept = find(view)) {
            messages.swap(kept->messages);
            diagnostics.swap(kept->diagnostics);
        }
    }

    PageHost::Kept* PageHost::find(weft_view_handle view) {
        for (auto& kept : views_) {
            if (same_view(kept.view, view)) {
                return &kept;
            }
        }
        return nullptr;
    }

    // A file a page names, by the path weft resolved against the page's base: read by the extension's reader. Left
    // unwritten when there is none (weft diagnoses it).
    weft_result PageHost::read(void* user, const char* path, size_t path_size, weft_resource_writer* file) {
        auto* host = static_cast<PageHost*>(user);
        Reader reader;
        {
            std::lock_guard lock(host->lock_);
            reader = host->reader_;
        }
        std::string bytes;
        if (!reader || !reader(std::string_view(path, path_size), bytes)) {
            return WEFT_OK;
        }
        return weft_resource_writer_set(file, bytes.data(), bytes.size());
    }
} // namespace weft_godot
