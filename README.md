# Nuenagon

Nuenagon は、将棋エンジン Dainagon（MCTS）と合議（Dajo）するための、αβ探索エンジンです。
このリポジトリは、マイルストーン N3（重みの読み込み・量子化・推論・差分更新）までを含みます。
設計は [docs/DESIGN.md](docs/DESIGN.md) の第E部、受け入れテストは [tests/test_n1_acceptance.py](tests/test_n1_acceptance.py) を参照してください。

## 構成

```
src/
  main.cpp              命令の受け付け（USI 風の標準入出力）
  board/
    bitboard.h          ビットボードと指し手生成（Dainagon から流用）
    shogi_utils.h       盤面 Board、push_move / undo_move、利き判定（Dainagon から流用）
    zobrist.h           ハッシュ値の乱数表（Dainagon から流用）
    game.h / game.cpp   局面と履歴、千日手の判定、perft、蓄積値の差分更新（null move を含む）
  eval/raypair/
    raypair.h / .cpp    特徴量 RayPair_v0 / v1 の計算（設計書 §F2。盤面から直接計算）
    network.h / .cpp    重みファイル（.shk）の読み込みと量子化、全計算・差分更新（§E3）、推論（§E4）
  mate/
    dfpn.h              詰み探索（df-pn、Dainagon から流用）
dainagon_source/        Dainagon のソース（参照用。変更しない）
tests/                  受け入れテスト（変更しない）
```

## ビルド

CMake 3.16 以上と、C++17 のコンパイラが必要です。

### Linux（g++ / clang++）

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

実行ファイルは `build/nuenagon` にできます。

### Windows（MSVC）

Visual Studio の開発者コマンドプロンプトで次を実行します。

```bat
cmake -S . -B build
cmake --build build --config Release
```

実行ファイルは `build\Release\nuenagon.exe` にできます。

## テスト

```sh
pip install pytest cshogi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python -m pytest -q tests
```

テストは `build/nuenagon`（Windows では `build/Release/nuenagon.exe`）を起動して、標準入出力で命令を送ります。
実行ファイルの場所は環境変数 `NUENAGON_BIN` で指定することもできます。

## 命令

| 命令 | 応答 |
|---|---|
| `usi` | `id name Nuenagon`、`id author Harumichi-Toshiki`、`usiok` |
| `isready` | `readyok` |
| `position startpos [moves …]` ／ `position sfen <盤面> <手番> <持ち駒> [<手数>] [moves …]` | なし（指し手は差分更新で進め、履歴を残す） |
| `sfen` | `sfen <盤面> <手番> <持ち駒>` |
| `moves` | `moves <合法手（USI）…>` |
| `perft <深さ>` | `perft <末端の局面数>` |
| `key` | `key <16桁の16進数のハッシュ値>` |
| `rep` | `rep <none\|draw\|win\|lose\|superior\|inferior>` |
| `mate <最大ノード数>` | `mate <USI の手>` または `mate none` |
| `features <v> <p>` | `features <特徴のインデックス（昇順・重複込み）…>`。v：0 = v0、1 = v1。p：0 = 先手視点、1 = 後手視点 |
| `linefeatures <v> <p> <L>` | `linefeatures <B1 の特徴（昇順・重複込み）…>`。ライン L（0〜51）だけ |
| `linesof <sq>` | `linesof <L0> <L1> <L2> <L3>`。マス sq（視点座標 = y*9+x）を通る4本のライン番号 |
| `setoption name <名前> value <値>` | なし。`EvalFile`（既定 `eval/nuenagon.shk`）、`EvalScale`（0 ならファイルの値）、`DebugCheckAccumulator`（true なら差分の蓄積値を全計算と比べる） |
| `isready` | 重みを読み込んで `info string Nuenagon eval: …` と `readyok`。失敗は `info string eval load error: <理由>`（`readyok` は出さない） |
| `eval` | `eval raw <int32> cp <int>`（手番側から見た値） |
| `acc <p>` ／ `accfull <p>` | `acc <W 個の整数>`：差分で保っている視点 p の蓄積値（int16、バイアス込み）。`accfull` は今の局面から全計算した値 |
| `undo` ／ `null` | なし。直前の手を戻す ／ 手番だけを入れ替える（null move） |
| `acccheck <手数> <乱数の種>` | `acccheck ok <回数>` または `acccheck fail <SFEN> <直前の手>`。ランダムに指し進め・戻し・null を混ぜ、毎回差分と全計算を比べる |
| `evalbench <回数>` | `info string evalbench push_us=… pop_us=… eval_us=…`（1手の差分更新と1回の評価の平均時間） |
| `quit` | 終了 |

知らない命令は無視します（`info string` を返します）。
