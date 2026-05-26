#include <Terminal/Command.hpp>
#include <Xylem/Xylem.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <Encoding/Yaml.hpp>

using namespace Xi;
using namespace Collection;
using namespace Terminal;
using namespace Xylem;

#include <Xi/Random.hpp>

TreeItem* convertToYamlTree(const TreeItem* node) {
    if (const RowNode* rn = dynamic_cast<const RowNode*>(node)) {
        TaggedTreeBranch* tb = new TaggedTreeBranch();
        tb->name = rn->name;
        
        // Ensure "id" is printed if available
        if (rn->rId > 0 && !rn->row.has("id")) {
            TaggedTreeItemT<String>* attrId = new TaggedTreeItemT<String>(String((long long)rn->rId));
            attrId->name = "rId";
            tb->add(attrId);
        }

        for (auto it = rn->row.begin(); it != rn->row.end(); ++it) {
            String val = it->value;
            // Truncate large blobs
            if (val.size() > 256) val = val.slice(0, 256) + "... (truncated)";
            TaggedTreeItemT<String>* attr = new TaggedTreeItemT<String>(val);
            attr->name = it->key;
            tb->add(attr);
        }
        
        for (usz i = 0; i < rn->size(); ++i) {
            if ((*rn)[i]) tb->add(convertToYamlTree((*rn)[i]));
        }
        return tb;
    } else if (const TreeBranch* branch = dynamic_cast<const TreeBranch*>(node)) {
        TaggedTreeBranch* tb = new TaggedTreeBranch();
        tb->name = node->getName().isEmpty() ? "Root" : node->getName();
        for (usz i = 0; i < branch->size(); ++i) {
            if ((*branch)[i]) tb->add(convertToYamlTree((*branch)[i]));
        }
        return tb;
    }
    return node->clone();
}

void printResult(const QueryResult& res) {
    if (res.treeResult) {
        printf("Graph Result Node Count: %llu\n", (unsigned long long)res.treeResult->size());
        TreeItem* yamlRoot = convertToYamlTree(res.treeResult);
        printf("%s\n", Encoding::toYAML(*yamlRoot).c_str());
        delete yamlRoot;
        delete res.treeResult;
    } else if (res.readRows.size() > 0) {
        printf("Rows Returned: %llu\n", (unsigned long long)res.readRows.size());
        TreeBranch root;
        for (usz i = 0; i < res.readRows.size() && i < 100; ++i) {
            TaggedTreeBranch* tb = new TaggedTreeBranch();
            tb->name = "Row";
            const auto& row = res.readRows[i];
            for (auto it = row.begin(); it != row.end(); ++it) {
                String val = it->value;
                if (val.size() > 256) val = val.slice(0, 256) + "... (truncated)";
                TaggedTreeItemT<String>* attr = new TaggedTreeItemT<String>(val);
                attr->name = it->key;
                tb->add(attr);
            }
            root.add(tb);
        }
        printf("%s\n", Encoding::toYAML(root).c_str());
        if (res.readRows.size() > 100) printf("  ... (+%llu more)\n", (unsigned long long)(res.readRows.size() - 100));
    } else {
        if (res.code != 0) printf("Result Code: %d\n", res.code);
    }
}

int main(int argc, char** argv) {
    Command args(argc, argv);
    args.description("Xylem Database CLI Interface").version("1.0.0");
    
    if (args.option("--help -h").description("Show help")) {
        printf("%s\n", args.help().data());
        return 0;
    }

    String dbPath = args.primary();
    if (dbPath.isEmpty()) {
        printf("Error: Missing database path.\nUsage: xy <path/to/db.xlm>\n");
        return 1;
    }

    XylemEngine xm;
    xm.config.deviceSize = 1024 * 1024 * 50;
    xm.config.blockSize = 4096;
    
    // File I/O for the block device
    int fd = open((const char*)dbPath.data(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        printf("Error: Could not open or create file %s\n", dbPath.data());
        return 1;
    }
    xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
        String buf;
        buf.allocate(maxOffset - offset);
        buf.fill(0xFF);
        pread(fd, buf.data(), buf.size(), offset);
        return buf;
    };
    xm.config.onDeviceWrite = [fd](u64 offset, String data) -> bool {
        return pwrite(fd, data.data(), data.size(), offset) == (ssize_t)data.size();
    };
    xm.config.onDeviceErase = [fd](u64 offset, u64 maxOffset) -> bool {
        String empty; empty.allocate(maxOffset - offset);
        empty.fill(0xFF);
        pwrite(fd, empty.data(), empty.size(), offset);
        return true;
    };

    
    // Auto Mount (format if it fails)
    if (!xm.mount()) {
        printf("Database not found or corrupt. Formatting new database at %s...\n", dbPath.data());
        if (!xm.format()) {
            printf("Error: Failed to format database!\n");
            return 1;
        }
        if (!xm.mount()) {
            printf("Error: Failed to mount database after formatting!\n");
            return 1;
        }
    }
    printf("Successfully mounted Xylem database at %s.\n", dbPath.data());

    // Interactive Loop
    String line;
    char buffer[4096];
    
    while (true) {
        printf("> ");
        if (!std::fgets(buffer, sizeof(buffer), stdin)) break;
        
        line = buffer;
        if (line.endsWith("\n")) line = line.slice(0, line.size() - 1);
        if (line.endsWith("\r")) line = line.slice(0, line.size() - 1);
        
        if (line.isEmpty()) continue;
        
        String uLine = line.toUpperCase();
        if (uLine == "EXIT" || uLine == "QUIT") break;

        Array<String> tokens = QueryParser::tokenize(line, Array<String>());
        if (tokens.size() == 0) continue;
        
        String cmd = tokens[0].toUpperCase();

        if (cmd == "IO") {
            if (tokens.size() < 3) {
                printf("Error: IO requires <linux_path> <xylem_path>\n");
                continue;
            }
            String linuxPath = tokens[1];
            String xylemPath = tokens[2];
            
            auto uploadFile = [&](const String& srcPath, const String& dstPath) {
                std::ifstream file((const char*)srcPath.data(), std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    printf("Error: Could not open Linux file %s\n", srcPath.data());
                    return;
                }
                std::streamsize size = file.tellg();
                file.seekg(0, std::ios::beg);
                String content;
                content.allocate(size);
                if (file.read((char*)content.data(), size)) {
                    Array<String> parts = dstPath.split("/");
                    Array<String> cleanParts;
                    for (usz i = 0; i < parts.size(); ++i) if (!parts[i].isEmpty()) cleanParts.push(parts[i]);
                    
                    if (cleanParts.size() == 0) return;
                    
                    String currentParentId = "0";
                    
                    // Create directory structure if missing
                    for (usz i = 0; i < cleanParts.size() - 1; ++i) {
                        String partName = cleanParts[i];
                        String q = "READ id WHERE name=%1 parent_id=%2";
                        Array<String> a; a.push(partName); a.push(currentParentId);
                        QueryResult r = xm.query(q, a);
                        
                        bool found = false;
                        if (r.readRows.size() > 0 && r.readRows[0].has("id")) {
                            currentParentId = *r.readRows[0].get("id");
                            found = true;
                        }
                        
                        if (!found) {
                            u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
                            String newId(rnd);
                            String wq = "WRITE name=%1 parent_id=%2 id=%3 type=dir perms=755";
                            Array<String> wa; wa.push(partName); wa.push(currentParentId); wa.push(newId);
                            xm.query(wq, wa);
                            currentParentId = newId;
                        }
                    }
                    
                    String fileName = cleanParts[cleanParts.size() - 1];
                    String fileIdQuery = "READ id WHERE name=%1 parent_id=%2";
                    Array<String> fa; fa.push(fileName); fa.push(currentParentId);
                    QueryResult rFile = xm.query(fileIdQuery, fa);
                    
                    bool fileFound = false;
                    String fileId;
                    if (rFile.readRows.size() > 0 && rFile.readRows[0].has("id")) {
                        fileFound = true;
                        fileId = *rFile.readRows[0].get("id");
                    }
                    
                    if (fileFound) {
                        Array<String> args; args.push(fileId); args.push(content);
                        xm.query("WRITE content:blob=%2 WHERE id=%1", args);
                    } else {
                        u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
                        fileId = String(rnd);
                        Array<String> args; args.push(fileName); args.push(currentParentId); args.push(fileId); args.push(content);
                        xm.query("WRITE name=%1 parent_id=%2 id=%3 type=file content:blob=%4 perms=644", args);
                    }
                    
                    printf("IO: Wrote %llu bytes to %s\n", (unsigned long long)size, dstPath.data());
                } else {
                    printf("Error: Failed to read file %s\n", srcPath.data());
                }
            };

            std::error_code ec;
            std::filesystem::path lPath((const char*)linuxPath.data());
            
            if (std::filesystem::is_directory(lPath, ec)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(lPath, ec)) {
                    if (entry.is_regular_file(ec)) {
                        std::string relPath = std::filesystem::relative(entry.path(), lPath, ec).string();
                        String dest = xylemPath;
                        if (!dest.endsWith("/")) dest += "/";
                        dest += relPath.c_str();
                        uploadFile(entry.path().string().c_str(), dest);
                    }
                }
            } else if (std::filesystem::is_regular_file(lPath, ec)) {
                uploadFile(linuxPath, xylemPath);
            } else {
                printf("Error: %s does not exist or is not a regular file/directory.\n", linuxPath.data());
            }
            continue;
        }

        if (cmd == "OI") {
            if (tokens.size() < 3) {
                printf("Error: OI requires <xylem_path> <linux_path>\n");
                continue;
            }
            String xylemPath = tokens[1];
            String linuxPath = tokens[2];
            
            String q = "GR EXTRACT \"" + xylemPath + "\"";
            QueryResult res = xm.query(q);
            if (res.treeResult && res.treeResult->size() > 0) {
                RowNode* rn = dynamic_cast<RowNode*>((*res.treeResult)[0]);
                if (rn && rn->row.has("content")) {
                    String content = *rn->row.get("content");
                    std::ofstream file((const char*)linuxPath.data(), std::ios::binary);
                    if (file.is_open()) {
                        file.write((const char*)content.data(), content.size());
                        printf("OI: Wrote %llu bytes to %s\n", (unsigned long long)content.size(), linuxPath.data());
                    } else {
                        printf("Error: Could not write to Linux file %s\n", linuxPath.data());
                    }
                } else {
                    printf("Error: Node has no 'content' column.\n");
                }
            } else {
                printf("Error: Xylem path not found.\n");
            }
            if (res.treeResult) delete res.treeResult;
            continue;
        }

        // Pass any other command directly to the Query Parser
        QueryResult res = xm.query(line);
        printResult(res);
    }

    xm.unmount();
    printf("Goodbye.\n");
    return 0;
}
