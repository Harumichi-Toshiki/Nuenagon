# N5.5 報告：探索の改良（途中の結果。既定の 2 件のテストが未達）

## 結果：未完了

- `python -m pytest -q tests`（既定の設定）：**107 passed, 2 failed**（1184 秒）
  - 失敗 1：`test_depth_gain`：N5H 8.00 → 新 9.92（**+1.92**。+2.0 に 0.08 届かず）
  - 失敗 2：`test_long_mates_not_worse`：N5H 16 問 → 新 13 問（**退行**）
  - 成功：`test_strength_vs_n5h`（N5H に 0.60 以上）、`test_required_options`、`test_all_options_off_still_correct` ほか
- `python check_protected.py`：OK

## 手順1：キット N5.5r の導入

- zip は `main` にあり（作業ブランチには無い）、SHA-256 一致。KIT.json の 3 ファイルも一致。
- `tests/test_n55_acceptance.py`、`tests/golden/raypair_v0_trained.shk`、`PROTECTED.sha256` を導入。コミット「Install kit N5.5r」。

## 手順2：全部切った版を N5H と同じ動きに戻す

- 変えた点：並べ替え・静止探索の 5 つを `Search*` の切り替えにした（`SearchOrderSEE`、`SearchHistoryGravity`、`SearchCounterMove`、`SearchQSEE`、`SearchLazyLegal`）。切ると N5H と同じ処理になる。また、LMR の番号（N5 と同じ 1 始まり）、手を取った後の記録（成りを含む）も N5 に合わせた。
- 確かめ：全部切った版と N5H を、Deterministic・Threads=1・`go nodes 100000`、RayPair 学習済み評価で、`SPEED_POSITIONS` の 12 局面を探索。**12/12 で bestmove・info 行・ノード数・読み筋が完全一致**。
- 前回の「既定と全部切った版が 27勝33敗で同じ」は、オプションが届いていなかったためではない（届いていて、探索は変わる）。同じ結果になったのは偶然と判断しているが、確定はしていない。

## 手順3：手法ごとの効き目（学習済み RayPair、1 手 0.2 秒、全部切った版が相手）

単独（他は全部切った状態で、その手法だけ入れる）。60 局（30 開始局面 × 先後）。深さは 1 手 1 秒の平均（全部切った版 7.33、N5H 7.33）。

| 手法 | 深さ | 勝率 | 判定 |
|---|---|---|---|
| SearchRFP | 8.08 | **0.825** | 採用 |
| SearchLazyLegal | 7.67 | 0.633 | 採用 |
| SearchLMP | 7.58 | 0.600 | 採用 |
| SearchOrderSEE | 7.50 | 0.567 | 採用 |
| SearchSEEPrune | 7.50 | 0.567 | 採用 |
| SearchLMRTable | 7.58 | 0.533 | 採用（土台） |
| SearchCounterMove | 7.42 | 0.500 | 既定 false |
| SearchNullMoveDynamic | 7.50 | 0.492 | 既定 true のまま（後述） |
| SearchHistoryGravity | 7.33 | 0.483 | 既定 false |
| SearchContHistory | 7.25 | 0.483 | 既定 true のまま（後述） |
| SearchIIR | 7.58 | 0.483 | 既定 false |
| SearchImproving | 7.42 | 0.450 | 既定 false |
| SearchDropPrune | 7.50 | 0.450 | 既定 true のまま（後述） |
| SearchMate1Ply | 7.33 | 0.467 | 既定 true のまま（後述） |
| SearchQSEE | 7.42 | 0.417 | 既定 false |
| SearchRazoring | 7.67 | 0.233 | 既定 false |

土台（RFP・LMP・OrderSEE・SEEPrune・LazyLegal・LMRTable）：深さ **8.67**、全部切った版に対して **0.883**（53勝7敗、60 局）。

土台の上に 1 つずつ足した結果（60 局）：

| 追加 | 深さ | 勝率（土台に対して） |
|---|---|---|
| SearchNullMoveDynamic | 8.75 | 0.600 → 100 局で **0.500** |
| SearchMate1Ply | 8.75 | 0.583 → 100 局で **0.540** |
| SearchContHistory | 9.42 | 0.483 |
| SearchIIR | 9.50 | 0.417 |
| SearchHistoryGravity | 8.67 | 0.492 |
| SearchCounterMove | 8.83 | 0.483 |
| SearchDropPrune | 8.75 | 0.517 |
| SearchImproving | 8.58 | 0.483 |

※ 60 局の標準誤差はおよそ ±0.06。60 局の 0.6 が 100 局で 0.5 に落ちたものがあるため、60 局の結果は採否の判断に十分でない。

## 既定値の扱い（確定事項と未決の事項）

- 既定 false にしたもの：SearchIIR、SearchHistoryGravity、SearchCounterMove、SearchImproving、SearchQSEE、SearchRazoring（コミット済み）。
- 既定 true のまま（持ち主の指示。4 つ）：SearchNullMoveDynamic、SearchMate1Ply、SearchContHistory、SearchDropPrune。
  - 持ち主の指示：「土台＋その 4 つ」対「土台だけ」を 100 局で測り、0.45 を下回ったら止める。
  - 結果：**0.510（51勝49敗、100 局）**。0.45 を上回るので続行。
  - ただし `test_required_options` が、この 4 つの既定を true と要求するため、既定を false にできない。この 4 つが既定 true のままなのが、長い詰将棋の退行の要因の可能性が高い（未確認）。

## 最終のテスト（既定の設定）

- `test_depth_gain`：N5H 8.00 → 新 9.92（+1.92、未達）
- `test_long_mates_not_worse`：N5H 16 → 新 13（未達・退行）
- `test_strength_vs_n5h`：合格（N5H に 0.60 以上。数値はテスト出力に出ていない）

## 参考にしたもの

- やねうら王（YaneuraOu）の探索と指し手並べ替えの考え方（LMR の表、RFP、LMP、SEE、履歴、1手詰め）。**コードは読んでいない・写していない**（ソースを参照せず、設計書 §E11 の指示と、自分の実装で組んだ）。

## 次の担当への引き継ぎ

1. **既定 true の 4 つの扱いを、持ち主が決める。** テスト `test_required_options` の要求（8 つ既定 true）と、測った結果（この 4 つは効かない・既定 true のままだと長い詰将棋が退行する可能性）が衝突している。キットの作り直しが必要かもしれない。
2. 長い詰将棋の退行（16→13）の原因を確かめる（SearchMate1Ply と SearchDropPrune の既定 true が疑わしい。土台のみで測り直す）。
3. 深さ +2.0 に 0.08 届かない。SearchContHistory と SearchIIR は深さを上げる（9.42、9.50）が強さに繋がらなかった。深さと強さの関係を調べる。
4. 測定は 100 局以上で。60 局の結果は ±0.06 のずれがあり、採否に使えない。
5. テスト出力の強さの数値を見るには `pytest -s` で実行する。

## `python check_protected.py` の結果

`OK`

## 追記（N5.5f：詰将棋の退化の修正）

### 手順1：キット N5.5f の導入
- zip は `main` にあり、SHA-256 一致。KIT.json の 3 ファイルも一致。コミット「Install kit N5.5f」。
- 作業ブランチには zip が無いため、削除は無し（zip は `main` だけにある）。

### 手順2：詰将棋（9 手以上の 17 問、Threads=1、movetime 3000、学習済み RayPair）

直列で測った解けた数：

| 設定 | 解けた数 |
|---|---|
| N5H | 16 |
| 既定（修正前） | 13 |
| RFP を切る | 16 |
| LMP を切る | 14 |
| LMRTable・SEEPrune・ContHistory・Mate1Ply・DropPrune・IIR・Improving・Razoring・OrderSEE・HistoryGravity・CounterMove・QSEE・LazyLegal を1つずつ切る | 13（変化なし） |
| NullMoveDynamic を切る | 12 |

- 原因：**RFP（静的評価による早い打ち切り）**。玉が囲まれた局面では、静的評価が詰みを隠す。
- 試した直し方：
  - RFP の余裕を大きくする（SearchRFPMargin 300・500）→ 14 問。部分的にしか直らない。
  - 相手の玉の逃げ道が1つ以下のときだけ RFP を使わない（`SearchRFPGuard`）→ 13 問。効かない（取りこぼす局面は玉の逃げ道が多い）。
  - **RFP を深さ 3 以下だけで使う（`SearchRFPDepth`）→ 16 問**（深さ 1 でも 16 問）。

### 最終の既定値
- RFP の深さ：**3**（前は 7）。
- 100 局（先の決定：既定の 4 つ（NullMoveDynamic・Mate1Ply・ContHistory・DropPrune）は既定 true のまま）：RFP 深さ 3 対 7 は **0.560（56 勝 44 敗）**。深さ（1 手 1 秒）は 9.83 対 9.92（差 0.09）。
- その他の既定：RFP・LMP・OrderSEE・SEEPrune・LazyLegal・LMRTable・RFPGuard・NullMoveDynamic・Mate1Ply・ContHistory・DropPrune は true。IIR・Improving・Razoring・HistoryGravity・CounterMove・QSEE は false。
- 新しい `Search*` オプション：`SearchRFPGuard`（既定 true。効果なし。残すかは持ち主が決める）、`SearchRFPDepth`（既定 3）。

### テスト
- `python -m pytest -q tests`：**109 passed**（1239 秒）。深さ・強さ・詰将棋の各テストを含む（テスト出力の数値は `-s` が無いため省略）。
  - 手測の深さ（1 手 1 秒、12 局面）：N5H 8.00 対 新 9.83（+1.83。基準 +1.5）。
- `python check_protected.py`：OK

### 引き継ぎ
1. 「既定 true の 4 つ」（NullMoveDynamic・Mate1Ply・ContHistory・DropPrune）は、測定で効かないか弱い。テスト `test_required_options` が既定 true を要求するため、そのまま残してある。持ち主が判断する。
2. `SearchRFPGuard` は効果が無かった。削除するか、持ち主が決める。
3. 測定の数値は 100 局以上で。60 局の結果は ±0.06 のずれがある。
4. 詰将棋の確認は直列で行うこと（4 並列だと 3 秒の制限で取りこぼす）。

## `python check_protected.py` の結果

`OK`
