# System とシーン

## 1. 文書の位置づけ

処理の実行単位である System と、データ・振る舞いを持つシーンノードとの関係を定める。
04 のレベルとグラフィックの分担を、編集できる形に具体化する。

ノードには Transform などのデータ Component と、データ・処理を一緒に持つ Behaviour Component を付ける。BehaviourSystem は対応するメソッドを持つ Behaviour を問い合わせて呼び出す（07）。

これでもれっきとした Entity Component System だが、純粋データ指向ではなく、オブジェクト指向とのハイブリッドである。
OGRE/Box3Dなどが内部でSoA型のデータ配置をするのに任せていて、throne自身は特にComponentをSoAで配置するようなことはしていない。
Behaviour Component が扱う Behaviour はオブジェクト指向、TransformやCollider/Rigidbody/Primitive/Mesh などはデータ指向である。

## 2. 確定事項

- 処理の実行単位を **System** と呼ぶ。
  - System はシーン上の存在ではない。シーン階層とは無関係に存在し、位置（Transform）を持つとは限らない。
    デバッグ表示だけを担うもの、バックグラウンドの読み込みだけを担うものも System として書ける。
  - ノードの振る舞いは `components.behaviour` のスクリプトインスタンスとして書く。データも処理も同じインスタンスが持ち、BehaviourSystem が問い合わせて実行する。
  - 進行役などの独立した処理は、シーンまたはレベルの System にスクリプトを直接付けて書くこともできる。これはノードの Behaviour の登録先を指定する仕組みではない。
  - 振る舞いは L^ のスクリプトで書く。将来は C/C++ でも書けるようにする。
- Unity の「GameObject は必ず Transform を持つ」設計も、Godot の「Transform を持たなくても Node としてシーン上にある」設計も採らない。
- System はレベルに属するものと、シーンに属するものがある。どちらのファイルにも System を置ける。
- **Systems ペイン**に実行順を表示する。シーン階層のペインとは別であり、各 System は名前だけを表示する。
- `main.lh`（プロジェクト直下。02 の 4.1）は残し、今後もゲーム全体のエントリーポイントとして働く。System とは別の存在である。
  「ゲームのプレイ」でだけ実行し、「今開いているレベルのプレイ」では実行しない（02 の 4.1）。
- シーンノード（描画側）は `*.scene.lton` に保存する。親子の階層を持つ。
- `*.scene.lton` は `*.prefab.lton`（プレハブ）の拡張として実装する。拡張点はシーン特有のデータ（グローバルなライティングや環境など。04 の 4 節）だけである。
- 1 つのレベルは複数のシーンを持てる。同じレベルのまま新しいシーンを読み込み、要らなくなったシーンを捨てることで、シームレスな移動を実現する。
- 例: SpinningCube の Behaviour Component に `spin.lh` を付ける。UpdateBehaviourSystem がその `update` を問い合わせて呼び出すため、Play 時にノードが回転する。
- 1 つの System は複数のスクリプト（振る舞い）を持てる。
- System は、振る舞いの欄とは別のデータ欄を持たない。各振る舞いのインスタンスごとの欄の値は System が保持するが、
  エディターでは「System に付けた各スクリプトのインスペクターを編集する」見た目にする。
- エディターの Systems ペインには、直接の System、エンジンの段、BehaviourSystem を統一して表示する（07）。
- エディターの UI は Dear ImGui の docking 版に切り替え、ペインをドッキングできるようにする。

## 3. シーン

シーンは `scenes/` 以下の `*.scene.lton` に保存する。シーンノードは ID・名前・親子関係と Component を持つ。Transform は必須ではない。

```lton
format = "ogrethrone-scene",
version = 2,
nodes = {
    {
        id = 1, name = "SpinningCube", parent = 0,
        components = {
            transform = { position = {0, 0.5, 0}, rotation = {0, 0, 0}, scale = {1, 1, 1} },
            box = { colour = {0.32, 0.62, 0.82} },
            behaviour = {{ script = "scripts/spin.lh", data = { degreesPerSecond = 45 } }},
        },
    },
},
systems = {},
```

親を子より前に保存し、兄弟の順番は保存順とする。プレハブは単一の根を持つノード階層であり、シーンと同じ Component・System の形式を使う。入れ子・上書き・バリアントは 06 による。

## 4. レベル

レベルは開始時に読み込むシーンインスタンスと、レベル固有の System を持つ。シーンも `systems` を持てる。例えば独立した進行役は次のように保存する。

```lton
format = "ogrethrone-level",
version = 3,
scenes = {{ id = 1, scene = "scenes/main.scene.lton" }},
systems = {
    {
        id = 1, name = "GameFlow",
        behaviour = {{ script = "scripts/game-flow.lh", systemScript = true^, data = {} }},
    },
},
```

［補足］`systems` に記述した直接のスクリプトには `throne.System` が渡る。対応する名前の実行段がなければ、UpdateBehaviourSystem の前に作る。エンジンの同期段や BehaviourSystem に直接のスクリプトを付けることはできない。段の順番と優先順位は 07 による。

保存するノード参照は `{ scene = シーンインスタンスID, node = ノードID, inner = {...} }` とする。`inner` は入れ子のプレハブへの経路で、不要なら省略する。シーン／プレハブ内の相対参照では `scene` を省略できる。参照先が読み込まれていないときは、スクリプトの既定値を保持する。

［提案］シーンの読み込み・破棄をスクリプトから行う API は、02 のゲーム進行 API と合わせて設計する。現在は未実装。

## 5. 振る舞い

ノードの振る舞いは `Behaviour = def^{ ... }` を公開するスクリプトとして書く。`components.behaviour` の要素ごとに別のインスタンスを作り、`data` の値を適用する。同じスクリプトを複数付けても、状態は共有しない。

```lhat
module^ samples.spin
import^ throne
public^let^ Behaviour = def^{
    self^{ @export degreesPerSecond = 90 },
    update = p^self^, node:throne.SceneNode, dt:number^ {
        let^ t = node.transform()
        if^ t != nil^ {
            t.setRotation(t.rotation() + throne.vec3(0, self^.degreesPerSecond * dt, 0))
        }
    },
}
```

BehaviourSystem は `update`・`lateupdate`・`onendofframe` の対応するメソッドを問い合わせて実行する。Behaviour 側からの所属 System の指定・登録は不要。通常の関数・手続きと、中断可能なメソッドを受け付ける。実行順・コルーチンの待ち先・有効状態は 07 による。

独立した進行役の `scripts/game-flow.lh` は、例えば次のように書ける。

```lhat
module^ samples.gameFlow
import^ throne
public^let^ System = def^{
    start = p^self^, system:throne.System { throne.log(system.name()) },
}
```

［補足］直接のスクリプトは `systemScript = true^` なら `System`、省略時は `Behaviour` を公開する。どちらもノードへの付加とは別の実行単位であり、第 2 引数は `throne.System` となる。

`self^{ ... }` の `@export` 欄は Inspector で編集でき、`data` に保存する。対応する型は number・bool・string・SceneNode 参照。その他の欄はスクリプト内部の状態として保持する。保存した値は生成直後に既定値を上書きする。型が違う値や公開していない欄は警告して読み飛ばす。

## 6. エディター

- ペインは Scene（シーンノードの階層）、Systems、Inspector、Viewport、Browser とし、ドッキングできるようにする。
- Systems ペインは、段の実行順と、BehaviourSystem の条件・最終 priority・対象の並びを表示する。
- Scene ペインでは、ドラッグでノードの親子を付け替えられる。
- Inspector は、選んでいるのがシーンノードか System かで中身を切り替える。
  System では、振る舞いの追加・削除と、各振る舞いの欄の編集ができる。
- ビューポートでのクリックは、シーンノードを選ぶ。

［補足］ノードから System への逆参照（実装済み）:

- エディターは、表示中のすべての System（レベル・シーン・プレハブ由来・上書きで足したもの）の欄のノード参照を解決し、「ノード → それを指す（System, 振る舞い, 欄）」の索引を持つ。上書きを適用した値で、Play と同じ規則で解決する。展開をやり直すたびに作り直す。
- 索引はエディター専用（`src/editor/References.h`）で、エディターの実行ファイルにだけ入る。データ（シーン・プレハブ・レベル）にもランタイムにも持たないので、ゲームのビルドには含まれない。
- ノードの Inspector に「Referenced by」として「System名 > スクリプト > 欄」を並べ、クリックでその System を選ぶ。
- 参照されているノードを Delete すると、どの欄が指していたかをログに出す。
- ノードを複製すると、そのノードを指す System も複製し、複製した System は複製したノードを指す。対象は、同じファイルの System と、Level の段ではそのシーンインスタンスを指すレベルの System。

## 改定履歴（要約）

- 初版。

- ノードの振る舞いを Behaviour Component にまとめ、BehaviourSystem が問い合わせる形に整理した（07）。
