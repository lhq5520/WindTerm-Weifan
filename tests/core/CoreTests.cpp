#include "WtTest.h"

#include "wt/core/CharWidth.h"
#include "wt/core/Status.h"
#include "wt/core/Utf8.h"

using namespace wt::core;

WT_TEST(utf8_decodes_ascii_and_multibyte) {
    WT_CHECK(fromUtf8("abc") == U"abc");
    WT_CHECK(fromUtf8("\xC3\xA9") == U"é");             // é
    WT_CHECK(fromUtf8("\xE4\xB8\xAD\xE6\x96\x87") == U"中文");
    WT_CHECK(fromUtf8("\xF0\x9F\x98\x80") == U"\U0001F600"); // 😀
}

WT_TEST(utf8_sequence_split_across_chunks) {
    Utf8Decoder decoder;
    std::u32string out;
    auto sink = [&out](char32_t cp) { out.push_back(cp); };
    decoder.decode("\xE4", sink);
    WT_CHECK(out.empty());
    WT_CHECK(decoder.hasPendingSequence());
    decoder.decode("\xB8", sink);
    decoder.decode("\xAD!", sink);
    WT_CHECK(out == U"中!");
    WT_CHECK(!decoder.hasPendingSequence());
}

WT_TEST(utf8_invalid_input_yields_replacement) {
    WT_CHECK(fromUtf8("\xFF") == U"�");
    WT_CHECK(fromUtf8("\xC0\xAF") == U"��");      // overlong
    WT_CHECK(fromUtf8("\xED\xA0\x80") == U"���"); // surrogate
    WT_CHECK(fromUtf8("\xE4\xB8x") == U"�x");          // truncated, next byte reprocessed
    WT_CHECK(fromUtf8("\xE4\xB8") == U"�");            // truncated at end of input
}

WT_TEST(utf8_roundtrip_encoding) {
    const std::u32string text = U"aé中\U0001F600";
    WT_CHECK(fromUtf8(toUtf8(text)) == text);
    WT_CHECK_EQ(toUtf8(std::u32string(1, char32_t{0x110000})), std::string("\xEF\xBF\xBD"));
}

WT_TEST(char_width_classes) {
    WT_CHECK_EQ(charWidth(U'a'), 1);
    WT_CHECK_EQ(charWidth(U'é'), 1);
    WT_CHECK_EQ(charWidth(U'中'), 2);
    WT_CHECK_EQ(charWidth(U'한'), 2);
    WT_CHECK_EQ(charWidth(U'Ａ'), 2); // fullwidth A
    WT_CHECK_EQ(charWidth(U'\U0001F600'), 2);
    WT_CHECK_EQ(charWidth(U'́'), 0); // combining acute
    WT_CHECK_EQ(charWidth(U'​'), 0); // zero width space
    WT_CHECK_EQ(charWidth(U'\x07'), 0);
    WT_CHECK_EQ(charWidth(U'─'), 1);
}

WT_TEST(status_carries_message_and_errno) {
    WT_CHECK(Status::ok().isOk());
    const Status s = Status::error("boom", 2);
    WT_CHECK(!s);
    WT_CHECK_EQ(s.message(), std::string("boom"));
    WT_CHECK_EQ(s.systemError(), 2);
}

WT_TEST(char_width_generated_tables) {
    WT_CHECK_EQ(charWidth(U'­'), 1);       // soft hyphen stays visible (wcwidth convention)
    WT_CHECK_EQ(charWidth(U'ᅠ'), 0);       // Hangul Jungseong filler / medial vowels
    WT_CHECK_EQ(charWidth(U'‍'), 0);       // ZWJ
    WT_CHECK_EQ(charWidth(U'️'), 0);       // variation selector 16
    WT_CHECK_EQ(charWidth(U'ำ'), 1);       // Thai SARA AM is a spacing letter
    WT_CHECK_EQ(charWidth(U'ั'), 0);       // Thai MAI HAN-AKAT is a combining mark
    WT_CHECK_EQ(charWidth(U'\U0001F680'), 2);   // rocket (emoji presentation)
    WT_CHECK_EQ(charWidth(U'\U0001FAE8'), 2);   // shaking face (Unicode 15)
    WT_CHECK_EQ(charWidth(U'　'), 2);       // ideographic space
    WT_CHECK_EQ(charWidth(U'｡'), 1);       // halfwidth ideographic full stop
    WT_CHECK_EQ(charWidth(U'\U00020000'), 2);   // CJK extension B
    WT_CHECK_EQ(charWidth(U'\U000E0001'), 0);   // language tag (Cf)
}
