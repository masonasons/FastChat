#include "core/attachments.h"
#include "core/platform.h"
#include "core/util.h"

#include <filesystem>

std::string AttachmentType(const std::string& name, const std::string& data) {
    auto starts = [&](const char* magic, size_t n, size_t at = 0) { return data.size() >= at + n && data.compare(at, n, magic, n) == 0; };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (starts("\xFF\xD8\xFF", 3)) return "image/jpeg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "image/gif";
    if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return "image/webp";
    if (starts("%PDF-", 5)) return "application/pdf";
    // Text: valid UTF-8 without NUL bytes (source, notes, CSV, JSON, logs...).
    if (data.find('\0') == std::string::npos && IsValidUtf8(data)) return "text/plain";
    (void)name;
    return std::string();
}

bool MakeAttachment(const std::string& name, const std::string& data, Attachment& out, std::string& error) {
    std::string type = AttachmentType(name, data);
    if (type.empty()) {
        error = name + " is not something FastChat can attach. It attaches images (PNG, JPEG, GIF, WebP), PDFs and text files.";
        return false;
    }
    size_t limit = StartsWith(type, "image/") ? kMaxImageBytes : type == "application/pdf" ? kMaxPdfBytes : kMaxTextBytes;
    if (data.size() > limit) {
        error = name + " is " + FormatBytes(data.size()) + "; FastChat attaches " +
                (StartsWith(type, "image/") ? "images" : type == "application/pdf" ? "PDFs" : "text files") + " up to " +
                FormatBytes(limit) + ".";
        return false;
    }
    out.name = name;
    out.mediaType = type;
    out.data = data;
    // A UTF-8 byte order mark says nothing to the model.
    if (out.IsText() && StartsWith(out.data, "\xEF\xBB\xBF")) out.data.erase(0, 3);
    return true;
}

bool LoadAttachment(const std::string& path, Attachment& out, std::string& error) {
    std::string name = PathToUtf8(PathFromUtf8(path).filename());
    std::error_code ec;
    auto size = std::filesystem::file_size(PathFromUtf8(path), ec);
    if (ec) {
        error = "Could not read " + name + ".";
        return false;
    }
    if (size > kMaxPdfBytes) {
        error = name + " is " + FormatBytes(size) + ", too large to attach.";
        return false;
    }
    std::string data;
    if (!ReadFileText(path, data)) {
        error = "Could not read " + name + ".";
        return false;
    }
    return MakeAttachment(name, data, out, error);
}

std::string AttachedTextBlock(const std::string& name, const std::string& text) {
    return "[Attached file: " + name + "]\n" + text;
}

void AttachToMessage(Message& m, const std::vector<Attachment>& attachments) {
    for (const auto& a : attachments) {
        if (a.IsText()) {
            if (!m.content.empty()) m.content += "\n\n";
            m.content += AttachedTextBlock(a.name, a.data);
        } else {
            m.images.push_back({a.mediaType, Base64Encode(a.data), a.name});
        }
    }
}

std::string DescribeAttachment(const Attachment& a) {
    std::string kind = StartsWith(a.mediaType, "image/") ? "image" : a.mediaType == "application/pdf" ? "PDF" : "text file";
    return a.name + ", " + FormatBytes(a.data.size()) + " " + kind;
}

std::string AttachmentsSuffix(const Message& m) {
    if (m.images.empty()) return std::string();
    std::vector<std::string> names;
    for (const auto& i : m.images) {
        if (i.name.empty()) return m.images.size() == 1 ? std::string(", with an image")
                                                        : ", with " + std::to_string(m.images.size()) + " images";
        names.push_back(i.name);
    }
    std::string list = names[0];
    for (size_t i = 1; i < names.size(); i++) list += (i + 1 == names.size() ? " and " : ", ") + names[i];
    return ", with " + list;
}
