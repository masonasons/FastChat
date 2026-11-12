#pragma once

// Conversations and messages in SQLite (fastchat.db in the data folder).
// Safe to call from any thread; calls are serialized.

#include "core/conversation.h"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

class Database {
public:
    ~Database();
    bool Open(const std::string& path);
    void Close();
    const std::string& LastError() const { return m_error; }

    std::vector<Conversation> ListConversations();
    bool GetConversation(int64_t id, Conversation& out);
    bool CreateConversation(Conversation& c);      // fills in id and times
    bool UpdateConversation(const Conversation& c); // title, folder, pinned, settings, updated time
    bool TouchConversation(int64_t id);
    bool DeleteConversation(int64_t id);

    std::vector<Message> LoadMessages(int64_t conversationId);
    bool AddMessage(Message& m);                    // fills in id (and createdAt if 0)
    bool UpdateMessage(const Message& m);
    bool DeleteMessage(int64_t id);
    // Delete `id` and every later message of its conversation.
    bool DeleteMessagesFrom(int64_t conversationId, int64_t id);
    bool DeleteAllMessages(int64_t conversationId);

private:
    bool Exec(const char* sql);

    sqlite3* m_db = nullptr;
    std::recursive_mutex m_mutex;
    std::string m_error;
};

Database& AppDatabase();
