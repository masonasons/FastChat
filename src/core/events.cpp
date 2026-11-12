#include "core/events.h"

namespace {

const EventInfo kEvents[] = {
    {"message_sent", "Message sent", true, false, "Sent", 660, 0, 45},
    {"response_started", "Response started", true, false, "{model} is responding", 520, 780, 40},
    {"reasoning_started", "Thinking started", false, false, "Thinking", 400, 500, 40},
    {"response_complete", "Response complete", true, false, "Response complete", 880, 1320, 60},
    {"response_error", "Error", true, true, "Error: {error}", 220, 160, 120},
    {"generation_stopped", "Generation stopped", true, true, "Stopped", 500, 330, 60},
    {"tool_call", "Tool call", true, true, "{tool}", 700, 0, 30},
    {"tool_result", "Tool result", false, false, "{summary}", 600, 0, 25},
    {"approval_needed", "Command needs approval", true, true, "Approve command: {summary}", 990, 990, 80},
    {"conversation_opened", "Conversation opened", false, false, "{conversation}", 440, 660, 35},
    {"conversation_created", "Conversation created", true, false, "Created {conversation}", 440, 880, 50},
    {"title_generated", "Title generated", false, true, "Titled {conversation}", 760, 0, 30},
    {"message_deleted", "Message deleted", true, false, "Deleted", 300, 0, 40},
    {"compacted", "Conversation compacted", true, true, "Compacted", 620, 470, 50},
    {"task_started", "Agent or background task started", true, false, "Started {tool}", 560, 700, 35},
    {"task_finished", "Agent or background task finished", true, false, "{tool} {summary}", 700, 560, 35},
};

static_assert(sizeof(kEvents) / sizeof(kEvents[0]) == static_cast<size_t>(EventId::Count),
              "every event needs an entry");

}  // namespace

const EventInfo& GetEventInfo(EventId id) {
    return kEvents[static_cast<int>(id)];
}

EventId EventFromKey(const std::string& key) {
    for (int i = 0; i < static_cast<int>(EventId::Count); i++) {
        if (key == kEvents[i].key) return static_cast<EventId>(i);
    }
    return EventId::Count;
}

const char* EventPlaceholderHelp() {
    return "{conversation} {model} {provider} {error} {tool} {summary} {tokens} {cost} {duration}";
}
