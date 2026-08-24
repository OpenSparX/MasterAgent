#!/usr/bin/env python3
"""Local-only bridge for the MasterAgent real-time trace console."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


VIEWER_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = VIEWER_DIR.parent.parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MasterAgent 本地实时链路面板")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--binary", type=Path, default=PROJECT_ROOT / "build" / "master_agent")
    parser.add_argument("--config", type=Path, default=PROJECT_ROOT / "agent-config")
    parser.add_argument("--model-profile", default="")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--endpoint", default="")
    parser.add_argument("--fake-cloud", action="store_true")
    parser.add_argument("--deadline-ms", type=int, default=300000)
    parser.add_argument("--runtime-root", type=Path,
                        default=Path(tempfile.gettempdir()) / "masteragent-trace-console")
    return parser.parse_args()


class Configuration:
    def __init__(self, arguments: argparse.Namespace) -> None:
        self.host = arguments.host
        self.port = arguments.port
        self.binary = arguments.binary.expanduser().resolve()
        self.config = arguments.config.expanduser().resolve()
        self.model_profile = arguments.model_profile
        self.model = arguments.model.expanduser().resolve() if arguments.model else None
        self.endpoint = arguments.endpoint
        self.fake_cloud = arguments.fake_cloud
        self.deadline_ms = max(1, min(arguments.deadline_ms, 300000))
        self.runtime_root = arguments.runtime_root.expanduser().resolve()
        self.runtime_root.mkdir(parents=True, exist_ok=True)
        self.run_lock = threading.Lock()


CONFIG: Configuration


def projected_event(record: dict) -> dict:
    payload = record.get("payload_summary", {})
    if isinstance(payload, str):
        try:
            payload = json.loads(payload)
        except json.JSONDecodeError:
            payload = {"summary": "无法解析的安全摘要"}
    return {
        "event_id": record.get("event_id", ""),
        "event_type": record.get("event_type", ""),
        "module": record.get("module", ""),
        "operation": record.get("operation", ""),
        "stage": payload.get("stage", "") if isinstance(payload, dict) else "",
        "status": payload.get("status", record.get("outcome", "")) if isinstance(payload, dict) else record.get("outcome", ""),
        "outcome": record.get("outcome", ""),
        "request_id": record.get("request_id", ""),
        "trace_id": record.get("trace_id", ""),
        "span_id": record.get("span_id", ""),
        "plan_id": record.get("plan_id", ""),
        "execution_id": record.get("execution_id", ""),
        "occurred_at_utc_ms": record.get("occurred_at_utc_ms", 0),
        "occurred_at_mono_ns": record.get("occurred_at_mono_ns", 0),
        "error_ref": record.get("error_ref", ""),
        "payload": payload,
    }


def projected_debug_event(record: dict) -> dict:
    payload = record.get("payload", {})
    if record.get("event_type") == "MODEL_INPUT" and isinstance(payload, dict):
        prompt = payload.get("prompt", "")
        if isinstance(prompt, str):
            sections = {}
            markers = ["SYSTEM", "SKILL_CANDIDATES", "TOOLS", "MEMORY", "USER", "PROTOCOL"]
            for index, marker in enumerate(markers):
                prefix = marker + ":\n"
                start = prompt.find(prefix)
                if start < 0:
                    continue
                start += len(prefix)
                end = len(prompt)
                for following in markers[index + 1:]:
                    found = prompt.find("\n" + following + ":\n", start)
                    if found >= 0:
                        end = min(end, found)
                sections[marker.lower()] = prompt[start:end]
            payload = dict(payload)
            payload["prompt_sections"] = sections
    encoded_record = json.dumps(record, ensure_ascii=False, sort_keys=True).encode("utf-8")
    return {
        "event_id": "debug:" + record.get("event_type", "") + ":" +
                    record.get("job_id", "") + ":" + hashlib.sha256(encoded_record).hexdigest()[:20],
        "event_type": record.get("event_type", "DEBUG_DETAIL"),
        "module": "本地调试通道",
        "operation": payload.get("operation", "展示本次请求的本地调试内容") if isinstance(payload, dict) else "",
        "stage": record.get("stage", ""),
        "status": record.get("status", "DETAIL"),
        "outcome": "LOCAL_DEBUG_ONLY",
        "request_id": record.get("request_id", ""),
        "trace_id": record.get("trace_id", ""),
        "span_id": "",
        "plan_id": "",
        "execution_id": "",
        "job_id": record.get("job_id", ""),
        "phase": record.get("phase", ""),
        "occurred_at_utc_ms": 0,
        "occurred_at_mono_ns": 0,
        "error_ref": "",
        "local_debug": True,
        "payload": payload,
    }


def read_new_events(journal: Path, offset: int) -> tuple[int, list[dict]]:
    if not journal.exists():
        return offset, []
    with journal.open("rb") as source:
        source.seek(offset)
        data = source.read()
    if not data:
        return offset, []
    last_newline = data.rfind(b"\n")
    if last_newline < 0:
        return offset, []
    complete = data[: last_newline + 1]
    next_offset = offset + len(complete)
    events: list[dict] = []
    for raw_line in complete.splitlines():
        if not raw_line.strip():
            continue
        try:
            frame = json.loads(raw_line)
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        records = frame.get("records", []) if frame.get("_journal_kind") == "event_batch" else [frame]
        for record in records:
            if isinstance(record, dict):
                events.append(projected_event(record))
    events.sort(key=lambda item: (item.get("occurred_at_mono_ns", 0), item.get("event_id", "")))
    return next_offset, events


def read_new_debug_events(path: Path, offset: int) -> tuple[int, list[dict]]:
    if not path.exists():
        return offset, []
    with path.open("rb") as source:
        source.seek(offset)
        data = source.read()
    if not data:
        return offset, []
    last_newline = data.rfind(b"\n")
    if last_newline < 0:
        return offset, []
    complete = data[: last_newline + 1]
    events = []
    for raw_line in complete.splitlines():
        try:
            record = json.loads(raw_line)
            if record.get("debug_schema") == "masteragent.local-debug/v1":
                events.append(projected_debug_event(record))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
    return offset + len(complete), events


class Handler(SimpleHTTPRequestHandler):
    server_version = "MasterAgentTraceConsole/1.0"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(VIEWER_DIR), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("面板：" + fmt % args)

    def send_json(self, status: int, payload: dict) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self) -> None:
        if urlparse(self.path).path == "/api/health":
            self.send_json(200, {
                "ready": CONFIG.binary.is_file() and CONFIG.config.is_dir(),
                "config_bundle": str(CONFIG.config),
                "model_profile": CONFIG.model_profile or "配置包默认模型",
                "runtime_root": str(CONFIG.runtime_root),
            })
            return
        super().do_GET()

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/api/run":
            self.send_error(404)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if size <= 0 or size > 65536:
                raise ValueError("请求正文大小不合法")
            request = json.loads(self.rfile.read(size))
            text = request.get("text", "")
            if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > 16000:
                raise ValueError("请输入 1 到 16000 字节的文本")
            if not CONFIG.binary.is_file():
                raise ValueError(f"找不到 MasterAgent：{CONFIG.binary}")
            if not CONFIG.config.is_dir():
                raise ValueError(f"找不到配置包：{CONFIG.config}")
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json(400, {"error": str(error)})
            return

        if not CONFIG.run_lock.acquire(blocking=False):
            self.send_json(409, {"error": "当前已有请求在运行，请等待它结束"})
            return
        try:
            self.run_agent(text.strip())
        finally:
            CONFIG.run_lock.release()

    def stream(self, payload: dict) -> bool:
        try:
            self.wfile.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
            self.wfile.flush()
            return True
        except (BrokenPipeError, ConnectionResetError):
            return False

    def run_agent(self, text: str) -> None:
        run_id = f"run-{int(time.time() * 1000)}-{os.getpid()}"
        runtime = CONFIG.runtime_root / run_id
        runtime.mkdir(parents=True, exist_ok=False)
        command = [str(CONFIG.binary), f"--runtime={runtime}", f"--config={CONFIG.config}",
                   f"--deadline-ms={CONFIG.deadline_ms}", "--local-debug-trace"]
        if CONFIG.model_profile:
            command.append(f"--model-profile={CONFIG.model_profile}")
        if CONFIG.model:
            command.append(f"--model={CONFIG.model}")
        if CONFIG.endpoint:
            command.append(f"--endpoint={CONFIG.endpoint}")
        if CONFIG.fake_cloud:
            command.append("--fake-cloud")
        command.append(text)

        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        process = subprocess.Popen(command, cwd=PROJECT_ROOT, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8")
        journal = runtime / "data_log" / "events.jsonl"
        offset = 0
        pipeline_debug = runtime / "local_debug" / "pipeline.jsonl"
        model_debug = runtime / "local_debug" / "model_io.jsonl"
        pipeline_debug_offset = 0
        model_debug_offset = 0
        connected = True
        while process.poll() is None and connected:
            offset, new_events = read_new_events(journal, offset)
            pipeline_debug_offset, pipeline_events = read_new_debug_events(
                pipeline_debug, pipeline_debug_offset)
            model_debug_offset, model_events = read_new_debug_events(
                model_debug, model_debug_offset)
            new_events.extend(pipeline_events)
            new_events.extend(model_events)
            for event in new_events:
                connected = self.stream({"type": "event", "event": event})
                if not connected:
                    break
            time.sleep(0.03)

        stdout, stderr = process.communicate()
        offset, new_events = read_new_events(journal, offset)
        pipeline_debug_offset, pipeline_events = read_new_debug_events(
            pipeline_debug, pipeline_debug_offset)
        model_debug_offset, model_events = read_new_debug_events(
            model_debug, model_debug_offset)
        new_events.extend(pipeline_events)
        new_events.extend(model_events)
        for event in new_events:
            if connected:
                connected = self.stream({"type": "event", "event": event})
        if not connected:
            return
        try:
            raw_result = json.loads(stdout)
            # The application JSON also contains developer-only raw model
            # fixtures. Never forward those to the browser; the console uses
            # DataLog's redacted event projection as its detailed source.
            result = {
                "request_id": raw_result.get("request_id", ""),
                "trace_id": raw_result.get("trace_id", ""),
                "success": bool(raw_result.get("success", False)),
                "pending": bool(raw_result.get("pending", False)),
                "reply": raw_result.get("reply", ""),
                "plan_id": raw_result.get("plan_id", ""),
                "plan_state": raw_result.get("plan_state", ""),
                "error_code": raw_result.get("error_code", ""),
                "error_message": raw_result.get("error_message", ""),
                "turn_summary": raw_result.get("turn_summary", ""),
                "runtime_directory": str(runtime),
            }
            self.stream({"type": "result", "result": result})
        except json.JSONDecodeError:
            safe_error = stderr.strip().splitlines()[-1] if stderr.strip() else "MasterAgent 未返回有效结果"
            self.stream({"type": "error", "message": safe_error[:500]})


def main() -> None:
    global CONFIG
    CONFIG = Configuration(parse_args())
    server = ThreadingHTTPServer((CONFIG.host, CONFIG.port), Handler)
    print(f"MasterAgent 实时链路面板：http://{CONFIG.host}:{CONFIG.port}")
    print(f"MasterAgent：{CONFIG.binary}")
    print(f"配置包：{CONFIG.config}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n面板已停止")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
