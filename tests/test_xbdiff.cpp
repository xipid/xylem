#include <Xylem/XBDiff.hpp>
#include <Terminal/Format.hpp>

using namespace Xylem;
using namespace Terminal;

int main() {
    Info("--- XBDiff Unit Tests ---");

    // Test 1: Simple Diff Creation and Application
    {
        Info("Test 1: Create diff and apply");
        String oldData = "Hello World, this is Xylem database!";
        String newData = "Hello Xylem World, this is a cool database!";
        
        XBDiff df = XBDiff::create(oldData, newData);
        String applied = df.toBinaryContent();
        
        if (applied == newData) {
            Success("Test 1 passed: Diff applied matches new data!");
        } else {
            Error("Test 1 failed: Applied content mismatch! Got: " + applied);
            return 1;
        }
    }

    // Test 2: Binary Serialization and Deserialization
    {
        Info("Test 2: Serialize and deserialize XBDiff");
        String oldData = "abcdefghijklmnopqrstuvwxyz";
        String newData = "abcdefg12345klmnopqrstuvwxyz!!";
        
        XBDiff df = XBDiff::create(oldData, newData);
        String bin = df.toBinary();
        
        XBDiff df2 = XBDiff::fromBinary(bin, oldData);
        String applied = df2.toBinaryContent();
        
        if (applied == newData) {
            Success("Test 2 passed: Serialized/Deserialized diff application matches!");
        } else {
            Error("Test 2 failed: Mismatch after deserialize! Got: " + applied);
            return 1;
        }
        
        Info(df2.toString());
    }

    // Test 3: Edit Utilities (splice, set, operator[])
    {
        Info("Test 3: Edit utilities (splice, set, operator[])");
        String base = "abcdefgh";
        XBDiff df(base);
        
        // Size check
        if (df.size() != 8) {
            Error("Initial size should be 8, got " + String::from((u64)df.size()));
            return 1;
        }
        
        // operator[] read
        if (df[0] != 'a' || df[7] != 'h') {
            Error("Initial index read incorrect");
            return 1;
        }
        
        // operator[] write
        df[0] = 'z';
        u8 val = df.getByte(0);
        if (df[0] != 'z') {
            Error("operator[] write failed, got " + String::from((const u8*)&val, 1));
            return 1;
        }
        
        // set
        df.set(2, "XYZ"); // ab -> zcXYZfgh ? Wait, 0=z, 1=b, 2=X, 3=Y, 4=Z, 5=f, 6=g, 7=h
        if (df.toBinaryContent() != "zbXYZfgh") {
            Error("set failed, got: " + df.toBinaryContent());
            return 1;
        }
        
        // splice (delete & insert)
        df.splice(2, 3, "12"); // zb12fgh
        if (df.toBinaryContent() != "zb12fgh") {
            Error("splice failed, got: " + df.toBinaryContent());
            return 1;
        }
        
        Success("Test 3 passed: Edit utilities worked perfectly!");
    }

    Success("All XBDiff tests completed successfully!");
    return 0;
}
