# Tree-Typed Columns (YAML/JSON)

For complex rows, standard flat column values (strings/integers) are not enough. You might want to store hierarchical configuration files, metadata dictionaries, or array lists.

Xylem supports tree serialization directly inside table columns by utilizing YAML or JSON.

---

## Writing Tree Columns
To write nested configurations, serialize the tree into a text format (like YAML) and insert it as a column value:

```cpp
#include <Xylem/Xylem.hpp>

void saveProfile(Xylem::XylemEngine& xm) {
    // Hierarchical profile
    String yamlContent = 
        "name: Alice\n"
        "skills:\n"
        "  - cpp\n"
        "  - rust\n";

    xm.write({
        {"id", "=", "user_101"},
        {"profile", "=", yamlContent}
    });
}
```

---

## Reading and Parsing Tree Columns
When you read the column back, parse it back into a Tree using Xylem's parsing helpers (provided via `xic`):

```cpp
#include <Xylem/Xylem.hpp>
#include <Encoding/Yaml.hpp>

void loadProfile(Xylem::XylemEngine& xm) {
    auto rows = xm.read({"profile"}, {WHERE("id", "=", "user_101")});
    if (rows.empty()) return;

    String profileYaml = rows[0]["profile"];

    // Parse back into an XiC TaggedTreeBranch structure
    TaggedTreeBranch root;
    Encoding::parseYAML(profileYaml, root);

    // Retrieve child properties
    auto nameItem = root.get<TaggedTreeItemT<String>>("name");
    if (nameItem) {
        printf("Username: %s\n", nameItem->value.data());
    }
}
```

This allows storing document-oriented structures within your tabular rows.
