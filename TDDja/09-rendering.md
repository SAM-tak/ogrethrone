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

## 7. ParticleFX2

［補足］Particle System Component は ParticleFX2 のテンプレート名、任意のプロジェクト相対 .particle2 パス、有効フラグを持つ。Inspector で編集でき、シーン保存、プレハブ上書き、query("ParticleSystem") に対応する。空のパスは組み込みの 5 テンプレートを参照する。

［補足］particlefx2.level.lton は元の噴水、光輪、PBS 煙、Unlit 煙、PBS 球粒子と移動台・マーカーを移植する。元の煙定義は Affector の継承が衝突するため、media/particles/Showcase.particle2 で継承を展開し、Scaler／Rotator／ColourImage を分離する。色の増加率の先頭 + も除去し、charconv で元の数値を読み取れるようにする。画像と HLMS 材質は固定済み OGRE Media を利用し、単独の大理石 Billboard は縦の Plane で表す。

［補足］粒子プレビューは編集時も動作し、Particles メニューで停止・再開・再スタートできる。Play 開始と Stop で共有粒子プールを消去し、Pause 中は OGRE ControllerManager の時間係数を 0 にして放出・位置・寿命を停止する。OGRE のフレーム時間には Throne の dt を渡す。カメラと UI は継続する。テンプレートや有効フラグの変更もプールを再スタートする。

［補足］Particles メニューで Alpha Blending／Alpha Hashing を比較できる。単一サンプルのビューポートなので MSAA A2C の比較は含めない。統計にシステム数と粒子の概数（SIMD の処理枠数）を表示する。--smoke-showcases は 4 レベルを開き、5 系統を単独表示した際の描画、停止中の全生存粒子の位置・寿命、方式変更、Stop の消去、再入場を検査する。

［補足］光輪の aureola.png は輝度＋アルファの PNG で、STBI は RG8 として読み込む。通常の RGBA 読み取りでは存在しない A が 1 となり、透明な四隅に黒い板が現れる。Unlit の読み取りを R→RGB、G→Alpha に割り当てる。描画テストは光輪の単独表示で背景の一様色が黒く覆われないこと、アルファブレンドと深度書き込み OFF を検査する。

［補足］シーンの RGB は背景と粒子を合成済みだが、描画先の Alpha は粒子によって変わる。ImGui の標準 Image 材質でこれを再度透過合成すると、RGB のキャプチャでは正常でも UI には黒い四角が現れる。ビューポート専用の Image 材質だけを不透明コピーにする。固定済み ImGui バックエンドが prepareForRender で生成するテクスチャ別材質を設定し、フォントなど他の UI の透過は維持する。--smoke-showcases は影を有効にした粒子シーンについて、光輪単独と Alpha Blending／Hashing のウィンドウ出力を取得し、UI 表示部分を除いた画素がシーンの RGB と一致することも検査する。

## 8. モデルと環境マップ

［補足］Model Component はプロジェクト相対の FBX パスと、法線マップが DirectX 規約かどうかのフラグを持つ。読み込みは ufbx（v0.23.1、MIT、third_party/ufbx）で、メートル・+Y 上・右手系へ変換する。ファイルは ModelLibrary が一度だけ読み、メッシュごとの OGRE Mesh とマテリアルごとの PBS データブロックを共有する。配置はメッシュインスタンスごとに Item を作る。ファイル内のライトとカメラは配置しない。Model だけのノードは、モデルの境界を選択枠とピックに使う。

［補足］マテリアルはメタリックワークフロー。テクスチャは WRAP と異方性 8 でサンプルする（OGRE 既定の CLAMP では、UV が 0〜1 を超えるタイル模様が端の色一色になる）。FBX の透過色（transparency_color × factor）を持つマテリアルはアルファブレンド（深度書き込みなし、両面）とし、ガラスと液体の反射を残す。不透明度 0.25 未満（グラス、窓）は OGRE の Refractive モードで画面空間屈折（強さ 0.05）とし、その Item を描画キュー 200 に置く。液体と瓶は Transparent モードのまま第 1 パスに描き、手前のグラスの屈折に映す（屈折は第 1 パスの画像だけを背景にするため、同じパスの液体は上書きされる）。透過マテリアルだけでできたメッシュは影を落とさない。BaseColor（アルファは不透明度、DXT5 ならアルファテスト 0.5 と両面）、Normal、glTF 式 ORM（R=遮蔽、G=粗さ、B=金属度）、Emissive を使う。ORM はデータブロック単位のカスタムピースで SampleSpecular／SampleRoughness の参照チャンネルを差し替え、既存のショーケース材質には影響させない。DirectX 法線は接線の w 符号で扱う。

［補足］レベルの rendering は environment（プロジェクト相対の正距円筒 HDR）と environmentScale を持つ。CPU で 512² のキューブマップ（2×2 スーパーサンプル）と 9 係数の球面調和へ変換する。ミップは OGRE の IblSpecular 計算パス（Compute/Algorithms/IBL）で GGX 粗さ別に事前フィルタする。キューブマップは空と、自前の反射マップを持たない全 PBS マテリアルの反射に、球面調和は AmbientSh の拡散環境光に使う。放射輝度は 64 で打ち切り、HDR 内の太陽は Light Component の平行光源に任せる。空の太陽は既定の太陽か、最初の平行光源 Component の向きに追従する。

［補足］ビューポートはシーンを RGBA16F に 2 パスで描く。描画キュー 0〜199 を Opaque へ（深度は共有の D32 テクスチャ）、それを Hdr へ複製し、キュー 200 以上を Opaque と深度を屈折元として Hdr へ描く（影ノードは再利用）。その後クアッドパス（media/postprocess/Tonemap）で露出（EV）を掛けて表示先へ書く。レベルの toneMapping が真なら ACES フィルミック曲線（Narkowicz の近似）、偽なら従来どおりクランプする。既定は偽・EV 0 で、既存レベルの見た目は変わらない。

［補足］samples/bistro は ORCA の Amazon Lumberyard Bistro（CC BY 4.0）を 3 レベルで再現する。データ（Bistro_v5_2/）は Git 管理外。

- exterior: FBX の太陽の向きと色、カメラを開始視点に、同梱 HDR を環境マップに使う。
- interior: FBX の 4 点光源（影あり）と、壁灯・ランタンの発光メッシュ重心に置いた点光源で照らす（Falcor はレイトレースで発光面を光源にする）。色は参照画像に合わせ FBX の暖色より白寄りにした。
- interior-wine: 同じ内装を約 1.47 倍した版（光源なし）。天井灯・壁灯・ランタンの重心に点光源を置き、満たしたワイングラスを透過で描く。
- plaza: exterior と同じシーンを ORCA の外観プレビュー 1 枚目（Le Petit Coin の角とスクーター）の視点で見る。参照のカメラ値は公開されていないため、構図を目視で合わせた。画角は FBX カメラと同じ 34°（レベルの fieldOfView）、露出 +1.3 EV。平均 RGB は 109/99/94（参照 104/103/102）。参照の方が中立色で、日向の石畳が明るい。

`ogrethrone --capture <folder> [file] [level]` はプロジェクトとレベルを開き、テクスチャのストリーミング完了後のビューポートを PPM で保存して終了する。OGRE のアサートはダイアログでなく標準エラーへ出す。ORCA 公開の参照画像（外観 2、内装 3）と比較し、色調・素材・ガラスを確認した。

［補足］固定済み OGRE fork に以下を加えた。

- PBS テンプレート: 単チャンネルの発光マップで `.rrr` を midf4 へ代入していた誤りを `.rrra` に修正。
- STBI コーデック: Radiance HDR（.hdr）を RGBA32F で読む。
- HlmsPbs: アルファテストの影キャスターが反射マップを除いたテクスチャ数を宣言し、Vulkan が反射込みの記述子セット全体をバインドしてルートレイアウトと食い違っていた。反射マップも数に含める。
- PBS 屈折テンプレート: GLSL の max に 3 引数を渡していた（金属ワークフローでコンパイル不能）。2 段の max に修正。
- SceneManager::clearScene: ルートに付いたままのスカイ矩形を再アタッチして例外になっていた。未アタッチ時だけ付け直す。

［補足］Point／Spot の Light Component は castShadows を持つ。影ノードは平行光源の 3 カスケードに加え、1024² の FOCUSED 影マップ 4 枠を同じアトラス（4096×4096）に持ち、OGRE がカメラに近い影付き局所光へ割り当てる。点光源はキューブから DPSM へ変換する（Common/DPSM.material を読む）。Bistro 内装では天井灯 4 灯が影を落とす。

［未決］液体の吸収（体積）と間接光（GI）は扱わない。液体は色付きの半透明で近似する。参照はパストレースなので、日陰の明るさと色の中立さは近似にとどまる。

## 9. 間接光（GI）

［補足］Bistro の参照画像は Falcor のパストレースで、建物間の照り返しが日陰を明るく中立色にしている。現状は HDR 環境の球面調和と反射キューブマップのみで、plaza の平均 RGB は 109/99/94（参照 104/103/102）。差の主因は一次反射光の欠如。

［提案］固定済み OGRE-Next の既存機能から次の 3 段階で選ぶ。

- A. 局所キューブマッププローブ（ParallaxCorrectedCubemapAuto ＋ PccPerPixelGridPlacement）。シーンを数か所でキューブに描き、反射と EnvFeatures_DiffuseGiFromReflectionProbe による拡散 GI に使う。追加コードが最小で静的シーン向き。プローブ位置はレベルに保存し、エディターで再焼き込みする。
- B. VCT（VctCascadedVoxelizer ＋ VctLighting）。シーンをボクセル化し、拡散・鏡面の間接光を円錐追跡で求める。動的光源にも追従するが、280 万三角形の外観ではボクセル化の時間とメモリが大きい。
- C. IrradianceField（B のボクセルから放射照度プローブ格子を作る）。B より軽く、拡散 GI のみ。

まず A を samples/bistro に入れ、参照との平均 RGB と日陰の明るさで効果を測る。足りなければ C、動的光源が必要になった時点で B へ進む。

［補足］A〜C の判断までの暫定として、単一の光プローブを入れた。レベルの lightProbe と probePosition で指定し、テクスチャのストリーミング完了後に 1 回、その位置から 64² RGBA16F のキューブへシーンを描く（描画キュー 0〜199 と 201 以上。屈折ガラスの 200 は除く）。CPU へ読み戻して球面調和へ投影し、HDR 環境の拡散光（AmbientSh）を置き換える。反射は HDR 環境のまま（単一プローブの反射は視差が合わない）。撮影時のシーンは HDR 環境で照らされているので、空・太陽に加え周囲の面からの一次反射光が入り、建物に遮られた空の分は暗くなる。位置を変えるか Recapture で撮り直す。Bistro の 4 レベルで有効。plaza の平均 RGB は 100/91/87（参照 104/103/102）。残る差は主に色温度で、FBX の太陽色（橙寄り）による。

［未決］A〜C のどれを採るか。焼き込み結果をプロジェクトに保存するか、レベルを開くたびに作るか。

## 10. 参考

- [OGRE-Next Compositor: PSSM / CSM と安定化](https://ogrecave.github.io/ogre-next/api/latest/compositor.html)
- 固定済みソースの `ShadowMapFromCode` サンプル、`ShadowNodeHelper`、`HlmsPbs::ShadowFilter`。
