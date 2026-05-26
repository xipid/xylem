#include <Xylem/Watcher.hpp>

namespace Xylem {

Watcher::Watcher(TableStore* ts) : tableStore(ts) {}

u64 Watcher::watch(const Array<Clauses>& clauses) {
    u64 id = nextWatchId++;
    WatchEntry entry;
    entry.clauses = clauses;
    activeWatches.set(id, entry);
    return id;
}

u64 Watcher::watch(const Array<Clauses>& clauses, Func<void(Map<String, String>)> cb) {
    u64 id = nextWatchId++;
    WatchEntry entry;
    entry.clauses = clauses;
    entry.callback = cb;
    activeWatches.set(id, entry);
    return id;
}

bool Watcher::unwatch(u64 id) {
    if (activeWatches.has(id)) {
        activeWatches.remove(id);
        return true;
    }
    return false;
}

Array<Map<String, String>> Watcher::pull(u64 id) {
    if (activeWatches.has(id)) {
        auto& entry = *activeWatches.get(id);
        Array<Map<String, String>> result = entry.inbox;
        entry.inbox.clear();
        return result;
    }
    return Array<Map<String, String>>();
}

void Watcher::notify(const Map<String, String>& changedRow) {
    for (auto& pair : activeWatches) {
        if (tableStore->evaluateClauses(changedRow, pair.value.clauses) >= 0.0f) {
            if (pair.value.callback) {
                pair.value.callback(changedRow);
            } else {
                pair.value.inbox.push(changedRow);
            }
        }
    }
}

} // namespace Xylem
