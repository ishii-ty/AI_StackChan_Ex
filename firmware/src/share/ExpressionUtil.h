#ifndef _EXPRESSION_UTIL_H
#define _EXPRESSION_UTIL_H

#include <Arduino.h>
#include <Avatar.h>

// 文字列("neutral"/"happy"/"angry"/"sad"/"doubt"/"sleepy")をm5avatar::Expressionへ変換する。
// 語彙はsrc/llm/ChatGPT/FunctionCall.cppのset_avatar_expression()と揃えている。
// 未知の値・空文字列はfallbackを返す。
m5avatar::Expression expressionFromString(const String& s, m5avatar::Expression fallback);

// UTF-8のコードポイント単位でsをmaxChars文字に切り詰めて返す。
// マルチバイト文字の途中で切らないよう、後続バイト(先頭ビットが10xxxxxx)は文字数に数えない。
String truncateUtf8(const String& s, size_t maxChars);

#endif
