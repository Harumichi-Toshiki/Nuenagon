# N8 報告：探索の深化（singular・ProbCut・連続履歴・静的評価の補正など）

## 結果：未完了（合格条件を満たさない）

- `python -m pytest -q tests`（既定の設定、Release）：**153 passed, 3 failed**（2193.7 秒）
  - 失敗 1：`test_n8_acceptance.py::test_n8_depth_gain`。深さ（1 手 1 秒、12 局面）が N7 9.83 → 新 9.75（条件は +2.0 以上）。
  - 失敗 2：`test_n8_acceptance.py::test_n8_strength_vs_n7`。N7 に対して 100 局、勝率 **0.530**（53 勝 47 敗。条件は 0.65 以上）。
  - 失敗 3：`test_n55_acceptance.py::test_long_mates_not_worse`。長い詰将棋が N5H 16 → 新 15。**同じテストを単独で実行すると合格**（49.95 秒）。直接の計測（同じ既定の版）は 2 回とも 16 問。3 秒の制限で境界の 1 問が時々ぶれるためと見ている（未確認）。
  - 合格：`test_n8_long_mates_not_worse`（N7 16 問 以上）、`test_n8_required_options`、`test_n8_all_options_off_still_correct`、N1〜N7 の残り。
- `python check_protected.py`：**OK**（コミットごとに実行）。
- 全部の N8 手法を切った版は、N7 と決定的モードで **完全に同じ探索**（12 局面、`go nodes 100000`、bestmove・info・ノード数が 12/12 一致。`tools/n8_tools.py det`）。

## 手順 1：キット N8 の導入

- `nuenagon_kit_n8.zip` の SHA-256 は `c851c798…46be` と一致。外に展開し、`KIT.json` の 2 ファイルの SHA-256 も一致。`PROTECTED.sha256` と `tests/test_n8_acceptance.py` だけをコピーした（コミット `8f862f9`「Install kit N8」）。一番上の zip は削除した。

## 作った手法（すべて `Search*` のオプション。既定は測定で決めた）

| オプション | 中身 |
|---|---|
| `SearchSingular`（`SearchSingularMargin`） | 置換表の手が他より明らかに良いとき 1 手延長。multi-cut と負の延長を含む |
| `SearchProbCut`（`SearchProbCutMargin`） | 取る手（SEE ≥ 0）の浅い探索で β + 余裕 を超えたら返す |
| `SearchLMRHistory` | LMR をカットノードで 1 段増やし、履歴の効きを細かくする |
| `SearchContLearn` | 連続履歴を学習する。**N7 では ContHistory を読むだけで、一度も書いていなかった**（HistoryGravity が入っているときだけ書いていた） |
| `SearchContDeep` | 連続履歴に 4 手前（1・2・4 手前）も使う |
| `SearchCapHist` | 取る手の履歴（攻め駒・取られる駒の種類ごと）で並べ替える |
| `SearchCorrHist`（`SearchCorrRate`） | 玉の位置（自玉・相手玉）ごとに、探索の結果と静的評価のずれを覚えて補正する |
| `SearchCheckExt` | 王手になる手を 1 手延長する |
| `SearchKingDanger` | 相手の玉の逃げ道が 1 つ以下の節点で、LMR と静かな手の枝刈りを使わない |
| `SearchQCheck` | 静止探索の最初の 1 手で、王手になる静かな手（SEE ≥ 0）も読む（**棄却**） |
| `SearchDropLate` | 打つ手を、他の静かな手の後ろに並べる（**棄却**） |
| `SearchSingularDouble` | singular の延長を、差が大きいとき 2 手にする（**測っていない**） |

## 測定（N7 に対して、1 手 0.2 秒、Threads=1、学習済み RayPair、テストと同じ開始局面）

### 単独の効き目（他は切った状態。深さは 1 手 1 秒・12 局面の平均）

| 手法 | 深さ（N7 比） | 勝率（60 局） | 勝率（100 局） | 判定 |
|---|---|---|---|---|
| SearchCorrHist | +0.08 | 0.575 | 0.640 → 再測定 0.560（下の注を参照） | 既定 true |
| SearchContLearn | 0.00 | 0.583 | 0.560 | 組み合わせで上回らず、既定 false |
| SearchProbCut | +0.42 | 0.517 | 0.560 | 組み合わせで悪化、既定 false |
| SearchKingDanger | +0.17 | 0.483 | — | 既定 false |
| SearchCheckExt | −1.25 | 0.517 | — | 既定 false |
| SearchLMRHistory | −0.33 | 0.450 | — | 既定 false |
| SearchCapHist | 0.00 | 0.450 | — | 既定 false |
| SearchSingular | −0.50 | 0.433（余裕 3）、0.467（余裕 6） | — | 既定 false |
| SearchContDeep（ContLearn と） | −0.08 | 0.583（ContLearn と同じ） | — | 既定 false |
| SearchDropLate | — | 0.483 | — | 棄却 |
| SearchQCheck | −1.08 | 0.250 | — | 棄却 |

注：深さは時間固定の測定なので、同じ基準でも ±0.3 ぶれる（N7 の値は 9.83〜10.08）。

### 積み上げ（100 局、N7 に対する勝率）

| 構成 | 勝率（100 局） | 備考 |
|---|---|---|
| CorrHist（既定の rate 64） | **0.640** | 1 回目。同じ設定の 2 回目は 0.560、フルテストの中では 0.530 |
| CorrHist（rate 32） | 0.500 | |
| CorrHist（rate 128） | 0.540 | |
| CorrHist ＋ RFP 深さ 5 | 0.450 | 深さは 1 秒で +0.50、1 手 1 秒で増えても強くならない（詰みも 1 問減る） |
| CorrHist ＋ ContLearn | 0.560 | |
| CorrHist ＋ ProbCut | 0.515 | |
| ContLearn ＋ CorrHist ＋ ProbCut | 0.560 | |
| ContLearn | 0.560 | |
| ProbCut | 0.560 | |

- CorrHist 単独の 3 回の平均は 0.577（300 局）。標準誤差は約 0.03。ただし同じ設定の 2 回で 0.08 ぶれたので、100 局では 0.05 程度の差しか分からない。
- 組み合わせは、どれも CorrHist 単独の上を行かなかった。

### 100 万ノードでの深さ（12 局面の平均、`go nodes 1000000`）

| 版 | 平均 | 各局面 |
|---|---|---|
| N7 | 10.92 | 12 13 13 13 11 10 7 8 12 12 9 11 |
| 新（既定） | **11.50（+0.58）** | 13 12 14 16 11 9 8 9 12 11 11 12 |

## 最終の既定値

- 新しい手法：`SearchCorrHist` だけ **true**（`SearchCorrRate` 64、補正の幅 ±300 cp、更新の重み min(深さ, 8) / 64）。他はすべて false。
- 既存の値は N5.5 のまま（`SearchLMRTable`・`SearchRFP`（深さ 3）・`SearchNullMoveDynamic`・`SearchLMP`・`SearchSEEPrune`・`SearchContHistory`・`SearchMate1Ply`・`SearchDropPrune`・`SearchOrderSEE`・`SearchLazyLegal` は true、`SearchIIR`・`SearchImproving`・`SearchRazoring`・`SearchHistoryGravity`・`SearchCounterMove`・`SearchQSEE`・`SearchRFPGuard` は false）。
- 新しい spin：`SearchSingularMargin` 3、`SearchProbCutMargin` 200、`SearchCorrRate` 64。

## 注意・引き継ぎ

1. **合格条件のうち 2 つは満たしていない**（深さ +2.0、勝率 0.65）。個々の手法の効き目は小さく（+0.0〜+0.1 の勝率）、組み合わせでも伸びなかった。深さだけなら RFP の深さを 5 にすると 1 秒で +0.5 伸びるが、強さは 0.450 に落ちた。
2. **詰みのテストは時間で決まるため、ぶれる**（前述の失敗 3）。`SearchCorrHist` の既定では、直接の計測で N7 と同じ 16 問。
3. `SearchContLearn`：N7 の ContHistory は書き込みが無く、実質的に働いていなかった（この修正は既定 false のまま）。
4. `SearchQCheck` が大きく負けた理由は確かめていない（静止探索の枝の増加か、評価の偏りか）。
5. 測定の誤差：同じ設定の 100 局を 2 回で勝率が 0.08 変わった。100 局では 0.05 未満の差は判断できない。
6. 未着手：入玉の扱い、3 手詰めの判定、`SearchSingularDouble` の測定、`SearchDropLate` の別の形、玉の危険度に応じた枝刈りの調整。
7. 測定の道具：`tools/n8_tools.py`（`det`・`depth`・`match`）。対局は `tests/test_n8_acceptance.py` の開始局面と同じもの。

## 最終のコミット

- `8f862f9`：Install kit N8（手順 1）
- `0eb991d`：Add N8 search options (all default off)
- `102d203`：Set SearchCorrHist default to true
- この報告のコミット：REPORT_N8.md を追加（`git log` の最新）
