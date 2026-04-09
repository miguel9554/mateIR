#pragma once

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace custom_hdl {

class SimValue {
public:
    SimValue() = default;

    static SimValue zero(int width, bool is_signed = false);
    static SimValue ones(int width, bool is_signed = false);
    static SimValue fromU64(uint64_t value, int width, bool is_signed = false);
    static SimValue fromI64(int64_t value, int width, bool is_signed = false);
    static SimValue fromDecimalString(const std::string& text, int width, bool is_signed = false);
    static SimValue fromHexString(const std::string& text, int width, bool is_signed = false);
    static SimValue random(int width, bool is_signed, std::mt19937_64& rng);
    static SimValue concat(std::span<const SimValue> parts);

    int width() const { return width_; }
    bool isSigned() const { return signed_; }
    bool isZero() const;
    uint64_t lowU64() const;

    bool getBit(int bit) const;
    void setBit(int bit, bool value);

    SimValue resized(int width, bool is_signed) const;
    SimValue slice(int high, int low) const;
    SimValue shl(uint64_t amount) const;
    SimValue shr(uint64_t amount, bool arithmetic) const;
    SimValue negated() const;

    SimValue bitwiseNot() const;
    SimValue bitwiseAnd(const SimValue& rhs) const;
    SimValue bitwiseOr(const SimValue& rhs) const;
    SimValue bitwiseXor(const SimValue& rhs) const;
    SimValue bitwiseXnor(const SimValue& rhs) const;

    SimValue add(const SimValue& rhs) const;
    SimValue sub(const SimValue& rhs) const;
    SimValue mul(const SimValue& rhs) const;

    bool eq(const SimValue& rhs) const;
    bool unsignedLt(const SimValue& rhs) const;
    bool signedLt(const SimValue& rhs) const;

    bool reductionAnd() const;
    bool reductionOr() const;
    bool reductionXor() const;

    std::string toBinaryString() const;

private:
    int width_ = 0;
    bool signed_ = false;
    std::vector<uint64_t> words_;

    explicit SimValue(int width, bool is_signed);

    static size_t wordCount(int width);
    void maskTopWord();
    void mulAddSmall(uint32_t mul, uint32_t add);
    void copyBitsFrom(const SimValue& src, int src_start, int dst_start, int count);
};

} // namespace custom_hdl
