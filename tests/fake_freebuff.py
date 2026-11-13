#!/usr/bin/env python3
"""A stand-in for Freebuff's terminal interface, for FastChat's tests.

Freebuff has no headless mode, so FastChat drives it the way BlindPilot does: a
pseudo-terminal, the prompt pasted into its composer, and the answer taken from
the chat Freebuff saves. This fake paints a composer, reads the pasted prompt,
and writes those same files under $FREEBUFF_CONFIG_DIR (or ~/.config/manicode):

  projects/<project>/chats/<id>/chat-messages.json
  projects/<project>/chats/<id>/log.jsonl

A prompt containing "run" makes it report a command and a file change; a prompt
that names attachments reports them, as the real one reads files itself.
"""
import json
import os
import re
import sys

START = b"\x1b[200~"  # bracketed paste, which is how the prompt arrives
END = b"\x1b[201~"
COMPOSER = "Enter a coding task or / for commands"


def read_until(marker):
    data = b""
    while marker not in data:
        ch = sys.stdin.buffer.read(1)
        if not ch:
            return None
        data += ch
    return data


def main():
    # The interface is UTF-8, and a Windows console's own code page is not.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = sys.argv[1:]
    if "--cwd" not in args:
        return 2
    cwd = args[args.index("--cwd") + 1]
    resume = args[args.index("--continue") + 1] if "--continue" in args else None
    root = os.environ.get("FREEBUFF_CONFIG_DIR") or os.path.expanduser("~/.config/manicode")
    project = os.path.basename(os.path.abspath(cwd).rstrip("/\\"))
    chats = os.path.join(root, "projects", project, "chats")
    os.makedirs(chats, exist_ok=True)

    if resume:
        chat_id = resume
    else:
        number = 1
        chat_id = "fb-1"
        while os.path.exists(os.path.join(chats, chat_id)):
            number += 1
            chat_id = "fb-%d" % number
    chat = os.path.join(chats, chat_id)
    os.makedirs(chat, exist_ok=True)
    log_path = os.path.join(chat, "log.jsonl")
    messages_path = os.path.join(chat, "chat-messages.json")

    def log(message):
        with open(log_path, "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"msg": message, "pid": os.getpid()}) + "\n")

    # A boot writes the drop first and the fingerprint once sign-in is settled;
    # FastChat waits for that before it sends anything.
    log("[chat-runtime] Freebuff session over; holding queued messages until rejoin")
    sys.stdout.write("\x1b[2J\x1b[HFreeBuff 0.2.26\r\n")
    sys.stdout.flush()
    log("Enhanced CLI fingerprint generated successfully")
    sys.stdout.write("\r\n" + COMPOSER + "\r\n\xe2\x80\xba")
    sys.stdout.flush()

    read_until(START)
    pasted = read_until(END)
    if pasted is None:
        return 0
    prompt = pasted.decode("utf-8", "replace")
    sys.stdin.buffer.read(1)  # the Enter that submits it

    first = next((line.strip() for line in prompt.splitlines() if line.strip()), "")
    sys.stdout.write("\r\n  " + first + "\r\n  thinking...\r\n")
    sys.stdout.flush()

    blocks = []
    if "run" in prompt:
        blocks.append(
            {
                "type": "tool",
                "toolCallId": "t1",
                "toolName": "bash",
                "status": "completed",
                "input": {"command": "dir /b"},
                "output": "main.cpp\nExit code: 0",
            }
        )
        blocks.append(
            {
                "type": "tool",
                "toolCallId": "t2",
                "toolName": "write_file",
                "status": "completed",
                "input": {"path": "src/main.cpp"},
                "output": "updated src/main.cpp",
            }
        )
    text = "All done." + (" Resumed " + resume + "." if resume else "")
    saved = re.findall(r"\(saved at (.+?)\)\]", prompt)
    if saved:
        text += " Images: " + ", ".join(
            "%s %d bytes" % (os.path.basename(path), os.path.getsize(path)) for path in saved
        ) + "."
    if "cannot read PDFs" in prompt:
        text += " A PDF was named."
    blocks.append({"type": "text", "textType": "text", "content": text})

    messages = []
    if os.path.exists(messages_path):
        try:
            with open(messages_path, encoding="utf-8") as handle:
                messages = json.load(handle)
        except ValueError:
            messages = []
    turn = len(messages)
    messages.append({"id": "divider-%d" % turn, "variant": "ai", "blocks": [{"type": "mode-divider"}]})
    messages.append({"id": "user-%d" % turn, "variant": "user", "content": prompt})
    messages.append({"id": "ai-%d" % turn, "variant": "ai", "blocks": blocks})
    with open(messages_path, "w", encoding="utf-8") as handle:
        json.dump(messages, handle)

    # The answer is saved first; "Main prompt finished" is what says the turn is
    # over, and it is written after so a reader that sees it always sees the chat.
    sys.stdout.write("\x1b[2J\x1b[HFreeBuff 0.2.26\r\n\r\n  " + first + "\r\n\r\n" + text + "\r\n\r\n"
                     + COMPOSER + "\r\n\xe2\x80\xba")
    sys.stdout.flush()
    log("Main prompt finished")
    return 0


if __name__ == "__main__":
    sys.exit(main())
