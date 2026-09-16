import os
import unittest
from unittest.mock import patch

from test_quota_snapshot import SERVICE


class RuntimeSecurityTests(unittest.TestCase):
    def test_rejects_missing_or_short_required_token(self):
        with patch.object(SERVICE, "REQUIRE_WALLPAPER_TOKEN", True), \
                patch.dict(os.environ, {"WALLPAPER_TOKEN": "short"}, clear=False):
            with self.assertRaisesRegex(RuntimeError, "at least 32"):
                SERVICE.validate_runtime_security("127.0.0.1")

    def test_rejects_direct_public_bind(self):
        with patch.object(SERVICE, "REQUIRE_WALLPAPER_TOKEN", True), \
                patch.dict(os.environ, {"WALLPAPER_TOKEN": "x" * 32}, clear=False):
            with self.assertRaisesRegex(RuntimeError, "loopback"):
                SERVICE.validate_runtime_security("0.0.0.0")

    def test_accepts_loopback_with_strong_token(self):
        with patch.object(SERVICE, "REQUIRE_WALLPAPER_TOKEN", True), \
                patch.dict(os.environ, {"WALLPAPER_TOKEN": "x" * 32}, clear=False):
            SERVICE.validate_runtime_security("127.0.0.1")


if __name__ == "__main__":
    unittest.main()
