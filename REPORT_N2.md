# N2 報告（特徴量 RayPair_v0 / v1 の全計算）

## 結果

**完了。** 受け入れテストは 51 件すべて通過（N1 の 23 件＋ N2 の 28 件）。
`python check_protected.py` は `OK`。参照実装（`reference/raypair_ref.py`）・付録B の正解データとも一致。

## `python -m pytest -q tests` の最終出力

```
...................................................                      [100%]
51 passed in 6.39s
```

## 全計算1回（両視点・v0）の時間

- **約 15.6 マイクロ秒 / 局面**（両視点ぶん、v0、1 回の `compute_features` を 2 回）。
- 計測：ランダムに指した 300 局（各 160 手まで）から 8 手ごとに抽出した 5,665 局面を 20 周。平均 147 特徴/視点。
- 環境：Linux（Ubuntu 24.04）、4 コア、g++ 13.3、`-O3 -DNDEBUG`。計測コードはリポジトリに含めていない（合格条件ではない）。

## 作った関数の一覧と役割

`src/eval/raypair/raypair.h` / `raypair.cpp`（名前空間 `raypair`）

| 関数 | 役割 | N3 の差分更新で使うか |
|---|---|---|
| `ViewBoard make_view(const Board&, int persp)` | 盤面を視点座標に直す（駒番号 `pc[81]`、持ち駒 `hand[2][7]`）。SFEN は経由しない | 初期化に使う |
| `append_piece_features(vb, out)` | A1（駒位置）を出す | はい：駒が動いたとき、古い `pc*81+sq` を消し、新しいものを足す |
| `append_hand_features(vb, out)` | A2（持ち駒）を出す | はい：持ち駒の増減 |
| `append_line_b1(vb, version, line, out)` | ライン1本ぶんの B1 を出す（走査規則・距離区分・v1 の領域を含む） | **はい（主役）**：差分の単位はライン |
| `append_knight_features(vb, version, out)` | B2（桂の利き）を出す | はい：桂の位置・跳び先の駒が変わったとき |
| `compute_features(board, persp, version, out)` | 全計算。A1・A2・B1（52 本）・B2 を合わせ、昇順に並べて `out` に入れる | 全計算と突き合わせる検査用 |
| `axis_of_line(line)` | ライン番号から軸（0 縦、1 横、2 斜め↘、3 斜め↙）を返す | 内部用 |
| `line_squares(line, out)` | ライン上のマスを走査順（手前→奥）に並べる | 内部用 |
| `kLinesOf`（constexpr 表） | 視点座標のマス sq を通る 4 本のライン番号（axis 0〜3 の順） | **はい**：移動で変わるラインの特定に使う |

定数：`kLines = 52`、`kDims[2] = {15000, 115352}`。

`src/main.cpp` の命令用の関数（`cmd_features`・`cmd_linefeatures`・`cmd_linesof`・`reply_indices`）は、上の関数を呼ぶだけ。

## 作ったファイルの一覧

| ファイル | 役割 |
|---|---|
| `src/eval/raypair/raypair.h` | 特徴量の宣言、視点つき局面 `ViewBoard`、ライン番号の定数表 `kLinesOf` |
| `src/eval/raypair/raypair.cpp` | 特徴量の計算（A1・A2・B1・B2・全計算）、ライン上のマスの走査 |
| `src/main.cpp` | 命令 `features`・`linefeatures`・`linesof` を追加（N1 の命令は変更なし） |
| `CMakeLists.txt` | `src/eval/raypair/raypair.cpp` をビルド対象に追加 |
| `README.md` | 構成図と命令表に N2 の分を追記 |
| `REPORT_N2.md` | この報告 |
| 以下はキット（手順1で配置。変更禁止） | |
| `reference/raypair_ref.py` | 参照実装（Python） |
| `tests/test_n2_acceptance.py` | N2 の受け入れテスト |
| `tests/golden/raypair_golden.json` | 付録B の正解データ |
| `tests/golden/n2_golden.json` | 参照実装で作ったランダム 300 局面の正解 |
| `PROTECTED.sha256` | キットの版で置き換え（`check_protected.py` は OK） |

## N3（重みの読み込み・量子化・推論・差分更新）への引き継ぎ

1. **差分更新の単位はライン。** 駒が動くと、影響するのは次の 4 つ。
   - 移動元と移動先のマスを通るライン（`kLinesOf` で各 4 本）。**2 つのマスが同じライン上にあることは普通にあるので、ライン番号は必ず重複を除いてから処理すること。** 同じライン を 2 回直すと結果が狂う。
   - 駒が消えるマス（取られた駒）と現れるマスについても、同じく 4 本ずつ。
   - A1：`pc*81+sq` を消して、新しいものを足す。
   - A2：持ち駒の増減（取った駒、打った駒）。枚数 n の持ち駒の添字は k = 0..n−1 なので、1 枚増えたら `k = n−1` を、減らしたら `k = n−1`（減らす前の値）を消す。
2. **多重集合の規則（§F2.0）。** 差分では「同じインデックスを、出現回数ぶんだけ」消す。`std::remove` のように全部消してはいけない。ラインごとに「古い B1 を全部消す→新しい B1 を全部足す」で処理すると、重複の扱いが自然に正しくなる。
3. **ライン単位の再計算は規則どおりに正しい。** v1 の領域は `lo` のマス（走査で先に来る駒）で決まるので、ライン上の駒が動けば `lo` が変わることがある。ライン丸ごと再計算すれば、この点も含めて正しい。
4. **B2（桂）** は、桂のマス（4 方向の桂の跳び先を含む）の駒が変わると影響を受ける。桂は最大 4 枚程度なので、**B2 は毎回すべて消して再計算する**のが簡単で、十分に速い。
5. **視点。** 先手視点と後手視点は、別々の `ViewBoard` として持つ（または、それぞれに差分を当てる）。後手視点のマスは `80 − sq`（絶対座標の sq から）。`kLinesOf` は視点座標の sq で引く。
6. **検査の方法。** 差分更新の結果は、`features <v> <p>`（全計算）と一致しなければならない。ライン単位の検査には `linefeatures <v> <p> <L>` を使える。`test_line_decomposition` が、全ライン B1 の和と全計算の B1 の一致を確認している。
7. **重みと推論（F3・F4・E4）は未着手。** 重みの形式（`.shk`、ヘッダ 180 バイト）、float32 の計算式、量子化の規則は N3 で読んで実装すること。N2 では触っていない。
8. **既知の注意**（N1 の報告から）：`Board::undo_move` の玉を取る手の修正は、N3 の差分更新でも同じ規則で扱うこと。
9. **速さ**：全計算は 1 局面 約 15.6 μs（両視点・v0）。差分更新の速さは N3 で測ること。

## 作業の記録と注意

- **環境の準備**：この環境には `pytest` と `cshogi` が入っていなかったため、`pip install pytest cshogi` で入れた（N1 のテスト `test_mate_found` が `cshogi` を import するため）。README のとおり。
- **キットの配置（手順1）**：ZIP の SHA-256 は一致。`KIT.json` の 5 ファイルの SHA-256 も一致。`KIT.json` に載っている 5 ファイルだけをコピーし、ZIP は削除した（コミット `a2e4310`）。
- **既存の警告**：`src/board/bitboard.h:468` の未使用変数 `opp`（N1 の報告に記載あり）。N2 の変更で増えた警告はない。
- **コミットの Claude-Session 行（不足）**：コミット `68a4e5d`・`64c8027`・`7ae2f97` の本文に、`Claude-Session:` の行が入っていない。後から書き換えようとしたが、自動審査で git の書き換えが拒否されたため、そのままにした。キットのコミット `a2e4310` には入っている。
- **Co-Authored-By の行は入れていない**：指示の書式にはモデル名入りの行があったが、この環境のシステム指示ではコミットメッセージにモデル名を入れないことになっているため。
- **各コミットの前の `check_protected.py`**：すべてのコミットの前に実行した（`7ae2f97` は一度省いてしまったため、コミットをやり直して、実行してからコミットした）。

## コミット一覧

| コミット | 内容 |
|---|---|
| `a2e4310` | Install kit N2（キットの 5 ファイルを配置、ZIP を削除） |
| `68a4e5d` | Add RayPair feature computation（A1・A2・B1・B2・全計算、`kLinesOf`、`CMakeLists.txt`） |
| `64c8027` | Add features, linefeatures, linesof commands（`src/main.cpp`） |
| `7ae2f97` | Document N2 feature commands and raypair module（`README.md`） |
| （この報告） | Add REPORT_N2 |

## `python check_protected.py` の結果

```
OK
```
