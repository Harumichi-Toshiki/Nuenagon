// ファイルの読み込みと、端末への出力の文字コード（Windows 対応）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

// path のファイルを丸ごと読む。Windows では、パスを UTF-8 として幅広文字に直して開き、だめなら
// そのまま（ANSI のコードページとして）開く。相対パスで見つからなければ、実行ファイルのあるフォルダからの相対でも探す。
bool read_whole_file(const std::string& path, std::vector<uint8_t>& out);

// read_whole_file と同じ探し方で、ファイルが開けるか。先頭 n バイトを読む（n=0 なら開けるかだけ）。
bool read_head(const std::string& path, uint8_t* buf, size_t n);

// 端末（GUI）へ出す1行を、その OS の GUI が期待する文字コードに直す。Windows では UTF-8 → ANSI（日本語なら Shift-JIS）。
// Windows 以外ではそのまま。
std::string to_console(const std::string& utf8);

}  // namespace platform
