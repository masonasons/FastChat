#!/usr/bin/env python3
"""A stand-in for `codex exec --json`, for FastChat's tests.

Emits the documented JSONL events. A prompt containing "run" makes it run a
command and change a file; the sandbox it was given is reported in its
reasoning so tests can check it.
"""
import json
import sys

args = sys.argv[1:]
sandbox = args[args.index("--sandbox") + 1] if "--sandbox" in args else "?"
resume = args[args.index("resume") + 1] if "resume" in args else None
prompt = sys.stdin.read()


def out(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


out({"type": "thread.started", "thread_id": resume or "thread-1"})
out({"type": "turn.started"})
out({"type": "item.completed", "item": {"id": "item_0", "type": "reasoning", "text": "sandbox=" + sandbox}})
if "run" in prompt:
    out({"type": "item.started", "item": {"id": "item_1", "type": "command_execution", "command": "dir /b",
         "status": "in_progress"}})
    out({"type": "item.completed", "item": {"id": "item_1", "type": "command_execution", "command": "dir /b",
         "aggregated_output": "main.cpp\n", "exit_code": 0, "status": "completed"}})
    out({"type": "item.completed", "item": {"id": "item_2", "type": "file_change", "status": "completed",
         "changes": [{"path": "src/main.cpp", "kind": "update"}]}})
text = "All done." + (" Resumed " + resume + "." if resume else "")
out({"type": "item.updated", "item": {"id": "item_3", "type": "agent_message", "text": "All"}})
out({"type": "item.completed", "item": {"id": "item_3", "type": "agent_message", "text": text}})
out({"type": "turn.completed", "usage": {"input_tokens": 120, "cached_input_tokens": 80, "output_tokens": 15,
     "reasoning_output_tokens": 5}})
