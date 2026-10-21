# N1 報告（盤面・指し手生成・千日手・詰み探索）

## 結果

**完了。** 受け入れテストは 23 件すべて通過。cshogi との突き合わせも不一致 0 件。
ただし、GitHub への push は一度も成功していません（後述の「push の状況」を参照）。

## `python -m pytest -q tests` の最終出力

```
.......................                                                  [100%]
23 passed in 5.77s
```

## perft 5（初期局面）の速さ

- 結果: 19,861,490（正しい値）
- 時間: 約 0.6 秒（2 回計測、0.58 秒と 0.65 秒）
- 速さ: 末端の局面数 ÷ 時間で、毎秒約 3,300 万（合格条件ではない）
- 環境: Linux（Ubuntu 24.04）、Intel Xeon 2.10GHz、4 コア（1 スレッドで計測）、g++ 13.3、CMake Release（-O3）

## Dainagon のコードから変えた点

**取り除いたもの**
- `#include <onnxruntime_cxx_api.h>`（`shogi_utils.h`）
- `BoardFeatures` 構造体と `make_features` 関数（`shogi_utils.h`。DL 用の特徴量の生成）
- `search.h`、`vocab.h`、`chunagon_v20.12.cpp`（コピーしていない）

**直したもの**
- `shogi_utils.h` の `Board::undo_move`：玉を取った手を戻すとき、持ち駒を減らしていた。`push_move` と同じく玉は持ち駒に加えないので、戻すときも減らさないようにした。合法手だけでは起きないが、対称性のため。
- `dfpn.h` の `DfpnSearcher::search_mate`：詰み手が確定しないとき、「最初の手」を返していたフォールバックを削除。詰みが確定した子が見つからなければ `false`（`mate none`）を返す。
- `dfpn.h` の `DFPN_TT_SIZE`：`extern` の宣言を `inline int DFPN_TT_SIZE = 1 << 20;`（約 80MB の TT）に変えた。
- 全ファイル：CRLF を LF に変え、BOM を外した。

**足したもの**
- `bitboard.h`：`#include <cstdlib>`（`std::abs` 用）。
- `src/board/game.h` / `game.cpp`：局面と履歴のクラス `Game`、千日手の判定 `repetition()`、`perft()`。
- `src/main.cpp`：命令の受け付け。
- `CMakeLists.txt`、`README.md`、`.gitignore`。

**残したもの（N1 では未使用）**
- `shogi_utils.h` の `push_usi`、`get_tsume_moves`、`can_declare_win`、`get_attacks` など。N2 以降で使う可能性があるため削除していない。

## 千日手の判定の仕組み

`Game` は `Board` と、局面ごとの履歴 `HistoryEntry`（全体のハッシュ `key`、持ち駒を除いたハッシュ `board_key`、手番、手番側が王手されているか、持ち駒）を持つ。指し手は `Board::push_move` / `undo_move` で進め、戻す（差分更新）。履歴も同時に積む・外す。

`repetition()` は次のように判定する（cshogi の `is_draw()` と同じ決まり）。
1. 手番・盤面・両者の持ち駒が同じ過去の局面（偶数手前）があれば、その局面以降の王手の有無で判定する。相手の手がすべて王手なら `win`、手番側の手がすべて王手なら `lose`、それ以外は `draw`。
2. 1 がなく、盤面と手番が同じで、手番側の持ち駒が過去の局面以上（全駒種で以上、かつ 1 種類以上で多い）なら `superior`、以下なら `inferior`。
3. どちらもなければ `none`。

## 作ったファイルの一覧

| ファイル | 役割 |
|---|---|
| `CMakeLists.txt` | ビルド設定（C++17、Linux と MSVC の両方） |
| `src/main.cpp` | USI 風の命令の受け付け（usi、isready、position、sfen、moves、perft、key、rep、mate、quit） |
| `src/board/bitboard.h` | ビットボードと指し手生成（Dainagon から流用） |
| `src/board/shogi_utils.h` | 盤面 `Board`、`push_move` / `undo_move`、利き判定（Dainagon から流用、DL 部分を削除） |
| `src/board/zobrist.h` | ハッシュ値の乱数表（Dainagon から流用） |
| `src/board/game.h` | 局面と履歴のクラス `Game`、千日手の状態、perft の宣言 |
| `src/board/game.cpp` | `Game` の実装、千日手の判定、perft |
| `src/mate/dfpn.h` | 詰み探索 df-pn（Dainagon から流用、直し済み） |
| `README.md` | ビルドとテストの手順、命令の一覧 |
| `.gitignore` | `build/` などを除外 |
| `REPORT_N1.md` | この報告 |

## Windows（MSVC）でのビルド手順

Visual Studio の開発者コマンドプロンプトで次を実行する。

```bat
cmake -S . -B build
cmake --build build --config Release
```

実行ファイルは `build\Release\nuenagon.exe`。テストは `python -m pytest -q tests` で実行する（`NUENAGON_BIN` で場所を指定することもできる）。

**注意**: MSVC でのビルドは、この環境では確認していない。Linux でしか使えない関数は使っていない（`__builtin_ctzll` は `_MSC_VER` で分岐済み、`<intrin.h>` も MSVC のみ）。ただし、MSVC の警告やエラーは未確認。

## 検証（cshogi との突き合わせ）

受け入れテストとは別に、ランダム対局で `rep` の応答を cshogi の `Board.is_draw()` と比べた（検証スクリプトはリポジトリの外）。
- 1 回目: 40 局（各 100 手まで）、判定 1,022 件、不一致 0。出た結果は none・draw・win・inferior。
- 2 回目（シード 7）: 120 局（各 100 手まで）、判定 3,132 件、不一致 0。出た結果は none・draw・inferior・superior・lose。

**注意**: ランダム対局では `win`・`lose`・`superior` がまれにしか出ない。6 種類すべての判定はゴールデンデータ（各 25 件、計 150 件）のテストでも確認している。

## push の状況

- 最初の push は GitHub の 403（App の権限不足）で失敗した。push 権限を追加する要求は承認されなかった。
- その後、権限を直していただき、`git push -u origin claude/optimistic-hamilton-6kln6o` は成功している。
- この報告を含む最後のコミットは、push 前にこのファイルに書き込んだため、その後の push で反映する。

## 次の担当（N2：特徴量）への引き継ぎ

1. **特徴量は未実装**。Dainagon の `make_features` と `BoardFeatures`（`dainagon_source/shogi_utils.h` の 786〜893 行目）は取り除いた。N2 では、`docs/DESIGN.md` の §F を基に作り直す。Dainagon の版は参考にするだけで、そのまま使うと仕様と違う可能性がある（チャンネル番号、駒番号の対応、後手の盤の回転）。
2. **履歴と王手の情報**：`Game::history_` の `HistoryEntry::in_check` は、手番側が王手されているかを持つ。N2・N3 で差分更新をするときも、`Game::push` / `pop` を通すこと。
3. **`Board::undo_move` の修正**：玉を取る手の戻し方を直した。N3 の差分更新でも、取る手は同じ規則で扱う。
4. **詰み探索の TT** は `1 << 20` 件、約 80MB。N4 で探索と共有するなら、サイズと `clear()` の扱いを決める。
5. **コンパイル警告**が残っている（`bitboard.h` の `-Warray-bounds`、`-Wunused-variable`、`dfpn.h` の符号比較）。機能には影響しないが、N5 までに整理する。
6. **MSVC のビルドは未確認**。Windows で一度ビルドして確認する。
7. **速さ**: perft 5 は毎秒約 3,300 万。N5 で高速化の対象にする。

## `python check_protected.py` の結果

```
OK
```
