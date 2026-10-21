# REPORT_N4（N4 前半）

## 結果

**中断**（停止条件 4：強さのテスト `test_beats_baseline` が安定して通らない）

- 全テスト：77 件中 76 件が通過。失敗は `test_beats_baseline` の 1 件だけ。
- 強さのテストの結果（10 局、対戦相手は `tests/n4_baseline.py`、1 手 500ms）
  - 全体実行 1 回目：エンジン 9 勝・対戦相手 1 勝・引き分け 0
  - 単独実行 1 回目：10 局とも勝ち（通過）
  - 単独実行 2 回目：エンジン 7 勝・対戦相手 3 勝・引き分け 0（失敗）
  - 単独実行 3 回目：未実施（上の 2 回目の後で打ち切り）
- 探索の不具合は直していない。負けた局の原因は未調査。
- 中断の判断：強さのテストは「負けなし」が条件。3 回中 2 回で負けが出ており、探索の不具合（または手の選び方の弱さ）を疑う必要があるが、ここまでで原因を特定できていない。

## `python -m pytest -q tests` の最終出力（全体実行）

```
FAILED tests/test_n4_acceptance.py::test_beats_baseline - AssertionError: 簡...
1 failed, 76 passed in 271.70s (0:04:31)
```

内訳：N1〜N3 は 62 件すべて通過（単独実行：`62 passed in 20.16s`）。N4 は 15 件中 14 件通過。

## bench（`bench [スレッド数]`、深さ 6 の固定 4 局面の合計）

| スレッド | ノード数 | NPS |
|---|---|---|
| 1 | 952,562 | 207,892 |
| 4 | 1,598,452 | 534,778 |

測った環境：4 論理 CPU、Intel Xeon 2.10GHz（クラウドの container）。速度は合格条件ではない（高速化は N5）。目標の 250 万 NPS には届いていない。

## 探索の構成（使った手法）

- 反復深化、aspiration window（深さ 4 以上、窓 ±20 から倍々に広げる）、PVS
- 置換表：全ジョブ共有、世代つき、キーXOR データのロックなし構造、詰みは根からの手数に換算して保存
- null move pruning（深さ 3 以上、R = 2 + 深さ/4）
- LMR（静かな手、3 手目以降。6 手目以降は 2 段）、王手延長（1 本の読み筋につき 3 回まで）
- 静止探索：駒を取る手のみ。王手を受けている局面では全回避手（ただし静止探索の手数は 16、王手の応酬は 4 手まで）
- 指し手の並べ替え：置換表の手 > 駒を取る手（MVV-LVA）> ヒント > キラー > 履歴
- 千日手（`Game::repetition()`）、最大手数（`MaxPly`）、`DrawValue`、入玉宣言（`bestmove win`、ジョブの開始時に判定）
- 詰み：評価は詰みの値 31000 − 手数の形。cp は ±29000 に制限。根で df-pn を 30 万ノードまで使い、見つかればその手で終える
- Lazy SMP：helper は奇数番号のスレッドで偶数段を飛ばす。Deterministic 時は helper なし
- 時間管理：movetime、持ち時間＋加算（持ち時間/25 + 加算×3/4）、秒読み（時間＝秒読み−100ms の余裕）、`NetworkDelay` を差し引く。`ponder` は ponderhit まで時間を計らない

## ジョブ・スレッドプール・ルートの手ごとの記録（作り）

- ジョブ（`search::Job`）：局面（`Game` の複製）、制限（`Limits`）、設定（`Options`）、置換表、ヒントの表、スレッドプールを持つ。USI の `go` は専用スレッド（driver）で 1 ジョブとして動かし、終わったら driver が `bestmove` を出す。`stop` と `ponderhit` はジョブのフラグと締切を変える。
- スレッドプール（`search::ThreadPool`）：`Threads − 1` 本の helper スレッドを作り、ジョブの helper 作業をキューで受ける。キューは FIFO で、優先度は未実装。
- ルートの手ごとの記録（`search::RootMove`）：`move`、`score`、`bound`、`depth`、`seldepth`、`pv`、`nodes` を持つ。反復ごとに並べ替え、MultiPV（`multipv k` の info を出す）と `searchmoves` はこの上に作った。

## 後半（N4 後半）で足す機能の「受け口」の場所

- ジョブ：`src/search/search.h` の `search::Job`。複数ジョブの同時実行は、driver とプールの仕組みで形は作ってあるが、USI からは 1 つずつしか起動していない（同時実行は未検証）。
- 優先度・スレッドの割り当て：`ThreadPool::submit` のキューに優先度を入れる場所（未実装）。
- ヒントの表：`search::HintTable`（`set` / `find` / `clear`、上限 65,536 局面で古いものから捨てる）。並べ替えと LMR（確率 0.10 以上の手は減らさない）で参照している。外から設定する USI/DJP の命令はまだない。
- パスの探索：`Job` のコンストラクタの `null_root` 引数。立てると根でパスしてから探索する（王手中はエラーにせず `resign` を返す。DJP のエラー応答は未実装）。
- 置換表の世代：`TranspositionTable::new_generation()`（`go` のたびに進む）。パス済みの印（ハッシュへの混ぜ込み）は未実装。
- 出来事の通知：`Reporter`（`search.cpp` の無名名前空間）。`info string event best_move_changed`、`score_swing`（150cp 以上）、`mate_found` / `mated_found` を出す。脅威（threat）は未実装。
- 決定的モード：`Deterministic`（helper なし）。ただし時間制限を外してノード数だけで止める仕組みは未実装（テストは `go nodes N` で確認）。

## 作ったファイル・変えたファイルの一覧

- `src/search/search.h`：探索の宣言（Limits、Options、RootMove、HintTable、TranspositionTable、ThreadPool、Job）。
- `src/search/search.cpp`：探索本体、置換表、スレッドプール、ジョブ、報告。
- `src/main.cpp`：USI の応答（全オプション）、`go`、`stop`、`ponderhit`、`usinewgame`、`gameover`、`bench`、`isready` で置換表とスレッドプールを作る。N1〜N3 の命令は変えていない。
- `src/mate/dfpn.h`：詰みの手数を返す `get_mate_length()` を追加（探索結果は変えていない）。
- `CMakeLists.txt`：`search.cpp` の追加、Threads ライブラリのリンク。
- 取り込み（手順 1）：`docs/DESIGN.md`（v1.3）、`PROTECTED.sha256`、`tests/test_n4_acceptance.py`、`tests/n4_baseline.py`、`tests/golden/material_v0.shk` と、直下の `nuenagon_kit_n4.zip` の削除。
- `REPORT_N4.md`：この報告。

## 次の担当への引き継ぎ事項

1. `test_beats_baseline` が通らない。最初にやること：負けた局の棋譜を取る（`play_game` の手を出力）。探索の不具合（評価の符号、千日手の扱い、静止探索の穴）と、手の選び方の弱さのどちらかを切り分ける。対戦相手は 2 手読みなので、探索が 2 手より浅くならないか（時間の 500ms で深さ 5 程度）も確かめる。
2. 速度は 20 万 NPS（1 スレッド）。目標の 250 万には遠い。N5 で高速化する前に、正しさを先に固める。
3. 時間管理：秒読み・持ち時間の式は簡易版。電竜戦の本番条件（先手 3 分＋2 秒、後手 10 分＋2 秒）での時間切れの確認は未実施。
4. 未実装：`CpuAffinity`・`UsePhysicalCoresOnly` は受け付けて保持するだけ（N5）。ジョブの優先度、複数ジョブの同時実行の検証、パスした局面の置換表の印、脅威の検出、DJP。
5. 読み筋の `info` の `nodes` は 1024 ノード単位で共有へ足すので、helper のノードは終わりに足す（報告は main の分のみ正確）。
6. `Deterministic` は helper を止めるだけ。時間ではなくノード数で制限する点は、テストの `go nodes` で確認している。

## `python check_protected.py` の結果

`OK`
