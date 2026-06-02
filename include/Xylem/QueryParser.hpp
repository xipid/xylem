#ifndef XYLEM_QUERYPARSER_HPP
#define XYLEM_QUERYPARSER_HPP

#include <Xylem/Format.hpp>
#include <Xylem/Query.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>
#include <Collection/String.hpp>
#include <Collection/Tree.hpp>

namespace Xylem {

class XylemEngine; // Forward declaration

struct QueryResult {
    int code = -1;
    Collection::TreeBranch* treeResult = nullptr;
    Collection::Array<Collection::Map<Collection::String, Collection::String>> readRows;
};

class QueryParser {
public:
    static QueryResult execute(XylemEngine* engine, const Collection::String& query, const Collection::Array<Collection::String>& args = Collection::Array<Collection::String>());

    // Visible for testing or internal composition
    static Collection::Array<Collection::String> tokenize(const Collection::String& query, const Collection::Array<Collection::String>& args);
    static Collection::Array<GraphOp> parseExtract(const Collection::String& pathStr, bool recursive = true);
};

} // namespace Xylem

#endif // XYLEM_QUERYPARSER_HPP
