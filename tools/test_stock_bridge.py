import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

from fastapi.testclient import TestClient

import stock_bridge


class StockBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        (root / "snapshot.json").write_text(
            '{"message":"Ready","quotes":{"AAPL":{"price":"123.45",'
            '"change":"+1.00","pct":"+0.82%","stamp":"2026-08-13",'
            '"spark":[120,121,123.45]}}}',
            encoding="utf-8",
        )
        os.environ["TEST_STOCK_BRIDGE_TOKEN"] = "unit-test-token"
        self.config = {
            "_config_dir": str(root),
            "auth_token_env": "TEST_STOCK_BRIDGE_TOKEN",
            "snapshot_file": "snapshot.json",
            "one_job_at_a_time": True,
            "presets": {
                "test": {
                    "cwd": ".",
                    "command": [sys.executable, "-c", "print('PROGRESS=1/1')"],
                }
            },
        }
        self.client = TestClient(stock_bridge.create_app(self.config))
        self.auth = {"Authorization": "Bearer unit-test-token"}

    def tearDown(self):
        self.client.close()
        os.environ.pop("TEST_STOCK_BRIDGE_TOKEN", None)
        self.temp.cleanup()

    def test_authentication_is_required(self):
        self.assertEqual(self.client.get("/api/status").status_code, 401)

    def test_watch_snapshot_protocol(self):
        response = self.client.get(
            "/api/watch/snapshot?symbols=AAPL", headers=self.auth
        )
        self.assertEqual(response.status_code, 200)
        self.assertIn("VERSION=1\n", response.text)
        self.assertIn("QUOTE=AAPL|123.45|+1.00|+0.82%|2026-08-13", response.text)
        self.assertIn("SPARK=AAPL|120,121,123.45", response.text)

    def test_only_allowlisted_presets_run(self):
        denied = self.client.post(
            "/api/jobs", headers=self.auth, json={"preset": "arbitrary-command"}
        )
        self.assertEqual(denied.status_code, 404)

        started = self.client.post(
            "/api/jobs", headers=self.auth, json={"preset": "test"}
        )
        self.assertEqual(started.status_code, 202)
        deadline = time.time() + 5
        state = ""
        while time.time() < deadline:
            state = self.client.get("/api/status", headers=self.auth).json()["state"]
            if state in {"complete", "failed"}:
                break
            time.sleep(0.05)
        self.assertEqual(state, "complete")


if __name__ == "__main__":
    unittest.main()
