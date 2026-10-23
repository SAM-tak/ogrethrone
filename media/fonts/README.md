# Editor font

`NotoSansJP-Regular.otf` and `NotoSansSC-Regular.otf` are the unmodified Japanese and
Simplified Chinese subsets of Noto Sans CJK, version 2.004, from the upstream `Sans2.004` tag:

Copyright © 2014-2021 Adobe (http://www.adobe.com/).

- [Japanese font](https://github.com/notofonts/noto-cjk/blob/Sans2.004/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf),
  SHA-256: `dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073`.
- [Simplified Chinese font](https://github.com/notofonts/noto-cjk/blob/Sans2.004/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf),
  SHA-256: `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`.
- [License](https://github.com/notofonts/noto-cjk/blob/Sans2.004/LICENSE), included as `OFL.txt` (SIL Open Font License 1.1).

The editor uses the Japanese font for Latin and Japanese text, including project names,
paths and input fields. Its atlas includes all BMP glyphs available in it, and the common
Simplified Chinese characters it lacks (about 2,500, as Dear ImGui lists them) from the
Simplified Chinese font, for names in Chinese models and assets.
