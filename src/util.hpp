#pragma once

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace ipbrs {

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// SplitMix64: small, fast and deterministic. Used for datasets and damage selection.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n ? next() % n : 0; }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

private:
    std::uint64_t state_;
};

inline std::uint64_t mix_seed(std::uint64_t seed, std::uint64_t index) {
    return Rng(seed ^ (index * 0xD1B54A32D192ED03ull + 0x8CB92BA72F3D8DD7ull)).next();
}

// Minimal streaming JSON writer for result records.
class Json {
public:
    Json& begin_object();
    Json& end_object();
    Json& begin_array();
    Json& end_array();
    Json& key(const std::string& name);
    Json& value(const std::string& text);
    Json& value(const char* text) { return value(std::string(text)); }
    Json& value(std::uint64_t number);
    Json& value(std::uint32_t number) { return value(static_cast<std::uint64_t>(number)); }
    Json& value(std::int64_t number);
    Json& value(int number) { return value(static_cast<std::int64_t>(number)); }
    Json& value(double number);
    Json& value(bool flag);
    template <class T>
    Json& field(const std::string& name, const T& v) { key(name); return value(v); }
    std::string str() const { return out_.str(); }

private:
    void separate();
    void escape(const std::string& text);
    std::ostringstream out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

} // namespace ipbrs
