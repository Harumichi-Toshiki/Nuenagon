# N5H 報告：HalfKP 対応（やねうら王の halfkp_256x2-32-32）

## 結果：完了

- `python -m pytest -q tests`：**106 passed**（390.03 秒。N1〜N5 の 89 件＋N5H の 17 件）
- `python check_protected.py`：**OK**（各コミットの前に実行）
- コミット：`37fbd98 Install kit N5H`、`e4bef17 Add HalfKP ... (N5H)`、およびこの報告。push 済み。

## 手順 0・1 の経緯

- 手順 0：作業ブランチに N5 の成果が入っていたので、取り込みは不要。
- 手順 1：最初、リポジトリの一番上に `nuenagon_kit_n5h.zip` が無く、作業を止めて報告した（前の報告 `3800870`）。その後、持ち主の指示で確かめると、zip は **`main` ブランチ**に入っていた。作業ブランチには無かっただけ。`origin/main` からこのファイルだけを取り出し、SHA-256（`85b5a58b…`）と KIT.json の各ファイルのハッシュが一致することを確かめてから導入した。変更禁止ファイルは、キットの KIT.json に載っているものだけを変えた（`docs/DESIGN.md` は v1.5）。zip は削除。

## 速さ（N5 の `test_speedup` の基準：N4.1 の 2 倍以上）

同じ日の同じ計測（12 局面、Threads=1、300000 ノード、各局面は速い方の 1 回の合計）：

| 版 | 合計時間 | N4.1 に対する比 |
|---|---|---|
| N4.1（ae08a26） | 17.26 秒 | 1.00 |
| N5（HalfKP 組み込み前、1a5095e） | 7.65 秒 | **2.26 倍** |
| N5H（HalfKP 組み込み後、e4bef17） | 7.68 秒 | **2.19 倍** |

- RayPair の探索の速さは、組み込み前後で 0.4 % しか変わらない（時間の差）。比の差は N4.1 側の計測のばらつき。
- N5H の `test_speedup` は、106 件の全テストの中で合格。
- 組み込みの方針：`Game` の差分更新は、評価の種類で分ける分岐を push・pop・null の入口に 1 回ずつだけ置いた。RayPair の処理の中身は変えていない。

## evalbench（局面は中盤。`test_report_evalbench` の出力）

| 評価関数 | push | pop | eval（隠れ層以降） |
|---|---|---|---|
| RayPair（material_v0.shk） | 0.719 µs | 0.058 µs | 0.197 µs |
| HalfKP（normal.bin） | 0.246 µs | 0.038 µs | 0.401 µs |

（RayPair の値は、同じ日の別の実行で 0.88 µs / 0.165 µs。実行ごとに少しばらつく。）

## bench（1 スレッド。nps）

- RayPair（material_v0.shk）：478,337 nps／592,567 nps（2 回）
- HalfKP（normal.bin）：688,959 nps／783,570 nps（2 回）

**注意**：bench の探索はノード数が違う（RayPair 79,404 ノード、HalfKP 222,534 ノード）。評価関数が違うので、探索の木も違う。NPS は「同じ探索の速さ」ではないので、比較の材料にならない。評価関数の構造の差は、同じ探索で測ること（上の evalbench と、後の対局実験）。

計測環境：Intel Xeon Processor @ 2.10GHz、論理 CPU 4、物理コア 4（仮想マシン）。

## 評価関数の種類の切り替えの作り

- 読み込み：`isready` の前に、EvalFile の先頭 4 バイトが `0x7AF32F16`（リトルエンディアン）かを見る（`file_is_halfkp`）。そうなら HalfKP、そうでなければ今までどおり .shk（RayPair）。
- `Game` に `set_network`（RayPair）と `set_halfkp`（HalfKP）があり、どちらも蓄積値を開始局面から作り直す。`raw_eval()` が種類に応じて出力層の値を返す。
- 探索の評価値：`Worker::evaluate` が、HalfKP なら `raw / FV_SCALE`（0 の方へ切り捨て）、RayPair なら `cp_of`。どちらも `kMateCp` で挟む。
- 探索の検査（`DebugCheckAccumulator`）と、mate 探索の条件は、両方の重みに対応させた。

## HalfKP の差分更新の作り（数行）

1 手ごとに、指す前の盤から `MoveInfo`（移動元・移動先・動いた駒・取られた駒・指した側・打った駒種）を取る。指した後の盤で両視点を更新する：移動元の駒を引き、移動先の駒（成りの後）を足す。取った駒は引き、取った側の持ち駒の「今の枚数の 1 枚目」を足す。打った駒は、持ち駒の「打った後の枚数」の番号を引き、打った先を足す。**玉を動かした視点だけ全計算**で作り直す（もう一方の視点は、玉が特徴に入らないので差分だけ）。取られた玉は持ち駒に入らない。マス番号は内部の `y×9+x` から HalfKP の `筋×9+段`（1一=0、9九=80）へ変換する。

## 作ったファイル・変えたファイル

| ファイル | 役割 |
|---|---|
| `src/eval/halfkp/halfkp.{h,cpp}` | 新規。nn.bin の読み込み・検査、全計算、差分更新、推論（AVX2 は maddubs） |
| `src/eval/simd.h` | 新規。`NUENAGON_SIMD_AVX2` の既定値（RayPair と HalfKP で共有） |
| `src/eval/raypair/network.h` | SIMD の定義を `simd.h` に移した（他は変えていない） |
| `src/board/game.{h,cpp}` | HalfKP の蓄積値の stack、`set_halfkp`、`raw_eval`。push・pop・null・検査の入口に分岐を追加。RayPair の経路は変えていない |
| `src/search/search.{h,cpp}` | `EvalRef` に HalfKP の重みと `fv_scale`。評価値の換算を分岐。詰み探索・検査の条件を両方に対応 |
| `src/main.cpp` | EvalFile による切り替え、`FV_SCALE` オプション（spin 1〜128、既定 16）、`eval`・`acc`・`accfull`・`evalbench` の HalfKP 対応、`make_ref`・`attach_eval` |
| `CMakeLists.txt` | `halfkp.cpp` の追加 |
| `docs/DESIGN.md`、`tests/*halfkp*`、`tests/test_n5h_acceptance.py`、`tests/golden/halfkp_golden.json`、`PROTECTED.sha256`、`check_protected.py` | キット N5H の導入（手順 1。KIT.json の範囲だけ） |

## 次の担当への引き継ぎ事項

1. **HalfKP の評価は、対局で強さを比べる段階に進める**。評価関数の差（RayPair と HalfKP）を測るには、同じ探索・同じノード数で対局する。NPS の比較は、探索の木が違うので使えない。
2. **HalfKP の推論は、AVX2 を使っても、隠れ層 2 以降は小さい計算のまま**（evalbench の eval が 0.40 µs で、RayPair の 0.20 µs の倍）。速くするなら、隠れ層 2（32×32）と出力層を 1 命令列にまとめる。ただし結果が 1 ビットも変わらないことを、`test_same`系と `test_scalar_build_matches` で確かめること。
3. **玉の手の差分更新は、玉を動かした視点だけ全計算する**（約 125,388 特徴 × 256 の全行の走査に近いが、40 行ほどなので速い）。玉の手の割合が高い終盤で、この全計算の回数を測ること。
4. **駒箱の数（38 − 盤と持ち駒の非玉の駒数）は、差分更新では変わらない前提**。対局の途中で駒の総数が変わる経路が増えたら、この前提を見直すこと（今の着手生成では変わらない）。
5. **HalfKP の探索の評価値は、cp の上限（kMateCp）で挟む**。やねうら王の ±29999 の挟み方とは違う（テストでは、cp が詰みの値を超えないことだけ確かめている）。対局で差が出るなら、この扱いを確かめる。
6. **AVX2 はコンパイル時に決まる**（N5 の引き継ぎ 6 と同じ）。
7. **Windows の確認は未実施**（クロスコンパイラが無い）。N5 の CPU 固定と同じく、HalfKP の AVX2 も、Windows 用ビルドの実機確認が必要。

## `python check_protected.py` の結果

`OK`
