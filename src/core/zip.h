#pragma once

// Reading zip files (sound packs, Basilisk conversations) with miniz. The
// whole file is read into memory, so any path works.

#include <memory>
#include <string>

class ZipFile {
public:
    ZipFile();
    ~ZipFile();
    ZipFile(const ZipFile&) = delete;
    ZipFile& operator=(const ZipFile&) = delete;

    bool Open(const std::string& path, std::string& error);
    int Count() const;
    // An entry's path in the zip, with / between folders (valid UTF-8).
    std::string Name(int index) const;
    bool IsFolder(int index) const;
    bool Read(int index, std::string& data, std::string& error) const;
    // The entry with this path (case-sensitive, / between folders), or -1.
    int Find(const std::string& name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
