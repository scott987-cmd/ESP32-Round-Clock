import unittest
from unittest.mock import patch
from test_quota_snapshot import SERVICE


class WallpaperPromptTests(unittest.TestCase):
    def test_empty_recognition_never_calls_paid_provider(self):
        with patch.object(SERVICE.urllib.request, 'urlopen') as provider:
            for prompt in ('。', '...', '！？', ' \n ', ''):
                with self.subTest(prompt=prompt), self.assertRaises(ValueError):
                    SERVICE.generate_wallpaper_from_prompt(prompt)
            provider.assert_not_called()


if __name__ == '__main__':
    unittest.main()
