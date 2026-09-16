import importlib.util
import json
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("wallpaper_service.py")
sys.path.insert(0, str(MODULE_PATH.parent))
if "opencc" not in sys.modules:
    opencc = types.ModuleType("opencc")

    class OpenCC:
        def __init__(self, _configuration):
            pass

        def convert(self, text):
            return text

    opencc.OpenCC = OpenCC
    sys.modules["opencc"] = opencc
SPEC = importlib.util.spec_from_file_location("wallpaper_service", MODULE_PATH)
SERVICE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SERVICE)


class QuotaSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.library_temp=tempfile.TemporaryDirectory()
        self.library_patch=patch.object(SERVICE.artwork_library,'ROOT',Path(self.library_temp.name)/'library')
        self.library_patch.start()
    def tearDown(self):
        self.library_patch.stop();self.library_temp.cleanup()
    def test_accepts_llmquota_dashboard_without_mutating_values(self):
        expected = {
            "generatedAt": "2026-09-14T02:35:08Z",
            "reports": [{
                "agentName": "Codex CLI",
                "last30dBillableTokens": 69252159,
                "statuses": [{"used": 26, "limit": 100}],
            }],
        }
        payload = json.dumps(expected).encode()
        self.assertEqual(SERVICE.validate_quota_snapshot(payload), expected)

    def test_rejects_derived_or_incomplete_payload(self):
        payload = json.dumps({"generatedAt": "now", "total": 123}).encode()
        with self.assertRaisesRegex(ValueError, "no reports"):
            SERVICE.validate_quota_snapshot(payload)

    def test_rejects_negative_report_tokens(self):
        payload = json.dumps({
            "generatedAt": "now",
            "reports": [{"agentName": "Codex", "last30dBillableTokens": -1}],
        }).encode()
        with self.assertRaisesRegex(ValueError, "invalid last30dBillableTokens"):
            SERVICE.validate_quota_snapshot(payload)

    def test_codex_reset_feed_preserves_signal_identity(self):
        feed = {
            "fetched_at": "2026-09-14T02:12:10Z",
            "stale": False,
            "signal": {
                "tweet_id": "2098685367058612394",
                "summary": "Reset all propagated.",
                "at": "2026-09-12T08:09:17Z",
                "kind": "candidate",
                "active": True,
            },
        }

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return None

            def read(self, limit=-1):
                return json.dumps(feed).encode()

        SERVICE.codex_reset_cache = None
        with patch.object(SERVICE, "analyze_codex_reset",
                          return_value=("MiniMax 判断: 重置已生效, 请刷新 Codex 用量确认.", "minimax")), \
                patch.object(SERVICE.urllib.request, "urlopen", return_value=Response()):
            result = SERVICE.fetch_codex_reset()
        self.assertEqual(result["signalId"], "2098685367058612394")
        self.assertFalse(result["active"])
        self.assertFalse(result["forecastAvailable"])
        self.assertEqual(result["source"], "codex-reset.com")
        self.assertEqual(result["analysisProvider"], "rule")

    def test_reset_analysis_messages_are_compact_and_conservative(self):
        self.assertIn("已生效", SERVICE.reset_analysis_message("confirmed"))
        self.assertIn("可能", SERVICE.reset_analysis_message("likely"))
        self.assertIn("未确认", SERVICE.reset_analysis_message("uncertain"))

    def test_reset_partial_source_failure_retries_without_five_minute_cache(self):
        with patch.object(SERVICE, 'codex_reset_cache', {'forecastAvailable':False}), \
                patch.object(SERVICE, 'codex_reset_cache_time', 100), \
                patch.object(SERVICE.time_module, 'monotonic', return_value=131), \
                patch.object(SERVICE.urllib.request, 'urlopen', side_effect=OSError('offline')) as call:
            with self.assertRaisesRegex(ValueError, 'sources unavailable'):
                SERVICE.fetch_codex_reset()
            self.assertEqual(call.call_count, 2)

    def test_reset_healthy_cache_does_not_hammer_public_apis(self):
        cached = {'forecastAvailable':True}
        with patch.object(SERVICE, 'codex_reset_cache', cached), \
                patch.object(SERVICE, 'codex_reset_cache_time', 100), \
                patch.object(SERVICE.time_module, 'monotonic', return_value=131), \
                patch.object(SERVICE.urllib.request, 'urlopen') as call:
            self.assertIs(SERVICE.fetch_codex_reset(), cached)
            call.assert_not_called()

    def test_music_downmixes_stereo_pcm_without_changing_duration(self):
        stereo = b"\x10\x00\x30\x00\xf0\xff\xd0\xff"
        self.assertEqual(SERVICE.downmix_pcm16le(stereo, 2), b"\x20\x00\xe0\xff")

    def test_music_generation_stores_mono_pcm(self):
        response = {
            "base_resp": {"status_code": 0},
            "data": {"audio": "10003000f0ffd0ff"},
            "extra_info": {
                "music_sample_rate": 16000,
                "music_channel": 2,
                "music_duration": 250,
            },
        }

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return None

            def read(self, limit=-1):
                return json.dumps(response).encode()

        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict("os.environ", {"MINIMAX_API_KEY": "test-key"}), \
                patch.object(SERVICE, "MUSIC_PCM", Path(temporary) / "music.pcm"), \
                patch.object(SERVICE, "MUSIC_METADATA", Path(temporary) / "music.json"), \
                patch.object(SERVICE.urllib.request, "urlopen", return_value=Response()):
            metadata = SERVICE.generate_music("calm night piano")
            self.assertEqual(metadata["channels"], 1)
            self.assertEqual(SERVICE.MUSIC_PCM.read_bytes(), b"\x20\x00\xe0\xff")

    def test_voice_wallpaper_generation_activates_a_valid_clock_payload(self):
        image = SERVICE.Image.new("RGB", (12, 12), (20, 80, 140))
        encoded = SERVICE.io.BytesIO()
        image.save(encoded, format="PNG")
        response = {
            "base_resp": {"status_code": 0},
            "data": {"image_base64": [SERVICE.base64.b64encode(encoded.getvalue()).decode()]},
        }

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return None

            def read(self, limit=-1):
                return json.dumps(response).encode()

        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict("os.environ", {"MINIMAX_API_KEY": "test-key"}), \
                patch.object(SERVICE, "CURRENT_RAW", Path(temporary) / "current.rgb565"), \
                patch.object(SERVICE, "CURRENT_PNG", Path(temporary) / "current.png"), \
                patch.object(SERVICE, "METADATA", Path(temporary) / "metadata.json"), \
                patch.object(SERVICE.urllib.request, "urlopen", return_value=Response()):
            metadata = SERVICE.generate_wallpaper_from_prompt("aurora over a quiet lake")
            self.assertEqual(metadata["provider"], "minimax/image-01")
            self.assertEqual(SERVICE.CURRENT_RAW.stat().st_size, SERVICE.PAYLOAD_BYTES)


if __name__ == "__main__":
    unittest.main()
