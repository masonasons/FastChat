#include "core/zip.h"
#include "core/util.h"

#include <miniz.h>

struct ZipFile::Impl {
    std::string data;
    mz_zip_archive zip{};
    bool open = false;
    ~Impl() {
        if (open) mz_zip_reader_end(&zip);
    }
    std::string LastError() {
        return mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    }
};

ZipFile::ZipFile() : m(std::make_unique<Impl>()) {}
ZipFile::~ZipFile() = default;

bool ZipFile::Open(const std::string& path, std::string& error) {
    if (!ReadFileText(path, m->data)) {
        error = "Could not open " + path + ".";
        return false;
    }
    if (!mz_zip_reader_init_mem(&m->zip, m->data.data(), m->data.size(), 0)) {
        error = path + " could not be read as a zip (" + m->LastError() + ").";
        return false;
    }
    m->open = true;
    return true;
}

int ZipFile::Count() const {
    return m->open ? static_cast<int>(mz_zip_reader_get_num_files(&m->zip)) : 0;
}

std::string ZipFile::Name(int index) const {
    mz_zip_archive_file_stat st;
    if (!m->open || !mz_zip_reader_file_stat(&m->zip, static_cast<mz_uint>(index), &st)) return std::string();
    return ReplaceAll(SanitizeUtf8(st.m_filename), "\\", "/");
}

bool ZipFile::IsFolder(int index) const {
    return m->open && mz_zip_reader_is_file_a_directory(&m->zip, static_cast<mz_uint>(index));
}

bool ZipFile::Read(int index, std::string& data, std::string& error) const {
    size_t size = 0;
    void* p = m->open ? mz_zip_reader_extract_to_heap(&m->zip, static_cast<mz_uint>(index), &size, 0) : nullptr;
    if (!p) {
        error = "Could not unpack " + Name(index) + " from the zip (" + m->LastError() + ").";
        return false;
    }
    data.assign(static_cast<const char*>(p), size);
    mz_free(p);
    return true;
}

int ZipFile::Find(const std::string& name) const {
    for (int i = 0; i < Count(); i++) {
        if (Name(i) == name) return i;
    }
    return -1;
}
