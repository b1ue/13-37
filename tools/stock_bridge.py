#!/usr/bin/env python3
"""Authenticated local bridge between T-Watch 1337 and Python stock jobs.

The watch stays a thin client.  This process owns market-data credentials,
launches only explicitly allow-listed commands, and exposes a compact text
snapshot that fits comfortably on the ESP32-S3.
"""

from __future__ import annotations

import argparse
import hmac
import json
import os
import re
import subprocess
import threading
import time
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from fastapi import Depends, FastAPI, Header, HTTPException, Query
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel


@dataclass
class Job:
    id: str
    preset: str
    device_name: str
    state: str = "queued"
    progress: str = ""
    message: str = ""
    created_at: float = field(default_factory=time.time)
    started_at: float | None = None
    finished_at: float | None = None
    return_code: int | None = None
    log_tail: list[str] = field(default_factory=list)
    process: subprocess.Popen[str] | None = field(default=None, repr=False)


class JobRequest(BaseModel):
    preset: str
    device_name: str = "t-watch-1337"


class BridgeState:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.jobs: dict[str, Job] = {}
        self.acked_alerts: set[str] = set()
        self.quote_cache: dict[str, tuple[float, dict[str, Any]]] = {}


def load_config(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data.get("presets", {}), dict):
        raise ValueError("presets must be an object")
    data["_config_dir"] = str(path.resolve().parent)
    return data


def resolve_path(config: dict[str, Any], value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = Path(config["_config_dir"]) / path
    return path.resolve()


def clean_protocol(value: Any) -> str:
    return str(value if value is not None else "").replace("\r", " ").replace("\n", " ").replace("|", "/")


def read_snapshot(config: dict[str, Any]) -> dict[str, Any]:
    value = config.get("snapshot_file", "")
    if not value:
        return {}
    path = resolve_path(config, value)
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def alpha_vantage_quote(config: dict[str, Any], state: BridgeState, symbol: str) -> dict[str, Any] | None:
    env_name = config.get("alpha_vantage_api_key_env", "ALPHAVANTAGE_API_KEY")
    api_key = os.environ.get(env_name, "")
    if not api_key:
        return None
    ttl = int(config.get("quote_cache_seconds", 300))
    now = time.time()
    with state.lock:
        cached = state.quote_cache.get(symbol)
        if cached and now - cached[0] < ttl:
            return cached[1]

    query = urllib.parse.urlencode({
        "function": "TIME_SERIES_DAILY",
        "symbol": symbol,
        "outputsize": "compact",
        "apikey": api_key,
    })
    request = urllib.request.Request(
        f"https://www.alphavantage.co/query?{query}",
        headers={"User-Agent": "t-watch-1337-stock-bridge/1"},
    )
    try:
        with urllib.request.urlopen(request, timeout=12) as response:
            payload = json.load(response)
    except (OSError, ValueError):
        return None
    series = payload.get("Time Series (Daily)", {})
    if not isinstance(series, dict) or not series:
        return None
    dates = sorted(series, reverse=True)
    latest = float(series[dates[0]]["4. close"])
    previous = float(series[dates[1]]["4. close"]) if len(dates) > 1 else latest
    change = latest - previous
    percent = (change / previous * 100.0) if previous else 0.0
    spark_dates = list(reversed(dates[:24]))
    quote = {
        "price": f"{latest:.2f}",
        "change": f"{change:+.2f}",
        "pct": f"{percent:+.2f}%",
        "stamp": dates[0],
        "spark": [float(series[day]["4. close"]) for day in spark_dates],
    }
    with state.lock:
        state.quote_cache[symbol] = (now, quote)
    return quote


def latest_job(state: BridgeState) -> Job | None:
    with state.lock:
        return max(state.jobs.values(), key=lambda item: item.created_at, default=None)


def public_job(job: Job) -> dict[str, Any]:
    return {
        "id": job.id,
        "preset": job.preset,
        "device_name": job.device_name,
        "state": job.state,
        "progress": job.progress,
        "message": job.message,
        "created_at": job.created_at,
        "started_at": job.started_at,
        "finished_at": job.finished_at,
        "return_code": job.return_code,
        "log_tail": list(job.log_tail),
    }


def run_job(config: dict[str, Any], state: BridgeState, job: Job, preset: dict[str, Any]) -> None:
    command = preset.get("command")
    if not isinstance(command, list) or not command or not all(isinstance(part, str) and part for part in command):
        with state.lock:
            job.state = "failed"
            job.message = "Preset has no valid command"
            job.finished_at = time.time()
        return
    cwd = resolve_path(config, preset.get("cwd", "."))
    env = os.environ.copy()
    env.update({str(key): str(value) for key, value in preset.get("env", {}).items()})
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
    except OSError as exc:
        with state.lock:
            job.state = "failed"
            job.message = f"Launch failed: {exc}"
            job.finished_at = time.time()
        return

    progress_pattern = re.compile(r"(?:PROGRESS\s*[=:]\s*)?(\d+\s*/\s*\d+|\d{1,3}%)", re.I)
    with state.lock:
        job.process = process
        job.state = "running"
        job.started_at = time.time()
        job.message = "Python workflow running"
    assert process.stdout is not None
    for raw_line in process.stdout:
        line = raw_line.rstrip()
        if not line:
            continue
        match = progress_pattern.search(line)
        with state.lock:
            job.log_tail.append(line[-240:])
            del job.log_tail[:-20]
            if match:
                job.progress = match.group(1).replace(" ", "")
            job.message = line[-120:]
    process.stdout.close()
    return_code = process.wait()
    with state.lock:
        job.return_code = return_code
        job.process = None
        job.finished_at = time.time()
        if job.state == "cancelling":
            job.state = "cancelled"
            job.message = "Cancelled"
        else:
            job.state = "complete" if return_code == 0 else "failed"
            if not job.message:
                job.message = "Complete" if return_code == 0 else f"Exited {return_code}"


def create_app(config: dict[str, Any]) -> FastAPI:
    state = BridgeState()
    token_env = config.get("auth_token_env", "STOCK_BRIDGE_TOKEN")
    expected_token = os.environ.get(token_env, config.get("auth_token", ""))
    if not expected_token:
        raise RuntimeError(f"Set {token_env}; the bridge refuses to run without authentication")

    def authorize(authorization: str | None = Header(default=None)) -> None:
        scheme, _, supplied = (authorization or "").partition(" ")
        if scheme.lower() != "bearer" or not hmac.compare_digest(supplied, expected_token):
            raise HTTPException(status_code=401, detail="Invalid bearer token")

    app = FastAPI(title="T-Watch 1337 Stock Bridge", version="1.0")

    @app.get("/api/status", dependencies=[Depends(authorize)])
    def status() -> dict[str, Any]:
        job = latest_job(state)
        return {"state": job.state if job else "idle", "job": public_job(job) if job else None}

    @app.get("/api/presets", dependencies=[Depends(authorize)])
    def presets() -> dict[str, Any]:
        return {"presets": sorted(config.get("presets", {}))}

    @app.get("/api/alerts", dependencies=[Depends(authorize)])
    def alerts() -> dict[str, Any]:
        items = read_snapshot(config).get("alerts", [])
        with state.lock:
            return {"alerts": [item for item in items if str(item.get("id", "")) not in state.acked_alerts]}

    @app.post("/api/alerts/{alert_id}/ack", dependencies=[Depends(authorize)])
    def acknowledge(alert_id: str) -> dict[str, bool]:
        with state.lock:
            state.acked_alerts.add(alert_id)
        return {"ok": True}

    @app.post("/api/jobs", dependencies=[Depends(authorize)], status_code=202)
    def start_job(request: JobRequest) -> dict[str, Any]:
        preset = config.get("presets", {}).get(request.preset)
        if not isinstance(preset, dict):
            raise HTTPException(status_code=404, detail="Unknown preset")
        if config.get("one_job_at_a_time", True):
            with state.lock:
                if any(job.state in {"queued", "running", "cancelling"} for job in state.jobs.values()):
                    raise HTTPException(status_code=409, detail="A job is already running")
        job = Job(id=uuid.uuid4().hex[:12], preset=request.preset, device_name=request.device_name)
        with state.lock:
            state.jobs[job.id] = job
        threading.Thread(target=run_job, args=(config, state, job, preset), daemon=True).start()
        return public_job(job)

    @app.post("/api/jobs/{job_id}/cancel", dependencies=[Depends(authorize)])
    def cancel_job(job_id: str) -> dict[str, Any]:
        with state.lock:
            job = state.jobs.get(job_id)
            if not job:
                raise HTTPException(status_code=404, detail="Unknown job")
            process = job.process
            if job.state not in {"queued", "running"}:
                return public_job(job)
            job.state = "cancelling"
        if process:
            process.terminate()
        return public_job(job)

    @app.get("/api/watch/snapshot", response_class=PlainTextResponse, dependencies=[Depends(authorize)])
    def watch_snapshot(symbols: str = Query(default="AAPL,MSFT,SPY,QQQ")) -> str:
        requested = []
        for value in symbols.split(","):
            symbol = re.sub(r"[^A-Z0-9.\-]", "", value.upper())[:11]
            if symbol and symbol not in requested:
                requested.append(symbol)
            if len(requested) == 5:
                break
        snapshot = read_snapshot(config)
        quotes = snapshot.get("quotes", {}) if isinstance(snapshot.get("quotes", {}), dict) else {}
        job = latest_job(state)
        status_text = snapshot.get("message", "")
        if job:
            status_text = job.message
            status_line = f"{job.state}|{status_text}|{job.progress}"
        else:
            status_line = f"idle|{status_text or 'Ready'}|"
        lines = ["VERSION=1", f"STATUS={clean_protocol(status_line)}"]
        for symbol in requested:
            quote = quotes.get(symbol)
            if not isinstance(quote, dict):
                quote = alpha_vantage_quote(config, state, symbol)
            if not quote:
                continue
            lines.append("QUOTE=" + "|".join(clean_protocol(value) for value in (
                symbol, quote.get("price", ""), quote.get("change", ""),
                quote.get("pct", ""), quote.get("stamp", ""))))
            spark = quote.get("spark", [])
            if isinstance(spark, list) and spark:
                lines.append(f"SPARK={symbol}|" + ",".join(clean_protocol(value) for value in spark[-24:]))
        if len(lines) == 2:
            lines.append("ERROR=No quote data; configure snapshot_file or ALPHAVANTAGE_API_KEY")
        return "\n".join(lines) + "\n"

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("tools/stock_bridge.json"))
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    args = parser.parse_args()
    config = load_config(args.config)
    import uvicorn
    uvicorn.run(
        create_app(config),
        host=args.host or config.get("host", "127.0.0.1"),
        port=args.port or int(config.get("port", 8737)),
        log_level="info",
    )


if __name__ == "__main__":
    main()
