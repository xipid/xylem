# Graph Traversal Engine

Xylem features a native graph traversal engine. In traditional relational databases, querying hierarchical directories or graph relationships (like friends-of-friends) requires complex recursive joins. Xylem handles this with a custom path-traversal pipeline.

## Graph Query Clauses
Xylem implements three core operations for navigating hierarchies:
1.  `MATCH(clauses)`: Set the starting root node(s) for the query.
2.  `FOLLOW(clauses)`: Hop from the current nodes to target nodes using a relational link.
3.  `REPEATFOLLOW(clauses, stop_clauses)`: Recursively traverse links until either the target nodes are exhausted or the nodes match a specific `stop_clause`.

The `parent.id` dynamic variable binds the field values of the predecessor node during traversal.

---

## Graph Read (Extracting to XiC Trees)
You can read a graph structure from the database into a hierarchical `TreeBranch` (a tree structure provided by the `xic` helper library):

```cpp
// Traverse down from the root directory recursively
Collection::TreeBranch* tree = xm.graphRead(
    {}, // Read all columns
    GRAPH(
        MATCH(WHERE("id", "=", "root_id")),
        REPEATFOLLOW(WHERE("parent_id", "=", "parent.id"))
    )
);

// 'tree' now contains a nested hierarchy matching your directory tree!
delete tree;
```

### In Query Strings (CLI):
You can query graph structures via CLI using the `GR EXTRACT` macro:
```sql
> GR EXTRACT "/home/user/documents"
```
This is translated under the hood into a graph search:
```
WHERE name="home" FOLLOW parent_id=parent.id, name="user" FOLLOW parent_id=parent.id, name="documents"
```

---

## Graph Write (Cascade Modifications)
You can update multiple matching nodes across a hierarchical path in a single atomic transaction.

```cpp
// Update modification times on all folders, and clear contents of all nested files
xm.graphWrite(GRAPH(
    MATCH(WHERE("id", "=", "root_id")),
    SET(WHERE("mod_time", "=", "updated_at_root")),
    
    REPEATFOLLOW(
        WHERE("parent_id", "=", "parent.id"), 
        WHERE("type", "=", "file") // Stop when we hit a file
    ),
    SET(WHERE("mod_time", "=", "updated_at_dir")), // Updates all directories
    
    FOLLOW(WHERE("type", "=", "file")),
    SET(WHERE("content", "=", "cleared")) // Update file contents
));
```

This ensures updates to directory metadata and nested files occur atomically.
