#pragma once

// Conversations from other apps, made into FastChat conversations. Today:
// Basilisk (.bskc, https://github.com/SigmaNight/basiliskLLM).

#include "core/conversation.h"

#include <string>
#include <vector>

class ZipFile;

struct ImportedConversation {
    Conversation conversation;       // title, type, times and settings; not saved yet
    std::vector<Message> messages;   // in order, the first a note saying where it came from
    std::string source;              // "Basilisk"
    int exchanges = 0;               // the user's messages brought over
};

// The files the import reads, for an open dialog ("Basilisk conversations (*.bskc)|*.bskc|...").
const char* ImportFileFilter();

// Reads another app's conversation file. `error` says why not, in a sentence.
bool ImportConversationFile(const std::string& path, ImportedConversation& out, std::string& error);

// Basilisk's conversation.json; `zip` (may be null) is the .bskc it came from,
// for its attachments. `name` stands in for a missing title.
bool ImportBasiliskJson(const std::string& jsonText, const ZipFile* zip, const std::string& name,
                        ImportedConversation& out, std::string& error);

// "2026-11-08T16:15:20.483556" (local time), or with Z or +hh:mm: Unix seconds, 0 if unreadable.
int64_t ParseIsoTime(const std::string& text);
