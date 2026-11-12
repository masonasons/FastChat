#include "core/database.h"
#include "core/util.h"

#include <sqlite3.h>

Database& AppDatabase() {
    static Database db;
    return db;
}

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr); }
    ~Stmt() { if (m_stmt) sqlite3_finalize(m_stmt); }
    explicit operator bool() const { return m_stmt != nullptr; }
    Stmt& Bind(int i, int64_t v) { sqlite3_bind_int64(m_stmt, i, v); return *this; }
    Stmt& Bind(int i, double v) { sqlite3_bind_double(m_stmt, i, v); return *this; }
    Stmt& Bind(int i, const std::string& v) {
        sqlite3_bind_text(m_stmt, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    int Step() { return m_stmt ? sqlite3_step(m_stmt) : SQLITE_ERROR; }
    bool Run() { return Step() == SQLITE_DONE; }
    int64_t Int(int col) { return sqlite3_column_int64(m_stmt, col); }
    double Double(int col) { return sqlite3_column_double(m_stmt, col); }
    std::string Text(int col) {
        const unsigned char* t = sqlite3_column_text(m_stmt, col);
        return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(m_stmt, col)) : std::string();
    }

private:
    sqlite3_stmt* m_stmt = nullptr;
};

json ParseJson(const std::string& text) {
    if (text.empty()) return json();
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json() : j;
}

std::string ToolCallsToText(const std::vector<ToolCall>& calls) {
    if (calls.empty()) return std::string();
    json arr = json::array();
    for (const auto& c : calls) arr.push_back({{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
    return JsonDump(arr);
}

std::vector<ToolCall> ToolCallsFromText(const std::string& text) {
    std::vector<ToolCall> calls;
    json arr = ParseJson(text);
    if (!arr.is_array()) return calls;
    for (const auto& c : arr) {
        calls.push_back({c.value("id", std::string()), c.value("name", std::string()), c.value("arguments", std::string())});
    }
    return calls;
}

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS conversations (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    type TEXT NOT NULL DEFAULT 'chat',
    folder TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    updated INTEGER NOT NULL,
    pinned INTEGER NOT NULL DEFAULT 0,
    settings TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY,
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    role TEXT NOT NULL,
    content TEXT NOT NULL DEFAULT '',
    reasoning TEXT NOT NULL DEFAULT '',
    tool_calls TEXT NOT NULL DEFAULT '',
    tool_call_id TEXT NOT NULL DEFAULT '',
    tool_name TEXT NOT NULL DEFAULT '',
    tool_error INTEGER NOT NULL DEFAULT 0,
    provider_id TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    usage TEXT NOT NULL DEFAULT '{}',
    created INTEGER NOT NULL,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    stop_reason TEXT NOT NULL DEFAULT '',
    provider_data TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS messages_by_conversation ON messages(conversation_id, id);
)SQL";

}  // namespace

Database::~Database() {
    Close();
}

bool Database::Open(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (sqlite3_open_v2(path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        m_error = m_db ? sqlite3_errmsg(m_db) : "Could not open the database";
        Close();
        return false;
    }
    sqlite3_busy_timeout(m_db, 3000);
    Exec("PRAGMA journal_mode=WAL;");
    Exec("PRAGMA foreign_keys=ON;");
    return Exec(kSchema);
}

void Database::Close() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool Database::Exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        m_error = err ? err : "SQL error";
        sqlite3_free(err);
        return false;
    }
    return true;
}

static Conversation ReadConversation(Stmt& s) {
    Conversation c;
    c.id = s.Int(0);
    c.title = s.Text(1);
    c.type = s.Text(2) == "code" ? ConversationType::Code : ConversationType::Chat;
    c.folder = s.Text(3);
    c.createdAt = s.Int(4);
    c.updatedAt = s.Int(5);
    c.pinned = s.Int(6) != 0;
    c.settings = ConversationSettingsFromJson(ParseJson(s.Text(7)));
    c.messageCount = s.Int(8);
    c.totalCost = s.Double(9);
    c.totalInputTokens = s.Int(10);
    c.totalOutputTokens = s.Int(11);
    return c;
}

#define CONVERSATION_COLUMNS                                                                               \
    "c.id, c.title, c.type, c.folder, c.created, c.updated, c.pinned, c.settings, "                       \
    "(SELECT COUNT(*) FROM messages m WHERE m.conversation_id = c.id), "                                   \
    "(SELECT COALESCE(SUM(MAX(COALESCE(json_extract(m.usage, '$.cost'), 0), 0)), 0) FROM messages m "      \
    "  WHERE m.conversation_id = c.id), "                                                                  \
    "(SELECT COALESCE(SUM(json_extract(m.usage, '$.in')), 0) FROM messages m WHERE m.conversation_id = c.id), " \
    "(SELECT COALESCE(SUM(json_extract(m.usage, '$.out')), 0) FROM messages m WHERE m.conversation_id = c.id) "

std::vector<Conversation> Database::ListConversations() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Conversation> list;
    if (!m_db) return list;
    Stmt s(m_db, "SELECT " CONVERSATION_COLUMNS " FROM conversations c ORDER BY c.pinned DESC, c.updated DESC");
    while (s.Step() == SQLITE_ROW) list.push_back(ReadConversation(s));
    return list;
}

bool Database::GetConversation(int64_t id, Conversation& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "SELECT " CONVERSATION_COLUMNS " FROM conversations c WHERE c.id = ?");
    s.Bind(1, id);
    if (s.Step() != SQLITE_ROW) return false;
    out = ReadConversation(s);
    return true;
}

bool Database::CreateConversation(Conversation& c) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    if (!c.createdAt) c.createdAt = NowSeconds();
    c.updatedAt = c.createdAt;
    Stmt s(m_db, "INSERT INTO conversations (title, type, folder, created, updated, pinned, settings) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
    s.Bind(1, c.title)
        .Bind(2, std::string(c.type == ConversationType::Code ? "code" : "chat"))
        .Bind(3, c.folder)
        .Bind(4, c.createdAt)
        .Bind(5, c.updatedAt)
        .Bind(6, static_cast<int64_t>(c.pinned))
        .Bind(7, JsonDump(ConversationSettingsToJson(c.settings)));
    if (!s.Run()) {
        m_error = sqlite3_errmsg(m_db);
        return false;
    }
    c.id = sqlite3_last_insert_rowid(m_db);
    return true;
}

bool Database::UpdateConversation(const Conversation& c) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "UPDATE conversations SET title = ?, folder = ?, pinned = ?, settings = ?, type = ? WHERE id = ?");
    s.Bind(1, c.title)
        .Bind(2, c.folder)
        .Bind(3, static_cast<int64_t>(c.pinned))
        .Bind(4, JsonDump(ConversationSettingsToJson(c.settings)))
        .Bind(5, std::string(c.type == ConversationType::Code ? "code" : "chat"))
        .Bind(6, c.id);
    return s.Run();
}

bool Database::TouchConversation(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "UPDATE conversations SET updated = ? WHERE id = ?");
    s.Bind(1, NowSeconds()).Bind(2, id);
    return s.Run();
}

bool Database::DeleteConversation(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt m(m_db, "DELETE FROM messages WHERE conversation_id = ?");
    m.Bind(1, id);
    m.Run();
    Stmt s(m_db, "DELETE FROM conversations WHERE id = ?");
    s.Bind(1, id);
    return s.Run();
}

std::vector<Message> Database::LoadMessages(int64_t conversationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Message> list;
    if (!m_db) return list;
    Stmt s(m_db, "SELECT id, role, content, reasoning, tool_calls, tool_call_id, tool_name, tool_error, provider_id, "
                 "model, usage, created, duration_ms, stop_reason, provider_data FROM messages "
                 "WHERE conversation_id = ? ORDER BY id");
    s.Bind(1, conversationId);
    while (s.Step() == SQLITE_ROW) {
        Message m;
        m.id = s.Int(0);
        m.conversationId = conversationId;
        m.role = RoleFromKey(s.Text(1));
        m.content = s.Text(2);
        m.reasoning = s.Text(3);
        m.toolCalls = ToolCallsFromText(s.Text(4));
        m.toolCallId = s.Text(5);
        m.toolName = s.Text(6);
        m.toolError = s.Int(7) != 0;
        m.providerId = s.Text(8);
        m.model = s.Text(9);
        m.usage = UsageFromJson(ParseJson(s.Text(10)));
        m.createdAt = s.Int(11);
        m.durationMs = s.Int(12);
        m.stopReason = s.Text(13);
        m.providerData = ParseJson(s.Text(14));
        list.push_back(std::move(m));
    }
    return list;
}

bool Database::AddMessage(Message& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    if (!m.createdAt) m.createdAt = NowSeconds();
    Stmt s(m_db, "INSERT INTO messages (conversation_id, role, content, reasoning, tool_calls, tool_call_id, "
                 "tool_name, tool_error, provider_id, model, usage, created, duration_ms, stop_reason, provider_data) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.Bind(1, m.conversationId)
        .Bind(2, std::string(RoleKey(m.role)))
        .Bind(3, m.content)
        .Bind(4, m.reasoning)
        .Bind(5, ToolCallsToText(m.toolCalls))
        .Bind(6, m.toolCallId)
        .Bind(7, m.toolName)
        .Bind(8, static_cast<int64_t>(m.toolError))
        .Bind(9, m.providerId)
        .Bind(10, m.model)
        .Bind(11, JsonDump(UsageToJson(m.usage)))
        .Bind(12, m.createdAt)
        .Bind(13, m.durationMs)
        .Bind(14, m.stopReason)
        .Bind(15, m.providerData.is_null() ? std::string() : JsonDump(m.providerData));
    if (!s.Run()) {
        m_error = sqlite3_errmsg(m_db);
        return false;
    }
    m.id = sqlite3_last_insert_rowid(m_db);
    TouchConversation(m.conversationId);
    return true;
}

bool Database::UpdateMessage(const Message& m) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "UPDATE messages SET content = ?, reasoning = ?, tool_calls = ?, usage = ?, duration_ms = ?, "
                 "stop_reason = ?, provider_data = ?, model = ?, provider_id = ?, tool_error = ? WHERE id = ?");
    s.Bind(1, m.content)
        .Bind(2, m.reasoning)
        .Bind(3, ToolCallsToText(m.toolCalls))
        .Bind(4, JsonDump(UsageToJson(m.usage)))
        .Bind(5, m.durationMs)
        .Bind(6, m.stopReason)
        .Bind(7, m.providerData.is_null() ? std::string() : JsonDump(m.providerData))
        .Bind(8, m.model)
        .Bind(9, m.providerId)
        .Bind(10, static_cast<int64_t>(m.toolError))
        .Bind(11, m.id);
    return s.Run();
}

bool Database::DeleteMessage(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "DELETE FROM messages WHERE id = ?");
    s.Bind(1, id);
    return s.Run();
}

bool Database::DeleteMessagesFrom(int64_t conversationId, int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "DELETE FROM messages WHERE conversation_id = ? AND id >= ?");
    s.Bind(1, conversationId).Bind(2, id);
    return s.Run();
}

bool Database::DeleteAllMessages(int64_t conversationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    Stmt s(m_db, "DELETE FROM messages WHERE conversation_id = ?");
    s.Bind(1, conversationId);
    return s.Run();
}
