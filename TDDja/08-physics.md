# Primitive と Box3D の物理シミュレーション

## 1. 目的

Box・Sphere・Capsule・Plane を表示し、Collider と RigidBody を付けたシーンを Play で Box3D によって動かす。Stop は実行用の写しと物理世界を捨て、編集時の状態に戻す。

## 2. Component

描画は `BoxPrimitive`・`SpherePrimitive`・`CapsulePrimitive`・`PlanePrimitive` から一つを選ぶ。Transform は描画と Collider に共通する。描画のない物理ノードも作れる。

［補足］球の既定の半径は 0.5、カプセルの半径は 0.5・高さは 2 とする。カプセルは Y 軸方向で、高さには上下の半球も含む。Plane は XZ 面の有限の長方形で、既定は幅・奥行きとも 10。描画用の面は両面表示する。

衝突形状は `BoxCollider`・`SphereCollider`・`CapsuleCollider`・`PlaneCollider` とする。同じノードに複数を付けると、一つのボディに複数の形状を持たせる。描画形状と衝突形状は独立して編集できる。

［補足］Box3D には無限平面のボディ形状がないため、PlaneCollider は上向きの三角形 2 枚による有限メッシュとする。Static または Kinematic でのみ使用でき、Dynamic は Play 開始前にエラーにする。面の外へ出れば落下し、裏からの接触は受けない。

［補足］Collider はローカルの `centre`、`density`（kg/m³）、`friction`、`restitution`、`sensor` を持つ。質量は Box3D が形状と密度から計算する。Unity の Rigidbody.mass に相当する別の質量欄は設けない。センサーは重なりを検出する形状で、衝突応答を生じない。イベントのスクリプト API はまだない。

`RigidBody` は Box3D の Static／Kinematic／Dynamic を選び、初期の線速度・角速度、線形・角の減衰、重力倍率、Bullet（CCD）を持つ。角速度の単位はエディターとスクリプトでは度/秒、Box3D への受け渡しではラジアン/秒とする。

［補足］Collider だけのノードは Static とする。RigidBody を付けたノードは Transform を必須とする。同じノード上の Collider のみがその RigidBody に所属し、子の Collider を親の RigidBody へ自動でまとめる機能は持たない。

## 3. 配置とスケール

シーンインスタンスの配置とノード階層を合成して Box3D のワールド位置・回転へ渡し、結果を親の座標系へ戻して Transform を更新する。Transform のない中間ノードは親の配置をそのまま渡す。

［補足］Box は軸ごとのスケールを使う。SphereCollider は全軸で最大のスケールを半径へ使う。CapsuleCollider は X/Z の最大値を半径へ、Y を全高へ使い、全高が直径より短くなる場合は球として扱う。球やカプセルの非一様スケールは、描画と衝突形状が完全には一致しない。PlaneCollider は頂点にスケールを焼き込み、表示範囲と一致させる。

## 4. 実行の段

- Play 開始時: 展開済みシーンからボディ・形状を作る。編集用シーンには触れない。
- ApplyPhysics: 前フレームに完了したボディの位置・回転・速度をノードへ戻す。
- ユーザーの段: Behaviour／System がノードや RigidBody の速度を変更する。
- SyncRender: 次フレームの描画用の写しを作り、物理へ渡す位置・回転・速度・設定もコピーする。
- 並行の組: OGRE は前の描画用の写しを描く。Box3D はノードへ触れず、独立した物理世界を固定 1/60 秒・4 サブステップで進める。
- SyncSimulation: 全ワーカーの終了を待ち、完成した物理結果は次の ApplyPhysics まで保持する。

［補足］これにより、直接の Behaviour は 1 フレーム後、物理の結果は 2 フレーム後に描かれる。直列切替でも同じ写し・同期位置を使う。物理の開始・停止は Play と連動し、編集モードでは計算しない。長いフレームの取り戻しは最大 0.25 秒とする。

## 5. 保存・編集

ノードの `components` に `sphere`・`capsule`・`plane`、`boxCollider`・`sphereCollider`・`capsuleCollider`・`planeCollider`・`rigidBody` の表を保存する。既存の scene version 2／prefab version 3 の Component 形式を使う。

Inspector で追加・削除・各欄の変更ができる。プレハブの内部では、元からある Component の値だけを上書きでき、Revert で戻せる。追加・削除は編集中のファイル自身の通常ノードだけで行える。

［補足］Edit → Add Primitive で作ったノードには、描画形状と対応する Collider を付ける。動かすには RigidBody を追加する。PlaneCollider のあるノードへ RigidBody を追加したときの既定は Static とする。

## 6. スクリプト

`node.sphere()`・`capsule()`・`plane()`、`boxCollider()`・`sphereCollider()`・`capsuleCollider()`・`planeCollider()`・`rigidBody()` は型の付いた Component、または nil^ を返す。描画 Primitive は colour()/setColour() と寸法の取得、Collider は centre() と寸法の取得を持つ。

RigidBody は type()、linearVelocity()/setLinearVelocity()、angularVelocity()/setAngularVelocity() を持つ。速度の変更は次の物理計算に使われる。これらの組み込み Component の名前でも query できる。

```lhat
import^ throne
for^ _^, node in^ throne.query("SphereCollider", "RigidBody") {
    let^ body = node.rigidBody()
    if^ body != nil^ { body.setLinearVelocity(throne.vec3(0, 5, 0)) }
}
```

## 7. 動作確認

Play／Stop は F5。Play 中の Pause キーまたはメニューバーの Pause／Resume で一時停止／再開を切り替える。Pause 中は ScriptRuntime の update と Box3D の apply／prepare／advance、および描画用の写しの更新を呼ばず、保持した写しの描画とエディター操作のみを継続する。前フレームの物理タスクは既に結合済みであり、Pause 中にバックグラウンドで進めない。再開時に停止中の経過時間を追いつき計算せず、同じセッションを続行する。Pause 中の Stop も初期状態へ戻し、新たな Play は停止状態を引き継がない。

［補足］Pause 操作は Play 外では無効とし、Pause キーはカメラの右ドラッグ中も受け付ける。Pause 中も編集は Play と同様に無効にし、カメラ操作・描画・統計表示は継続する。

`samples/physics` の Box Spawn Point は `scripts/spawn-boxes.lh` を Update で実行し、0.1 秒ごとに Box を生成し続ける。経過時間の端数を保持し、遅いフレームでは複数回生成する。生成上限や使い回しは設けず、Stop で初期シーンに戻す。

［補足］`node.spawnBox(size)` は呼出元ノードの子に、ローカル原点・指定サイズの動的 Box を追加する。Transform、BoxPrimitive、BoxCollider、RigidBody を持ち、ハンドルを直ちに返す。サイズは有限の正の Vec3 とする。追加先は Play の展開結果のみで、元のシーンやプレハブは変更しない。生成済みノードの削除 API や Behaviour の動的追加は含まない。

［補足］既存ハンドルは添字で解決し、追加による配列再配置の後も有効。物理は SyncRender の prepare で未登録の追加ノードだけを登録し、既存ボディや積算時間を維持する。描画は完成済みの写しに追加ノードが現れた時点で OGRE ノードとメッシュを追加する。Stop 後は余分な描画資源も破棄する。

［補足］サンプルは (-2, 8, 0) から一辺 0.6 の箱を生成し、初速 (0, -8, 0) と角速度 (15, 25, 10) を与える。Inspector の interval で間隔を変更できる。

`samples/physics` をプロジェクトとして開き F5。球が跳ね、カプセルが倒れて転がり、箱が台へ落下する。F5 で停止すると初期配置へ戻る。

`--smoke-physics [project] [capture-directory]` はプロジェクトの一時コピーで Play を 240 フレーム実行し、描画の移動と Stop 後の復元を検査する。影の ON／OFF と、PSSM 境界の画素差を分けた復元の検証、出力画像の一覧は 09 による。Play 中に描画が変わらない場合、途中で Play が停止した場合、Stop 後に初期状態へ戻らない場合はエラーとする。元のプロジェクトとプロジェクト一覧は変更しない。

CPU テストは、各凸形状と有限平面の接触、直列／並行の一致、シーン配置の座標変換、型付きホスト API、保存・再読込、プレハブ上書き、描画メッシュの境界を検証する。
