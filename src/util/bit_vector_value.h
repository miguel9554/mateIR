#pragma once

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace mate {

// Word storage for BitVectorValue with inline capacity for one 64-bit word, so
// values up to 64 bits wide (the overwhelmingly common case in generated
// models) never touch the heap.
class BitVectorWords {
public:
    BitVectorWords() = default;
    explicit BitVectorWords(size_t count) : size_(count) {
        if (count > kInline) {
            heap_ = new uint64_t[count]();
        }
    }
    BitVectorWords(const BitVectorWords& other) { copyFrom(other); }
    BitVectorWords& operator=(const BitVectorWords& other) {
        if (this != &other) {
            release();
            copyFrom(other);
        }
        return *this;
    }
    BitVectorWords(BitVectorWords&& other) noexcept { moveFrom(other); }
    BitVectorWords& operator=(BitVectorWords&& other) noexcept {
        if (this != &other) {
            release();
            moveFrom(other);
        }
        return *this;
    }
    ~BitVectorWords() { release(); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint64_t* data() { return heap_ ? heap_ : inline_; }
    const uint64_t* data() const { return heap_ ? heap_ : inline_; }
    uint64_t& operator[](size_t i) { return data()[i]; }
    const uint64_t& operator[](size_t i) const { return data()[i]; }
    uint64_t& back() { return data()[size_ - 1]; }
    const uint64_t& back() const { return data()[size_ - 1]; }
    uint64_t* begin() { return data(); }
    uint64_t* end() { return data() + size_; }
    const uint64_t* begin() const { return data(); }
    const uint64_t* end() const { return data() + size_; }

    bool operator==(const BitVectorWords& other) const {
        if (size_ != other.size_) return false;
        const uint64_t* a = data();
        const uint64_t* b = other.data();
        for (size_t i = 0; i < size_; ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

private:
    static constexpr size_t kInline = 1;
    size_t size_ = 0;
    uint64_t inline_[kInline] = {0};
    uint64_t* heap_ = nullptr;

    void copyFrom(const BitVectorWords& other) {
        size_ = other.size_;
        if (size_ > kInline) {
            heap_ = new uint64_t[size_];
            for (size_t i = 0; i < size_; ++i) heap_[i] = other.heap_[i];
        } else {
            heap_ = nullptr;
            for (size_t i = 0; i < kInline; ++i) inline_[i] = other.inline_[i];
        }
    }
    void moveFrom(BitVectorWords& other) {
        size_ = other.size_;
        heap_ = other.heap_;
        for (size_t i = 0; i < kInline; ++i) inline_[i] = other.inline_[i];
        other.heap_ = nullptr;
        other.size_ = 0;
    }
    void release() {
        delete[] heap_;
        heap_ = nullptr;
    }
};

class BitVectorValue {
public:
    BitVectorValue() = default;

    static BitVectorValue zero(int width, bool is_signed = false);
    static BitVectorValue ones(int width, bool is_signed = false);
    static BitVectorValue fromU64(uint64_t value, int width, bool is_signed = false);
    static BitVectorValue fromI64(int64_t value, int width, bool is_signed = false);
    static BitVectorValue fromDecimalString(const std::string& text, int width, bool is_signed = false);
    static BitVectorValue fromHexString(const std::string& text, int width, bool is_signed = false);
    static BitVectorValue random(int width, bool is_signed, std::mt19937_64& rng);
    static BitVectorValue concat(std::span<const BitVectorValue> parts);
    static BitVectorValue aggregate(std::vector<BitVectorValue> elements);

    int width() const { return width_; }
    bool isSigned() const { return signed_; }
    bool isAggregate() const { return aggregate_; }
    const std::vector<BitVectorValue>& elements() const { return elements_; }
    const BitVectorValue& element(size_t index) const;
    bool isZero() const;
    uint64_t lowU64() const;

    bool getBit(int bit) const;
    void setBit(int bit, bool value);

    BitVectorValue resized(int width, bool is_signed) const;
    BitVectorValue slice(int high, int low) const;
    BitVectorValue shl(uint64_t amount) const;
    BitVectorValue shr(uint64_t amount, bool arithmetic) const;
    BitVectorValue negated() const;

    BitVectorValue bitwiseNot() const;
    BitVectorValue bitwiseAnd(const BitVectorValue& rhs) const;
    BitVectorValue bitwiseOr(const BitVectorValue& rhs) const;
    BitVectorValue bitwiseXor(const BitVectorValue& rhs) const;
    BitVectorValue bitwiseXnor(const BitVectorValue& rhs) const;

    BitVectorValue add(const BitVectorValue& rhs) const;
    BitVectorValue sub(const BitVectorValue& rhs) const;
    BitVectorValue mul(const BitVectorValue& rhs) const;

    bool eq(const BitVectorValue& rhs) const;
    bool unsignedLt(const BitVectorValue& rhs) const;
    bool signedLt(const BitVectorValue& rhs) const;

    bool reductionAnd() const;
    bool reductionOr() const;
    bool reductionXor() const;

    std::string toBinaryString() const;

private:
    int width_ = 0;
    bool signed_ = false;
    bool aggregate_ = false;
    BitVectorWords words_;
    std::vector<BitVectorValue> elements_;

    explicit BitVectorValue(int width, bool is_signed);

    static size_t wordCount(int width);
    void maskTopWord();
    void mulAddSmall(uint32_t mul, uint32_t add);
    void copyBitsFrom(const BitVectorValue& src, int src_start, int dst_start, int count);
};

} // namespace mate
