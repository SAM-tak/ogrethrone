# L^ スクリプトの結線

## 1. 文書の位置づけ

L^ をゲームエンジンのスクリプトとして結線する設計を定める。
対象は、エディターの Play モードでスクリプトを実行する最初の段階とする。
物理との連携、実行中のホットリロード、ゲーム単体の実行形式は対象外とする。

［補足］L^ の API の参照点は、`third_party/lhat` の `a4d452a`。

## 2. 確定事項

- スクリプトは、ゲーム全体のスクリプト（`main.lh`）とオブジェクトごとのスクリプトの両方を持つ。
- スクリプトはエディターの Play モードでのみ実行する。編集中は実行しない。
- スクリプト側の 3D ベクトルは、L^ 標準の `std.math.vector3.Vector3` ではなく、エンジン独自の host value 型とする。
- シーンデータは JSON から LTON へ移行する。書き出しは L^ の C API `lhatstdlib_lton_stringify` を使う（7 節）。
- オブジェクトに付けるスクリプトは `module^` に限り、公開するものに一定の形式を求める。オブジェクトごとの状態は、スクリプトの定義（`def^`）からオブジェクトごとに作るインスタンスが持つ（4.2）。
- `throne.Vec3` の座標系は OGRE に合わせる。右手系・Y 上・-Z 前方とする（5.1）。
- 削除済みのオブジェクトを指すハンドルは事前に検査できるようにし、検査せずに使った場合は `panic^` とする（5.2）。

## 3. Play モード

［提案］Play を押すと、編集中のドキュメントを複製し、その複製の上でスクリプトを実行する。
Stop で複製を捨て、編集中のドキュメントと表示を Play 前の状態に戻す。
Play 中はインスペクターなどからの編集を受け付けない。undo の履歴にも積まない。

［提案］Play ごとに `LhatProgram` と `LhatMachine` を新しく作り、Stop で破棄する。
これにより、スクリプトを編集して Play し直すだけで変更が反映される。実行中のホットリロードは後の段階で扱う。

［提案］Play 開始時に、`main.lh` と各オブジェクトのスクリプトをすべて検査・コンパイルする。
診断が 1 つでもあれば Play に入らず、診断をログパネルに出す。

［提案］1 フレームの実行順は次のとおり。

1. `main.lh` の `update`
2. 各オブジェクトのスクリプトの `update`（階層パネルの順）

各スクリプトの `start` は、Play に入った最初のフレームで、`update` より前に同じ順で 1 回呼ぶ。

［提案］実行時エラー（`panic^`、ゼロ除算、型エラーなど）が起きたら、トレースバックをログに出して Play を止める。
1 フレームの実行には `lhat_machine_set_budget` で上限を設ける。上限に達して `LHAT_RUN_SUSPENDED` が返ったら、無限ループとみなして同じく Play を止める。
［補足］中断したフレームを次のフレームで `lhat_machine_continue` により再開する方式も可能だが、その場合はフレームの途中状態を扱う必要があるため採らない。

## 4. スクリプトの形

### 4.1. `main.lh`

［提案］プロジェクトの `scripts/main.lh` をゲーム全体のスクリプトとする。存在しなければ実行しない。
次の関数を公開してよい。どちらも省略できる。

```lhat
public^let^start = p^{ ... }
public^let^update = p^dt:number^{ ... }
```

### 4.2. オブジェクトのスクリプト

［提案］`SceneObject` に、スクリプトのモジュールパス（例: `scripts/spin.lh`）を 1 つ持たせる。空ならスクリプトなし。

スクリプトは `module^` の単位でなければならない。
［提案］公開する形式は、`Behaviour` という名前の定義 1 つとする。
インスタンスごとの欄は `self^{ … }` に書く。メンバーの `start`・`update` はどちらも省略できる。

```lhat
module^

public^let^Behaviour = def^{
    self^{
        speed = 90,
    },
    start = p^self^, obj:throne.Object { ... },
    update = p^self^, obj:throne.Object, dt:number^ {
        var^ r = obj.rotation()
        r.y := r.y + self^.speed * dt
        obj.setRotation(r)
    },
}
```

［提案］Play 開始時、スクリプトを付けたオブジェクトごとに `Behaviour.new()` でインスタンスを 1 つ作る。
同じスクリプトを複数のオブジェクトに付けた場合、モジュールと定義は共有し、インスタンスはオブジェクトごとに別になる。

［提案］Play 開始時の検査で、次のいずれかに当たるスクリプトは診断として扱い、Play に入らない。

- `module^` でない
- `Behaviour` を公開していない、または `def^` の定義でない
- `start`・`update` が上の署名と合わない

## 5. ホスト API（`throne` モジュール）

### 5.1. `throne.Vec3`

［提案］`lhat_register_hostvalue_type` で登録する 3 要素の値型（`f32`）。

- フィールド `x`・`y`・`z` は `lhat_register_hostvalue_field` で直接読み書きする。
- 演算子は `+`、`-`（二項・単項）、`*`・`/`（スカラー）とする。
- メンバーは `dot`、`cross`、`length`、`normalized` とする。
- 生成は `throne.vec3(x, y, z)` とする。

座標系は OGRE と同じく右手系・Y 上・-Z 前方とする。
［補足］OGRE は `SceneNode::lookAt` の既定のローカル方向が `NEGATIVE_UNIT_Z` であり、カメラも -Z を向く。
Box3D も右手系・既定で Y 上（重力の既定値は `(0, -10, 0)`）であり、前方の概念は持たない。
Steam Audio も右手系・Y 上・-Z 前方である。したがって、3 つのライブラリとの間で軸の変換は要らず、成分をそのまま写せばよい。
`std.math.vector3` は +Z 前方なので、両者を混ぜると向きが反転する。

［補足］host value 型は、テーブル・クロージャのキャプチャ・`any^` のようにフレームより長く生きる場所に置けない（L^ 05 の 8.9）。
保持するには、言語が提供する `throne.Vec3.Box^` を使う。

［補足］C++ 側の `throne::Vec3` は `float` 3 つの標準レイアウトなので、host value のバイト列としてそのまま扱える。

### 5.2. `throne.Object`

［提案］シーンオブジェクトへのハンドルを `lhat_register_hostdata_type` で登録する。中身はオブジェクト ID だけを持つ。
L^ 側に所有権を渡さないため、`dispose` は登録しない。

- 読み取り: `id`、`name`
- 読み書き: `position`、`rotation`（度。X→Y→Z の順に適用）、`scale`。いずれも `throne.Vec3` の取得・設定メソッドとする。
- 読み書き: `colour`（`throne.Vec3` を RGB として扱う）

ハンドルがまだ有効かどうかは `valid()` で検査できる。
無効なハンドルに対して `valid()` 以外のメンバーを使った場合は `panic^` とし、Play を止める（3 節）。
［補足］この段階では Play 中に編集できず、オブジェクトを削除する API も無いため、無効なハンドルは生じない。
削除・生成の API を加える段階で、この規則が実際に働き始める。

### 5.3. 関数

［提案］

- `throne.find(name:string^) -> throne.Object|nil^`: 名前が一致する最初のオブジェクト
- `throne.log(message:string^)`: エディターのログパネルへの出力
- `throne.time() -> number^`: Play 開始からの経過秒数

## 6. 読み込み

［提案］ローダーはプロジェクトのディレクトリを根とし、ファイルシステムから直接読む。
モジュール名と相対パスの対応は L^ 05 の規則に従う。

［提案］L^ の標準ライブラリはビルドに含める（`LHAT_BUILD_STDLIB=ON`）。
スクリプトに公開する標準モジュールは、効果を持たない `std.math` だけから始める。
`std.io` や `std.net` などは、必要になった時点で個別に判断する。

## 7. シーンの LTON 化

［補足］読み込みには既存の `lhatstdlib_lton_parse` を使える。
LTON は `f^` の本体として評価されるため、`p^` の呼び出しを含められず、安全に読める（L^ 08 の 4）。

書き出しには `lhatstdlib_lton_stringify` を使う。program も machine も要らないが、
書き出すテーブルは `lhat_machine_make_table` などで machine 上に組み立てる。

［補足］シーンファイルは `*.throne.lton` とし、`format`・`version`・`objects` を持つテーブルとする。LTON 化は実装済みである。
`SceneObject` に追加するスクリプトのパス（4.2）は、スクリプトの結線と同時にシーン形式へ加える。

## 改定履歴（要約）

- 初版。
