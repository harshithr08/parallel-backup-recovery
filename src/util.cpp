#include "util.hpp"

#include <cmath>
#include <iomanip>

namespace ipbrs {

void Json::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ << ',';
        first_.back() = false;
    }
}

void Json::escape(const std::string& text) {
    out_ << '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"': out_ << "\\\""; break;
            case '\\': out_ << "\\\\"; break;
            case '\n': out_ << "\\n"; break;
            case '\r': out_ << "\\r"; break;
            case '\t': out_ << "\\t"; break;
            default:
                if (c < 0x20) out_ << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                   << static_cast<int>(c) << std::dec << std::setfill(' ');
                else out_ << c;
        }
    }
    out_ << '"';
}

Json& Json::begin_object() { separate(); out_ << '{'; first_.push_back(true); return *this; }
Json& Json::end_object() { out_ << '}'; first_.pop_back(); return *this; }
Json& Json::begin_array() { separate(); out_ << '['; first_.push_back(true); return *this; }
Json& Json::end_array() { out_ << ']'; first_.pop_back(); return *this; }

Json& Json::key(const std::string& name) {
    separate();
    escape(name);
    out_ << ':';
    after_key_ = true;
    return *this;
}

Json& Json::value(const std::string& text) { separate(); escape(text); return *this; }
Json& Json::value(std::uint64_t number) { separate(); out_ << number; return *this; }
Json& Json::value(std::int64_t number) { separate(); out_ << number; return *this; }
Json& Json::value(bool flag) { separate(); out_ << (flag ? "true" : "false"); return *this; }

Json& Json::value(double number) {
    separate();
    if (std::isfinite(number)) out_ << std::setprecision(10) << number;
    else out_ << "null";
    return *this;
}

} // namespace ipbrs
