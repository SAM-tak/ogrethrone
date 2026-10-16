# Runner とシーン

## 1. 文書の位置づけ

レベルの中で振る舞いを担う存在（Runner）と、描画側のシーンノードとの関係を定める。
04 のレベルとグラフィックの分担を、編集できる形に具体化する。

［補足］現状のレベルは「位置・色・スクリプトを持つキューブ」の並びであり、振る舞い（スクリプト）が描画される物に直接付いている。
これは Godot の Node スクリプトや Unity の GameObject のコンポーネントと同じ形である。

## 2. 確定事項

- 振る舞いを担う存在を **Runner** と呼ぶ。
  - Runner はシーン上の存在ではない。シーン階層とは無関係に存在し、位置（Transform）を持つとは限らない。
    デバッグ表示だけを担うもの、バックグラウンドの読み込みだけを担うものも Runner として書ける。
  - 振る舞いは、Runner に付く Behaviour（スクリプトの定義から Runner ごとに作るインスタンス）として書く。
    コンポーネントは純粋なデータに保ち、ECS の System に当たる仕組みは後で検討する。
  - 振る舞いは L^ のスクリプトで書く。将来は C/C++ でも書けるようにする。
- Unity の「GameObject は必ず Transform を持つ」設計も、Godot の「Transform を持たなくても Node としてシーン上にある」設計も採らない。
- Runner はレベルに属するものと、シーンに属するものがある。どちらのファイルにも Runner を置ける。
- Runner の一覧を表示する **Runners ペイン**を新設する。シーン階層のペインとは別である。
- `main.lh`（プロジェクト直下。02 の 4.1）は残し、今後もゲーム全体のエントリーポイントとして働く。Runner とは別の存在である。
  「ゲームのプレイ」でだけ実行し、「今開いているレベルのプレイ」では実行しない（02 の 4.1）。
- シーンノード（描画側）は `*.scene.lton` に保存する。親子の階層を持つ。
- `*.scene.lton` は `*.prefab.lton`（プレハブ）の拡張として実装する。拡張点はシーン特有のデータ（グローバルなライティングや環境など。04 の 4 節）だけである。
- 1 つのレベルは複数のシーンを持てる。同じレベルのまま新しいシーンを読み込み、要らなくなったシーンを捨てることで、シームレスな移動を実現する。
- 例: SpinningCube という Runner が、SpinningCube というシーンノードへの参照をデータとして持ち、`spin.lh` を振る舞いとして持つ。
  `spin.lh` の制御対象にそのシーンノードを指定しているから、Play 時に SpinningCube のシーンノードが回転する。
- 1 つの Runner は複数のスクリプト（振る舞い）を持てる。
- Runner は、振る舞いの欄とは別のデータ欄を持たない。各振る舞いのインスタンスごとの欄の値は Runner が保持するが、
  エディターでは「Runner に付けた各スクリプトのインスペクターを編集する」見た目にする。
- 内部の実装は ECS（エンティティ・コンポーネント・システム）的な構造でよいが、その用語と構造はエディターに見せない。
- エディターの UI は Dear ImGui の docking 版に切り替え、ペインをドッキングできるようにする。

## 3. シーン

［提案］シーンのファイルは `scenes/` 以下の `*.scene.lton` とする。

```lton
format = "ogrethrone-scene",
version = 1,
nodes = {
    {
        id = 1,
        name = "SpinningCube",
        parent = 0,                  # 0 はシーンの根
        position = {0, 0.5, 0},      # 親に対するローカル座標
        rotation = {0, 0, 0},        # 度。X→Y→Z の順に適用
        scale = {1, 1, 1},
        colour = {0.32, 0.62, 0.82},
        # mesh = "models/crate.mesh"  # 04 の 3 節。当面は省略すると単位立方体
    },
},
```

［提案］`nodes` は、親が子より前に来る順に並べる。兄弟の順は、この並びの順とする。

［提案］プレハブのファイルは `prefabs/` 以下に置く。
［補足］シーンノードと Runner の書き方は、2 節によりプレハブとシーンで共通になる。実装でも、シーンの型はプレハブの型を拡張したものとする。

［補足］編集時はテキスト、ビルド時は L^ の事前コンパイル（`lhatstdlib_lton_write`、L^ 08 の 7改）でバイナリにできる。
OGRE の SceneFormat は JSON だけでバイナリ形式を持たないため、シーンの保存形式には使わず、外部の OGRE シーンを取り込む経路として残す。

## 4. レベル

［提案］レベル（04）は、開始時に読み込むシーンの並びと、レベルに属する Runner の並びを持つ。
シーン（3 節）も同じ形の `runners` を持てる。シーンに属する Runner は、そのシーンを使うどのレベルでも動く。

```lton
format = "ogrethrone-level",
version = 2,
scenes = { "scenes/main.scene.lton" },   # 開始時に読み込むシーン
runners = {
    {
        id = 1,
        name = "SpinningCube",
        behaviours = {
            {
                script = "scripts/spin.lh",
                fields = {
                    target = { scene = "scenes/main.scene.lton", node = 1 },   # シーンノードへの参照
                    degreesPerSecond = 90,
                },
            },
        },
    },
},
```

［提案］Runner は名前と、0 個以上の振る舞い（Behaviour）を持つ。振る舞いの欄に書いた値が、その Runner のデータである。
シーンノードへの参照は欄の値として持ち、シーンのパスとノードの ID で指す。
シーンに属する Runner が同じシーンのノードを指すときは、`scene` を省略できる。
参照先のシーンが読み込まれていないときは、その欄は `nil^` になる。

［提案］シーンの読み込み・破棄は、スクリプトから行えるようにする（`throne.loadScene(path)`、`throne.unloadScene(path)`）。
シーンを破棄すると、そのシーンに属する Runner も止まり、そのシーンのノードを指すハンドルは無効になる（02 の 5.2 の `valid()`）。

［補足］2 節の「シーンノードへの参照をデータとして持つ」は、振る舞いの欄の値として表す（Runner は独自のデータ欄を持たないため）。

## 5. 振る舞い

［提案］振る舞いのスクリプトは、02 の 4.2 と同じく `Behaviour` という定義を公開する `module^` とする。

```lhat
module^ samples.spin
import^ throne

public^let^Behaviour = def^{
    self^{
        target : throne.SceneNode|nil^ = nil^,
        degreesPerSecond = 90,
    },
    update = p^self^, runner:throne.Runner, dt:number^ {
        if^ self^.target != nil^ {
            var^ r = self^.target.rotation()
            r.y := r.y + self^.degreesPerSecond * dt
            self^.target.setRotation(r)
        }
    },
}
```

［提案］`self^{ … }` の欄が、インスペクターで編集できる欄になる。型は L^ の検査器が決めたものを使う（02 の 4.2 の補足と同じく `lhat_unit_export_conforms` などで読む）。

- `number^`・`string^`・`bool^`: そのまま編集する
- `throne.Vec3`: 3 つの数値として編集する
- `throne.SceneNode`: シーンノードを選ぶ
- `throne.Runner`: Runner を選ぶ

レベルに書いた欄の値は、インスタンスを作った直後に、その欄の既定値を上書きする。

［提案］`start`・`update` の第 2 引数は `throne.Runner` とする（現状の `throne.Object` から変える）。
`throne.SceneNode` は、現状の `throne.Object` のうち変換と色を扱う部分を引き継ぐ。

［提案］1 フレームの実行順は、シーンに属する Runner、レベルに属する Runner の順とする。それぞれの中では並びの順とする。
ゲームのプレイで `main.lh` がどう割り込むかは 02 の MAIN-01 に従う。

## 6. エディター

［提案］

- ペインは Scene（シーンノードの階層）、Runners、Inspector、Viewport、Browser とし、ドッキングできるようにする。
- Runners ペインは、レベルに属する Runner とシーンに属する Runner を分けて表示する。
- Scene ペインでは、ドラッグでノードの親子を付け替えられる。
- Inspector は、選んでいるのがシーンノードか Runner かで中身を切り替える。
  Runner では、振る舞いの追加・削除と、各振る舞いの欄の編集ができる。
- ビューポートでのクリックは、シーンノードを選ぶ。

## 改定履歴（要約）

- 初版。
