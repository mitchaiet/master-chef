# Third-party software

| Component | Included location | License |
| --- | --- | --- |
| MojoShader | `third_party/mojoshader` | zlib, see `LICENSE.txt` |
| stb_vorbis | `native/EngineHost/third_party/stb_vorbis.c` | MIT or public domain, notices in source |
| xxHash 0.8.3 | `native/EngineHost/third_party/xxhash` | BSD-2-Clause, see `LICENSE` |
| XWA decoder/lifter tools by sp00nznet | `third_party/xwa` | MIT, see `LICENSE` |

Vendored files retain their original notices. The app also bundles these
notices as `native/EngineVision/Resources/ThirdPartyNotices.txt`. Python
packages in `requirements-development.txt` are installed separately and
retain their own licenses. They are not vendored in the source archive.

Game files, HD texture packs and shader binaries are not stored in the Git
source tree. Separate Complete and visual-assets releases contain the
published assets and their notices; see [ASSET_NOTICES.md](docs/ASSET_NOTICES.md).
No project license grants rights to those materials. If you add optional
assets, retain the corresponding notices and source required by their
licenses in your local/distribution process.
