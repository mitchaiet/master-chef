#!/usr/bin/env python3
"""Ensure the documentation artwork exception cannot admit arbitrary binaries."""
from pathlib import Path
import tempfile
import unittest
from check_repository_hygiene import ROOT, DOCUMENTATION_ART, audit


class ArtworkHygieneTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root / 'docs/assets/master-chef-header.png'
        self.path.parent.mkdir(parents=True)
        self.art = (ROOT / 'docs/assets/master-chef-header.png').read_bytes()

    def test_exact_reviewed_art(self):
        self.path.write_bytes(self.art)
        self.assertEqual(audit(self.root, strict=True)[1], [])

    def test_changed_art_rejected(self):
        self.path.write_bytes(self.art + b'changed')
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'unreviewed-documentation-art')

    def test_different_path_rejected(self):
        self.path.with_name('unreviewed.png').write_bytes(self.art)
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'binary-file')

    def test_text_cannot_hide_under_image_name(self):
        self.path.write_text('unreviewed private input')
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'unreviewed-documentation-art')

    def test_layered_icons_require_exact_paths_and_bytes(self):
        for relative in DOCUMENTATION_ART:
            if not relative.startswith('native/'):
                continue
            with self.subTest(path=relative):
                icon = self.root / relative
                icon.parent.mkdir(parents=True, exist_ok=True)
                content = (ROOT / relative).read_bytes()
                icon.write_bytes(content)
                self.assertEqual(audit(self.root, strict=True)[1], [])
                icon.write_bytes(content + b'changed')
                self.assertEqual(audit(self.root, strict=True)[1][0][2], 'unreviewed-documentation-art')
                icon.unlink()
                unexpected = icon.with_name('Unreviewed.png')
                unexpected.write_bytes(content)
                self.assertEqual(audit(self.root, strict=True)[1][0][2], 'binary-file')
                unexpected.unlink()


if __name__ == '__main__':
    unittest.main()
