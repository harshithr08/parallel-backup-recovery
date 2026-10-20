#include "testing.hpp"

#include "sha256.hpp"
#include "util.hpp"

#include <string>
#include <vector>

using namespace ipbrs;

namespace {

std::string hex_of(const std::string& text) { return to_hex(sha256(text.data(), text.size())); }

} // namespace

// Known-answer vectors from FIPS 180-4 / NIST CSRC examples.
TEST(sha256_nist_vectors) {
    CHECK_EQ(hex_of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hex_of("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
             std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    CHECK_EQ(hex_of(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// Padding boundaries: 55/56/63/64/65 bytes straddle the length-field cut-over.
TEST(sha256_streaming_matches_one_shot) {
    Rng rng(3);
    std::vector<std::uint8_t> data(5000);
    for (std::uint8_t& b : data) b = static_cast<std::uint8_t>(rng.next());
    const std::size_t sizes[] = {0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 1000, 5000};
    for (std::size_t size : sizes) {
        const Digest expected = sha256(data.data(), size);
        Sha256 hasher;
        std::size_t done = 0;
        while (done < size) {
            const std::size_t piece = std::min<std::size_t>(size - done, 1 + rng.below(70));
            hasher.update(data.data() + done, piece);
            done += piece;
        }
        CHECK(hasher.finish() == expected);
    }
}

TEST(sha256_hex_round_trip_and_rejects_invalid) {
    const Digest digest = sha256("abc", 3);
    const std::string hex = to_hex(digest);
    Digest parsed{};
    CHECK(parse_hex(hex, 0, parsed));
    CHECK(parsed == digest);
    CHECK(parse_hex("xx" + hex, 2, parsed));
    CHECK(!parse_hex(hex.substr(0, 63), 0, parsed));       // too short
    std::string upper = hex;
    upper[0] = 'B';
    CHECK(!parse_hex(upper, 0, parsed));                    // catalog hex is lowercase
    std::string bad = hex;
    bad[10] = 'g';
    CHECK(!parse_hex(bad, 0, parsed));
}
