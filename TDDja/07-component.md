# コンポーネントと実行の段

## 1. 文書の位置づけ

シーンノードを Entity とし、そのデータを Component に分けて整理する。あわせて、Behaviour の置き場所と、System による実行順の制御を定める。

System とシーンの関係は 05 による。エンジンの処理と BehaviourSystem を 1 本の実行順に並べる。

## 2. 確定事項

- throne は、ECS とオブジェクト指向のハイブリッドを目指す。Flecs のような純粋な ECS は標榜しない。
  - データを Component に分け、照会（query）で処理する System を書ける（ECS の側）。
  - 特定のノードを指して処理する Behaviour も持てる（オブジェクト指向の側）。
- `scripts/spin.lh` は、これまでどおり Behaviour とする（System にはしない）。
- Behaviour は、System の中ではなく、ノードに Component のように付ける。
- BehaviourSystem は、対応するメソッドを持つ Behaviour を問い合わせて実行する。Behaviour 側から System へ登録する形式は採らない。メソッドは通常の関数・手続きでも、中断可能なものでもよい。
- Systems パネルには、エンジンの処理も名前で並ぶ。System であることを示す角括弧の注記は付けない。例えば次のとおり。

  ```text
  FetchInput
  UpdateLoop
      UpdateBehaviourSystem
      Animation
      Box3DStep
  OGRE
  ```

  - OGRE（描画）は、SyncSimulation で写したシーンをもとに、メインスレッドで描画を発行する。
  - Box3DStep は、UpdateLoop の 1 回ごとに物理を固定 1 刻み進める。
  - Animation は、UpdateLoop の 1 回ごとにアニメーションを適用する。
- `UpdateBehaviourSystem` は `update`、`LateUpdateBehaviourSystem` は `lateupdate`、`EndOfFrameBehaviourSystem` は `onendofframe` を問い合わせる。同じ Behaviour インスタンスが複数の System に参加する。

［補足］Unreal の Tick Group と Tick の依存関係は、最終的な実行順が見えないという欠点を持つ。throne の System は、並べた一覧がそのまま実行順になることで、これを避ける（Unity DOTS・Bevy・Flecs も、前後の制約から順番を計算する点では同じ欠点を持つ）。

## 3. Component

シーンノードは、これまでどおり Node と呼ぶ（ID・名前・親子関係と Component の記述を持つので、ECS でいう「ただの ID」である Entity とは呼ばない）。親子関係（`parent`）は Node が持ち、Transform には持たせない。それ以外のデータは Component として持つ。今の欄は次のように分ける。

| 今の欄 | Component |
| --- | --- |
| `position`・`rotation`・`scale` | `Transform` |
| `colour` と「単位立方体である」こと | `BoxPrimitive`（`colour`） |

［提案］箱以外の形は、Component を足して表す（`SpherePrimitive`、モデルを読み込む `Mesh` など）。`Light`・`Camera` なども Component とする。

Component はそれぞれ省略できる。Transform を持たない Node は、親の配置をそのまま子へ渡す。描画用の Primitive を持たない Node は何も描かない。Box 以外の形状は 08 による。

スクリプトからは Component を引いてから扱う。Node から位置などを直接取る API は持たない。

```lhat
let^ t = node.transform()     // throne.Transform|nil^
if^ t != nil^ { t.setPosition(t.position() + throne.vec3(1, 0, 0)) }
node.box()                     // throne.BoxPrimitive|nil^（colour() / setColour()）
```

［補足］名前の文字列で引く形（`getComponent("Transform")`）にすると、L^ の検査では戻り値の型が名前ごとに変わることを表せず、`any^` になる。そのため型の付いた引き方にした。ユーザー定義 Component の拡張方式は C/C++ を前提に今後設計する（6 節）。

［補足］`prefab`・`overrides` は編集のための構造で、Component ではない（Unity でもプレハブの仕組みは Component ではない）。

ノードの形式は次のとおりとする（後方互換は持たない。シーンは version 2、プレハブは version 3）。Behaviour は `components.behaviour` に保存する。

```lton
{
    id = 7484712563498500096,
    name = "SpinningCube",
    parent = 0,
    components = {
        transform = { position = {0, 0.5, 0}, rotation = {0, 0, 0}, scale = {1, 1, 1} },
        box = { colour = {0.32, 0.62, 0.82} },
        behaviour = {
            { script = "scripts/spin.lh", data = { degreesPerSecond = 45 } },
            { script = "scripts/pop.lh", data = { interval = 1.5, height = 0.5, duration = 0.4 } },
        },
    },
},
```

上書き（06 の 4 節）は、Component ごとの表に欄を持つ（`transform = { position = ... }`、`box = { colour = ... }`）。上書きした欄は、その Node がその Component を持つときだけ効く。

［補足］Behaviour 以外の Component を、インスタンスの中の Node に足したり消したりする上書きは、まだない。Behaviour の一覧はインスタンスごとに置き換え・追加・削除できる。その他の Component の追加・削除は、編集中のファイルが持つ普通の Node で行う。

Camera コンポーネントは視点を持つ。位置と向きはノードの Transform（ノードのローカル −Z を向き、+Y が上。OGRE のカメラと同じ）で決め、コンポーネントは画角（`fieldOfView`、度）とニア／ファークリップ（`nearClip`・`farClip`）だけを持つ（`camera = { fieldOfView = 34, nearClip = 0.1, farClip = 1000 }`）。エディターでは、インスペクターの Align View to Camera でシーンビューのカメラをノードの位置・向き・画角に合わせ（シーンビューはロールを持たない）、Align Camera to View でシーンビューの位置・向きをノードの Transform へ（親の座標へ直し、スケールは保つ）、画角をコンポーネントへ書く（Undo 1 回分）。クリップ距離・DoF・AA 手法は写し合わない（シーンビューは自分の値をレベルのインスペクターで持つ）。カメラを通して見る `--capture`（viewFromFirstCamera）だけは、それらも含めて全部を使う。Camera コンポーネントには DoF（focusDistance、fStop）と AA 手法（antiAliasing: TAA／SMAA／None、既定 TAA）もある（09）。AA はカメラ（ビュー）の性質とした: TAA の履歴・ジッタ・再投影はビュー単位で、カメラごとに使い分けたい。将来の単独実行では、画質設定（プロジェクト／ユーザー）を上限とする 2 層にする。レベルは視点を持たない（以前の openingCamera・target・yaw・pitch・distance・fieldOfView は、Bistro の参照画像の視点を再現するために足したもので、各レベル専用のシーン `scenes/<level>-view.scene.lton` の Camera ノードへ移した）。シーンビューのカメラの設定はレベルのインスペクター（Level パネルの先頭のレベルの行）で直接編集でき、保存しない。`--capture` はレベルの最初の Camera ノード（シーンインスタンス順、階層順）の視点で撮る。スクリプトからの Camera コンポーネントの取得は、まだない。

［補足］ECS の性能面（アーキタイプによる詰めた配置など）は、今は採らない。概念として ECS の整理を採り、保存と展開はノードごとのままとする。

## 4. ノードの Behaviour

ノードの `components.behaviour` は Behaviour コンポーネントであり、その一覧にスクリプト参照と `data` をいくつでも持てる。同じスクリプトを複数付けても別のインスタンスになる。Behaviour はオブジェクト指向側の仕組みであり、データと処理を分離せず、1 つのスクリプトインスタンスに持つ。各付加は、スクリプト、インスタンスの `data`、有効状態、共通およびフェーズ別の実数 priority の上書きを持つ。所属 System の指定は持たない。

ノードに付いた Behaviour の対象は、そのノード自身とする。`start`・`update`・`lateupdate`・`onendofframe` には、そのノードが渡る。

```lhat
start = p^self^, node:throne.SceneNode { ... },
update = p^self^, node:throne.SceneNode, dt:number^ { ... },
```

［補足］これで、ノードを複製すると Behaviour も付いてくる。今のように、複製に合わせて System を複製する必要はなくなる。Inspector では、ノードの Component と Behaviour が並んで見える。

プレハブのノードに付いた Behaviour は、インスタンスごとに 1 つずつ生まれる（今のプレハブの System と同じ）。インスタンスは、Behaviour の欄の値を上書きでき、Behaviour を足したり消したりできる。

今の、System が直接持つ Behaviour（`target` の欄で特定のノードを指すもの）は残す。演出や進行役のように、特定のノードを外から動かす処理に使う（2 節のオブジェクト指向の側）。

## 5. 実行の段

System は、実行の段の並びになる。レベル全体で 1 本の並びを持ち、上から順に実行する。

- エンジンの System: FetchInput、Animation、Box3DStep（シミュレーションの列）、PreRenderEventSystem、OGRE、PostRenderEventSystem（描画の列）、SyncSimulation、SyncRender（同期）。消去・並べ替えはできない。
- BehaviourSystem: UpdateBehaviourSystem、LateUpdateBehaviourSystem、EndOfFrameBehaviourSystem。対応するメソッドを持つ有効な Behaviour を問い合わせる。
- ユーザーの System: 名前を付けてシミュレーションの列の FetchInput より後に追加できる。直接付けた Behaviour を実行する。EndOfFrameBehaviourSystem より前なら UpdateLoop の中で走る。
- 更新は固定刻み（1/60 秒）だけとする（Unity でいう FixedUpdate のみ）。UpdateLoop は経過時間の刻み数だけ回り（1 フレームで取り戻すのは最大 0.25 秒）、その中の段の `dt` は 1/60 秒、EndOfFrameBehaviourSystem 以降の段の `dt` はそのフレームで進んだ時間とする。
- レベルの `serialExecution` で 2 通りのフレームを選ぶ。

  ```text
  遅延あり（serialExecution = false^、既定）
  SyncSimulation
  Parallel
      Rendering（メインスレッド）
          PreRenderEventSystem
          OGRE
          PostRenderEventSystem
      Simulation（ワーカースレッド）
          FetchInput
          UpdateLoop
              UpdateBehaviourSystem
              LateUpdateBehaviourSystem
              Animation
              Box3DStep
          EndOfFrameBehaviourSystem
  SyncRender

  遅延なし（serialExecution = true^）
  FetchInput
  UpdateLoop
      UpdateBehaviourSystem
      LateUpdateBehaviourSystem
      Animation
      Box3DStep
  EndOfFrameBehaviourSystem
  SyncSimulation
  PreRenderEventSystem
  OGRE
  PostRenderEventSystem
  SyncRender
  ```

  - SyncSimulation はシミュレーションのシーンを描画用の写しへコピーする。SyncRender は描画とシミュレーションの合流点で、フレームの終わり。
  - 遅延ありでは、描画が前フレームの写しを描く間に、シミュレーションが今のフレームを計算する。スクリプトと物理の変更は 1 フレーム後に描かれる。遅延なしでは同じフレームで描かれ、並行には走らない。
  - L^ の VM は一度に 1 スレッドだけで動く。シミュレーションの列のスレッドか、フレームの間（SyncRender から次の SyncSimulation まで、エディター UI とコンソール）のメインスレッド。
  - PreRenderEventSystem／PostRenderEventSystem は、描画スレッドで RenderEvent Component（Behaviour とは別の Component）の `prerender`／`postrender` を実行する。遅延ありではスクリプトがシミュレーションのスレッドの L^ マシンに属するため、描画スレッドでは直接実行できない。そこで別の Component とし、別の L^ マシンで描画用の写しに対して動かす。

［補足］RenderEvent は `components.renderEvent` に Behaviour と同じ形の一覧で保存し、スクリプトは `RenderEvent = def^{ ... }` を公開する。描画用の写しは読み取り専用で、setter と `spawnBox` は panic する。`dt` はそのフレームの経過時間。priority の上書きは持たない。Pause 中は実行しない。Behaviour と状態は共有しない。

BehaviourSystem の `order_by` は、そのコンテキストで解決済みの有限実数 priority の昇順とする。同値はシーンインスタンス順、ノード階層順、付加順で決める。

スクリプトの `self^{ ... }` に `priority`、`updatePriority`、`lateupdatePriority`、`onendofframePriority` を宣言して型の既定値とする。付加ごとにも共通・フェーズ別の上書きを持つ。優先順位は「付加のフェーズ別 > 付加の共通 > 型のフェーズ別 > 型の共通 > 0」。Inspector で上書きを編集し、Systems で最終値と対象の並びを見る。

［補足］全 Behaviour のレジストリと、問い合わせに合うインスタンスの一覧を保つ。追加・削除・有効状態・priority の変更は次フレーム先頭で反映する。順位が変わる変更時だけソートし、コルーチンの待ち先の移動はソート済み一覧からの抽出で処理する。付加には永続 ID を持たせ、他の付加の削除でインデックスが変わっても状態を保持する。

### 5.0.1. 中断可能なメソッド

メソッドを普通に呼び、結果がコルーチンの場合だけ保持して直ちに駆動する。通常の関数・手続きの戻り値は使用しない。各メソッドは独立し、中断中の `update` は `lateupdate`・`onendofframe` を止めない。同じメソッドの呼び出しは重複させない。

`yield^ throne.system("EndOfFrameBehaviourSystem")` のように System を待ち先にする。同フレーム内でまだ通っていない段なら同フレーム、同じ段または通過済みの段なら次フレームで再開する。`yield^ nil^` は起点の System の次の訪問を待つ。`throne.currentSystem()` は再開先を含む実行中の System を返す。

［補足］`start` は最初のコールバック直前に一度だけ呼ぶ。`start` だけを持つ付加は Update で開始する。有効状態をオフにすると中断状態を保持したまま停止し、再有効化で続行する。付加の削除と Play の停止ではコルーチンを dispose し、`finally^` を実行する。メソッドの完了フレーム内に同じメソッドを新規に呼び直すことはない。

### 5.1. 並行の組とフレームの遅れ

描画・物理・アニメーションは、同じ並行の組で同時に走る（案 B）。描画だけを並行させ、物理とアニメーションを直列で済ませてから描く形（Unreal の従来の標準）は、古い形として採らない。

- 1 フレームの流れ: 入力 → Behaviour と System（直列） → 同期の段（描画用の写しを作る） → 並行の組 → 同期の段（物理とアニメーションの結果をノードへ戻す）。
- 並行の組の中では、描画は組の前に作った写しを描き、物理とアニメーションはこのフレームの結果を計算する。
- 物理やアニメーションを通った動きは、2 フレーム遅れて画面に出る（描画は 2 フレーム遅れが標準）。Behaviour が直接動かしたものは 1 フレーム遅れで出る。
- しばらくは、同期的なアニメーションの処理（描画の前に、同じフレームのうちにアニメーションを確定させること）は提供しない。ただし Behaviour による制御は除く。

［補足］1 つの写しの中の値は、どれも同じ時点に確定したものなので、組み合わせがずれて見えることはない。遅れが増えるだけである。アニメーションが物理の結果を使う場合（足の IK、ラグドール）は、1 つ前のフレームの物理を見る。

並行の組は、スイッチ 1 つで直列にも走らせられるようにする（デバッグと再現性の確認のため）。直列にしても写しの受け渡しは同じにして、遅れのフレーム数を変えない。そうすれば、直列と並行で結果が変わらない。

### 5.2. スレッドとジョブ

Box3D には、C++ の小さなスレッドプールを渡す（Box3D はホストが渡すタスクのコールバックで内部を並列化するので）。Box3D の処理には std.task を使わない。

［提案］L^ の `std.task`（ワーカースレッドの上で、命令の切れ目でタスクを替えるジョブの仕組み）は、次に使う。

- ゲーム側の非同期の読み込み: レベルや LTON の読み込みと解析、手続き的な生成などのデータの準備。テクスチャーやメッシュの GPU への転送は、OGRE-Next が自前のスレッドで行う。
- アニメーションの並列化: 段の進行役を L^ のジョブにし、重い計算は小分けにした C++ のホスト関数にする。

［補足］std.task の切り替えは VM の命令の境目でだけ起きる。ホスト関数（C++）の呼び出しは途中で切れないので、長い C++ の処理は小分けにする。タスクに渡る値は写しで、ホストの値（`throne.Vec3`、`throne.SceneNode`）は渡せない。ホスト関数の登録は全マシンで共有されるので、ジョブから呼ぶ throne のホスト関数はスレッド安全でなければならない。

［補足］C++ のプールと std.task のプールのスレッドの数は、合わせてコア数に収まるように決める（取り合いを避けるため）。

［補足］`throne.system(name)` はスクリプト System（ユーザー段か BehaviourSystem）のハンドル（`throne.System|nil^`）を返す。ネイティブのエンジン段は待ち先にできない。待ち先が今の段より後なら同じ UpdateLoop の回で、同じか前なら次の回で再開する（EndOfFrameBehaviourSystem 以降は次のフレーム）。`node.setBehaviourPriority(index, numberOrNil)`、`setBehaviourPhasePriority(index, system, numberOrNil)`、`setBehaviourEnabled(index, bool)`、`addBehaviour(script)`、`removeBehaviour(index)` は次フレームのレジストリを変更する。index は 0 から始め、priority に nil を渡すと継承に戻る。

### 5.3. 現在のエンジンとの結線

［補足］現在の実装では、レベルの `stages` にシミュレーションの列の段名を順に保存する。既定は FetchInput → UpdateBehaviourSystem → LateUpdateBehaviourSystem → Animation → Box3DStep → EndOfFrameBehaviourSystem。描画の列と同期段は固定で、保存しない。段を持たない直接の System は、その名前のユーザー段を UpdateBehaviourSystem の前に作る。同じ名前の System は同じ段で走る。

入力は UI の構築前に取得し、Behaviour と System はユーザー段で実行する。遅延ありでは、シミュレーションの列を専用のワーカー 1 本で走らせ、描画の列はメインスレッドで走らせる。SyncRender では両方の終了を待つ。処理に失敗しても、ワーカーを回収してからエラーを返す。

［補足］物理ボディとシーンの Component の結線は 08 で実装した。Box3DStep はノードの変更を入力として取り込み、1 刻み進めて、結果をノードへ書き戻す。内部タスクには待機中も子タスクを実行できる C++ プールを渡す。アニメーションクリップ／スケルトンの Component は未実装で、Animation の段にはまだ処理対象がない（ECS-07）。

## 6. System

System の段に Behaviour を直接付け、L^ スクリプトから照会に合うノードを処理できる。スクリプトによる System 型の定義は提供しない。

```lhat
for^ _^, node in^ throne.query("Transform", "BoxPrimitive") { ... }
```

ユーザー定義の Component と System は C/C++ で記述する方針とし、登録・拡張の仕組みは今後設計する。スクリプトでデータ専用 Component を定義する Script Component は提供しない。ノードのスクリプトは Behaviour としてデータと処理をまとめて持つ。

［補足］System に直接付けるスクリプトは `Behaviour` を公開する。`start`・`update` の引数には `throne.System` を渡す。編集用の欄は `@export` を使う。

`query` は指定したすべての組み込み Component を持つノードの写しの一覧を返す。引数なしなら全ノード、未知の名前なら空の一覧。順番はシーンインスタンス順、その中の階層順。組み込み Component は型付きの `transform()`・`box()` などで取得する。

`node.component(throne.Transform)` のように型を指定して、組み込み Component にアクセスできる。シグネチャは `f^self^, type^ -> ARG0.T0|nil^;`。`self^` を除いた最初の引数の型パラメーターを戻り値に使い、この例では `throne.Transform|nil^` となる。未付加なら `nil^` を返す。Component 以外の型は実行時エラーとする。既存の `node.transform()` なども残す。

ノードの Behaviour の上書きは `overrides.nodes[].behaviour` に一覧を保存する。空の一覧は全削除、省略は継承を意味する。

## 7. 段階

次の順で実装した。

1. Component への整理: ノードの欄を `Transform` と `BoxPrimitive` に分け、形式・上書き・Inspector を Component の単位にする。（実装済み）
2. ノードの Behaviour: ノードの保存・実行、プレハブの上書き、Inspector、priority・有効状態・付加の変更 API。（実装済み）
3. エンジンの System: レベルの段の保存、Systems 表示・並べ替え、並行の組、直列切替、描画の写し、Box3D のタスクプール。（実装済み）
4. System に付ける Behaviour の型の検査・生成・欄の保存、組み込み Component の照会を実装。ユーザー定義 Component／System の C/C++ 拡張方式は今後設計する。

## 8. 未決事項

- **ECS-07**: アニメーションを L^ のジョブで回したときの、ループと小分けの呼び出しのコストが、アニメーションの量に見合うか（試作で測る）。

## 改定履歴（要約）

- 初版。
- ECS-01（並行の組の同期と遅れ）を決定: 案 B を採り、描画は 2 フレーム遅れを標準とした（5.1）。Box3D には C++ のスレッドプールを渡すことにした（5.2）。

- 段階 2〜4 を実装。ECS-02 はエンジン順固定、ECS-03 は System 取得、ECS-04 は Component/System の def^ と名前による照会、ECS-05 はレベル単位、ECS-06 はホスト側の順次呼び出しで具体化した。

- System は Behaviour のメソッドを問い合わせて実行する形に整理し、フェーズ別 priority と独立したコルーチンの再開を導入した。

- Behaviour を `components.behaviour` に統合し、各スクリプトの保存データを `data` に統一。旧ノード直下の behaviours は廃止し、basic の Spin Component／Spin System を Behaviour に置き換えた。

- Script Component を廃止。ユーザー定義 Component／System は C/C++ を前提に今後設計する。

- スクリプトによる System 型定義を廃止。`node.component(type)` は型付きアクセスとして提供し、未付加なら `nil^` を返す。
