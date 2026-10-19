# ウィンドウと入力デバイス

## 1. 採用方針

OS ウィンドウの作成には、今後も OGRE の API を使う。
ゲームパッドなどの入力デバイスには [SAM-tak/OIS の for-throne ブランチ](https://github.com/SAM-tak/OIS/tree/for-throne)を採用する。

## 2. 初期セットアップ

［補足］third_party/ois の Git submodule として取得し、親リポジトリが記録したコミットに固定する。
OIS は静的リンクとし、デモをビルドしない。Windows は DirectInput と XInput を有効にする。
SDL は導入しない。ヘッドレス構成では、ウィンドウに結び付く OIS のビルドと初期化を省く。

［補足］src/input/OisInput が、OGRE の作成したウィンドウに OIS を接続する。
Windows は HWND、Linux の Vulkan／X11 は XCB ウィンドウ ID を渡す。
起動時にジョイスティック／ゲームパッドを列挙して開き、各フレームの UI・シミュレーション処理前に capture する。
機器が 0 台でも起動できる。入力機器はウィンドウが破棄される前に解放する。
Windows の DirectInput は非排他・フォアグラウンド取得とし、エディターのキーボード・マウス・IME は現在の ImGui プラットフォーム処理を使う。

［補足］OisInput::joystickCount() と joystick(index) で、OIS の機器情報と JoyStickState を参照できる。
ボタン・軸・POV・スライダーは機器固有の生データであり、入力アクションや統一されたパッド配列にはまだ変換しない。
入力アクション、スクリプト API、自動ホットプラグ検出、パッドによるエディター UI 操作は未実装。
機器の接続は起動前に行う。検出数と機器名は OgreThrone.log に記録する。
