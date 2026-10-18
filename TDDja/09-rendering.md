# Clustered Forward と影

## 1. 採用方針

既定の描画に OGRE-Next の Forward Clustered を使う。Primitive は HlmsPbs の物理ベースの照明を受ける。影は深度シャドウマップを使い、太陽光には PSSM（Cascaded Shadow Maps）を適用し、PCF で境界をフィルターする。

［補足］Clustered Forward はローカルライトの割当てを行い、影は独立した深度アトラスから評価する。この組合せは OGRE-Next が備える HlmsPbs と CompositorShadowNode を使って実装する。独自の照明シェーダーやスクリーン空間だけの影は追加しない。

## 2. 現在の既定設定

［補足］現実装の数値は次のとおりとする。シーン単位の照明編集や品質設定の保存はまだ設けず、ビューポートの既定のプレビュー照明として使う。

- Forward Clustered: 16×8×24 セル、セルあたり最大 64 ローカルライト、奥行き範囲 0.1〜100。
- 太陽光: Directional Light、方向 (-0.6, -1, -0.4) を正規化、power scale は π。
- 環境光: 上半球 RGB (0.22, 0.25, 0.30)、下半球 (0.06, 0.07, 0.09)、上向き軸は +Y。
- 太陽光の影: 3 段 PSSM。近い段は 2048²、残りは各 1024²。1 枚の 2048×3072 深度アトラスに配置する。
- PSSM: 最初の 2 段を安定化し、lambda 0.7、split padding 1、blend 0.125、fade 0.2、影の遠方距離は 100。
- フィルター: HlmsPbs::PCF_3x3。深度比較とバイアス処理には OGRE-Next の標準実装を使う。
- Primitive の材質: 粗さ 0.65、誘電体の Fresnel 0.04、拡散色は Component の colour。

［補足］現時点の影を落とすライトは、この既定の太陽光 1 個。Point／Spot のライト Component や追加シャドウ枠はまだない。Forward Clustered 自体はローカルライトに対応する経路として有効化している。

## 3. ジオメトリとエディター表示

Primitive の各頂点は外向きの単位法線を持つ。箱は面ごとの法線、Sphere／Capsule は曲面法線、Plane は +Y を使い、三角形の向きを揃える。従来の頂点色に焼き込んだ明暗は使わず、PBS で照明と影を計算する。

Plane を含めて両面照明とし、Primitive は影を落とし、他の Primitive の影も受ける。グリッド・選択枠・ImGui は非照明表示を保つ。グリッドと選択枠は影を落とさない。

View → Shadows は太陽光の影だけを切り替える。拡散光と環境光は保ち、シーンファイルや物理シミュレーションは変更しない。この切替はエディターの表示状態で、保存対象ではない。

## 4. 実行との関係

OGRE は従来どおりメインスレッドで、完成済みの描画用の写しを使う。影の深度描画と本描画は同じ OGRE ノードの配置を参照するので、物体と影は同じ時点を表示する。Box3D のワーカーは OGRE オブジェクトや影のテクスチャーを操作しない。

ビューポートのリサイズで Workspace を作り直しても、同じシャドウノード定義を再利用する。HlmsUnlit と HlmsPbs のテンプレートは固定済みの OGRE サブモジュールから読み込む。

## 5. 検証

`samples/physics` を開いて F5。球・カプセル・箱の移動に追随する影が地面と台へ落ちる。View → Shadows で比較できる。

CPU テストは、各 Primitive の法線が単位長であること、三角形の表向きと法線が一致することを検証する。

Vulkan の `--smoke-physics` は Forward Clustered が選択されていることを確認し、同じ初期配置の影 ON／OFF の画像が異なることを検査する。Play 中の移動と、Stop 後に影が再び現れることも検査する。

［補足］PSSM のカメラ合わせにより、同じ配置へ戻しても PCF 境界に微小な画素差が出ることがある。そのため Stop の復元は影 OFF 同士の画像で完全一致を要求し、影の復元は別に ON／OFF の差分で確認する。画像は 2／3／60／240／244／245 フレーム目の PPM を出力する。

## 6. 参考

- [OGRE-Next Compositor: PSSM / CSM と安定化](https://ogrecave.github.io/ogre-next/api/latest/compositor.html)
- 固定済みソースの `ShadowMapFromCode` サンプル、`ShadowNodeHelper`、`HlmsPbs::ShadowFilter`。
