# 探索の調整の手順（対局だけで行う）

この文書は、コードを変えずに、対局と測定の道具だけで探索を調整する手順。オプションの一覧は `docs/SEARCH_PARAMS.md`。

## 道具

| 道具 | 何をするか |
|---|---|
| `tools/match_nodes.py` | 2 つの USI エンジンを、ノード数（`go nodes N`）で対局させる。勝率と誤差（±2σ）を出す |
| `tools/count_mates_nodes.py` | 長い詰将棋（既定は 9 手以上 17 問）を、ノード数で解かせて数える |
| `tools/check_default_equiv.py` | 既定の設定で、新版の探索が基準と完全に同じかを確かめる（12 局面、info と bestmove を比べる） |
| `tools/n8_stats.py` | 12 局面の探索統計（深さ・置換表・枝刈りの割合など）。`--nodes` で固定、`--opts` でオプション |
| `tools/usi_util.py` | 上の道具が使う USI の部品（直接使わなくてよい） |

すべて、エンジンの名前に依らず、実行ファイルのパスで指定する。

## 準備

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
python3 tools/check_default_equiv.py --ref build/nuenagon --new build/nuenagon --nodes 100000
```

同じ実行ファイル同士なので、12/12 が出るはず（基準の版と違う版で比べるときは、`--ref` に基準の版を指定する）。

## 評価関数

既定は `tests/golden/material_v0.shk`（テストの既定）。別の評価関数で測るときは、`--common-opts` で指定する（例：`--common-opts "EvalFile=/path/to/nn.bin,FV_SCALE=13"`）。評価関数の選び方は、結果を分けて報告すること。

## 手順 1：オプションを 1 つ変えた版と既定の版を対局させる

```bash
python3 tools/match_nodes.py \
  --a build/nuenagon --a-opts "SearchP_RFPImproving=40" --a-nodes 100000 \
  --b build/nuenagon --b-opts "" --b-nodes 100000 \
  --games 200 --workers 3 --out results/rfpimp40.jsonl
```

- `--out` に 1 局ごとに追記する。途中で止まっても、同じコマンドで続きから指す。
- 結果の表示：`A 対 B：200 局 A 95 勝 104 敗 1 分 勝率 0.477 誤差（±2σ）±0.07`。
- 途中経過だけ見るには `--summary-only` を付ける。
- 200 局の誤差は、約 ±0.07（±2σ）。0.07 より小さい差は、200 局では見分けられない。差を確かめるなら 600 局以上にする。
- 対局は、A と B で 1 手の時間（ノード数）が同じになるように、両方 `100000` にする。

## 手順 2：深さの効きを測る（ノードを倍にした自分自身との対局）

「1 段深く読むと、どれだけ勝つか」を測る。

```bash
python3 tools/match_nodes.py \
  --a build/nuenagon --a-nodes 200000 \
  --b build/nuenagon --b-nodes 100000 \
  --games 200 --workers 3 --out results/double_nodes.jsonl
```

勝率が 0.5 より大きければ、ノードを倍にした版（約 1 段深い）が強い。その値が、探索の強さを上げる仕組みの「価値」の物差しになる。

## 手順 3：深さとノード数を測る（静かに効果を見る）

ノード数で止めると、結果は決定的になる（同じ版・同じ設定なら、毎回同じ）。

```bash
python3 tools/n8_stats.py --nodes 1000000 --exe build/nuenagon --opts "SearchP_LMRBase=85"
```

平均の深さ（`depth=`）と、枝刈りの割合（`fh1rate`・`tt_cut`・`nmp_cut`）が見られる。深さが伸びても、強さが伸びるとは限らない（N8.3・N8.5 の報告を参照）。深さは、必ず対局の結果と合わせて判断する。

## 手順 4：長い詰将棋を減らしていないかを確かめる

```bash
python3 tools/count_mates_nodes.py --exe build/nuenagon --opts "SearchP_..." --nodes 300000
```

既定の版の数（17 問中の解けた数）より減っていたら、その枝刈りは詰みを壊している。長い詰将棋の数は、どの調整でも減らさないこと。

## 手順 5：新しい設定を既定にするかの判断

1. 手順 1 で、既定に対して 600 局以上で勝率が 0.5 を明確に超える（誤差を超える）。
2. 手順 3 で、深さ・ノード数が想定どおり。
3. 手順 4 で、長い詰将棋が減っていない。
4. 別の評価関数（`--common-opts`）でも、同じ方向の結果になる。

すべて満たしたら、既定値を変える（`src/search/search.h` の `Options` の既定値）。既定を変えたら、`tools/check_default_equiv.py` の結果も、変えた理由と一緒に記録する。

## 注意

- 対局は、ノード数で止める（`--a-nodes` など）。時間で止めると、負荷に依って結果がぶれる。
- 対局の並列数（`--workers`）は CPU の数までにする。並列数を増やしても結果は変わらないが、計測時間が縮むだけ（ノード固定で、決定的モード）。
- 1 つのオプションを変えるたびに、既定と比べる。複数を同時に変えると、どれが効いたか分からない。
