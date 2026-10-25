#include "WtTest.h"

#include "wt/core/Base64.h"
#include "wt/core/Json.h"

using namespace wt::core;

WT_TEST(base64_round_trip_and_rfc4648_vectors) {
    WT_CHECK_EQ(base64Encode(""), std::string(""));
    WT_CHECK_EQ(base64Encode("f"), std::string("Zg=="));
    WT_CHECK_EQ(base64Encode("fo"), std::string("Zm8="));
    WT_CHECK_EQ(base64Encode("foo"), std::string("Zm9v"));
    WT_CHECK_EQ(base64Encode("foobar"), std::string("Zm9vYmFy"));
    WT_CHECK(base64Decode("Zm9vYmFy") == std::optional<std::string>("foobar"));
    WT_CHECK(base64Decode("Zm8=") == std::optional<std::string>("fo"));
    WT_CHECK(base64Decode("Zm8") == std::optional<std::string>("fo")); // unpadded
    WT_CHECK(base64Decode("Zm9v\nYmFy") == std::optional<std::string>("foobar"));
    WT_CHECK(!base64Decode("Zm9v!"));
    WT_CHECK(!base64Decode("Z"));
    WT_CHECK(!base64Decode("Zg==Zg"));
    std::string binary;
    for (int i = 0; i < 256; ++i) {
        binary.push_back(static_cast<char>(i));
    }
    WT_CHECK(base64Decode(base64Encode(binary)) == std::optional<std::string>(binary));
}

WT_TEST(hex_round_trip) {
    WT_CHECK_EQ(hexEncode("\x01\xab"), std::string("01ab"));
    WT_CHECK(hexDecode("01AB") == std::optional<std::string>("\x01\xab"));
    WT_CHECK(!hexDecode("0"));
    WT_CHECK(!hexDecode("zz"));
}

WT_TEST(json_parses_and_dumps_values) {
    const auto doc = Json::parse(R"({"name":"tab \"1\"","size":12,"ratio":0.5,"on":true,"off":false,
        "none":null,"list":[1,2,[3]],"nested":{"a":-1e3}})");
    WT_REQUIRE(doc.has_value());
    WT_CHECK_EQ((*doc)["name"].toString(), std::string("tab \"1\""));
    WT_CHECK_EQ((*doc)["size"].toInt(), 12);
    WT_CHECK_EQ((*doc)["ratio"].toDouble(), 0.5);
    WT_CHECK((*doc)["on"].toBool());
    WT_CHECK(!(*doc)["off"].toBool(true));
    WT_CHECK((*doc)["none"].isNull());
    WT_CHECK((*doc)["missing"].isNull());
    WT_CHECK_EQ((*doc)["list"].items().size(), std::size_t(3));
    WT_CHECK_EQ((*doc)["nested"]["a"].toInt(), -1000);
    // Type mismatches fall back.
    WT_CHECK_EQ((*doc)["name"].toInt(7), 7);
    WT_CHECK_EQ((*doc)["size"].toString("x"), std::string("x"));

    const std::string compact = doc->dump();
    WT_CHECK(Json::parse(compact) == doc);
    WT_CHECK(Json::parse(doc->dump(2)) == doc);
    WT_CHECK_EQ(Json::parse("[1.25,\"\\u00e9\\ud83d\\ude00\\n\"]")->dump(), std::string("[1.25,\"é😀\\n\"]"));
}

WT_TEST(json_builds_documents) {
    Json root;
    root["b"] = 2;
    root["a"]["x"] = "y";
    root["list"].push(true);
    root["list"].push(Json());
    WT_CHECK_EQ(root.dump(), std::string(R"({"a":{"x":"y"},"b":2,"list":[true,null]})"));
    WT_CHECK(root.contains("a"));
    WT_CHECK(!root.contains("z"));
    WT_CHECK_EQ(Json(std::int64_t(9007199254740991LL)).dump(), std::string("9007199254740991"));
    WT_CHECK_EQ(Json(std::string("\x01")).dump(), std::string("\"\\u0001\""));
}

WT_TEST(json_rejects_malformed_input) {
    std::string error;
    WT_CHECK(!Json::parse("{", &error));
    WT_CHECK(!error.empty());
    WT_CHECK(!Json::parse("[1,]"));
    WT_CHECK(!Json::parse("{\"a\" 1}"));
    WT_CHECK(!Json::parse("01"));
    WT_CHECK(!Json::parse("1 2"));
    WT_CHECK(!Json::parse("\"tab\there\""));
    WT_CHECK(!Json::parse("tru"));
    WT_CHECK(!Json::parse(std::string(1000, '[') + std::string(1000, ']'))); // depth limit
    WT_CHECK(Json::parse(std::string(200, '[') + std::string(200, ']')).has_value());
}

WT_TEST(json_lone_high_surrogate_keeps_the_next_character) {
    WT_CHECK_EQ(Json::parse("\"\\ud800\\u0041\"")->toString(), std::string("\xEF\xBF\xBD" "A"));
    WT_CHECK_EQ(Json::parse("\"\\udc00x\"")->toString(), std::string("\xEF\xBF\xBD" "x"));
    WT_CHECK_EQ(Json::parse("\"\\ud83d\\ude00\"")->toString(), std::string("\xF0\x9F\x98\x80"));
}
