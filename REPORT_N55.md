# N5.5 報告：探索の改良（途中で中断）

## 結果：中断

中断条件 2（取り込み直後の確認で、N5.5 の 5 件以外が失敗）に当たった。

## 取り込み直後のベースライン（コミット 2368746 の時点・改造前の探索）

`python -m pytest -q tests`：**4 failed, 105 passed**（1072 秒）

- N5.5 の 3 件（予想どおり）：`test_depth_gain`、`test_required_options`、`test_strength_vs_n5h`
- **N5.5 以外の 1 件（中断の原因）**：`tests/test_n5_acceptance.py::test_scalar_build_same_results`
  - 既定のビルド（AVX2）と `NUENAGON_SIMD=OFF` のビルドで、44 局面の bestmove が違う。
  - 同じ局面 1 つ（`material/opening`、`go nodes 150000`、Threads=1、Deterministic=true）でも、info の行数が違う（AVX2 9 行、scalar 12 行）。つまり探索の木が違う。
  - 静的評価（`eval`）は、1 局面で一致した（raw -49）。違いは探索の中、差分更新か隠れ層の計算のどこか。
  - 未確認：N5 の報告では両ビルドが正解データと完全一致とされているが、この比較は今回の再現で成り立っていない。原因（SIMD の飽和・演算順序、または N5 のコードの不具合）は未特定。

- N5.5 の 2 件（`test_long_mates_not_worse`、`test_all_options_off_still_correct`）は、この時点で通っている。

## 作業の状態（途中のコミット）

- `2368746` Install kit N5.5：キットの取り込み（SHA-256 と KIT.json の検証済み）。`check_protected.py` は OK。
- `21a0aa2` WIP：探索の改良の実装。`Search*` の check オプション 11 個（既定 true）。
  - 疑似合法手の生成と合法性の確認の後回し、SEE、減衰つきの履歴、連続履歴、カウンター手、LMR の表、RFP、動的な null move と検証、LMP、SEE 枝刈り、IIR、improving、razoring、1 手詰め、遠い打つ手の枝刈り。
  - ビルドは通る。**深さ・強さ・全テストは未測定**。改造後の全テストは未実行。

## 次の担当への引き継ぎ

1. まず `test_scalar_build_same_results` の原因を特定する（AVX2 と scalar の差分更新・隠れ層の結果を、1 手ずつ比べる）。N5 の不具合なら、N5 の修正として別に扱う。
2. その後、WIP の探索を塊ごとに測る（`test_depth_gain` は N5H と比べる）。
3. 測定の注意：テスト実行中に `build/` を作り直さない。開発ビルドは `build_dev/`（未コミット）を使った。

## `python check_protected.py` の結果

`OK`

## 追記：測り直し（コミット 9b761b9 の探索）

- 深さ（1 手 1 秒、12 局面）：N5H 7.83 ／ 新（既定）9.33 [10, 9, 10, 9, 9, 9, 9, 8, 12, 11, 6, 10] total nodes 5319540
- 強さ（200 ms、N5H 対、60 局）：既定 27W 33L 0D（勝率 0.450）／ 11 手法すべて false：27W 33L 0D（勝率 0.450）
- 2 つの結果が完全に一致したのは不審。入り切りのオプションが効いているかは、要確認（手法を切った版で指し手が変わるかを、個別に確かめていない）。
- 結論：N5.5 は未達（深さ +2.0 も、強さ 0.60 も届いていない）。引き継ぎ：(1) 2 つの結果が一致する原因を確かめる、(2) 手法を 1 つずつ切って 60 局以上で比べる、(3) LMP・RFP の幅は SearchLMPScale・SearchRFPMargin で調整する。
