#include <Encoding/Yaml.hpp>
#include <Sec/Crypto.hpp>
#include <Terminal/Command.hpp>
#include <Xylem/Xylem.hpp>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace Xi;
using namespace Collection;
using namespace Terminal;
using namespace Xylem;

#include <Terminal/Prompt.hpp>
#include <Xi/Random.hpp>

String toHexString(const String &data) {
  const char *hexChars = "0123456789abcdef";
  String hex;
  hex.allocate(data.size() * 2);
  for (usz i = 0; i < data.size(); ++i) {
    u8 byte = data[i];
    hex.push(hexChars[(byte >> 4) & 0xF]);
    hex.push(hexChars[byte & 0xF]);
  }
  return hex;
}

String resolvePath(const String &currentDir, const String &path) {
  if (path.isEmpty() || path == "/")
    return "/";
  String res = path.startsWith("/")
                   ? path
                   : (currentDir.endsWith("/") ? currentDir + path
                                               : currentDir + "/" + path);
  Array<String> parts = res.split("/");
  Array<String> cleanParts;
  for (usz i = 0; i < parts.size(); ++i) {
    if (parts[i].isEmpty() || parts[i] == ".")
      continue;
    if (parts[i] == "..") {
      if (cleanParts.size() > 0)
        cleanParts.pop();
    } else {
      cleanParts.push(parts[i]);
    }
  }
  String finalPath = "";
  for (usz i = 0; i < cleanParts.size(); ++i) {
    finalPath += "/" + cleanParts[i];
  }
  return finalPath.isEmpty() ? "/" : finalPath;
}

RowNode *getDeepestRowNode(TreeItem *item) {
  if (!item)
    return nullptr;
  if (RowNode *rn = dynamic_cast<RowNode *>(item)) {
    return rn;
  }
  if (TreeBranch *tb = dynamic_cast<TreeBranch *>(item)) {
    if (tb->size() > 0) {
      return getDeepestRowNode((*tb)[0]);
    }
  }
  return nullptr;
}

String getPathId(XylemEngine &xm, const String &absolutePath) {
  if (absolutePath == "/" || absolutePath.isEmpty()) {
    return "0";
  }
  Array<String> parts = absolutePath.split("/");
  Array<String> cleanParts;
  for (usz i = 0; i < parts.size(); ++i) {
    if (!parts[i].isEmpty()) {
      cleanParts.push(parts[i]);
    }
  }
  if (cleanParts.size() == 0)
    return "0";

  String currentId = "0";
  for (usz i = 0; i < cleanParts.size(); ++i) {
    String query = "READ id WHERE parent_id=%1 name=%2";
    Array<String> args;
    args.push(currentId);
    args.push(cleanParts[i]);
    QueryResult qr = xm.query(query, args);
    if (qr.readRows.size() == 0 || !qr.readRows[0].has("id")) {
      return ""; // Not found
    }
    currentId = *qr.readRows[0].get("id");
  }
  return currentId;
}

bool getPathRow(XylemEngine &xm, const String &absolutePath,
                Map<String, String> &outRow) {
  if (absolutePath == "/" || absolutePath.isEmpty()) {
    outRow.set("id", "0");
    outRow.set("type", "dir");
    outRow.set("name", "");
    outRow.set("parent_id", "0");
    return true;
  }
  String id = getPathId(xm, absolutePath);
  if (id.isEmpty())
    return false;

  String query = "READ WHERE id=%1";
  Array<String> args;
  args.push(id);
  QueryResult qr = xm.query(query, args);
  if (qr.readRows.size() == 0)
    return false;
  outRow = qr.readRows[0];
  return true;
}

void recursiveRemove(XylemEngine &xm, const String &id) {
  String q = "READ id WHERE parent_id=%1";
  Array<String> args;
  args.push(id);
  QueryResult qr = xm.query(q, args);
  for (usz i = 0; i < qr.readRows.size(); ++i) {
    const auto &row = qr.readRows[i];
    if (row.has("id")) {
      recursiveRemove(xm, *row.get("id"));
    }
  }
  String delQ = "REMOVE WHERE id=%1";
  Array<String> delArgs;
  delArgs.push(id);
  xm.query(delQ, delArgs);
}

TreeItem *convertToYamlTree(const TreeItem *node) {
  if (const RowNode *rn = dynamic_cast<const RowNode *>(node)) {
    TaggedTreeBranch *tb = new TaggedTreeBranch();

    // Ensure "id" is printed if available
    if (rn->rId > 0 && !rn->row.has("id")) {
      TaggedTreeItemT<String> *attrId =
          new TaggedTreeItemT<String>(String((long long)rn->rId));
      attrId->name = "rId";
      tb->add(attrId);
    }

    for (auto it = rn->row.begin(); it != rn->row.end(); ++it) {
      String val = it->value;
      // Truncate large blobs
      if (val.size() > 256)
        val = val.slice(0, 256) + "... (truncated)";
      TaggedTreeItemT<String> *attr = new TaggedTreeItemT<String>(val);
      attr->name = it->key;
      tb->add(attr);
    }

    if (rn->size() > 0) {
      TaggedTreeArrayBranch<TreeItem> *childrenBranch =
          new TaggedTreeArrayBranch<TreeItem>();
      childrenBranch->setName("children");
      for (usz i = 0; i < rn->size(); ++i) {
        if ((*rn)[i])
          childrenBranch->add(convertToYamlTree((*rn)[i]));
      }
      tb->add(childrenBranch);
    }
    return tb;
  } else if (const TreeBranch *branch =
                 dynamic_cast<const TreeBranch *>(node)) {
    TaggedTreeArrayBranch<TreeItem> *tb = new TaggedTreeArrayBranch<TreeItem>();
    tb->setName(branch->getName());
    for (usz i = 0; i < branch->size(); ++i) {
      if ((*branch)[i])
        tb->add(convertToYamlTree((*branch)[i]));
    }
    return tb;
  }
  return node->clone();
}

String colorizeYAML(const String &yaml) {
  if (!isatty(fileno(stdout)))
    return yaml;

  String res;
  Array<String> lines = yaml.split("\n");
  for (usz i = 0; i < lines.size(); ++i) {
    String line = lines[i];
    if (line.isEmpty() && i == lines.size() - 1)
      continue;

    bool hasColon = false;
    usz colonIdx = 0;
    for (usz j = 0; j < line.size(); ++j) {
      if (line[j] == ':') {
        hasColon = true;
        colonIdx = j;
        break;
      }
    }
    if (hasColon) {
      String prefix = line.slice(0, colonIdx);
      String suffix = line.slice(colonIdx + 1, line.size());
      res += "\033[36m" + prefix + "\033[0m:\033[32m" + suffix + "\033[0m\n";
    } else {
      res += line + "\n";
    }
  }
  return res;
}

void printResult(const QueryResult &res) {
  if (res.treeResult) {
    printf("Graph Result Node Count: %llu\n",
           (unsigned long long)res.treeResult->size());
    TreeItem *yamlRoot = convertToYamlTree(res.treeResult);
    printf("%s", colorizeYAML(Encoding::toYAML(*yamlRoot)).c_str());
    delete yamlRoot;
    delete res.treeResult;
  } else if (res.readRows.size() > 0) {
    printf("Rows Returned: %llu\n", (unsigned long long)res.readRows.size());
    TreeBranch root;
    for (usz i = 0; i < res.readRows.size() && i < 100; ++i) {
      TaggedTreeBranch *tb = new TaggedTreeBranch();
      tb->name = "Row";
      const auto &row = res.readRows[i];
      for (auto it = row.begin(); it != row.end(); ++it) {
        String val = it->value;
        if (val.size() > 256)
          val = val.slice(0, 256) + "... (truncated)";
        TaggedTreeItemT<String> *attr = new TaggedTreeItemT<String>(val);
        attr->name = it->key;
        tb->add(attr);
      }
      root.add(tb);
    }
    printf("%s", colorizeYAML(Encoding::toYAML(root)).c_str());
    if (res.readRows.size() > 100)
      printf("  ... (+%llu more)\n",
             (unsigned long long)(res.readRows.size() - 100));
  } else {
    if (res.code != 0)
      printf("Result Code: %d\n", res.code);
  }
}

int main(int argc, char **argv) {
  Command args(argc, argv);
  args.description("Xylem Database CLI Interface").version("1.0.0");

  if (args.option("--help -h").description("Show help")) {
    printf("%s\n", args.help().data());
    return 0;
  }

  String dbPath = args.primary();
  if (dbPath.isEmpty()) {
    printf("Error: Missing database path.\nUsage: xy <path/to/db.xy>\n");
    return 1;
  }

  XylemEngine xm;
  struct stat st;
  if (stat((const char *)dbPath.data(), &st) == 0 && st.st_size > 0) {
    xm.config.deviceSize = st.st_size;
  } else {
    xm.config.deviceSize = 0; // Starts at minimum, expands
  }
  xm.config.deviceExpands = true;
  xm.config.blockSize = 4096;
  xm.config.readSize = 4096;
  xm.config.writeSize = 4096;
  xm.maxCache =
      1024 * 1024 *
      128; // 128MB Cache (prevents table thrashing during bulk operations)

  // File I/O for the block device
  int fd = open((const char *)dbPath.data(), O_RDWR | O_CREAT, 0644);
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
    String empty;
    empty.allocate(maxOffset - offset);
    empty.fill(0xFF);
    pwrite(fd, empty.data(), empty.size(), offset);
    return true;
  };

  // Auto Mount (format if it fails)
  if (!xm.mount()) {
    printf("Database not found or corrupt. Formatting new database at %s...\n",
           dbPath.data());
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
  bool isInteractive = isatty(fileno(stdin));
  String pwd = "/";
  String line;
  Array<String> commandHistory;

  while (true) {
    line = Terminal::Prompt::readLine("> ", &commandHistory);
    if (line == "\x04" || line == "\x03")
      break;

    if (line.isEmpty())
      continue;

    String uLine = line.toUpperCase();
    if (uLine == "EXIT" || uLine == "QUIT")
      break;

    // pwd and cd are xy-binary specific
    Array<String> tokens = QueryParser::tokenize(line, Array<String>());
    if (tokens.size() == 0)
      continue;

    String cmd = tokens[0].toUpperCase();

    if (cmd == "CD") {
      if (tokens.size() < 2) {
        pwd = "/";
      } else {
        String targetPath = resolvePath(pwd, tokens[1]);
        if (targetPath == "/") {
          pwd = "/";
        } else {
          Map<String, String> row;
          if (getPathRow(xm, targetPath, row)) {
            if (row.has("type") && *row.get("type") == "dir") {
              pwd = targetPath;
            } else {
              printf("Error: %s is not a directory.\n", targetPath.c_str());
            }
          } else {
            // Automatically create directories like mkdir -p
            Array<String> parts = targetPath.split("/");
            Array<String> cleanParts;
            for (usz i = 0; i < parts.size(); ++i)
              if (!parts[i].isEmpty())
                cleanParts.push(parts[i]);

            String currentParentId = "0";
            for (usz i = 0; i < cleanParts.size(); ++i) {
              String partName = cleanParts[i];
              String q = "READ id WHERE name=%1 parent_id=%2";
              Array<String> a;
              a.push(partName);
              a.push(currentParentId);
              QueryResult r = xm.query(q, a);

              if (r.readRows.size() > 0 && r.readRows[0].has("id")) {
                currentParentId = *r.readRows[0].get("id");
              } else {
                u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
                String newId(rnd);
                String wq =
                    "WRITE name=%1 parent_id=%2 id=%3 type=dir perms=755";
                Array<String> wa;
                wa.push(partName);
                wa.push(currentParentId);
                wa.push(newId);
                xm.query(wq, wa);
                currentParentId = newId;
              }
            }
            pwd = targetPath;
          }
        }
      }
      continue;
    }

    if (cmd == "PWD") {
      printf("%s\n", pwd.c_str());
      continue;
    }

    if (cmd == "CAT") {
      if (tokens.size() < 2) {
        printf("Error: CAT requires <path>\n");
        continue;
      }
      String targetPath = resolvePath(pwd, tokens[1]);
      QueryResult res = xm.query("CAT \"" + targetPath + "\"");
      bool printed = false;
      if (res.readRows.size() > 0 && res.readRows[0].has("content")) {
        printf("%s\n", res.readRows[0].get("content")->c_str());
        printed = true;
      }
      if (!printed) {
        printf("Error: %s not found or has no content.\n", targetPath.c_str());
      }
      if (res.treeResult)
        delete res.treeResult;
      continue;
    }
    if (cmd == "IO") {
      if (tokens.size() < 3) {
        printf("Error: IO requires <linux_path> <xylem_path>\n");
        continue;
      }
      String linuxPath = tokens[1];
      String xylemPath = resolvePath(pwd, tokens[2]);

      Map<String, String> dirCache;
      dirCache.set("0", "0");

      u64 txId = xm.lock();
      int txCount = 0;

      auto uploadFile = [&](const String &srcPath, const String &dstPath) {
        std::ifstream file((const char *)srcPath.data(),
                           std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
          printf("Error: Could not open Linux file %s\n", srcPath.data());
          return;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        String content;
        content.allocate(size);
        if (file.read((char *)content.data(), size)) {
          Array<String> parts = dstPath.split("/");
          Array<String> cleanParts;
          for (usz i = 0; i < parts.size(); ++i)
            if (!parts[i].isEmpty())
              cleanParts.push(parts[i]);

          if (cleanParts.size() == 0)
            return;

          String currentParentId = "0";
          String currentPathStr = "/";

          // Create directory structure if missing (using cache)
          for (usz i = 0; i < cleanParts.size() - 1; ++i) {
            String partName = cleanParts[i];
            currentPathStr += partName + "/";

            if (dirCache.has(currentPathStr)) {
              currentParentId = *dirCache.get(currentPathStr);
            } else {
              String q = "READ id WHERE name=%1 parent_id=%2";
              Array<String> a;
              a.push(partName);
              a.push(currentParentId);
              QueryResult r = xm.query(q, a);

              if (r.readRows.size() > 0 && r.readRows[0].has("id")) {
                currentParentId = *r.readRows[0].get("id");
              } else {
                u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
                String newId(rnd);
                String wq =
                    "WRITE name=%1 parent_id=%2 id=%3 type=dir perms=755";
                Array<String> wa;
                wa.push(partName);
                wa.push(currentParentId);
                wa.push(newId);
                xm.query(
                    wq,
                    wa); // Dirs are outside the batch tx for safety with READ
                currentParentId = newId;
              }
              dirCache.set(currentPathStr, currentParentId);
            }
          }

          String fileName = cleanParts[cleanParts.size() - 1];
          String fileIdQuery = "READ id WHERE name=%1 parent_id=%2";
          Array<String> fa;
          fa.push(fileName);
          fa.push(currentParentId);
          QueryResult rFile = xm.query(fileIdQuery, fa);

          bool fileFound = false;
          String fileId;
          if (rFile.readRows.size() > 0 && rFile.readRows[0].has("id")) {
            fileFound = true;
            fileId = *rFile.readRows[0].get("id");
          }

          String colName = (content.size() > 512) ? "content:blob" : "content";

          if (fileFound) {
            Array<Clause> writeCols;
            Clause cBlob;
            cBlob.col = colName;
            cBlob.op = "=";
            cBlob.val = content;
            writeCols.push(cBlob);

            Array<Clauses> queryClauses;
            Clauses group;
            Clause cId;
            cId.col = "id";
            cId.op = "=";
            cId.val = fileId;
            group.push(cId);
            queryClauses.push(group);

            xm.write(writeCols, queryClauses, txId);
          } else {
            u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
            fileId = String(rnd);

            Array<Clause> writeCols;
            Clause c1;
            c1.col = "name";
            c1.op = "=";
            c1.val = fileName;
            writeCols.push(c1);
            Clause c2;
            c2.col = "parent_id";
            c2.op = "=";
            c2.val = currentParentId;
            writeCols.push(c2);
            Clause c3;
            c3.col = "id";
            c3.op = "=";
            c3.val = fileId;
            writeCols.push(c3);
            Clause c4;
            c4.col = "type";
            c4.op = "=";
            c4.val = "file";
            writeCols.push(c4);
            Clause c5;
            c5.col = colName;
            c5.op = "=";
            c5.val = content;
            writeCols.push(c5);
            Clause c6;
            c6.col = "perms";
            c6.op = "=";
            c6.val = "644";
            writeCols.push(c6);

            xm.write(writeCols, Array<Clauses>(), txId);
          }

          txCount++;
          if (txCount >= 200) {
            xm.unlock(txId);
            txId = xm.lock();
            txCount = 0;
          }

          printf("IO: Wrote %llu bytes to %s\n", (unsigned long long)size,
                 dstPath.data());
        } else {
          printf("Error: Failed to read file %s\n", srcPath.data());
        }
      };

      std::error_code ec;
      std::filesystem::path lPath((const char *)linuxPath.data());

      if (std::filesystem::is_directory(lPath, ec)) {
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(lPath, ec)) {
          if (entry.is_regular_file(ec)) {
            std::string relPath =
                std::filesystem::relative(entry.path(), lPath, ec).string();
            String dest = xylemPath;
            if (!dest.endsWith("/"))
              dest += "/";
            dest += relPath.c_str();
            uploadFile(entry.path().string().c_str(), dest);
          }
        }
      } else if (std::filesystem::is_regular_file(lPath, ec)) {
        uploadFile(linuxPath, xylemPath);
      } else {
        printf("Error: %s does not exist or is not a regular file/directory.\n",
               linuxPath.data());
      }

      if (txCount > 0)
        xm.unlock(txId);
      xm.flush();
      continue;
    }

    if (cmd == "OI") {
      if (tokens.size() < 3) {
        printf("Error: OI requires <xylem_path> <linux_path>\n");
        continue;
      }
      String xylemPath = resolvePath(pwd, tokens[1]);
      String linuxPath = tokens[2];

      String q = "GR EXTRACT \"" + xylemPath + "\"";
      QueryResult res = xm.query(q);
      if (res.treeResult && res.treeResult->size() > 0) {
        struct ExtractContext {
            XylemEngine* xm;
            void (*extract)(ExtractContext*, const TreeItem*, const String&);
        };
        ExtractContext ctx;
        ctx.xm = &xm;
        ctx.extract = [](ExtractContext* c, const TreeItem* node, const String& lPath) {
            if (!node) return;
            const RowNode* rn = getDeepestRowNode(const_cast<TreeItem*>(node));
            if (!rn) return;
            
            String type = "";
            if (rn->row.has("type")) type = *rn->row.get("type");
            
            if (type == "dir") {
                std::filesystem::create_directories((const char*)lPath.data());
                for (usz i = 0; i < rn->size(); ++i) {
                    if ((*rn)[i]) {
                        const RowNode* child = getDeepestRowNode((*rn)[i]);
                        if (child && child->row.has("name")) {
                            String childName = *child->row.get("name");
                            c->extract(c, (*rn)[i], lPath + "/" + childName);
                        }
                    }
                }
            } else {
                String content;
                bool foundContent = false;
                if (rn->row.has("content")) {
                    content = *rn->row.get("content");
                    foundContent = true;
                } else if (rn->row.has("content:blob")) {
                    content = c->xm->getBlob(rn->row.get("content:blob")->toInt());
                    foundContent = true;
                }
                if (foundContent) {
                    std::ofstream file((const char*)lPath.data(), std::ios::binary);
                    if (file.is_open()) {
                        file.write((const char*)content.data(), content.size());
                        printf("OI: Wrote %llu bytes to %s\n", (unsigned long long)content.size(), lPath.data());
                    } else {
                        printf("Error: Could not write to Linux file %s\n", lPath.data());
                    }
                } else {
                    printf("Error: Node has no 'content' column.\n");
                }
            }
        };
        ctx.extract(&ctx, (*res.treeResult)[0], linuxPath);
      } else {
        printf("Error: Xylem path not found.\n");
      }
      if (res.treeResult)
        delete res.treeResult;
      continue;
    }

    // Intercept path commands to apply pwd
    if (cmd == "LS" || cmd == "CAT" || cmd == "UNLINK") {
      if (tokens.size() > 1) {
        String targetPath = resolvePath(pwd, tokens[1]);
        line = cmd + " \"" + targetPath + "\"";
        for (usz i = 2; i < tokens.size(); ++i) {
          line += " " + tokens[i];
        }
      } else if (cmd == "LS") {
        line = "LS \"" + pwd + "\"";
      }
    } else if (cmd == "TEE") {
      if (tokens.size() > 2) {
        String targetPath = resolvePath(pwd, tokens[1]);
        line = "TEE \"" + targetPath + "\" " + tokens[2];
        for (usz i = 3; i < tokens.size(); ++i) {
          line += " " + tokens[i];
        }
      }
    }

    // Pass any other command directly to the Query Parser
    QueryResult res = xm.query(line);
    printResult(res);
    if (cmd == "WRITE" || cmd == "WRITEVOLATILE" || cmd == "REMOVE" ||
        cmd == "TEE" || cmd == "UNLINK") {
      // xm.flush() is now done internally instantly by XylemEngine.
    }
    
    if (cmd == "VACUUM" || cmd == "VACCUM" || cmd == "DESTROY") {
      u64 unused = xm.getUnusedBlockSpace();
      u64 newSize = xm.config.deviceSize - unused;
      if (newSize > 0) {
        truncate((const char*)dbPath.data(), newSize);
        printf("xy: File truncated to %llu bytes\n", (unsigned long long)newSize);
      }
    }
  }

  xm.vaccum();
  u64 unused = xm.getUnusedBlockSpace();
  u64 newSize = xm.config.deviceSize - unused;
  xm.destroy();
  if (newSize > 0 && newSize < xm.config.deviceSize) {
    truncate((const char*)dbPath.data(), newSize);
    printf("Vacuumed on exit. File size reduced to %llu bytes.\n", (unsigned long long)newSize);
  }
  
  printf("Goodbye.\n");
  return 0;
}
