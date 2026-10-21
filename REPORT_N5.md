# N5 報告：高速化（結果を変えずに速くする）と CPU の割り当て

## 結果：完了

- `python -m pytest -q tests`：**89 passed**（407.76 秒。N1 23 ＋ N2 28 ＋ N3 11 ＋ N4 15 ＋ N4.1 4 ＋ N5 8）
- `python check_protected.py`：**OK**（各コミットの前に実行）
- 速さ（`test_speedup` が合格。N4.1 を `ae08a26` から作って比べる）：
  - 同じ探索（Threads=1・Deterministic=false・go nodes 300000、12 局面）の合計時間：N4.1 **20.17 秒**、N5 **9.33 秒**、比 **2.16 倍**（各局面は速い方の 1 回の時間を合計。ノード数は全局面で N4.1 と一致）
  - 途中の計測（同じ方法）：差分更新の作り直し後 1.42 倍、探索の確保の除去後 1.64 倍、利きの判定の短絡と王手の再利用後 2.11 倍、ライン表後 2.16 倍
- `evalbench 200000`（局面は中盤）：
  - N4.1：push **2.614 µs**、pop 0.055 µs、eval（隠れ層以降）**0.784 µs**
  - N5：push **0.823 µs**、pop 0.054 µs、eval **0.208 µs**
- `bench`（深さ 6・4 局面の合計。ノード数は両者で一致）：
  - 1 スレッド：N4.1 245,074 nps → N5 571,251 nps（2.33 倍）
  - 4 スレッド：N4.1 714,413 nps → N5 1,512,653 nps（2.12 倍）
- 測った環境：Intel Xeon Processor @ 2.10GHz、論理 CPU 4、物理コア 4（仮想マシン）。計測は他の負荷を止めて行った。

## 正しさ

- 正解データ（`tests/golden/n5_golden.json`、47 局面・info 357 行）と、既定のビルドと SIMD OFF のビルドの両方で完全に一致。
- 差分更新は全計算と一致（`DebugCheckAccumulator` で探索中に検査、不一致なし）。
- 探索のノード数は、12 局面すべてで N4.1 と完全に一致。

## 高速化で変えたこと（効いた量は上の比の変化）

1. **差分更新の作り直し**（20.9 → 14.7 秒）：`Game` が両視点の `ViewBoard` を持ち、指す・戻すたびに変わったマス（移動元・移動先）と持ち駒だけを写す。81 マスを調べない。ラインは変わったマスを通るものだけ（重複なし）、そのうち変わった区間の組だけを指す前と後で足し引きする。桂は、変わったマス自身と、その跳び先に当たるマスの桂だけ。
2. **AVX2 の足し引き**：蓄積値の 128 要素を ymm 8 本に載せたまま全行を足し引きし、最後に 1 回だけ書き戻す。int16 の足し引きは mod 2^16 なので、順序によらず同じ結果。隠れ層1は `maddubs` / `madd` で計算（積の和は ±32,258 で飽和しない）。
3. **確保の除去**（14.7 → 12.6 秒）：利きの方向リストの `std::vector` を静的配列に（`get_slider_attacks`）。指し手の一覧と並べ替えの点数を、深さごとの作業領域に（`negamax`・`qsearch`）。差分更新の一覧は固定長の配列。
4. **利きの判定の短絡**（12.6 → 9.8 秒）：相手にその種類の飛び駒が無いときは利きを計算しない。王手の有無は、進めるときに作った履歴の値を使う（`gives_check` も同じ値。追加の計算なし）。差分更新の B1 は、指す前と後を 1 回の走査で追う。
5. **ライン表**（9.8 → 9.3 秒）：ラインのマス・マス数・軸をコンパイル時の表にした。

## 差分更新の新しい作り（数行）

`Game::push` は `set_view_squares(view, p, board, {from, to})` で両視点を写し、そのとき `ViewDelta`（変わったマス・指す前の駒・指す前の持ち駒）を得る。`update_accumulator(net, p, view, delta, acc)` は、A1・A2（変わったマス・持ち駒の差）、B1（変わったマスを通るライン上で、変わった区間を含む組。指す前は引き、後は足す）、B2（変わったマスに関係する桂。指す前は引き、後は足す）を集め、AVX2 で一度に足し引きする。`Game::pop` は戻した盤から変わったマスを写し直す。

## CPU の割り当て

- Linux：`/sys/devices/system/cpu/cpu*/topology/`（`physical_package_id`・`core_id`）で物理コアを数え、`sched_getaffinity` で使える論理 CPU を得る。固定は `pthread_setaffinity_np`。
- Windows：`GetLogicalProcessorInformationEx(RelationProcessorCore)` で物理コアを数え、`SetThreadGroupAffinity` で固定（`プロセッサグループ * 64 + ビット` を論理 CPU 番号にする）。`#ifdef _WIN32` で分けている。
- 固定する CPU：`CpuAffinity`（auto、または "0,2,4-7" の一覧）と `UsePhysicalCoresOnly`（物理コアごとに最も小さい番号の 1 つ）から決める。ワーカーは `cpus[(番号+1) % 個数]`、ジョブの駆動スレッド（メインの探索）は `cpus[0]` に固定する。
- 固定に失敗しても探索は続け、`info string` で知らせる。
- isready の後に `info string Nuenagon cpu: logical=… physical=… threads=… affinity=… physical_only=… cpus=…`（`readyok` の前）。
- 詰み探索の停止：`stop` を詰み探索の続行の合図に繋いだ（`go infinite` の直後の `stop` は 0.15 秒以内に応答）。決定的モードでは、根の詰み探索を時間で止めない。

## SIMD のビルド

- CMake オプション `NUENAGON_SIMD`（既定 ON）：ON は `-mavx2`（MSVC は `/arch:AVX2`）と `NUENAGON_SIMD_AVX2=1`、OFF は SIMD を使わないコード（`NUENAGON_SIMD_AVX2=0`）。
- isready の冒頭に `info string Nuenagon build: simd=avx2|scalar`。
- 注意：AVX2 は**コンパイル時**に決まる。AVX2 の無い CPU で ON のビルドを動かすと落ちる（実行時の判定はしていない）。

## 作ったファイル・変えたファイル

| ファイル | 役割 |
|---|---|
| `REPORT_N5.md` | 本報告 |
| `CMakeLists.txt` | `NUENAGON_SIMD` オプション、`platform/cpu_affinity.cpp` の追加 |
| `.gitignore` | `build_scalar/` の追加 |
| `src/platform/cpu_affinity.{h,cpp}` | 新規。CPU の構成の調べ方、CPU 一覧の選び方、スレッドの固定（Linux／Windows） |
| `src/eval/raypair/raypair.{h,cpp}` | `ViewDelta`、`set_view_squares`、`b1_feature`・`knight_feature`、ライン表（全計算の関数は変えていない） |
| `src/eval/raypair/network.{h,cpp}` | 差分更新の作り直し、AVX2 の足し引き、隠れ層1の `maddubs` |
| `src/board/game.{h,cpp}` | 両視点の盤面を持つ、`gen_legal`・`in_check_now`、push・pop の変更 |
| `src/board/bitboard.h` | 利きの方向リストを静的配列に、飛び駒の利きの短絡 |
| `src/search/search.{h,cpp}` | 指し手の作業領域、王手の再利用、詰み探索の停止、ThreadPool の固定、駆動スレッドの固定 |
| `src/main.cpp` | CPU の情報行、isready の順序、プールの作り直し条件、bench の固定 |
| `docs/DESIGN.md`・`tests/*n5*`・`tests/golden/*`・`PROTECTED.sha256`・`check_protected.py` | キット N5 の導入（手順 1。変更禁止ファイルはキットのものだけ） |

## 次の担当への引き継ぎ

1. **並べ替えの方式を変えれば強さは上がるかもしれない**（今回は木を変えないので触っていない）。例：駒の価値の表と MVV-LVA、静止探索で取る手だけを作る関数、`has_non_pawn_material` を 81 マスの走査から作る。対局で確かめる段階で。
2. **決定的モードでも、探索本体の時間制限は残している**（`go movetime` などは N4 のまま）。§E5.9 は「すべての打ち切りをノード数だけで決め、時間では止めない」なので、次で揃えること。
3. 今の時間配分（中盤の 1 局面、1 スレッド）：差分更新 約 14%、玉の手の合法性判定の利き計算 約 14%、指し手生成 約 9%。玉の手ごとに `Position` を複写して `do_move` して利きを見る（`generate_moves`）のが大きい。
4. 根の詰み探索は `go` のたびに 65,536 項目の表を作る（N4.1 の引き継ぎ 1 の件。まだ残っている）。
5. Windows の CPU 固定（`SetThreadGroupAffinity`）と、64 CPU を超えるプロセッサグループは、この環境ではビルドも実行もできていない（クロスコンパイラが無い）。Windows の実機で `test_n5_acceptance.py` 相当を確かめること（テストは Linux 専用）。
6. AVX2 は コンパイル時に決まる。実行時の判定（CPUID）は未実装。配布するビルドを分けるなら、そのとき決めること。
7. 静止探索の SEE、王手延長、詰み探索の打ち切りの扱いは、N4.1 の引き継ぎのまま（変えていない）。

## `python check_protected.py` の結果

`OK`
