#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ainput {

// Results may finish in any order; each original input context has its own
// delivery order. Only the fcitx5 event loop mutates the queue.
template <typename Context, typename Outcome>
class DeliveryQueue {
public:
    struct Entry {
        uint64_t id;
        Context context;
        std::optional<Outcome> outcome;
    };

    void remember(uint64_t id, Context context) {
        entries_.emplace(id, Entry{id, std::move(context), std::nullopt});
    }

    const Entry *find(uint64_t id) const {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }

    bool hasNewer(uint64_t id, const Context &context) const {
        for (auto it = entries_.upper_bound(id); it != entries_.end(); ++it) {
            if (it->second.context == context) return true;
        }
        return false;
    }

    std::vector<Entry> finish(uint64_t id, Outcome outcome) {
        auto it = entries_.find(id);
        if (it == entries_.end() || it->second.outcome) return {};
        const Context context = it->second.context;
        it->second.outcome = std::move(outcome);
        std::vector<Entry> ready;
        for (auto cursor = entries_.begin(); cursor != entries_.end();) {
            if (cursor->second.context != context) {
                ++cursor;
            } else if (!cursor->second.outcome) {
                break;
            } else {
                ready.push_back(std::move(cursor->second));
                cursor = entries_.erase(cursor);
            }
        }
        return ready;
    }

    std::vector<Context> discard() {
        std::vector<Context> targets;
        for (const auto &[id, entry] : entries_) targets.push_back(entry.context);
        entries_.clear();
        return targets;
    }

    auto size() const { return entries_.size(); }

private:
    std::map<uint64_t, Entry> entries_;
};

} // namespace ainput
