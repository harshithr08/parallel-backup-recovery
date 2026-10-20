#include "testing.hpp"

#include "util.hpp"

using namespace ipbrs;

TEST(json_writer_nests_and_separates_fields) {
    Json json;
    json.begin_object().field("a", 1).field("b", "x");
    json.key("list").begin_array().value(std::uint64_t(2)).value(true).end_array();
    json.key("inner").begin_object().field("c", 0.5).end_object();
    json.end_object();
    CHECK_EQ(json.str(), std::string("{\"a\":1,\"b\":\"x\",\"list\":[2,true],\"inner\":{\"c\":0.5}}"));
}

TEST(json_writer_escapes_strings) {
    Json json;
    json.begin_object().field("p", "a\"b\\c\n").end_object();
    CHECK_EQ(json.str(), std::string("{\"p\":\"a\\\"b\\\\c\\n\"}"));
}

TEST(rng_is_deterministic_and_bounded) {
    Rng a(42), b(42);
    for (int i = 0; i < 1000; ++i) CHECK_EQ(a.next(), b.next());
    Rng c(7);
    for (int i = 0; i < 1000; ++i) {
        CHECK(c.below(10) < 10);
        const double u = c.unit();
        CHECK(u >= 0.0 && u < 1.0);
    }
    CHECK(mix_seed(1, 0) != mix_seed(1, 1));
}
