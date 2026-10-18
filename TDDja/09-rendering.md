# Clustered Forward と影

## 1. 採用方針

既定の描画に OGRE-Next の Forward Clustered を使う。Primitive は HlmsPbs の物理ベースの照明を受ける。影は深度シャドウマップを使い、太陽光には PSSM（Cascaded Shadow Maps）を適用し、PCF で境界をフィルターする。

［補足］Clustered Forward はローカルライトの割当てを行い、影は独立した深度アトラスから評価する。この組合せは OGRE-Next が備える HlmsPbs と CompositorShadowNode を使って実装する。独自の照明シェーダーやスクリーン空間だけの影は追加しない。

## 2. 現在の既定設定

［補足］現実装の数値は次のとおりとする。明示指定がないレベルのプレビュー照明に用いる。レベルの rendering に環境光、既定太陽光の有無、Forward 方式・範囲、クラスタ表示、初期カメラ・空・グリッドを保存できる。

- Forward Clustered: 16×8×24 セル、セルあたり最大 96 ローカルライト、奥行き範囲 0.1〜100。
- 太陽光: Directional Light、方向 (-0.6, -1, -0.4) を正規化、power scale は π。
- 環境光: 上半球 RGB (0.22, 0.25, 0.30)、下半球 (0.06, 0.07, 0.09)、上向き軸は +Y。
- 太陽光の影: 3 段 PSSM。近い段は 2048²、残りは各 1024²。1 枚の 2048×3072 深度アトラスに配置する。
- PSSM: 最初の 2 段を安定化し、lambda 0.7、split padding 1、blend 0.125、fade 0.2、影の遠方距離は 100。
- フィルター: HlmsPbs::PCF_3x3。深度比較とバイアス処理には OGRE-Next の標準実装を使う。
- Primitive の材質: 粗さ 0.65、誘電体の Fresnel 0.04、拡散色は Component の colour。

［補足］Light Component は Directional、Point、Spot、AreaApprox、AreaLtc に対応する。影用の枠は Directional 1 個のままであり、既定太陽を無効にすれば、配置した Directional が利用する。Point／Spot の影と Area Light のソフトシャドウは未実装。Area Light は Clustered のリストではなく通常 Forward の枠から評価する。

## 3. ジオメトリとエディター表示

Primitive の各頂点は外向きの単位法線、UV、接線を持つ。箱は面ごとの法線、Sphere／Capsule は曲面法線、Plane は +Y を使い、三角形の向きを揃える。従来の頂点色に焼き込んだ明暗は使わず、PBS で照明と影を計算する。

Plane を含めて両面照明とし、Primitive は影を落とし、他の Primitive の影も受ける。グリッド・選択枠・ImGui は非照明表示を保つ。グリッドと選択枠は影を落とさない。

View → Shadows は太陽光の影だけを切り替える。拡散光と環境光は保ち、シーンファイルや物理シミュレーションは変更しない。この切替はエディターの表示状態で、保存対象ではない。

## 4. 実行との関係

［補足］ビューポートの既定のスカイボックスには OGRE-Next の AtmosphereNpr によるプロシージャルな青空と太陽を使う。View → Skybox で表示を切り替える。既定の Directional Light と太陽の方向を合わせるが、Light 自体は Atmosphere にリンクせず、既存の照明・環境光の値を保つ。fogDensity は 0 とする。LDR のビューポートに合わせ、skyPower は 0.5、horizonLimit は 0.15 とし、下向きの視線では地平線以下の落ち着いた色を使う。

［補足］空は背景深度の画面矩形として描画し、カメラに応じた視線から色を求める。シーンノード・物理ボディ・ピッキングの対象には追加しない。Quad.program と Atmosphere.material、および対応する GLSL／Any の固定済みリソースだけを読み込む。Editor 終了時は Atmosphere と専用リソースグループを破棄する。View の切替は表示状態のみであり、初期の空の有無はレベルの rendering.sky に保存する。

OGRE は従来どおりメインスレッドで、完成済みの描画用の写しを使う。影の深度描画と本描画は同じ OGRE ノードの配置を参照するので、物体と影は同じ時点を表示する。Box3D のワーカーは OGRE オブジェクトや影のテクスチャーを操作しない。

ビューポートのリサイズで Workspace を作り直しても、同じシャドウノード定義を再利用する。HlmsUnlit と HlmsPbs のテンプレートは固定済みの OGRE サブモジュールから読み込む。

## 5. 検証

［補足］View → Statistics で、ビューポート右上の簡易統計表示を切り替える。既定は表示。−／＋ボタンで通常表示と FPS だけの最小表示を切り替える。ImGui のフレーム時間の移動平均からエディター全体の FPS と ms/frame を示し、描画用の写しにあるノード数と、Play の物理ボディ数を併記する。ボタンはクリック可能とし、パネル外では通常のナビゲーションを使う。

［補足］展開時は steady_clock による CPU 側の壁時計時間も示す。UI、スクリプト／段の進行、描画用の写しのコピー、物理結果の反映と入力準備、Box3D の計算、OGRE ノード同期、renderOneFrame、物理完了待ちを分け、約 0.5 秒で平滑化する。Box3D の時間はワーカー側で計測し、結合後に公開する。描画と物理は並行なので合計しない。renderOneFrame は CPU の描画処理に加え、GPU・ドライバー・表示待ちも含み、GPU 単体のタイマーではない。

［補足］`--profile-physics [project] [output.csv]` は物理サンプルの一時コピーを 1/60 秒の固定刻みで実行し、300〜329 ボディのフレーム平均と各フレームの CSV を出力する。既定の CSV は out/physics-profile.csv。計測ではメインループの sleep を省き、VSync を無効化する。表示 FPS は固定刻みの影響を受けるので、比較には壁時計の ms を使う。元のプロジェクトとプロジェクト一覧は変更しない。

［補足］2026-10-19 の実測例（Ryzen 5 2600／GTX 1660 Ti、clang-cl、ウィンドウ 1280×720、既定の空と影、並行実行、300〜329 ボディの 180 フレーム平均）:

| 項目（ms） | Debug `/Od` | Release `/O2` |
| --- | ---: | ---: |
| フレーム全体の処理 | 29.581 | 3.589 |
| UI | 5.697 | 0.567 |
| スクリプト／段の進行 | 0.250 | 0.034 |
| 描画用の写しのコピー・更新 | 2.216 | 0.173 |
| 物理の反映＋準備 | 2.298 | 0.233 |
| 物理ステップ（並行） | 4.996 | 0.685 |
| 描画同期 | 2.926 | 0.136 |
| 描画／表示待ち | 16.171 | 2.437 |
| 物理完了待ち | 0.014 | 0.006 |

［補足］この条件では描画経路がフレームの主因であり、物理ステップは並行実行の内側に収まっている。Release との差から Debug の CPU 側の処理・検査コストが大きいと判断できる。ただし描画／表示待ちを GPU 時間と同一視しない。箱ごとに個別の ManualObject・PBS Datablock を作る現実装であり、個数だけから GPU の限界とは判断しない。全体処理時間の逆数も、実際の表示 FPS を保証するものではない。CSV は out/physics-profile-debug.csv と out/physics-profile-release.csv。

`samples/physics` を開いて F5。球・カプセル・箱の移動に追随する影が地面と台へ落ちる。View → Shadows で比較できる。

CPU テストは、各 Primitive の法線が単位長であること、三角形の表向きと法線が一致することを検証する。

`--smoke-navigation` は空 ON／OFF の描画画像の背景画素が変わることと、再表示で元に戻ることも検査する。空を表示した状態でフライ・Orbit・フォーカス・パン・ドリーの入力再生テストを実行する。

Vulkan の `--smoke-physics` は Forward Clustered が選択されていることを確認し、同じ初期配置の影 ON／OFF の画像が異なることを検査する。Play 中の移動と、Stop 後に影が再び現れることも検査する。

［補足］PSSM のカメラ合わせにより、同じ配置へ戻しても PCF 境界に微小な画素差が出ることがある。そのため Stop の復元は影 OFF 同士の画像で完全一致を要求し、影の復元は別に ON／OFF の差分で確認する。画像は 2／3／60／240／244／245 フレーム目の PPM を出力する。

## 6. マテリアルとショーケース

［補足］PBS Material Component は粗さ、Fresnel F0 または金属度、発光、UV スケール、Diffuse／Normal／Roughness／Specular／Reflection の画像パスを保持する。Primitive の colour が拡散色となる。Light とともに Inspector で追加・編集・削除でき、プレハブの既存 Component に対する値の上書きにも対応する。画像はプロジェクト相対パスを基本とし、@ogre/ のみ固定済み OGRE Media を参照する。

［補足］samples/showcase に Forward3D、PbsMaterials、AreaApproxLights の配置を Throne のレベル／シーンとして移植する。メッシュは組み込み Primitive を使い、元の岩・大理石・反射画像を再利用する。Forward3D は 128 点光源、64 本の柱と 16 個の回転物体、他の 2 レベルは 8×8 の粗さ／Fresnel 球列と 16 個の回転物体を持つ。Area の 2 光源はマスク付き Approx と LTC のプレビュー切替に対応する。

［補足］Lighting メニューの Forward 方式とクラスタ表示はレベルへ保存し、Area の全光源切替は比較用の一時状態とする。永続的な方式変更は個々の Light.type に保存する。元アプリの F キーは引き継がず、F5 と Pause を Throne の Play／Pause に使う。検証は showcase_data_smoke と --smoke-showcases で行う。

## 7. 参考

- [OGRE-Next Compositor: PSSM / CSM と安定化](https://ogrecave.github.io/ogre-next/api/latest/compositor.html)
- 固定済みソースの `ShadowMapFromCode` サンプル、`ShadowNodeHelper`、`HlmsPbs::ShadowFilter`。
