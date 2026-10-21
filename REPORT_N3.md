# N3 報告：重みの読み込み・量子化・推論・差分更新

## 結果

**完了**。受け入れテストは全部通った。`python -m pytest -q tests` の結果は 62 件（N1 23 ＋ N2 28 ＋ N3 11）すべて成功。`python check_protected.py` は `OK`。

## テストの最終出力

```
..............................................................           [100%]
62 passed in 22.63s
```

## 速さ（合格条件ではない）

測った環境：Intel(R) Xeon(R) Processor @ 2.10GHz、論理 CPU 4 個、Linux。Release ビルド。`evalbench 200000`（局面は初期局面から6手進めた後。合法手を順に指して戻す）。

| 項目 | v0（RayPair_v0、W=128、L1=16、L2=16） | v1（RayPair_v1、W=128、L1=32、L2=32） |
|---|---|---|
| 差分更新1手（両視点、`push`） | 約 3.35 μs | 約 3.35 μs |
| 戻す（`pop`） | 約 0.06 μs | 約 0.05 μs |
| 評価1回（隠れ層以降、`forward_raw`） | 約 0.67 μs | 約ー 1.57 μs |

参考：全計算は N2 の報告で約 15.6 μs（両視点・v0）。差分更新はそれより速い。

## 差分更新の仕組み

- **単位**：`Game::push` で、指す前と指した後の盤面を視点ごと（先手視点・後手視点）に `ViewBoard` にして比べる。駒の種類・有無が変わったマス（移動元・移動先・取られた駒のマス）が「変わったマス」になる。これは §E3.2 の「駒が変わったマス」と同じ。
- **A1・A2**：変わったマスの駒を引き、新しい駒を足す（`piece_feature`）。持ち駒は枚数が a → b のとき、増えた分の `k = a..b−1` を足し、減った分の `k = b..a−1` を引く（`hand_feature`）。
- **B1（ライン）**：変わったマスの 4 本のライン（`kLinesOf`）を集め、重複を除く（ライン番号の配列で 1 回だけ）。対象のラインは、指す前の盤面の B1 を全部引き、指した後の盤面の B1 を全部足す（`append_line_b1`）。
- **B2（桂）**：指す前の B2 を全部引き、指した後の B2 を全部足す。
- **戻し方**：蓄積値は `Game` が `acc_` というスタックで持つ。`push` で 1 段積み、`pop` で 1 段捨てる。差分を逆にかけるのではなく、スタックから戻すので確実。
- **null move**：盤面は変えず、手番だけ入れ替える（`push_null`）。蓄積値は視点ごとなので変わらない。手番が変わるので、評価では `acc[stm]` を先に連結する。`pop` で手番・ハッシュ・ビットボードを戻す。
- **千日手**：null の局面は履歴に入れるが、千日手の比較は null より前の局面に及ばない（直近の null で区切る。§E5.6「パスを1手として数えない」の解釈。下の「解釈」を参照）。

## 量子化・推論

- 読み込み時に、§F4 の検査（マジック・形式バージョン・予約領域・本体の長さ・ファイルの長さ・CRC32・特徴セット／ネットワーク／次元数／W／L1／L2 の組み合わせ）をすべて行う。CRC32 は表を自分で作った（IEEE 802.3）。
- 量子化は倍精度で掛け、0 から遠い方への四捨五入（`copysign(floor(|x|+0.5), x)`。参照実装と同じ式）。
- 推論は、int16 の蓄積値を `clamp(0, 127)` で uint8 にし、隠れ層は int32 の積和 ＋ bias を算術右シフト 6、`clamp(0, 127)`。最終出力 raw は int32。
- 評価値 `cp = round(raw × eval_scale / 8128)`（倍精度）。

## 作ったファイル・変えたファイル

| ファイル | 役割 |
|---|---|
| `src/eval/raypair/network.h` / `network.cpp` | 重みの読み込み・検査・量子化（`load_network`）、全計算（`full_accumulator`）、差分更新（`update_accumulator`）、推論（`forward_raw`）、cp への変換（`cp_of`） |
| `src/eval/raypair/raypair.h` / `raypair.cpp` | 追加：A1・A2 の 1 個ぶんの特徴のインデックス（`piece_feature`、`hand_feature`）。既存の出力は変えていない |
| `src/board/game.h` / `game.cpp` | 追加：蓄積値のスタックと差分更新（`push`・`pop`）、null move（`push_null`）、重みの設定（`set_network`、開始局面から作り直す）、蓄積値の検査（`accumulator_matches_full`）、千日手の null による区切り |
| `src/main.cpp` | 追加：`setoption`、`isready`（読み込み）、`eval`、`acc`、`accfull`、`undo`、`null`、`acccheck`、`evalbench`。USI のオプション一覧（`usi` の応答）と `DebugCheckAccumulator` |
| `CMakeLists.txt` | `src/eval/raypair/network.cpp` を追加 |
| `README.md` | N3 の構成・命令・オプションを追記 |
| `REPORT_N3.md` | この報告 |
| 手順1で配置（変更禁止） | `PROTECTED.sha256`、`tests/m4_ref.py`、`tests/test_n3_acceptance.py`。キットの ZIP は削除 |

## 解釈・判断した点

1. **`isready` と既定の重みファイル（N1 との矛盾の解消）**：N1 のテストは、重みを指定せずに `isready` → `readyok` を要求する。一方、N3 の仕様は `isready` で `EvalFile`（既定 `eval/nuenagon.shk`）を読み、失敗すれば `readyok` を出さない。この 2 つを両立させるため、**`EvalFile` を一度も指定しておらず、既定のファイルも無いとき**は、評価なし（`info string eval: no weight file`）で `readyok` を返すことにした。`EvalFile` を明示したときに失敗すれば、仕様どおり `info string eval load error` を出し、`readyok` は出さない（テストの `missing` ケースもこの経路）。
2. **千日手と null move**：§E5.6「千日手の判定では、パスを1手として数えない」を、「null の局面より前とは千日手の比較をしない（null で履歴を区切る）」と解釈した。N3 の受け入れテストには千日手と null を組み合わせたものが無い。N4（探索）で意味を確かめること。
3. **cp のクリップは N3 では行わない**：テストの仕様（`eval` は「クリップしない」）に合わせた。§E4 の「詰みの値と重ならないよう ±(詰み値 − 1) にクリップ」は、詰みの値を決める N4 で行うこと。
4. **差分更新の書き方**：§E3.2 の規則を、「変わったマス」を視点の盤面の差から求める形で実装した。A1・A2・B1・B2 の結果は規則どおり（同じ多重集合の差）。取る・成る・打つ・玉が動く、のどの手でも同じ処理になる。
5. **`DebugCheckAccumulator`**：探索がまだ無いので、局面を変える命令（`position`・`undo`・`null`）のあとに検査し、不一致なら `info string accumulator mismatch <SFEN> <直前の手>` を出す。探索中の一定間隔での検査（§E3.4）は N4 で行う。

## 作業の記録と注意

- **キット（手順1）**：ZIP の SHA-256 は一致。`KIT.json` の 3 ファイルの SHA-256 も一致。`KIT.json` の 3 ファイルだけをコピーし、ZIP を削除した（コミット `0e1eb90` Install kit N3）。
- **コミットの書式の不備**：キットのコミット `0e1eb90` の本文に、モデル名を含む `Co-Authored-By` 行を書いてしまった。システム指示では、コミットにモデル名を入れてはいけない。このコミットは push 済みのため、履歴を書き換えずに残した。N3 のほかのコミットは、この行を入れずに作った（後で直したもの）。
- **N2 からの既存の警告**：`src/board/bitboard.h` の未使用変数 `opp`、`src/mate/dfpn.h:83` の符号比較、`bitboard.h:267` の配列境界の警告は、N1・N2 から残っているもの。N3 で増えた警告はない。
- **各コミットの前の `check_protected.py`**：すべてのコミットの前に実行し、`OK` だった。

## コミット一覧

| コミット | 内容 |
|---|---|
| `0e1eb90` | Install kit N3（キットの 3 ファイルを配置、ZIP を削除） |
| `f78e94e` | Add weight file loading, quantization and forward pass（`network.h/.cpp`、`raypair` の追加関数、`CMakeLists.txt`） |
| `f5ebd43` | Keep first-layer accumulators in Game with delta update and null move（`game.h/.cpp`） |
| `3ad09d5` | Add N3 USI commands（`src/main.cpp`） |
| （この報告の後のコミット） | README と REPORT_N3 |

## `python check_protected.py` の結果

```
OK
```

## 次の担当（N4：探索の中核と USI）への引き継ぎ

1. **評価の入口**：`Game::accumulator()` と `raypair::forward_raw(net, acc, board.turn)`、`raypair::cp_of(raw, scale)`。探索は `Game::push`・`pop`・`push_null` を使う。`Game` は `set_network` で重みを受け取る（`EvalState` の `net` を指す）。
2. **cp のクリップ**：§E4 の「±(詰み値 − 1)」は、詰みの値を決めてから `cp` に掛けること。`eval` 命令は今はクリップしない（テストの仕様）。
3. **DebugCheckAccumulator**：探索中、一定間隔（例：1,000 ノードごと）に `Game::accumulator_matches_full()` で検査し、不一致なら SFEN と直前の手を出して停止する（§E3.4）。今は局面を変える命令のあとだけ検査している。
4. **千日手と null**：`Game::repetition()` は直近の null より前を見ない。パスの探索（threat・tsumero）で、この扱いが §E5.6 の意図どおりかを確かめること。
5. **CPU の情報（§E2.5）**：`info string Nuenagon cpu: …` はまだ出していない（`EvalFile` の表示だけ）。CPU の割り当ては §E2.4 と合わせて N4 以降で行う。
6. **速さ（N5 で詰める）**：差分更新は、`push` の中で視点の盤面を 4 回作り直し、特徴のベクトルを毎回確保している。推論は素直な実装（SIMD なし）。速くするなら、まず `make_view` の再計算と `std::vector` の確保をやめる。
7. **重みの範囲**：読み込み時の検査は §F4 のものだけ。学習器が §F3.3 の範囲を守っている前提で、int16 のあふれは検査していない（`acccheck` の結果では、テストの範囲で一致）。
