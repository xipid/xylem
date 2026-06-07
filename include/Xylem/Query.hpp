#ifndef XYLEM_QUERY_HPP
#define XYLEM_QUERY_HPP

#include <Collection/String.hpp>
#include <Collection/Array.hpp>

namespace Xylem {

using namespace Collection;

struct Clause {
    String col;
    String op;
    String val;
};

struct Clauses : public Array<Clause> {
    bool isAssert = false; // From additions.md: ASSERT <clauses>
    bool isFollow = false;
    bool isRepeatFollow = false;
    bool isOrConnection = false;
};

// ─── Column Type System ──────────────────────────────────────────────────────

enum class ColType : u8 { STRING, BLOB, WATCH };

struct ParsedCol {
    String name;       // "content" (suffix stripped)
    ColType type;      // BLOB, WATCH, or STRING
    String rangeSpec;  // "[0:20]", "[30:]", "[30+]", "[+]", or empty
};

struct BlobRange {
    u64 start;
    u64 end;       // 0 = until value/data ends
    bool isInsert; // true for [30+] (shift data right, insert)
    bool isAppend; // true for [+] (append to end)
    bool isSingleIndex; // true for [x]
    bool valid;    // whether a range was specified at all
};

// Parse "content:blob[0:20]" → {name="content", type=BLOB, range="[0:20]"}
// Parse "_cmd:watch"          → {name="_cmd",    type=WATCH, range=""}
// Parse "_cmd"                → {name="_cmd",    type=WATCH, range=""} (auto)
// Parse "status"              → {name="status",  type=STRING,  range=""}
inline ParsedCol parseCol(const String& raw) {
    ParsedCol result;
    result.type = ColType::STRING;

    // Find range spec [...]
    long long bracketPos = -1;
    for (usz i = 0; i < raw.size(); ++i) {
        if (raw[i] == '[') { bracketPos = (long long)i; break; }
    }

    String nameAndType = (bracketPos >= 0) ? raw.slice(0, bracketPos) : raw;
    result.rangeSpec = (bracketPos >= 0) ? raw.slice(bracketPos) : String();

    // Find type suffix :blob or :watch
    long long colonPos = -1;
    for (usz i = 0; i < nameAndType.size(); ++i) {
        if (nameAndType[i] == ':') { colonPos = (long long)i; break; }
    }

    if (colonPos >= 0) {
        result.name = nameAndType.slice(0, colonPos);
        String suffix = nameAndType.slice(colonPos + 1);
        if (suffix == "blob") result.type = ColType::BLOB;
        else if (suffix == "watch") result.type = ColType::WATCH;
    } else {
        result.name = nameAndType;
    }

    // Auto-detect _ prefix as WATCH
    if (result.type == ColType::STRING && result.name.size() > 0 && result.name[0] == '_') {
        result.type = ColType::WATCH;
    }

    return result;
}

inline BlobRange parseBlobRange(const String& spec) {
    BlobRange r = {0, 0, false, false, false, false};
    if (spec.isEmpty() || spec.size() < 3) return r; // Minimum: "[+]"
    if (spec[0] != '[' || spec[spec.size() - 1] != ']') return r;

    String inner = spec.slice(1, spec.size() - 1); // Strip [ and ]
    r.valid = true;

    // [+] = append
    if (inner == "+") {
        r.isInsert = true;
        r.isAppend = true;
        return r;
    }

    // Check for + suffix (insert mode): [30+]
    if (inner.size() > 0 && inner[inner.size() - 1] == '+') {
        r.isInsert = true;
        inner = inner.slice(0, inner.size() - 1);
    }

    // Parse start:end or start:
    long long colonIdx = -1;
    for (usz i = 0; i < inner.size(); ++i) {
        if (inner[i] == ':') { colonIdx = (long long)i; break; }
    }

    auto parseU64 = [](const String& s) -> u64 {
        u64 val = 0;
        for (usz i = 0; i < s.size(); ++i) {
            if (s[i] >= '0' && s[i] <= '9') val = val * 10 + (s[i] - '0');
        }
        return val;
    };

    if (colonIdx >= 0) {
        String startStr = inner.slice(0, colonIdx);
        String endStr = inner.slice(colonIdx + 1);
        if (!startStr.isEmpty()) r.start = parseU64(startStr);
        if (!endStr.isEmpty()) r.end = parseU64(endStr);
    } else {
        // No colon, check if it's insert mode or single index lookup
        if (r.isInsert) {
            r.start = parseU64(inner);
        } else {
            r.start = parseU64(inner);
            r.isSingleIndex = true;
        }
    }

    return r;
}

// ─── Convenience helpers ─────────────────────────────────────────────────────

inline Clauses WHERE(const String& col, const String& op, const String& val) {
    Clauses c;
    c.push({col, op, val});
    return c;
}

inline Clauses ASSERT_WHERE(const String& col, const String& op, const String& val) {
    Clauses c;
    c.push({col, op, val});
    c.isAssert = true;
    return c;
}

// Support for AND chaining: WHERE("a","=","1") && WHERE("b","=","2")
inline Clauses operator&&(const Clauses& a, const Clauses& b) {
    Clauses result = a;
    for (const auto& clause : b) {
        result.push(clause);
    }
    result.isAssert = a.isAssert || b.isAssert;
    result.isOrConnection = a.isOrConnection || b.isOrConnection;
    return result;
}

// Support for OR grouping
template<typename... Args>
Array<Clauses> OR(Args... args) {
    Array<Clauses> result;
    (result.push(args), ...);
    return result;
}

enum class GraphOpType {
    MATCH,
    FOLLOW,
    REPEATFOLLOW,
    UNTIL,
    SET,
    REMOVE
};

struct GraphOp {
    GraphOpType type;
    Array<Clauses> query;
    Array<Clauses> untilQuery; // Used for REPEATFOLLOW
    Array<Clause> writeSet;    // Used for SET
};

inline GraphOp MATCH(const Array<Clauses>& q) { return {GraphOpType::MATCH, q, {}, {}}; }
inline GraphOp MATCH(const Clauses& q) { Array<Clauses> arr; arr.push(q); return {GraphOpType::MATCH, arr, {}, {}}; }

inline GraphOp FOLLOW(const Array<Clauses>& q) { return {GraphOpType::FOLLOW, q, {}, {}}; }
inline GraphOp FOLLOW(const Clauses& q) { Array<Clauses> arr; arr.push(q); return {GraphOpType::FOLLOW, arr, {}, {}}; }

inline GraphOp REPEATFOLLOW(const Array<Clauses>& q, const Array<Clauses>& u = Array<Clauses>()) { return {GraphOpType::REPEATFOLLOW, q, u, {}}; }
inline GraphOp REPEATFOLLOW(const Clauses& q, const Array<Clauses>& u = Array<Clauses>()) { Array<Clauses> arr; arr.push(q); return {GraphOpType::REPEATFOLLOW, arr, u, {}}; }
inline GraphOp REPEATFOLLOW(const Clauses& q, const Clauses& u) { Array<Clauses> aq; aq.push(q); Array<Clauses> au; au.push(u); return {GraphOpType::REPEATFOLLOW, aq, au, {}}; }
inline GraphOp REPEATFOLLOW(const Array<Clauses>& q, const Clauses& u) { Array<Clauses> au; au.push(u); return {GraphOpType::REPEATFOLLOW, q, au, {}}; }

inline GraphOp UNTIL(const Array<Clauses>& q) { return {GraphOpType::UNTIL, q, {}, {}}; }
inline GraphOp UNTIL(const Clauses& q) { Array<Clauses> arr; arr.push(q); return {GraphOpType::UNTIL, arr, {}, {}}; }

inline GraphOp SET(const Array<Clause>& s) { return {GraphOpType::SET, {}, {}, s}; }
inline GraphOp SET(const Clause& s) { Array<Clause> arr; arr.push(s); return {GraphOpType::SET, {}, {}, arr}; }

// Helper for variadic GraphOp chaining
template<typename... Args>
Array<GraphOp> GRAPH(Args... args) {
    Array<GraphOp> result;
    (result.push(args), ...);
    return result;
}

} // namespace Xylem

#endif // XYLEM_QUERY_HPP
