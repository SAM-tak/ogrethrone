# レベルとグラフィックのリソース

## 1. 文書の位置づけ

ゲームの舞台となるデータ（レベル）と、その描画に使うグラフィックのリソースの分担を定める。

［補足］OGRE-Next には、SceneManager の中身（シーンノード、Item、ライト、マテリアル、テクスチャ、メッシュなど）を
JSON で書き出し・読み込む SceneFormat コンポーネント（`Components/SceneFormat`）がある。
ただしこれは描画のスナップショットであり、ゲームオブジェクトの ID・スクリプト・物理のようなゲーム側の情報を持たない。
旧 OGRE 1.x の DotScene（`.scene`）は OGRE-Next には含まれない。

## 2. 確定事項

- ゲーム側の情報は LTON のファイルに持ち、これを「レベル」と呼ぶ。「シーン」の呼称は OGRE の描画側（`SceneManager`、`SceneNode`、SceneFormat）に譲る。
- グラフィックのリソースは、基本的に OGRE の形式をそのまま採用する。
- グラフィックはレベルとは別のファイルとし、レベルのゲームオブジェクトがそれをパスで参照する。

［補足］コードの命名もこれに従う。ゲーム側のデータは `Level`、エディターは `Editor` とし、`Scene` を含む名前は OGRE の描画に関わるものだけに使う。

## 3. レベル

［提案］レベルのファイルは `levels/` 以下の `*.level.lton` とする。中身は現行のシーン形式（`format`・`version`・`objects`）を引き継ぎ、`format` を `"ogrethrone-level"` とする。
［補足］後方互換性は保たないため、現行の `*.throne.lton` は読み込まない。

［提案］ゲームオブジェクトは、現行の項目（ID、名前、変換、色、スクリプト）に加え、描画するものをパスで参照する。

```lton
{
    id = 1,
    name = "Crate",
    position = {0, 0.5, 0},
    rotation = {0, 0, 0},
    scale = {1, 1, 1},
    mesh = "models/crate.mesh",                  # OGRE の v2 メッシュ
    material = "materials/crate.material.json",  # Hlms の JSON マテリアル。省略時はメッシュの既定
    script = "scripts/crate.lh",
},
```

［提案］`mesh` を持たないオブジェクトは、当面は現行どおり単位立方体として描く。

## 4. グラフィックのリソース

［提案］次は OGRE の形式のまま使う。

- メッシュ: `.mesh`（v2）
- マテリアル: Hlms の JSON（`.material.json`）
- テクスチャ: OGRE が読める画像形式
- コンポジター: `.compositor` スクリプト

［提案］ライトやプローブを含む背景・環境は、SceneFormat の書き出したフォルダとして置き、レベルの最上位から参照する。

```lton
environment = "environments/hangar/scene.json",
```

## 5. 段階

［提案］最初の段階はプロジェクトとランチャー（03）に合わせ、ファイル名・`format` の改名と `levels/` への移動だけを行う。
メッシュ・マテリアルの参照と SceneFormat の取り込みは、アセットの取り込み（01 の STK-02）と合わせて後の段階で実装する。

## 改定履歴（要約）

- 初版。
