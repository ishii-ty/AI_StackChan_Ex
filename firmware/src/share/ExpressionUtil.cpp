#include "ExpressionUtil.h"

using namespace m5avatar;

Expression expressionFromString(const String& s, Expression fallback)
{
    String name = s;
    name.toLowerCase();

    if (name.equals("neutral")) {
        return Expression::Neutral;
    } else if (name.equals("happy")) {
        return Expression::Happy;
    } else if (name.equals("angry")) {
        return Expression::Angry;
    } else if (name.equals("sad")) {
        return Expression::Sad;
    } else if (name.equals("doubt")) {
        return Expression::Doubt;
    } else if (name.equals("sleepy")) {
        return Expression::Sleepy;
    }
    return fallback;
}

String truncateUtf8(const String& s, size_t maxChars)
{
    size_t chars = 0;
    size_t i = 0;
    size_t len = s.length();
    while (i < len) {
        // UTF-8の後続バイト(10xxxxxx)ではない = 新しい文字の先頭バイト
        uint8_t b = (uint8_t)s[i];
        if ((b & 0xC0) != 0x80) {
            if (chars == maxChars) {
                break;
            }
            chars++;
        }
        i++;
    }
    return s.substring(0, i);
}
