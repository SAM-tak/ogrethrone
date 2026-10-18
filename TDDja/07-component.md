# コンポーネントと実行の段

## 1. 文書の位置づけ

シーンノードを Entity とし、そのデータを Component に分けて整理する。あわせて、Behaviour の置き場所と、Runner による実行順の制御を定める。

Runner の現状は 05 による。この文書は 05 の「Runner が振る舞いを持つ」形を広げ、エンジンの処理と Behaviour を 1 本の実行順に並べる。

## 2. 確定事項

- throne は、ECS とオブジェクト指向のハイブリッドを目指す。Flecs のような純粋な ECS は標榜しない。
  - データを Component に分け、照会（query）で処理する System を書ける（ECS の側）。
  - 特定のノードを指して処理する Behaviour も持てる（オブジェクト指向の側）。
- `scripts/spin.lh` は、これまでどおり Behaviour とする（System にはしない）。
- Behaviour は、Runner の中ではなく、ノードに Component のように付ける。
- 実行のタイミングは、コルーチンの中断（Unity の `yield return new WaitForEndOfFrame()` のようなもの）ではなく、Runner の位置で指定する。中断を使う方法は非効率なので採らない。
- Runners パネルには、エンジンの処理も Runner（System）として並ぶ。例えば次のとおり。

  ```text
  FetchInput [System]
  ApplyPhysics [System]
  OGRE [System] / Box3D [System] / Animator [System]    ← 並行して走る組
  ```

  - OGRE（描画）は、前のフレームで組み上がったシーンの情報をもとに、メインスレッドで描画を発行する。
  - Box3D は、前のフレームで確定した位置をもとに、別スレッドで物理を計算する。
  - Animator は、別スレッドで、今のフレームのデータにアニメーションを適用する。
- Behaviour をどの Runner に差し込むかで、実行のタイミングを制御する。差し込みは UI からも、スクリプトからもできる。

［補足］Unreal の Tick Group と Tick の依存関係は、最終的な実行順が見えないという欠点を持つ。throne の Runner は、並べた一覧がそのまま実行順になることで、これを避ける（Unity DOTS・Bevy・Flecs も、前後の制約から順番を計算する点では同じ欠点を持つ）。

## 3. Component

シーンノードは、これまでどおり Node と呼ぶ（ID・名前・親子関係と Component の記述を持つので、ECS でいう「ただの ID」である Entity とは呼ばない）。親子関係（`parent`）は Node が持ち、Transform には持たせない。それ以外のデータは Component として持つ。今の欄は次のように分ける。

| 今の欄 | Component |
| --- | --- |
| `position`・`rotation`・`scale` | `Transform` |
| `colour` と「単位立方体である」こと | `BoxPrimitive`（`colour`） |

［提案］箱以外の形は、Component を足して表す（`SpherePrimitive`、モデルを読み込む `Mesh` など）。`Light`・`Camera` なども Component とする。

Component はそれぞれ省略できる。Transform を持たない Node は、親の配置をそのまま子へ渡す。BoxPrimitive を持たない Node は何も描かない。

スクリプトからは Component を引いてから扱う。Node から位置などを直接取る API は持たない。

```lhat
let^ t = node.transform()     // throne.Transform|nil^
if^ t != nil^ { t.setPosition(t.position() + throne.vec3(1, 0, 0)) }
node.box()                     // throne.BoxPrimitive|nil^（colour() / setColour()）
```

［補足］名前の文字列で引く形（`getComponent("Transform")`）にすると、L^ の検査では戻り値の型が名前ごとに変わることを表せず、`any^` になる。そのため型の付いた引き方にした。スクリプトで定義する Component（6 節）は、そのとき名前で引く汎用の形を足す。

［補足］`prefab`・`overrides` は編集のための構造で、Component ではない（Unity でもプレハブの仕組みは Component ではない）。

ノードの形式は次のとおりとする（後方互換は持たない。シーンは version 2、プレハブは version 3）。`behaviours` は段階 2 で足す。

```lton
{
    id = 7484712563498500096,
    name = "SpinningCube",
    parent = 0,
    components = {
        transform = { position = {0, 0.5, 0}, rotation = {0, 0, 0}, scale = {1, 1, 1} },
        box = { colour = {0.32, 0.62, 0.82} },
    },
    behaviours = {
        { script = "scripts/spin.lh", runner = "Update", fields = { degreesPerSecond = 90 } },
    },
},
```

上書き（06 の 4 節）は、Component ごとの表に欄を持つ（`transform = { position = ... }`、`box = { colour = ... }`）。上書きした欄は、その Node がその Component を持つときだけ効く。

［補足］インスタンスの中の Node に Component を足したり消したりする上書きは、まだない。Inspector で Component を足し・消しできるのは、編集中のファイルが持つ普通の Node だけである。

［補足］ECS の性能面（アーキタイプによる詰めた配置など）は、今は採らない。概念として ECS の整理を採り、保存と展開はノードごとのままとする。

## 4. ノードの Behaviour

［提案］ノードの `behaviours` に、Behaviour をいくつでも付けられる。各 Behaviour は、スクリプト、走る Runner の名前、欄の値を持つ。

［提案］ノードに付いた Behaviour の対象は、そのノード自身とする。`start`・`update` には、Runner ではなくそのノードが渡る。

```lhat
start = p^self^, node:throne.SceneNode { ... },
update = p^self^, node:throne.SceneNode, dt:number^ { ... },
```

［補足］これで、ノードを複製すると Behaviour も付いてくる。今のように、複製に合わせて Runner を複製する必要はなくなる。Inspector では、ノードの Component と Behaviour が並んで見える。

［提案］プレハブのノードに付いた Behaviour は、インスタンスごとに 1 つずつ生まれる（今のプレハブの Runner と同じ）。インスタンスは、Behaviour の欄の値を上書きでき、Behaviour を足したり消したりできる。

［提案］今の、Runner が直接持つ Behaviour（`target` の欄で特定のノードを指すもの）は残す。演出や進行役のように、特定のノードを外から動かす処理に使う（2 節のオブジェクト指向の側）。

## 5. 実行の段

［提案］Runner は、実行の段の並びになる。レベル全体で 1 本の並びを持ち、上から順に実行する。

- エンジンの Runner（System）: FetchInput、ApplyPhysics など。名前を持ち、消せない。並び替えられるかは ECS-02。
- ユーザーの Runner: 名前を付けて、並びの好きな位置に足せる。初めから `Update` を 1 つ置く。
- 並行の組: 複数の Runner を、同時に走る 1 つの段として並べる（2 節の OGRE / Box3D / Animator）。

［提案］ノードの Behaviour は、指定した Runner の段で走る。同じ段の中では、レベルのシーンインスタンスの順、その中のノードの階層の順（親が先、兄弟は上から）で走る。この順は Runners パネルで、段ごとに展開して見られる。

［提案］Runner が直接持つ Behaviour（4 節の最後）も、その Runner の段で走る。段の中では、ノードの Behaviour より先に走る。

［補足］L^ のマシンは 1 つのスレッドで動くので、Behaviour は並行の組の中では走らせられない。並行の組の前後の段に差し込む。

### 5.1. 並行の組とフレームの遅れ

描画・物理・アニメーションは、同じ並行の組で同時に走る（案 B）。描画だけを並行させ、物理とアニメーションを直列で済ませてから描く形（Unreal の従来の標準）は、古い形として採らない。

- 1 フレームの流れ: 入力 → Behaviour と System（直列） → 同期の段（描画用の写しを作る） → 並行の組 → 同期の段（物理とアニメーションの結果をノードへ戻す）。
- 並行の組の中では、描画は組の前に作った写しを描き、物理とアニメーションはこのフレームの結果を計算する。
- 物理やアニメーションを通った動きは、2 フレーム遅れて画面に出る（描画は 2 フレーム遅れが標準）。Behaviour が直接動かしたものは 1 フレーム遅れで出る。
- しばらくは、同期的なアニメーションの処理（描画の前に、同じフレームのうちにアニメーションを確定させること）は提供しない。ただし Behaviour による制御は除く。

［補足］1 つの写しの中の値は、どれも同じ時点に確定したものなので、組み合わせがずれて見えることはない。遅れが増えるだけである。アニメーションが物理の結果を使う場合（足の IK、ラグドール）は、1 つ前のフレームの物理を見る。

［提案］並行の組は、スイッチ 1 つで直列にも走らせられるようにする（デバッグと再現性の確認のため）。直列にしても写しの受け渡しは同じにして、遅れのフレーム数を変えない。そうすれば、直列と並行で結果が変わらない。

### 5.2. スレッドとジョブ

Box3D には、C++ の小さなスレッドプールを渡す（Box3D はホストが渡すタスクのコールバックで内部を並列化するので）。Box3D の処理には std.task を使わない。

［提案］L^ の `std.task`（ワーカースレッドの上で、命令の切れ目でタスクを替えるジョブの仕組み）は、次に使う。

- ゲーム側の非同期の読み込み: レベルや LTON の読み込みと解析、手続き的な生成などのデータの準備。テクスチャーやメッシュの GPU への転送は、OGRE-Next が自前のスレッドで行う。
- アニメーションの並列化: 段の進行役を L^ のジョブにし、重い計算は小分けにした C++ のホスト関数にする。

［補足］std.task の切り替えは VM の命令の境目でだけ起きる。ホスト関数（C++）の呼び出しは途中で切れないので、長い C++ の処理は小分けにする。タスクに渡る値は写しで、ホストの値（`throne.Vec3`、`throne.SceneNode`）は渡せない。ホスト関数の登録は全マシンで共有されるので、ジョブから呼ぶ throne のホスト関数はスレッド安全でなければならない。

［補足］C++ のプールと std.task のプールのスレッドの数は、合わせてコア数に収まるように決める（取り合いを避けるため）。

［提案］スクリプトから差し込むには、`throne` に Runner を名前で引く API と、Behaviour の段を変える API を足す（具体形は ECS-03）。

## 6. System

［提案］L^ スクリプトでも System を定義できる。System は Runner の段に置き、照会に合うノードを処理する。

```lhat
for^ node in throne.query("Transform", "Spin") { ... }
```

［補足］スクリプトで Component の型を定義する方法が要る。今の Behaviour の `self^{ ... }` の欄の定義に近い形が使える（ECS-04）。

## 7. 段階

［提案］次の順で進める。

1. Component への整理: ノードの欄を `Transform` と `BoxPrimitive` に分け、形式・上書き・Inspector を Component の単位にする。（実装済み）
2. ノードの Behaviour: ノードに Behaviour を付け、ノードを対象に走らせる。Runner は段として、ユーザーの Runner（`Update`）から始める。
3. エンジンの Runner: FetchInput・ApplyPhysics・描画などを段として並べる。並行の組を入れる。
4. System と Component の型をスクリプトで定義する。

## 8. 未決事項

- **ECS-02**: エンジンの Runner の並びを、ユーザーが並び替えられるか（固定か）。
- **ECS-03**: スクリプトから Runner を引き、Behaviour の段を変える API の形。
- **ECS-04**: スクリプトで Component の型を定義する方法と、System の照会の API の形。
- **ECS-05**: Runner の並びは、レベルごとに持つか、プロジェクトに 1 本持つか。
- **ECS-06**: 直列への切り替えで、std.task のタスクを 1 つずつ最後まで走らせる方法（std.task に足すか、throne がタスクにせず順に呼ぶか）。
- **ECS-07**: アニメーションを L^ のジョブで回したときの、ループと小分けの呼び出しのコストが、アニメーションの量に見合うか（試作で測る）。

## 改定履歴（要約）

- 初版。
- ECS-01（並行の組の同期と遅れ）を決定: 案 B を採り、描画は 2 フレーム遅れを標準とした（5.1）。Box3D には C++ のスレッドプールを渡すことにした（5.2）。
