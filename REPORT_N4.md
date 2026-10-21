# REPORT_N4（N4 前半）

## 結果

**中断**（停止条件 1：キットのインストールに失敗）

- 手順1の 1. にある `nuenagon_kit_n4.zip` が見つからなかった。
  - リポジトリ直下にない（`git ls-files` に `.zip` はない）。
  - ファイルシステム全体を `find` しても見つからない。
  - `origin/claude/jolly-rubin-z08u37` にもない。
  - `origin/main` にあるのは `nuenagon_kit_n1.zip` だけで、`nuenagon_kit_n4.zip` はない。
- 手順の指示どおり、zip が無い時点で作業を止めた。SHA-256 の照合、展開、コピー、探索の実装には進んでいない。
- 変更は行っていない。`src/`、`tests/`、`docs/` などには触れていない。

## 作業ブランチの状態

- ブランチ：`claude/jolly-rubin-z08u37`（HEAD `3817f0b`）
- N3 の成果（`src/eval/raypair/network.h` を含む）は取り込み済み。手順0は満たしている。
- `python check_protected.py` → `OK`

## 未実施（キットが届いたら再開する項目）

- `python -m pytest -q tests` の実行（N1〜N3 の 62 件の取り込み直後確認、N4 の 15 件）
- 強さのテスト（`test_beats_baseline`）の結果：未実施
- bench の NPS（1スレッド・全スレッド）：未実施
- 探索の構成、ジョブ・スレッドプール・ルートの手ごとの記録、後半の受け口：未実装

## 次の担当への引き継ぎ

1. `nuenagon_kit_n4.zip` をリポジトリ直下に置いてもらう必要がある。`origin/main` には N1 のキットしかない。持ち主が N4 のキットをどこに置くか確認してほしい。
2. キットが届いたら、手順1（SHA-256 `acaea4e808eb701e7c3dcb3678daeddcf24e41e43af28184d18ea54af727c63a` の照合、`KIT.json` による各ファイルの照合、コピー、zip の削除、`check_protected.py` の確認）から再開する。
3. 手順1の条件（zip の有無、ハッシュ、自動審査、`check_protected.py`）のどれかで失敗したら、別の方法は試さず、また報告する。

## `python check_protected.py`

`OK`
