# シーン音声と Mixer

## 1. 実行の段と担当

シミュレーション列の UpdateLoop は UpdateBehaviourSystem、LateUpdateBehaviourSystem、
Animation、Box3DStep、SteamAudio、SoLoud の順とする。その後に EndOfFrameBehaviourSystem が来る。
描画列は PreRenderEventSystem、OGRE、PostRenderEventSystem のままとする。

SteamAudio は音響形状と材質を同期し、リスナー相対方向・距離減衰・遮蔽・透過を更新し、反射ワーカーの結果を回収・要求する。
SoLoud は再生・停止命令と音量・ループ・バス設定を反映する。EndOfFrame 以降の命令と
UpdateLoop が 0 回のフレームの命令は、次回の SoLoud まで保持する。
旧レベルのステージ列には Box3DStep の直後へ両段を補完する。同名のユーザー System は拒否する。

音声コールバックは別に動き、Steam Audio の直接音・HRTF・反射畳み込み・Ambisonics デコード、SoLoud のミキシングと出力を行う。
シーンを直接参照せず、原子的に公開したパラメータの整合する組を読む。更新と競合した場合は
直前の組を使い、更新完了を待たない。play3d() は併用しない。
ストリーミングの読み込みと巻き戻しは先読みワーカーで行い、コールバックは有限長のリング
バッファだけを読む。不足時は無音を出す。

## 2. Component と API

AudioSource は `components.audioSource` に保存する。

| 欄 | 既定値・意味 |
| --- | --- |
| enabled / spatial | true / true。spatial=false は 2D |
| clip | 空。プロジェクト相対パス |
| volume | 1、範囲 0～100 |
| loop / autoPlay / streaming | いずれも false |
| occlusion / transmission | いずれも true。3D の直接音へ適用し、occlusion=false なら両方を迂回する |
| reflections / reflectionVolume | false / 1。3D の反射・残響を有効化、反射音量は 0～10 |
| bus | 1（Master） |
| minDistance / maxDistance | 1m / 100m。最小距離まで等倍、最大距離で無音となる線形減衰 |

3D はモノラルの事前ロード（読込後、コールバック外で 48 kHz に線形補間）、2D はモノラル／ステレオの事前ロードとストリーミングに対応する。
AudioListener は `components.audioListener` に enabled=true、整数 priority=0 を保存する。
最大 priority を選び、同順位はシーンインスタンス順・階層順とする。不在なら 3D を無音で進める。
レベル Inspector の Scene View Audio Listener は既定で有効。Play 中、Scene View が表示されているときは
シーンビューカメラの位置・姿勢を使い、整数 priority の最大値を含む全シーン Listener より優先する。
OFF または Scene View 非表示では通常のシーン Listener 選択へ戻る。カメラと同じく保存対象外のエディター設定とする。
カメラ姿勢は描画スレッドで値として取り込み、SteamAudio ステージへ渡す。ワーカーから OGRE を参照しない。
位置は階層の Transform に従い、向きは回転だけで求める。減衰距離にノードのスケールを掛けない。

node.audioSource() / node.audioListener()、型付き component() と query を用意する。
AudioSource の play/pause/stop は命令を予約する。play は停止・終了から先頭再生、一時停止から
再開し、再生中には重複再生しない。playing() は最後に反映した再生状態を返す。
volume/setVolume、loop/setLoop、bus/setBus、enabled/setEnabled、occlusion/setOcclusion、
transmission/setTransmission、reflections/setReflections、reflectionVolume/setReflectionVolume を公開する。
Listener は enabled/setEnabled、priority/setPriority を公開する。RenderEvent の変更は禁止する。

保存データと再生状態は分け、各プレハブインスタンスが独立した状態を持つ。
自動再生は Play 開始と実行中の新規生成で 1 回だけ行う。無効化・削除・クリップ変更で停止する。
読み込み失敗は対象を停止し Console に通知する。Play 停止・終了・レベル切替で再生状態を破棄する。
一時停止は再生位置を保持する。編集中は自動再生しない。

## 3. Mixer と編集

レベルの mixer 欄は *.mixer.lton のプロジェクト相対パス。未指定は Master のみとし、
指定されたファイルが不正・欠損なら Play 開始を拒否する。
MixerDefinition は保存定義、MixerInstance は実行中の SoLoud Bus 群とする。

ファイルは format="ogrethrone-mixer"、version=1、buses の一覧を持つ。
nextBusId は削除後も ID を再利用しないための採番位置として保存する。
各バスの欄は id、parent、name、volume、mute。Master は id=1、parent=0、name="Master"。
その他のバスは必ず Master へ到達する木とする。循環・欠損親・重複 ID は拒否する。
AudioSource は名前ではなく ID で接続し、欠損 ID は警告して Master へ接続する。
新規 Mixer は Master、Music、SFX、UI、Ambience を持つ。

Browser の Mixers から作成・選択し、Mixer 画面でバスを追加・削除・改名・再接続する。
削除したバスの子は Master へ移す。音量とミュートは Play 中にも反映する。
Source／Listener は Inspector、シーン・プレハブ、既存の上書き方式、複製、Undo/Redo に統合する。
Mixer も文書の保存・未保存状態・Undo/Redo の対象とする。

## 4. 音響形状・材質と直接音シミュレーション

AcousticGeometry は `components.acousticGeometry` に保存する。描画や Collider の有無とは独立して付ける。
既定は enabled=true、shape="Render"。Render は同じノードの Primitive と FBX のレスト形状を使う。
shape="Box" は boxSize（既定 {1,1,1}m）の見えない箱として使い、描画形状を必要としない。
FBX は描画と同じ右手系・Y 上・メートルに変換し、ファイル内のメッシュ配置を反映する。
OGRE .mesh の音響読込とスキニング変形への追従は対象外とし、Box で近似できる。
Transform の階層、シーンインスタンス、移動・回転・拡縮には追従する。

材質は Component 全体で共通の absorption（低・中・高域、既定 {0.2,0.3,0.4}）、
scattering（既定 0.5）、transmission（低・中・高域、既定 {0.1,0.05,0.01}）を持つ。
各係数は 0～1。直接音の透過に transmission を使う。反射シミュレーションには absorption と scattering を使う。
閉じた箱では表裏の面を通るため、透過率は壁全体ではなく各面の係数として解釈する。

形状は Steam Audio の子 Scene / StaticMesh として作り、親 Scene に InstancedMesh を配置する。
形状・材質が同じなら Transform の更新だけを行い、ローカルメッシュを毎刻み作り直さない。
有効状態・削除・プレハブ上書き・形状変更を同期し、FBX はパスと更新時刻でキャッシュする。
読めないモデルや形状のない Render 設定は Console に通知し、その音響形状を除外する。

直接音は raycast 遮蔽と最大 16 面の透過を固定 UpdateLoop で計算する。
Listener がない場合と 2D 音源は計算対象にしない。Listener と音源が同位置なら遮蔽物のない直接音とする。
シミュレーターの結果は方向・距離減衰・フラグと一緒に原子的に公開し、音声コールバックは
Steam Audio の DirectEffect と HRTF に適用する。透過は周波数依存とし、transmission=false なら
遮蔽だけを適用する。occlusion=false は遮蔽・透過の両方を無効にする。

Inspector と保存・プレハブ上書き・Undo/Redo に対応する。node.acousticGeometry()、型付き
component() と query を公開し、enabled、absorption、scattering、transmission をスクリプトで変更できる。
RenderEvent からの変更は禁止する。

## 5. 反射と残響

AudioSource の reflections を明示的に有効化すると、音響形状を使ったリアルタイムの反射を計算する。
直接音の Occlusion スイッチとは独立する。reflections=false は旧データとの互換性と不要な計算の抑制のための既定値。
2D・ストリーミングは対象外。Listener 不在では直接音・反射音ともミュートする。

固定 UpdateLoop は専用ワーカーへ位置と形状のスナップショットを渡すだけで、反射の完了を待たない。
最大 10 Hz、未完了ジョブは最大 1 件、最初の 16 音源までを対象とする。超過は Console で警告し、残りは直接音のみ。
計算中に動いた位置は次の要求で使う。起動直後は結果が届くまで直接音のみで再生する。
現状の固定予算は 4096 rays、16 bounces、1 秒 IR、1 次 Ambisonics（4 ch）、48 kHz / 512 samples。
長いホールなどの 1 秒を超える残響や品質設定の UI、ベイクは今後の対象とする。

各ジョブは独立した Scene・Simulator・Source を所有する。ローカル形状の不変な子 Scene を保持して
配置をスナップショット化し、直接音側の Scene 更新との競合を避ける。完了後は IR を変更しない。
形状・材質の版が古い結果と、削除・置換された音源の結果は新規公開しない。
更新が届くまで直前の IR を使い、形状変更後の既存の残響の尾は自然に流す。
Play 停止ではワーカーを終了まで回収する。これは音声コールバックや通常の固定更新の待機とは別とする。

音声コールバックは直接音処理前のモノラル PCM を反射用にも渡し、Steam Audio の convolution で
初期反射と残響の尾を生成する。ワールド方向の Ambisonics を現在の Listener 姿勢で回転して HRTF デコードし、
直接音へ加算する。反射には直接音の遮蔽・透過フィルターを重ねない。距離に伴う反射の強さは Steam Audio が計算する。
Source の最大距離では両経路を無音とする。reflectionVolume は反射だけ、volume と出力バスは両経路へ作用する。
独立した send バスは追加しない。Mixer の構成は維持する。

音源データが終わっても畳み込みと HRTF の尾を最後まで再生してから voice を終了する。
pause/resume は両経路の処理位置を保持し、stop・削除・無効化は両経路を停止する。
IR は原子的なポインターで公開し、コールバックの読取区間を原子的カウンターで示す。
古い所有者の解放はシミュレーション側で行い、コールバックでは Simulator の破棄・ロック・待機を行わない。
音源ごとの退役 IR は 2 件までに制限し、読取中で回収できなければ次の制御更新まで差し替えを延期する。
各 IR の消費 voice は 1 つ。Scene の AudioSource はもともと同時 1 voice に限定している。

C API の契約は [Simulation](https://valvesoftware.github.io/steam-audio/doc/capi/simulation.html)、
[Reflection Effect](https://valvesoftware.github.io/steam-audio/doc/capi/reflections-effect.html) を参照。

## 6. 検証データと後続対象

samples/audio を検証用プロジェクトとする。Kenney Impact Sounds と Interface Sounds から
各 1 点を CC0 で同梱し、原本・ライセンス・出典・取得日・SHA-256 を保持する。
Impact のステレオ原本から L/R 平均のモノラル WAV を作り、3D 音源で使う。
自前の CC0 ノイズと長いステレオ信号、既存の WAV/Ogg/MP3 テスト信号を併用する。
ビルド・CI から外部ダウンロードは行わない。

scene_acoustics_smoke は Primitive / Box / FBX、形状の移動・回転・拡縮・削除・無効化、
材質変更、透過の周波数特性、スクリプト、保存・プレハブ上書き、並列実行を検証する。

reflections_smoke は非同期 IR、形状・音源の版、吸音による残響低下、44100 Hz 音源の残響尾、
IR 差し替え中の音声読取、直接音が遮蔽された室内、Mixer mute、Pause、Listener 不在、スクリプトと保存を検証する。

ベイク、ドップラー、OneShot、send、
エフェクトチェーンとオートメーションは後続実装とする。
