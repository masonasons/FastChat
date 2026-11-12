#pragma once

// Files the user attaches to a message: images and PDFs go with it as
// themselves (Message::images); text files go into its text.

#include "core/conversation.h"

#include <string>
#include <vector>

struct Attachment {
    std::string name;       // the file's name
    std::string mediaType;  // "image/png", "application/pdf", "text/plain"...
    std::string data;       // its bytes
    bool IsText() const { return mediaType.rfind("text/", 0) == 0; }
};

// The largest files that are attached (the services' own limits, roughly).
constexpr size_t kMaxImageBytes = 5 * 1024 * 1024;
constexpr size_t kMaxPdfBytes = 30 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 1024 * 1024;

// The kind of file these bytes are, from their first bytes and the name:
// "image/png", "image/jpeg", "image/gif", "image/webp", "application/pdf",
// "text/plain", or "" for something FastChat does not attach.
std::string AttachmentType(const std::string& name, const std::string& data);

// A file read to be attached. `error` says why not: too large, or a kind
// FastChat cannot send.
bool LoadAttachment(const std::string& path, Attachment& out, std::string& error);
// The same for bytes already in memory (a pasted image).
bool MakeAttachment(const std::string& name, const std::string& data, Attachment& out, std::string& error);

// Puts them on the message: images and PDFs with it, text files at the end of
// its text ("[Attached file: notes.txt]" and the file).
void AttachToMessage(Message& m, const std::vector<Attachment>& attachments);

// How a text file reads in a message's text.
std::string AttachedTextBlock(const std::string& name, const std::string& text);

// "pic.png, 120 KB image" for lists.
std::string DescribeAttachment(const Attachment& a);

// What a message carries besides its text, for its row: ", with an image",
// ", with pic.png and notes.pdf". Empty when nothing.
std::string AttachmentsSuffix(const Message& m);
