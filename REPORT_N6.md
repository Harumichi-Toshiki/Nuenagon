# N6 報告：Dajo プロトコル DJP v0（設計書 §E9）

## 結果：中断（停止条件 5：受け入れテストの期待と、設計（§E9.4・§E9.8）の解釈が食い違う。`test_probe_verdicts` の 1 件）

- 実装は、DJP v0 の命令・ジョブ（`search`・`rank`・`probe`・`threat`・`mate`・`eval`）・スレッドの割り当て・`cancel`・`promote`・`hint` まで作った。
- `tests/test_n6_acceptance.py`：**16 件通過、1 件失敗**（`test_probe_verdicts`。下の「中断の理由」）。
- 既存のテスト（N1〜N5.5 の全部。ただし強さのテスト `test_strength_vs_n5h` を除く）：**124 件通過**。N5.5 までの USI の動作は、これらのテストで変わっていない。
- `test_strength_vs_n5h` は今回走らせていない（20〜40 分かかるため。後述の引き継ぎに含める）。
- `python -m pytest -q tests`（全体）は、中断したので最後まで回していない。
- `python check_protected.py`：`OK`（各コミットの前に実行）。

### 実行の記録

```
python -m pytest -q tests/test_n6_acceptance.py          → 1 failed, 16 passed（test_probe_verdicts）
python -m pytest -q tests -k "not strength"             → 1 failed, 124 passed, 1 deselected（377 秒）
                                                          （失敗は同じ test_probe_verdicts のみ）
```

## 中断の理由（停止条件 5、あわせて条件 3 に近い）

`test_probe_verdicts` の最後の検査：

```
probe move=5h5d threshold=0 nodes=30 mindepth=4 → 期待 verdict=unknown
```

実装の結果は `verdict=no depth=6 nodes=32`。深さ 6 の結論は、30 節点以内で読み切った結果です。

予算ごとに到達した深さ（同じ局面、`mindepth=1`、新しい置換表の読み取りなし）：

| nodes | 深さ | 実節点 |
|---|---|---|
| 5 | 1 | 6 |
| 10 | 3 | 13 |
| 15 | 4 | 16 |
| 20 | 5 | 23 |
| 25・30 | 6 | 32 |
| 300 | 19 | 322 |

- §E9.4 は「`mindepth` 未満の結論は `unknown`」と定めるだけで、深さ 4 以上を予算内で読み切った結論は有効です。§E9.8 も、時間切れのあとで深さつきの結論を返す例（`time=800`、`depth=11`）を示しています。
- 深さ 4 の検証が 16 節点で済むのは、この局面と探索の枝刈り（null window）の結果です。30 節点で深さ 4 未満に止めるには、探索を遅くするか、時間切れの結論を `unknown` にする設計変更が必要で、どちらも設計と食い違います。
- そのため、テストか設計のどちらかの前提が違うと判断し、テストは変えていません（変更禁止）。判断は持ち主に任せます。
- 同じ検査で、別に見つけた問題（直すべきもの）：予算の検査は段の終わりにしか行わないので、予算を超えた次の段が最後まで読まれ、`nodes` の値が予算を超える（この例では 32）。段の途中で打ち切る仕組み（予算を 1 節点ごとに検査する）が必要。

### 試した修正（同じテストの失敗に対して）

1. `probe` の読み筋（`pv`）が空だった：予算を使い切ったあとの全幅の読み直しが打ち切られていた → 読み直しをやめ、置換表から最善の応手をたどる方式にした（探索は増やさない）。解決。
2. `probe` の結論が、他のジョブの深い置換表の結論に頼っていた → `probe` だけ置換表を読まないようにした（書くのは従来どおり）。
3. 予算の検査の位置：節点数が `probe` のジョブの結果に入っていなかった（`nodes=0`）→ 節点数を足すようにした。
4. 上の 2 のあとも、この検査だけが残った（深さ 6 の結論が出る）。

## 実装した命令と caps

- 命令：`dajo hello`（版 0）、`dajo set`（`threads`・`affinity`・`physical_only`・`hash`・`draw_value`・`max_ply`・`hint_weight`・`hint_lmr_threshold`・`deterministic`・`eval_scale`。知らないキーは無視）、`dajo sysinfo`、`dajo sync`（`dajo synced`）、`dajo newgame`、`dajo cancel <番号>|all`、`dajo promote <番号> [time=] [nodes=] [depth=]`、`dajo hint probs=… position …`・`dajo hint clear`、`dajo job <番号> <種類> …`。
- 応答：`accepted`・`queued`・`started`・`move`・`evalres`・`result`・`promoted`・`synced`・`error`。
  - エラーのコード：`unknown_command`・`bad_arg`・`version`・`no_limit`・`illegal_position`・`illegal_move`・`in_check`・`no_such_job`・`duplicate_id`・`busy`。
- `hello` の前は `dajo` の行を出さない（`hello` 以外は黙って捨てる）。
- caps：`search,rank,probe,threat,mate,eval,hint,promote,sysinfo`。制御の命令（`set`・`cancel`・`sync`・`newgame`）は caps に入れていない（§E9.2 の例に合わせた）。
- 作っていない：`watch`（`dajo watch` は `unknown_command`）、`dajo info`・`dajo event` の行。

## ジョブとスレッドプール

- ジョブ（`search::Job`）は、ドライバのスレッドで動く。`threads` は `Threads`（容量）を超えない。既定は容量の全部。決定的モードは 1 スレッド。
- スレッドプールは USI と共有（`Threads - 1` 本）。ジョブが受け付けられると、容量（`Threads`）の中から `threads` を割り当てる。足りなければ `queued` で待たせ、空きができたら優先度の高い順（同じなら受けた順）に動かす。先頭が入らなければ、後ろは動かさない。
- `cancel`：待っているジョブは動かさず、その場で `result status=cancelled` を返す。動いているジョブは止めて、ドライバが `result` を返す（`result` は各番号で 1 回だけ）。`cancel all` は両方を含む。
- `promote`：動いているジョブ（その部分のジョブ）に、時間・節点・深さの上限を後から付ける。待っているジョブには `busy` を返す。
- 置換表は全ジョブで共有。ヒントの表は、ジョブが作られた時点の複製を持つ（書き換えるときは複製してから書く）。
- USI の `go` と同時には動く。ただし DJP のジョブが動いているあいだは、プールと置換表の作り直し（`go` 時の反映）を行わない。`setoption`・`isready` の前には、ジョブをすべて止めて終わるまで待つ。

## 各ジョブの作り

- `search`：`multipv`・`searchmoves`・`exclude`。反復ごとに上位 k 手の `move` を出す。`result` に `bestmove` と最後の段の値。
- `rank`：`moves`（省略時は全合法手）を MultiPV（k = 手の数）として読む。`exact_all` で、各段の全手を正確な値にする（並べ替えで前の段の値が上に来た手は、同じ位置から読み直す。読み直しは段ごとに 4k 回まで）。`result` の直前に、最後に完了した段の `move` の行を手ごとに 1 行ずつ出す。根の詰み探索は使わない。
- `probe`：`move` の手を指した局面を、窓 [T−1, T] の探索で深さ順に読む。読み切った段の結論を返す（`verdict`・`threshold`・`score`・`bound`・`depth`）。`margin` は、短い探索（深さ 6、節点は予算の 1/4）で最善の評価 B を求めて T = B − margin とする。`ref` があれば B をそれで置く。`no` のときは `pv`（この手を指したあとの、置換表の最善の応手をたどる）。`mindepth` 未満は `unknown`。置換表の結論は読まない（自分の予算で読む）。
- `threat`：パスした局面（`null_root`）で相手の最善手（`threat_move`）と評価（`score_pass` は我々の手番から見た値）を読む。`compare=1` なら同じ深さの通常の探索を行い `score_normal` と `gain` を返す。`tsumero=1` なら、パスした局面で相手の詰みを df-pn で探す（`tsumero=yes` と `mate_pv`、見つからなければ `unknown`）。王手の局面は `in_check`。
- `mate`：`side=self`（その局面の手番側の詰み）または `side=opponent`（パスした局面での相手の詰み）。df-pn で探し、見つかれば `mate=yes length=… pv=…`。見つからなければ `mate=unknown`（否定は証明しない。`no` は返さない）。
- `eval`：局面ごとに `evalres`。cp は USI の `eval` と同じ式（RayPair は `cp_of`、HalfKP は `raw / FV_SCALE`）。`qsearch=1` なら静止探索の値。`eval` は動かさずに、すぐ結果を返す。

## 作ったファイル・変えたファイル

- `src/dajo/dajo.h`・`src/dajo/dajo.cpp`（新規）：DJP の命令の処理、ジョブの割り当て（優先度・待ち・キャンセル・promote）、各種類のジョブの本体。
- `src/search/search.h`：`JobMode`、`Limits` の追加（`excludemoves`・`root_mate_probe`・`probe_threshold`・`probe_mindepth`・`exact_all`）、`Options` の追加（`hint_weight`・`hint_lmr_threshold`）、`SearchRFPGuard` の既定を `false`、`ProbeOut`・`RootReport`、`Job` の API（`probe`・`mate_found`・`mate_pv`・`set_root_report`・`extend`）、`qsearch_score`。
- `src/search/search.cpp`：`run_probe`・`run_mate`・`extend`・根の報告（`root_report`）・`exact_all` の並べ替え・`exclude`・ヒントの閾値・helper の締め切り（忙しい待ちをやめた）。USI の通常の探索の動きは変えていない。
- `src/main.cpp`：`dajo` の行の振り分け、`Host` の組み立て、`setoption`・`isready` の前の停止、`go` のときのプール・置換表の作り直しの省略（DJP が動いているとき）、ヒントの表を `shared_ptr` にした。
- `CMakeLists.txt`：`src/dajo/dajo.cpp` を追加。
- 取り込み（手順 1）：`PROTECTED.sha256`・`tests/test_n6_acceptance.py`（キット N6c の SHA-256 は KIT.json と一致）。リポジトリ直下の zip（`nuenagon_kit_n6c.zip`・`nuenagon_kit_n55f.zip`・`nuenagon_kit_n55r.zip`・`nuenagon_kit_n5h.zip`）を削除。
- `REPORT_N6.md`：この報告。

## 手順 0 と履歴について

- 作業ブランチは、指定の開発ブランチ `claude/great-carson-23kdt3` を `origin/main`（`563613f`）に合わせて作った。`main` と同じ commit から始まっているので、履歴は書き換えていない。
- コミット：`fd9f4c7`（Install kit N6）、`e4c492a`（search の変更）、`027e02b`（DJP の実装）。すべて push 済み。
- 各コミットのメッセージには、セッションの指示で求められた Co-Authored-By の行が入っている（モデル名を含む）。取り除くには履歴の書き換えが必要なので、行っていない。

## 既知の制限（設計との差）

1. `mate` は `no` を返さない（df-pn は否定を証明しない）。見つからなければ `unknown`。
2. `dajo set threads`・`affinity`・`physical_only`・`hash` は、ジョブが動いているあいだは反映を保留し、ジョブが無くなってから反映する（§E9.5 の「止めずに次の反復から」は未実装）。
3. `probe` の予算の検査は段の終わりに行うため、予算を超えて次の段が読まれることがある（上の「中断の理由」）。
4. `newgame` は置換表を空にする（§E5.3 の「世代を進める」だけでは、決定的モードの同じ結果が得られなかったため）。
5. 実行中の DJP のジョブがプールを占めると、USI の `go` の helper は待たされることがある（helper の作業は、本体が終わるまで始まらないことがあるが、本体は待たない）。
6. `hint_weight` は、0 のときはヒントを使わず、0 より大きければ並べ替えと LMR に同じように使う（重みの大きさでは変えていない。§E5.5 の「強さの設定」は未実装）。
7. `eval` のジョブは受け付けと同時に結果を返す（キューを使わない）。

## 次の担当への引き継ぎ（Dajo の中継を作るときに知っておくこと）

1. まず、`test_probe_verdicts` の最後の検査（`nodes=30`、`mindepth=4`）を、持ち主と決める。候補：(a) テストの期待を見直す（この局面では 30 節点で深さ 6 まで読める）、(b) 予算の検査を節点ごとにして、超えた段を結論にしない、(c) 時間切れと同じく深さつきの結論を返す（現状）。(b) は、予算超過の段の打ち切りを直す作業と一緒に行うのがよい。
2. 強さのテスト `test_strength_vs_n5h` を、今回の `SearchRFPGuard` の既定変更（false）のあとで回す。結果は未確認。
3. `rank` の全手の値は、段の終わりにしか確定しない。途中の段の行は、値が揃っていないことがある（`bound` が `lower`/`upper` のこともある）。確定値は `result` の直前の行。
4. `dajo set threads` は、ジョブが動いていれば保留される。合議が配分を変えるとき（相手の手番中に ponder を増やす等）は、ジョブを止めてから `set` を送る。
5. ponder（§E9.4 の `promote`）は、`search infinite=1` を投げたあと、予想が当たったら `promote time=…` を送る。相手の予想手の局面は、ジョブごとに別の `job` で送る（局面はジョブが持つ）。
6. `dajo newgame` は置換表を空にする。合議の局面の切り替えには、対局の区切りでだけ使うこと（毎手は使わない）。
