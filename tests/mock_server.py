#!/usr/bin/env python3
"""A stand-in for the AI services, for FastChat's tests.

Speaks just enough of each API: OpenAI chat completions (/openai), Anthropic
messages (/anthropic), Gemini (/gemini) and Ollama (/ollama), streaming. When the
request offers tools and has no tool result yet, the reply calls list_directory;
otherwise it streams a short text answer.

Usage: python mock_server.py PORT
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TEXT = ["Hello", " world.", " This is", " the second", " sentence."]


def has_tool_result(body, style):
    if style == "openai":
        return any(m.get("role") == "tool" for m in body.get("messages", []))
    if style == "ollama":
        return any(m.get("role") == "tool" for m in body.get("messages", []))
    if style == "anthropic":
        for m in body.get("messages", []):
            if isinstance(m.get("content"), list):
                if any(b.get("type") == "tool_result" for b in m["content"]):
                    return True
        return False
    if style == "gemini":
        for c in body.get("contents", []):
            if any("functionResponse" in p for p in c.get("parts", [])):
                return True
        return False
    return False


def image_placed_right(body, style):
    """Whether a screenshot from a tool result arrived where that API wants it."""
    if style == "openai" or style == "ollama":
        msgs = body.get("messages", [])
        tool_idx = [i for i, m in enumerate(msgs) if m.get("role") == "tool"]
        for i, m in enumerate(msgs):
            if m.get("role") != "user":
                continue
            has = bool(m.get("images")) if style == "ollama" else (
                isinstance(m.get("content"), list) and any(p.get("type") == "image_url" for p in m["content"]))
            if has and tool_idx and i > max(tool_idx):
                return True
        return False
    if style == "anthropic":
        for m in body.get("messages", []):
            for b in m.get("content", []) if isinstance(m.get("content"), list) else []:
                if b.get("type") == "tool_result" and isinstance(b.get("content"), list):
                    if any(c.get("type") == "image" for c in b["content"]):
                        return True
        return False
    if style == "gemini":
        for c in body.get("contents", []):
            parts = c.get("parts", [])
            if any("functionResponse" in p for p in parts) and any("inlineData" in p for p in parts):
                return True
        return False
    return False


def attachments_placed_right(body, style):
    """Whether an image and a PDF the user attached arrived as that API takes them
    (Ollama takes images only: the PDF is named in the text)."""
    if style == "openai":
        for m in body.get("messages", []):
            parts = m.get("content") if m.get("role") == "user" and isinstance(m.get("content"), list) else []
            image = any(p.get("type") == "image_url" and p["image_url"]["url"].startswith("data:image/png;base64,") for p in parts)
            pdf = any(p.get("type") == "file" and p["file"].get("filename") == "report.pdf" and
                      p["file"]["file_data"].startswith("data:application/pdf;base64,") for p in parts)
            if image and pdf:
                return True
        return False
    if style == "anthropic":
        for m in body.get("messages", []):
            blocks = m.get("content") if m.get("role") == "user" and isinstance(m.get("content"), list) else []
            types = {(b.get("type"), b.get("source", {}).get("media_type")) for b in blocks}
            if ("image", "image/png") in types and ("document", "application/pdf") in types:
                return True
        return False
    if style == "gemini":
        for c in body.get("contents", []):
            mimes = {p["inlineData"]["mimeType"] for p in c.get("parts", []) if "inlineData" in p}
            if c.get("role") == "user" and {"image/png", "application/pdf"} <= mimes:
                return True
        return False
    if style == "ollama":
        return any(m.get("role") == "user" and m.get("images") and "report.pdf" in m.get("content", "")
                   for m in body.get("messages", []))
    return False


SEEN = ["I", " see", " the", " image."]
ATTACHED = ["I", " see", " the", " attachments."]
FLAKY = {"count": 0}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_sse(self, events, content_type="text/event-stream"):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("x-ratelimit-remaining-requests", "99")
        self.end_headers()
        for e in events:
            data = e.encode("utf-8")
            self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def send_json(self, obj, status=200):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = self.path
        if p.startswith("/openai/models"):
            self.send_json({"data": [{"id": "mock-model", "created": 1, "context_length": 128000,
                                      "pricing": {"prompt": "0.000001", "completion": "0.000002"}}]})
        elif p.startswith("/openai/key"):
            self.send_json({"data": {"label": "test key", "limit": 10, "limit_remaining": 7.5, "usage": 2.5,
                                     "usage_daily": 0.5, "is_free_tier": False}})
        elif p.startswith("/openai/credits"):
            self.send_json({"data": {"total_credits": 20, "total_usage": 4.25}})
        elif p.startswith("/anthropic/models"):
            self.send_json({"data": [{"id": "claude-opus-5-5", "display_name": "Claude Opus 5.5",
                                      "max_input_tokens": 1000000, "max_tokens": 128000,
                                      "capabilities": {"thinking": {"supported": True, "types": {
                                          "adaptive": {"supported": True}}}}}], "has_more": False})
        elif p.startswith("/gemini/models"):
            self.send_json({"models": [{"name": "models/gemini-mock", "displayName": "Gemini Mock",
                                        "inputTokenLimit": 1048576, "outputTokenLimit": 65536,
                                        "supportedGenerationMethods": ["generateContent"]}]})
        elif p.startswith("/ollama/api/tags"):
            self.send_json({"models": [{"name": "mock:latest", "model": "mock:latest", "size": 1000}]})
        else:
            self.send_json({"error": {"message": "not found: " + p}}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        p = self.path
        if p.startswith("/flaky/chat/completions"):
            FLAKY["count"] += 1
            if FLAKY["count"] % 3 != 0:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.openai(body)
        elif p.startswith("/openai/chat/completions"):
            self.openai(body)
        elif p.startswith("/anthropic/messages"):
            self.anthropic(body)
        elif "/gemini/models/" in p:
            self.gemini(body)
        elif p.startswith("/ollama/api/chat"):
            self.ollama(body)
        elif p.startswith("/ollama/api/show"):
            self.send_json({"model_info": {"llama.context_length": 32768}, "capabilities": ["completion", "tools"]})
        elif p.startswith("/error"):
            self.send_json({"error": {"message": "mock failure"}}, 429)
        else:
            self.send_json({"error": {"message": "not found: " + p}}, 404)

    def openai(self, body):
        if body.get("model") == "fail":
            self.send_json({"error": {"message": "Model fail does not exist"}}, 400)
            return
        events = []
        def chunk(delta, finish=None, usage=None):
            c = {"id": "gen-1", "model": body.get("model"), "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
            if usage:
                c["choices"] = []
                c["usage"] = usage
            events.append("data: " + json.dumps(c) + "\n\n")
        chunk({"reasoning": "Let me think."})
        tool_names = [t["function"]["name"] for t in body.get("tools", [])]
        if "look" in tool_names and not has_tool_result(body, "openai"):
            # Computer use: look at the screen.
            chunk({"tool_calls": [{"index": 0, "id": "call_look", "type": "function",
                                   "function": {"name": "look", "arguments": "{}"}}]})
            chunk({}, "tool_calls")
        elif body.get("tools") and not has_tool_result(body, "openai"):
            chunk({"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                   "function": {"name": "list_directory", "arguments": ""}}]})
            chunk({"tool_calls": [{"index": 0, "function": {"arguments": "{\"path\":"}}]})
            chunk({"tool_calls": [{"index": 0, "function": {"arguments": " \".\"}"}}]})
            chunk({}, "tool_calls")
        else:
            for t in (ATTACHED if attachments_placed_right(body, "openai") else SEEN if image_placed_right(body, "openai") else TEXT):
                chunk({"content": t})
            chunk({}, "stop")
        chunk({}, usage={"prompt_tokens": 100, "completion_tokens": 20,
                         "prompt_tokens_details": {"cached_tokens": 40}, "cost": 0.0012})
        events.append("data: [DONE]\n\n")
        self.send_sse(events)

    def anthropic(self, body):
        assert body.get("thinking", {}).get("type") == "adaptive", body.get("thinking")
        assert "temperature" not in body
        ev = []
        def e(name, data):
            ev.append("event: %s\ndata: %s\n\n" % (name, json.dumps(data)))
        e("message_start", {"type": "message_start", "message": {"id": "msg_1", "model": body["model"],
            "usage": {"input_tokens": 10, "cache_read_input_tokens": 90, "cache_creation_input_tokens": 0}}})
        e("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}})
        e("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": "Hmm."}})
        e("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "sig123"}})
        e("content_block_stop", {"type": "content_block_stop", "index": 0})
        if body.get("tools") and not has_tool_result(body, "anthropic"):
            e("content_block_start", {"type": "content_block_start", "index": 1,
                                      "content_block": {"type": "tool_use", "id": "toolu_1", "name": "list_directory", "input": {}}})
            e("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": "{\"path\": "}})
            e("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": "\".\"}"}})
            e("content_block_stop", {"type": "content_block_stop", "index": 1})
            stop = "tool_use"
        else:
            # The thinking block must come back unchanged in the tool loop.
            if has_tool_result(body, "anthropic") and not image_placed_right(body, "anthropic"):
                assistant = [m for m in body["messages"] if m["role"] == "assistant"][-1]
                assert assistant["content"][0]["signature"] == "sig123", assistant
            e("content_block_start", {"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}})
            for t in (ATTACHED if attachments_placed_right(body, "anthropic") else SEEN if image_placed_right(body, "anthropic") else TEXT):
                e("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": t}})
            e("content_block_stop", {"type": "content_block_stop", "index": 1})
            stop = "end_turn"
        e("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 25}})
        e("message_stop", {"type": "message_stop"})
        self.send_sse(ev)

    def gemini(self, body):
        ev = []
        def chunk(parts, finish=None, usage=None):
            c = {"candidates": [{"content": {"role": "model", "parts": parts}}], "modelVersion": "gemini-mock"}
            if finish:
                c["candidates"][0]["finishReason"] = finish
            if usage:
                c["usageMetadata"] = usage
            ev.append("data: " + json.dumps(c) + "\r\n\r\n")
        chunk([{"text": "Considering.", "thought": True}])
        if body.get("tools") and not has_tool_result(body, "gemini"):
            chunk([{"functionCall": {"name": "list_directory", "args": {"path": "."}}, "thoughtSignature": "gsig"}], "STOP")
        else:
            if has_tool_result(body, "gemini") and not image_placed_right(body, "gemini"):
                model_turn = [c for c in body["contents"] if c["role"] == "model"][-1]
                assert any(p.get("thoughtSignature") == "gsig" for p in model_turn["parts"]), model_turn
            for t in (ATTACHED if attachments_placed_right(body, "gemini") else SEEN if image_placed_right(body, "gemini") else TEXT):
                chunk([{"text": t}])
            chunk([{"text": ""}], "STOP", {"promptTokenCount": 50, "candidatesTokenCount": 10, "thoughtsTokenCount": 5})
        self.send_sse(ev)

    def ollama(self, body):
        lines = []
        if body.get("tools") and not has_tool_result(body, "ollama"):
            lines.append({"model": body["model"], "message": {"role": "assistant", "content": "",
                          "tool_calls": [{"function": {"name": "list_directory", "arguments": {"path": "."}}}]}, "done": False})
        else:
            for t in (ATTACHED if attachments_placed_right(body, "ollama") else SEEN if image_placed_right(body, "ollama") else TEXT):
                lines.append({"model": body["model"], "message": {"role": "assistant", "content": t}, "done": False})
        lines.append({"model": body["model"], "message": {"role": "assistant", "content": ""}, "done": True,
                      "done_reason": "stop", "prompt_eval_count": 30, "eval_count": 12})
        self.send_sse([json.dumps(l) + "\n" for l in lines], "application/x-ndjson")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
