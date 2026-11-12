#!/usr/bin/env python3
"""A stand-in for `claude -p` in stream-json mode, for FastChat's tests.

Follows the events Claude Code 2.1 sends: system/init, stream_event deltas,
assistant messages (one per content block), user tool results, control_request
can_use_tool (answered on stdin), rate_limit_event, compact_boundary and result.

The prompt decides what happens: "write" asks to write a file (needs approval),
"run" runs a command (no approval, like a safe command), "/compact" compacts,
anything else is a plain answer. --resume <id> is echoed in the answer.
Messages that arrive on stdin after the first result are answered as further
turns ("Got: ..."), as Claude Code does with messages sent while it works; one
with "write" in it asks to write a file first, and that approval fails with
"Stream closed" if stdin has closed, as Claude Code's does. With
--replay-user-messages each message is echoed back when it is taken in.
"""
import json
import queue
import sys
import threading

args = sys.argv[1:]
resume = args[args.index("--resume") + 1] if "--resume" in args else None
no_tools = "--tools" in args and args[args.index("--tools") + 1] == ""
replay = "--replay-user-messages" in args
session = resume or "sess-1"

lines = queue.Queue()


def reader():
    for line in sys.stdin:
        lines.put(line)
    lines.put(None)


threading.Thread(target=reader, daemon=True).start()


def out(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def assistant(msg_id, block):
    out({"type": "assistant", "message": {"id": msg_id, "model": "claude-haiku-5-5", "role": "assistant",
         "content": [block], "usage": {"input_tokens": 3, "cache_read_input_tokens": 100,
                                       "cache_creation_input_tokens": 50, "output_tokens": 7}},
         "parent_tool_use_id": None, "session_id": session})


def stream_text(text):
    out({"type": "stream_event", "event": {"type": "message_start", "message": {"role": "assistant"}},
         "parent_tool_use_id": None, "session_id": session})
    out({"type": "stream_event", "event": {"type": "content_block_start", "index": 0,
         "content_block": {"type": "text", "text": ""}}, "parent_tool_use_id": None, "session_id": session})
    for piece in text.split(" "):
        out({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0,
             "delta": {"type": "text_delta", "text": piece + " "}}, "parent_tool_use_id": None, "session_id": session})


def taken_in(msg):
    if replay:
        out({"type": "user", "message": msg["message"], "uuid": msg.get("uuid"), "isReplay": True,
             "parent_tool_use_id": None, "session_id": session})


def result(cost):
    out({"type": "result", "subtype": "success", "is_error": False, "session_id": session, "total_cost_usd": cost,
         "stop_reason": "end_turn", "result": "ok",
         "usage": {"input_tokens": 6, "cache_read_input_tokens": 200, "cache_creation_input_tokens": 50, "output_tokens": 20}})


def next_message(timeout=None):
    try:
        line = lines.get(timeout=timeout)
    except queue.Empty:
        return "timeout"
    return None if line is None else json.loads(line)


def text_of(content):
    """A user message's text: the string, or its text blocks (with images and documents first)."""
    if isinstance(content, str):
        return content
    return "".join(b.get("text", "") for b in content if b.get("type") == "text")


def files_of(content):
    """The images and documents a user message carries: "image image/png", "document application/pdf"."""
    if isinstance(content, str):
        return []
    return [b["type"] + " " + b["source"]["media_type"] for b in content
            if b.get("type") in ("image", "document") and b.get("source", {}).get("type") == "base64" and b["source"].get("data")]


first = next_message()
prompt = text_of(first["message"]["content"])
attached = files_of(first["message"]["content"])
out({"type": "system", "subtype": "init", "session_id": session, "model": "claude-haiku-5-5",
     "tools": [] if no_tools else ["Bash", "Write"], "cwd": "."})
taken_in(first)
out({"type": "rate_limit_event", "rate_limit_info": {"status": "allowed", "unifiedWindows": {
    "seven_day": {"utilization": 0.25, "resetsAt": 1794499200, "status": "allowed"}}}, "session_id": session})

if prompt.startswith("/compact"):  # like Claude Code: what follows is instructions for the summary
    # As Claude Code 2.1 does: its status, the boundary, then the summary it carries on from.
    out({"type": "system", "subtype": "status", "status": "compacting", "session_id": session})
    out({"type": "system", "subtype": "status", "status": None, "compact_result": "success", "session_id": session})
    out({"type": "system", "subtype": "compact_boundary", "session_id": session,
         "compact_metadata": {"trigger": "manual", "pre_tokens": 31948, "post_tokens": 6933}})
    out({"type": "user", "message": {"role": "user", "content":
         "This session is being continued from a previous conversation that ran out of context. The summary below "
         "covers the earlier portion of the conversation.\n\n<system-reminder>\n<tools-used-before-summary>"
         "</tools-used-before-summary>\n</system-reminder>\nSummary:\n1. Primary Request and Intent:\n   The user said hello."},
         "parent_tool_use_id": None, "session_id": session})
    out({"type": "user", "message": {"role": "user", "content": "<local-command-stdout>Compacted </local-command-stdout>"},
         "parent_tool_use_id": None, "session_id": session})
    result(0.005)
elif "agent" in prompt and not no_tools:
    # A background shell and a subagent, as Claude Code 2.1 reports them.
    import os, tempfile
    out_file = os.path.join(tempfile.gettempdir(), "fastchat-fake-task.output")
    with open(out_file, "w") as f:
        f.write("Pinging 127.0.0.1\nReply from 127.0.0.1\n")
    assistant("msg_1", {"type": "tool_use", "id": "toolu_bg", "name": "Bash",
                        "input": {"command": "ping -n 6 127.0.0.1", "run_in_background": True}})
    out({"type": "system", "subtype": "task_started", "task_id": "bg1", "tool_use_id": "toolu_bg",
         "description": "Ping localhost", "is_backgrounded": True, "task_type": "local_bash"})
    out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_bg",
         "content": "Command running in background with ID: bg1. Output is being written to: " + out_file, "is_error": False}]},
         "parent_tool_use_id": None, "tool_use_result": {"stdout": "", "backgroundTaskId": "bg1"}})
    assistant("msg_2", {"type": "tool_use", "id": "toolu_agent", "name": "Agent",
                        "input": {"description": "count files", "prompt": "Count the files"}})
    out({"type": "system", "subtype": "task_started", "task_id": "ag1", "tool_use_id": "toolu_agent",
         "description": "count files", "subagent_type": "general-purpose", "is_backgrounded": False,
         "task_type": "local_agent", "prompt": "Count the files"})
    out({"type": "assistant", "message": {"id": "msg_sub1", "content": [{"type": "tool_use", "id": "toolu_sub",
         "name": "Bash", "input": {"command": "dir /b"}}]}, "parent_tool_use_id": "toolu_agent"})
    out({"type": "control_request", "request_id": "req-sub", "request": {"subtype": "can_use_tool", "tool_name": "Bash",
         "input": {"command": "dir /b"}, "tool_use_id": "toolu_sub"}})
    next_message()  # the approval
    out({"type": "system", "subtype": "task_progress", "task_id": "ag1", "tool_use_id": "toolu_agent",
         "description": "Running dir /b", "usage": {"total_tokens": 21254, "tool_uses": 1, "duration_ms": 821},
         "last_tool_name": "Bash"})
    out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_sub",
         "content": "one.txt\ntwo.txt", "is_error": False}]}, "parent_tool_use_id": "toolu_agent"})
    out({"type": "assistant", "message": {"id": "msg_sub2", "content": [{"type": "text", "text": "SUBAGENT: two files"}]},
         "parent_tool_use_id": "toolu_agent"})
    out({"type": "system", "subtype": "task_updated", "task_id": "ag1", "patch": {"status": "completed"}})
    out({"type": "system", "subtype": "task_notification", "task_id": "ag1", "tool_use_id": "toolu_agent",
         "status": "completed", "summary": "There are two files."})
    out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_agent",
         "content": [{"type": "text", "text": "There are two files."}], "is_error": False}]}, "parent_tool_use_id": None})
    out({"type": "system", "subtype": "task_notification", "task_id": "bg1", "tool_use_id": "toolu_bg",
         "status": "completed", "output_file": out_file, "summary": "Background command completed"})
    stream_text("Two files, and the ping finished.")
    assistant("msg_3", {"type": "text", "text": "Two files, and the ping finished."})
    result(0.02)
elif "write" in prompt and not no_tools:
    assistant("msg_1", {"type": "thinking", "thinking": "I will write it."})
    assistant("msg_1", {"type": "tool_use", "id": "toolu_1", "name": "Write",
                        "input": {"file_path": "notes.txt", "content": "hello"}})
    out({"type": "control_request", "request_id": "req-1", "request": {"subtype": "can_use_tool", "tool_name": "Write",
         "input": {"file_path": "notes.txt", "content": "hello"}, "tool_use_id": "toolu_1"}})
    answer = next_message()
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
    result(0.0125)
elif "run" in prompt and not no_tools:
    assistant("msg_1", {"type": "tool_use", "id": "toolu_2", "name": "Bash", "input": {"command": "echo hi"}})
    out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_2",
         "content": [{"type": "text", "text": "hi"}], "is_error": False}]}, "parent_tool_use_id": None})
    # A subagent's message (parent_tool_use_id set) is not part of the conversation.
    out({"type": "assistant", "message": {"id": "msg_sub", "content": [{"type": "text", "text": "SUBAGENT"}]},
         "parent_tool_use_id": "toolu_2"})
    stream_text("It printed hi.")
    assistant("msg_2", {"type": "text", "text": "It printed hi."})
    result(0.0125)
elif attached:
    text = "Saw " + " and ".join(attached) + " with: " + prompt
    stream_text(text)
    assistant("msg_1", {"type": "text", "text": text})
    result(0.0125)
else:
    text = "Hello from fake Claude." + (" Resumed " + resume + "." if resume else "")
    stream_text(text)
    assistant("msg_1", {"type": "text", "text": text})
    result(0.0125)

# Messages sent while it worked: each is another turn. Ends when stdin closes.
turn = 3
while True:
    msg = next_message(timeout=10)
    if msg is None or msg == "timeout":
        break
    if msg.get("type") != "user":
        continue
    taken_in(msg)
    text = "Got: " + text_of(msg["message"]["content"])
    if files_of(msg["message"]["content"]):
        text += " with " + " and ".join(files_of(msg["message"]["content"]))
    if "write" in text and not no_tools:
        out({"type": "control_request", "request_id": "req-%d" % turn, "request": {"subtype": "can_use_tool",
             "tool_name": "Write", "input": {"file_path": "later.txt", "content": "x"}, "tool_use_id": "toolu_w%d" % turn}})
        answer = next_message(timeout=10)
        if not isinstance(answer, dict):
            text += " (Stream closed)"
        else:
            text += " (" + answer["response"]["response"]["behavior"] + ")"
    stream_text(text)
    assistant("msg_%d" % turn, {"type": "text", "text": text})
    result(0.001)
    turn += 1
