"""A stand-in for `claude -p` in stream-json mode, for FastChat's tests.

Follows the events Claude Code 2.1 sends: system/init, stream_event deltas,
assistant messages (one per content block), user tool results, control_request
can_use_tool (answered on stdin), rate_limit_event and result.

The prompt decides what happens: "write" asks to write a file (needs approval),
"run" runs a command (no approval, like a safe command), anything else is a
plain answer. --resume <id> is echoed in the answer.
"""
import json
import sys

args = sys.argv[1:]
resume = args[args.index("--resume") + 1] if "--resume" in args else None
no_tools = "--tools" in args and args[args.index("--tools") + 1] == ""
session = resume or "sess-1"


def out(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def assistant(msg_id, block):
    out({"type": "assistant", "message": {"id": msg_id, "model": "claude-haiku-5-5", "role": "assistant",
         "content": [block], "usage": {"input_tokens": 3, "cache_read_input_tokens": 100,
                                       "cache_creation_input_tokens": 50, "output_tokens": 7}},
         "parent_tool_use_id": None, "session_id": session})


def stream_text(text):
    out({"type": "stream_event", "event": {"type": "content_block_start", "index": 0,
         "content_block": {"type": "text", "text": ""}}, "parent_tool_use_id": None, "session_id": session})
    for piece in text.split(" "):
        out({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0,
             "delta": {"type": "text_delta", "text": piece + " "}}, "parent_tool_use_id": None, "session_id": session})


def result(cost):
    out({"type": "result", "subtype": "success", "is_error": False, "session_id": session, "total_cost_usd": cost,
         "stop_reason": "end_turn", "result": "ok",
         "usage": {"input_tokens": 6, "cache_read_input_tokens": 200, "cache_creation_input_tokens": 50, "output_tokens": 20}})


first = json.loads(sys.stdin.readline())
prompt = first["message"]["content"]
out({"type": "system", "subtype": "init", "session_id": session, "model": "claude-haiku-5-5",
     "tools": [] if no_tools else ["Bash", "Write"], "cwd": "."})
out({"type": "rate_limit_event", "rate_limit_info": {"status": "allowed", "unifiedWindows": {
    "seven_day": {"utilization": 0.25, "resetsAt": 1794499200, "status": "allowed"}}}, "session_id": session})

if "write" in prompt and not no_tools:
    assistant("msg_1", {"type": "thinking", "thinking": "I will write it."})
    assistant("msg_1", {"type": "tool_use", "id": "toolu_1", "name": "Write",
                        "input": {"file_path": "notes.txt", "content": "hello"}})
    out({"type": "control_request", "request_id": "req-1", "request": {"subtype": "can_use_tool", "tool_name": "Write",
         "input": {"file_path": "notes.txt", "content": "hello"}, "tool_use_id": "toolu_1"}})
    answer = json.loads(sys.stdin.readline())
    behavior = answer["response"]["response"]["behavior"]
    if behavior == "allow":
        out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_1",
             "content": "File created successfully", "is_error": False}]}, "parent_tool_use_id": None})
        text = "Done."
    else:
        out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_1",
             "content": "The user declined this.", "is_error": True}]}, "parent_tool_use_id": None})
        text = "You declined."
    stream_text(text)
    assistant("msg_2", {"type": "text", "text": text})
elif "run" in prompt and not no_tools:
    assistant("msg_1", {"type": "tool_use", "id": "toolu_2", "name": "Bash", "input": {"command": "echo hi"}})
    out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_2",
         "content": [{"type": "text", "text": "hi"}], "is_error": False}]}, "parent_tool_use_id": None})
    # A subagent's message (parent_tool_use_id set) is not part of the conversation.
    out({"type": "assistant", "message": {"id": "msg_sub", "content": [{"type": "text", "text": "SUBAGENT"}]},
         "parent_tool_use_id": "toolu_2"})
    stream_text("It printed hi.")
    assistant("msg_2", {"type": "text", "text": "It printed hi."})
else:
    text = "Hello from fake Claude." + (" Resumed " + resume + "." if resume else "")
    stream_text(text)
    assistant("msg_1", {"type": "text", "text": text})
result(0.0125)
# Like Claude Code, wait for standard input to close.
sys.stdin.read()
