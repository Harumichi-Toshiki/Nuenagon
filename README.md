# Nuenagon

Nuenagon は、将棋エンジン Dainagon（MCTS）と合議（Dajo）するための、αβ探索エンジンです。
このリポジトリは、マイルストーン N1（盤面・指し手生成・千日手・詰み探索）までを含みます。
設計は [docs/DESIGN.md](docs/DESIGN.md) の第E部、受け入れテストは [tests/test_n1_acceptance.py](tests/test_n1_acceptance.py) を参照してください。

## 構成

```
src/
  main.cpp              命令の受け付け（USI 風の標準入出力）
  board/
    bitboard.h          ビットボードと指し手生成（Dainagon から流用）
    shogi_utils.h       盤面 Board、push_move / undo_move、利き判定（Dainagon から流用）
    zobrist.h           ハッシュ値の乱数表（Dainagon から流用）
    game.h / game.cpp   局面と履歴、千日手の判定、perft
  eval/raypair/
    raypair.h / .cpp    特徴量 RayPair_v0 / v1 の計算（設計書 §F2。盤面から直接計算）
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
| `quit` | 終了 |

知らない命令は無視します（`info string` を返します）。
