# 探索の数値の一覧（SearchP_…）

探索の中で直接書かれていた数値を、USI の spin オプションにしたもの（N8.5）。**既定値は、以前に直接書かれていた値そのもの**なので、既定のままなら探索は以前と完全に同じになる（`tools/check_default_equiv.py` で確かめられる）。

- 名前は `SearchP_` で始まる（RFP2 の詰み守りだけ `SearchRFP2Guard`）。
- 範囲（min・max）は、調整で使えそうな広めの値。範囲外は受け付けない。
- 小数の係数は、100 倍の整数で持つ（例：LMR の係数 0.75 は `SearchP_LMRBase = 75`）。
- 値は探索の開始時（Job の作成時）に構造体へ写して使うので、探索の速さは変わらない。LMR の表だけは、開始時に一度作る。
- 「残り深さ」は、その手を指す節点の残り深さ（`depth`）。

## 枝刈り（前向きの枝刈り）

| オプション | 既定 | 範囲 | 意味 | 効く深さ |
|---|---|---|---|---|
| `SearchP_RFPImproving` | 60 | 0〜300 | RFP の余裕に足す値（改善している局面） | 残り深さ ≤ SearchRFPDepth |
| `SearchP_RFP2Improving` | 60 | 0〜300 | RFP2 の同上 | 残り深さ ≤ SearchRFP2Depth |
| `SearchP_RazorDepth` | 2 | 0〜6 | razoring を使う最大の残り深さ | 残り深さ ≤ この値 |
| `SearchP_NMPDepth` | 3 | 1〜10 | null move を試す最小の残り深さ | 残り深さ ≥ この値 |
| `SearchP_NMPBase` | 2 | 0〜10 | null move の削減量 = base + depth / div | SearchNullMoveDynamic が off のとき |
| `SearchP_NMPDiv` | 4 | 1〜16 | 上の除数 | 同上 |
| `SearchP_NMPDynBase` | 3 | 0〜10 | 削減量 = base + depth / dyn_div + min(cap, (評価−β) / scale) | SearchNullMoveDynamic が on のとき |
| `SearchP_NMPDynDiv` | 3 | 1〜16 | 上の除数 | 同上 |
| `SearchP_NMPDynCap` | 3 | 0〜10 | 上の評価差の項の上限 | 同上 |
| `SearchP_NMPDynScale` | 200 | 1〜2000 | 上の評価差の項の割り（点数） | 同上 |
| `SearchP_NMPVerifyDepth` | 12 | 1〜30 | この残り深さ以上で、null move の後に検証探索をする | SearchNullMoveDynamic が on のとき |
| `SearchP_FutDepth` | 3 | 0〜8 | futility と手の数（LMP が off のとき）の枝刈りの最大の残り深さ | 残り深さ ≤ この値 |
| `SearchP_MoveCountBase` | 6 | 0〜30 | 手の数の枝刈り：legal ≥ base + scale × depth² で刈る | 残り深さ ≤ SearchP_FutDepth、LMP が off のとき |
| `SearchP_MoveCountScale` | 3 | 1〜10 | 上の係数 | 同上 |
| `SearchP_LMPDepth` | 8 | 1〜20 | LMP の最大の残り深さ | 残り深さ ≤ この値（SearchLMP が on のとき） |
| `SearchP_LMPBase` | 3 | 0〜10 | LMP の閾値 = SearchLMPScale × (base + depth²) / div | SearchLMP が on のとき |
| `SearchP_LMPDiv` | 2 | 1〜4 | 改善していない局面での除数（改善しているときは 1） | 同上 |
| `SearchP_Fut2Depth` | 12 | 0〜30 | 親の futility（SearchFut2）の最大の残り深さ | 残り深さ ≤ この値 |
| `SearchP_Fut2Base` | 100 | −1000〜1000 | 親の futility の余裕 = base + slope × depth + 履歴（±clamp） | SearchFut2 が on のとき |
| `SearchP_Fut2Slope` | 120 | 0〜500 | 上の深さの係数 | 同上 |
| `SearchP_Fut2Clamp` | 300 | 0〜2000 | 上の履歴の項の上限（±） | 同上 |
| `SearchP_HistPruneDepth` | 3 | 0〜10 | 連続履歴による刈り込み（SearchHistPrune）の最大の残り深さ | 同上 |
| `SearchP_SEEPruneDepth` | 6 | 0〜12 | SEE による枝刈りの最大の残り深さ | SearchSEEPrune が on のとき |
| `SearchP_SEEPruneCapMargin` | 100 | 0〜1000 | 取る手の SEE の下限 = −margin × depth | 同上（静かな手の下限は SearchSEEMargin） |
| `SearchP_IIRDepth` | 4 | 1〜12 | 内部の反復深化（SearchIIR）を使う最小の残り深さ | SearchIIR が on のとき |
| `SearchP_SingularDepth` | 7 | 1〜20 | singular の延長を試す最小の残り深さ | SearchSingular が on のとき |
| `SearchP_SingularTTDepth` | 3 | 0〜10 | 置換表の深さが、残り深さからこの値以内のときだけ試す | 同上 |
| `SearchP_ProbCutDepth` | 5 | 1〜20 | ProbCut を使う最小の残り深さ | SearchProbCut が on のとき |
| `SearchP_ProbCutReduce` | 4 | 1〜12 | ProbCut の深い探索で減らす深さ（保存は、この値 − 1 を減らした深さ） | 同上 |

## 手の並べ替え・LMR・履歴

| オプション | 既定 | 範囲 | 意味 | 効く場所 |
|---|---|---|---|---|
| `SearchP_LMRBase` | 75 | 0〜200 | LMR の表の係数（÷100。75 は 0.75） | SearchLMRTable・SearchLMR2 |
| `SearchP_LMRDiv` | 225 | 50〜1000 | LMR の表の除数（÷100。225 は 2.25） | 同上 |
| `SearchP_LMRHistClamp` | 2 | 0〜8 | 履歴による LMR の増減の上限（±） | 同上（SearchLMRHistory・SearchLMR2） |
| `SearchP_LMRLegal` | 4 | 1〜20 | 簡易の LMR（SearchLMRTable が off のとき）を使い始める手の順番 | 同上 |
| `SearchP_LMRLegal2` | 7 | 1〜30 | 簡易の LMR で 2 段減らす手の順番 | 同上 |
| `SearchP_HistBonusCap` | 1600 | 1〜16384 | 履歴のボーナス・減点の上限（depth² の上限） | 静かな手・連続履歴・取る手の履歴 |
| `SearchP_HistMax` | 1048576 | 1000〜16777216 | 履歴の値の上限（減衰なしの加算のとき） | SearchHistoryGravity が off のとき |

## 窓・評価の補正

| オプション | 既定 | 範囲 | 意味 | 効く場所 |
|---|---|---|---|---|
| `SearchP_AspDepth` | 4 | 1〜20 | aspiration window を使う最小の深さ | 反復深化（根） |
| `SearchP_AspDelta` | 20 | 1〜500 | aspiration window の初期の幅（点数） | 同上 |
| `SearchP_AspMaxDelta` | 500 | 50〜32000 | この幅を超えたら、窓を全幅にする | 同上 |
| `SearchP_CorrDepthCap` | 8 | 1〜32 | 静的評価の補正（SearchCorrHist）の学習に使う深さの上限 | SearchCorrHist が on のとき |

## RFP2 の詰み守り

| オプション | 既定 | 範囲 | 意味 |
|---|---|---|---|
| `SearchRFP2Guard` | 0 | 0〜2 | 0：今の条件（玉の近くに王手になりうる手が無く、1 手詰めも無いこと）。1：手番の側が手を指さずに相手に手番を渡したとき、相手に 1 手詰めが無いこと（脅しを見る）。2：1 に加えて、詰みの点数に近い β（|β| ≥ 詰みの値 − 500）を除く |

## 既存の数値（以前から spin だったもの）

以前から USI の spin になっている数値は、`src/main.cpp` の `kSearchSpins` を見る（例：`SearchRFPMargin`・`SearchRFPDepth`・`SearchFutilityMargin`・`SearchLMPScale`・`SearchSEEMargin`・`SearchQSEEMin`・`SearchProbCutMargin`・`SearchSingularMargin`・`SearchCorrRate`・`SearchTTKeepDeep` など）。

## 変えないもの（今回は option にしていない）

- 履歴の減衰の上限（`kHistLimit` = 16384）、静止探索の王手の上限（`SearchQCheckMax` で別にある）、詰み・評価の上限（`kMate`・`kMateCp`）、置換表の形式。
- 手の並べ替えの定数（取る手の点数の係数など）。
