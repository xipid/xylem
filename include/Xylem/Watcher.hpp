#ifndef XYLEM_WATCHER_HPP
#define XYLEM_WATCHER_HPP

#include <Xylem/Format.hpp>
#include <Xylem/Query.hpp>
#include <Xylem/TableStore.hpp>
#include <Collection/Map.hpp>
#include <Xi/Func.hpp>

namespace Xylem {

using namespace Collection;
using namespace Xi;

struct WatchEntry {
    Array<Clauses> clauses;
    Array<Map<String, String>> inbox;
    Func<void(Map<String, String>)> callback;
};

class Watcher {
public:
    TableStore* tableStore;
    Map<u64, WatchEntry> activeWatches;
    u64 nextWatchId = 1;

    Watcher(TableStore* ts);

    u64 watch(const Array<Clauses>& clauses);
    u64 watch(const Array<Clauses>& clauses, Func<void(Map<String, String>)> cb);
    bool unwatch(u64 id);
    Array<Map<String, String>> pull(u64 id);

    void notify(const Map<String, String>& changedRow);
};

} // namespace Xylem

#endif // XYLEM_WATCHER_HPP
