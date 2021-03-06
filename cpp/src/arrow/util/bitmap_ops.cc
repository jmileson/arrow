// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/util/bitmap_ops.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>

#include "arrow/buffer.h"
#include "arrow/result.h"
#include "arrow/util/align_util.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/bitmap_reader.h"
#include "arrow/util/bitmap_writer.h"
#include "arrow/util/logging.h"
#include "arrow/util/ubsan.h"

namespace arrow {
namespace internal {

int64_t CountSetBits(const uint8_t* data, int64_t bit_offset, int64_t length) {
  constexpr int64_t pop_len = sizeof(uint64_t) * 8;
  DCHECK_GE(bit_offset, 0);
  int64_t count = 0;

  const auto p = BitmapWordAlign<pop_len / 8>(data, bit_offset, length);
  for (int64_t i = bit_offset; i < bit_offset + p.leading_bits; ++i) {
    if (BitUtil::GetBit(data, i)) {
      ++count;
    }
  }

  if (p.aligned_words > 0) {
    // popcount as much as possible with the widest possible count
    const uint64_t* u64_data = reinterpret_cast<const uint64_t*>(p.aligned_start);
    DCHECK_EQ(reinterpret_cast<size_t>(u64_data) & 7, 0);
    const uint64_t* end = u64_data + p.aligned_words;

    for (auto iter = u64_data; iter < end; ++iter) {
      count += BitUtil::PopCount(*iter);
    }
  }

  // Account for left over bits (in theory we could fall back to smaller
  // versions of popcount but the code complexity is likely not worth it)
  for (int64_t i = p.trailing_bit_offset; i < bit_offset + length; ++i) {
    if (BitUtil::GetBit(data, i)) {
      ++count;
    }
  }

  return count;
}

template <bool invert_bits, bool restore_trailing_bits>
void TransferBitmap(const uint8_t* data, int64_t offset, int64_t length,
                    int64_t dest_offset, uint8_t* dest) {
  int64_t byte_offset = offset / 8;
  int64_t bit_offset = offset % 8;
  int64_t dest_byte_offset = dest_offset / 8;
  int64_t dest_bit_offset = dest_offset % 8;
  int64_t num_bytes = BitUtil::BytesForBits(length);
  // Shift dest by its byte offset
  dest += dest_byte_offset;

  if (bit_offset || dest_bit_offset) {
    data += byte_offset;

    const int64_t n_words = length / 64;
    if (n_words > 1) {
      auto load_word = [](const uint8_t* bytes) -> uint64_t {
        return BitUtil::ToLittleEndian(util::SafeLoadAs<uint64_t>(bytes));
      };
      auto shift_word = [](uint64_t current, uint64_t next, int64_t shift) -> uint64_t {
        if (shift == 0) return current;
        return (current >> shift) | (next << (64 - shift));
      };
      auto write_word = [](uint8_t* bytes, uint64_t word) {
        util::SafeStore(bytes, BitUtil::FromLittleEndian(word));
      };

      const uint64_t dest_mask = (1U << dest_bit_offset) - 1;
      auto data_current = load_word(data);
      auto dest_current = load_word(dest);

      for (int64_t i = 0; i < n_words - 1; ++i) {
        data += 8;
        const auto data_next = load_word(data);
        auto word = shift_word(data_current, data_next, bit_offset);
        data_current = data_next;
        if (invert_bits) {
          word = ~word;
        }

        if (dest_bit_offset) {
          word = (word << dest_bit_offset) | (word >> (64 - dest_bit_offset));
          auto dest_next = load_word(dest + 8);
          dest_current = (dest_current & dest_mask) | (word & ~dest_mask);
          dest_next = (dest_next & ~dest_mask) | (word & dest_mask);
          write_word(dest, dest_current);
          write_word(dest + 8, dest_next);
          dest_current = dest_next;
        } else {
          write_word(dest, word);
        }
        dest += 8;
      }

      length -= (n_words - 1) * 64;
    }

    internal::BitmapReader valid_reader(data, bit_offset, length);
    internal::BitmapWriter valid_writer(dest, dest_bit_offset, length);

    for (int64_t i = 0; i < length; i++) {
      if (invert_bits ^ valid_reader.IsSet()) {
        valid_writer.Set();
      } else {
        valid_writer.Clear();
      }
      valid_reader.Next();
      valid_writer.Next();
    }
    valid_writer.Finish();
  } else {
    // Take care of the trailing bits in the last byte
    int64_t trailing_bits = num_bytes * 8 - length;
    uint8_t trail = 0;
    if (trailing_bits && restore_trailing_bits) {
      trail = dest[num_bytes - 1];
    }

    if (invert_bits) {
      for (int64_t i = 0; i < num_bytes; i++) {
        dest[i] = static_cast<uint8_t>(~(data[byte_offset + i]));
      }
    } else {
      std::memcpy(dest, data + byte_offset, static_cast<size_t>(num_bytes));
    }

    if (restore_trailing_bits) {
      for (int i = 0; i < trailing_bits; i++) {
        if (BitUtil::GetBit(&trail, i + 8 - trailing_bits)) {
          BitUtil::SetBit(dest, length + i);
        } else {
          BitUtil::ClearBit(dest, length + i);
        }
      }
    }
  }
}

template <bool invert_bits>
Result<std::shared_ptr<Buffer>> TransferBitmap(MemoryPool* pool, const uint8_t* data,
                                               int64_t offset, int64_t length) {
  ARROW_ASSIGN_OR_RAISE(auto buffer, AllocateEmptyBitmap(length, pool));
  uint8_t* dest = buffer->mutable_data();

  TransferBitmap<invert_bits, false>(data, offset, length, 0, dest);

  // As we have freshly allocated this bitmap, we should take care of zeroing the
  // remaining bits.
  int64_t num_bytes = BitUtil::BytesForBits(length);
  int64_t bits_to_zero = num_bytes * 8 - length;
  for (int64_t i = length; i < length + bits_to_zero; ++i) {
    // Both branches may copy extra bits - unsetting to match specification.
    BitUtil::ClearBit(dest, i);
  }
  return buffer;
}

void CopyBitmap(const uint8_t* data, int64_t offset, int64_t length, uint8_t* dest,
                int64_t dest_offset, bool restore_trailing_bits) {
  if (restore_trailing_bits) {
    TransferBitmap<false, true>(data, offset, length, dest_offset, dest);
  } else {
    TransferBitmap<false, false>(data, offset, length, dest_offset, dest);
  }
}

void InvertBitmap(const uint8_t* data, int64_t offset, int64_t length, uint8_t* dest,
                  int64_t dest_offset) {
  TransferBitmap<true, true>(data, offset, length, dest_offset, dest);
}

Result<std::shared_ptr<Buffer>> CopyBitmap(MemoryPool* pool, const uint8_t* data,
                                           int64_t offset, int64_t length) {
  return TransferBitmap<false>(pool, data, offset, length);
}

Result<std::shared_ptr<Buffer>> InvertBitmap(MemoryPool* pool, const uint8_t* data,
                                             int64_t offset, int64_t length,
                                             std::shared_ptr<Buffer>* out) {
  return TransferBitmap<true>(pool, data, offset, length);
}

bool BitmapEquals(const uint8_t* left, int64_t left_offset, const uint8_t* right,
                  int64_t right_offset, int64_t bit_length) {
  if (left_offset % 8 == 0 && right_offset % 8 == 0) {
    // byte aligned, can use memcmp
    bool bytes_equal = std::memcmp(left + left_offset / 8, right + right_offset / 8,
                                   bit_length / 8) == 0;
    if (!bytes_equal) {
      return false;
    }
    for (int64_t i = (bit_length / 8) * 8; i < bit_length; ++i) {
      if (BitUtil::GetBit(left, left_offset + i) !=
          BitUtil::GetBit(right, right_offset + i)) {
        return false;
      }
    }
    return true;
  }

  // Unaligned slow case
  left += left_offset / 8;
  right += right_offset / 8;
  left_offset %= 8;
  right_offset %= 8;

  // process in 64 bits, may touch two adjacent words in one iteration
  const int64_t n_words = bit_length / 64;
  if (n_words > 1) {
    auto load_word = [](const uint8_t* bytes) -> uint64_t {
      return BitUtil::ToLittleEndian(util::SafeLoadAs<uint64_t>(bytes));
    };
    auto shift_word = [](uint64_t current, uint64_t next, int64_t shift) -> uint64_t {
      if (shift == 0) return current;
      return (current >> shift) | (next << (64 - shift));
    };

    auto left_current = load_word(left);
    auto right_current = load_word(right);

    for (int64_t i = 0; i < n_words - 1; ++i) {
      left += 8;
      auto left_next = load_word(left);
      auto left_word = shift_word(left_current, left_next, left_offset);
      left_current = left_next;

      right += 8;
      auto right_next = load_word(right);
      auto right_word = shift_word(right_current, right_next, right_offset);
      right_current = right_next;

      if (left_word != right_word) {
        return false;
      }
    }

    bit_length -= (n_words - 1) * 64;
  }

  // process in bit
  for (int64_t i = 0; i < bit_length; ++i) {
    if (BitUtil::GetBit(left, left_offset + i) !=
        BitUtil::GetBit(right, right_offset + i)) {
      return false;
    }
  }
  return true;
}

namespace {

template <template <typename> class BitOp>
void AlignedBitmapOp(const uint8_t* left, int64_t left_offset, const uint8_t* right,
                     int64_t right_offset, uint8_t* out, int64_t out_offset,
                     int64_t length) {
  BitOp<uint8_t> op;
  DCHECK_EQ(left_offset % 8, right_offset % 8);
  DCHECK_EQ(left_offset % 8, out_offset % 8);

  const int64_t nbytes = BitUtil::BytesForBits(length + left_offset % 8);
  left += left_offset / 8;
  right += right_offset / 8;
  out += out_offset / 8;
  for (int64_t i = 0; i < nbytes; ++i) {
    out[i] = op(left[i], right[i]);
  }
}

template <template <typename> class BitOp, typename LogicalOp>
void UnalignedBitmapOp(const uint8_t* left, int64_t left_offset, const uint8_t* right,
                       int64_t right_offset, uint8_t* out, int64_t out_offset,
                       int64_t length) {
  using Word = uint64_t;

  left += left_offset / 8;
  right += right_offset / 8;
  out += out_offset / 8;

  left_offset %= 8;
  right_offset %= 8;
  out_offset %= 8;

  const int64_t min_offset = std::min({left_offset, right_offset, out_offset});
  const int64_t min_nbytes = BitUtil::BytesForBits(length + min_offset);
  int64_t nwords = min_nbytes / sizeof(Word);

  // process in words, we may touch two words in each iteration
  if (nwords > 1) {
    BitOp<Word> op;
    constexpr int64_t bits_per_word = sizeof(Word) * 8;
    const Word out_mask = (1U << out_offset) - 1;

    length -= (nwords - 1) * bits_per_word;
    Word left_word0 = BitUtil::ToLittleEndian(util::SafeLoadAs<Word>(left));
    Word right_word0 = BitUtil::ToLittleEndian(util::SafeLoadAs<Word>(right));
    Word out_word0 = BitUtil::ToLittleEndian(util::SafeLoadAs<Word>(out));

    do {
      left += sizeof(Word);
      const Word left_word1 = BitUtil::ToLittleEndian(util::SafeLoadAs<Word>(left));
      Word left_word = left_word0;
      if (left_offset) {
        // combine two adjacent words into one word
        // |<-- left_word1 --->|<-- left_word0 --->|
        // +-------------+-----+-------------+-----+
        // |     ---     |  A  |      B      | --- |
        // +-------------+-----+-------------+-----+
        //                  |         |       offset
        //                  v         v
        //               +-----+-------------+
        //               |  A  |      B      |
        //               +-----+-------------+
        //               |<--- left_word --->|
        left_word >>= left_offset;
        left_word |= left_word1 << (bits_per_word - left_offset);
      }
      left_word0 = left_word1;

      right += sizeof(Word);
      const Word right_word1 = BitUtil::ToLittleEndian(util::SafeLoadAs<Word>(right));
      Word right_word = right_word0;
      if (right_offset) {
        right_word >>= right_offset;
        right_word |= right_word1 << (bits_per_word - right_offset);
      }
      right_word0 = right_word1;

      Word out_word = op(left_word, right_word);
      if (out_offset) {
        // break one word into two adjacent words, don't touch unused bits
        //               |<---- out_word --->|
        //               +-----+-------------+
        //               |  A  |      B      |
        //               +-----+-------------+
        //                  |         |
        //                  v         v       offset
        // +-------------+-----+-------------+-----+
        // |     ---     |  A  |      B      | --- |
        // +-------------+-----+-------------+-----+
        // |<--- out_word1 --->|<--- out_word0 --->|
        out_word = (out_word << out_offset) | (out_word >> (bits_per_word - out_offset));
        Word out_word1 = util::SafeLoadAs<Word>(out + sizeof(Word));
        out_word1 = BitUtil::ToLittleEndian(out_word1);
        out_word0 = (out_word0 & out_mask) | (out_word & ~out_mask);
        out_word1 = (out_word1 & ~out_mask) | (out_word & out_mask);
        util::SafeStore(out, BitUtil::FromLittleEndian(out_word0));
        util::SafeStore(out + sizeof(Word), BitUtil::FromLittleEndian(out_word1));
        out_word0 = out_word1;
      } else {
        util::SafeStore(out, BitUtil::FromLittleEndian(out_word));
      }
      out += sizeof(Word);

      --nwords;
    } while (nwords > 1);
  }

  // process in bits
  if (length) {
    auto left_reader = internal::BitmapReader(left, left_offset, length);
    auto right_reader = internal::BitmapReader(right, right_offset, length);
    auto writer = internal::BitmapWriter(out, out_offset, length);
    LogicalOp op;
    while (length--) {
      if (op(left_reader.IsSet(), right_reader.IsSet())) {
        writer.Set();
      } else {
        writer.Clear();
      }
      left_reader.Next();
      right_reader.Next();
      writer.Next();
    }
    writer.Finish();
  }
}

template <template <typename> class BitOp, typename LogicalOp>
void BitmapOp(const uint8_t* left, int64_t left_offset, const uint8_t* right,
              int64_t right_offset, int64_t length, int64_t out_offset, uint8_t* dest) {
  if ((out_offset % 8 == left_offset % 8) && (out_offset % 8 == right_offset % 8)) {
    // Fast case: can use bytewise AND
    AlignedBitmapOp<BitOp>(left, left_offset, right, right_offset, dest, out_offset,
                           length);
  } else {
    // Unaligned
    UnalignedBitmapOp<BitOp, LogicalOp>(left, left_offset, right, right_offset, dest,
                                        out_offset, length);
  }
}

template <template <typename> class BitOp, typename LogicalOp>
Result<std::shared_ptr<Buffer>> BitmapOp(MemoryPool* pool, const uint8_t* left,
                                         int64_t left_offset, const uint8_t* right,
                                         int64_t right_offset, int64_t length,
                                         int64_t out_offset) {
  const int64_t phys_bits = length + out_offset;
  ARROW_ASSIGN_OR_RAISE(auto out_buffer, AllocateEmptyBitmap(phys_bits, pool));
  BitmapOp<BitOp, LogicalOp>(left, left_offset, right, right_offset, length, out_offset,
                             out_buffer->mutable_data());
  return out_buffer;
}

}  // namespace

Result<std::shared_ptr<Buffer>> BitmapAnd(MemoryPool* pool, const uint8_t* left,
                                          int64_t left_offset, const uint8_t* right,
                                          int64_t right_offset, int64_t length,
                                          int64_t out_offset) {
  return BitmapOp<std::bit_and, std::logical_and<bool>>(pool, left, left_offset, right,
                                                        right_offset, length, out_offset);
}

void BitmapAnd(const uint8_t* left, int64_t left_offset, const uint8_t* right,
               int64_t right_offset, int64_t length, int64_t out_offset, uint8_t* out) {
  BitmapOp<std::bit_and, std::logical_and<bool>>(left, left_offset, right, right_offset,
                                                 length, out_offset, out);
}

Result<std::shared_ptr<Buffer>> BitmapOr(MemoryPool* pool, const uint8_t* left,
                                         int64_t left_offset, const uint8_t* right,
                                         int64_t right_offset, int64_t length,
                                         int64_t out_offset) {
  return BitmapOp<std::bit_or, std::logical_or<bool>>(pool, left, left_offset, right,
                                                      right_offset, length, out_offset);
}

void BitmapOr(const uint8_t* left, int64_t left_offset, const uint8_t* right,
              int64_t right_offset, int64_t length, int64_t out_offset, uint8_t* out) {
  BitmapOp<std::bit_or, std::logical_or<bool>>(left, left_offset, right, right_offset,
                                               length, out_offset, out);
}

Result<std::shared_ptr<Buffer>> BitmapXor(MemoryPool* pool, const uint8_t* left,
                                          int64_t left_offset, const uint8_t* right,
                                          int64_t right_offset, int64_t length,
                                          int64_t out_offset) {
  return BitmapOp<std::bit_xor, std::bit_xor<bool>>(pool, left, left_offset, right,
                                                    right_offset, length, out_offset);
}

void BitmapXor(const uint8_t* left, int64_t left_offset, const uint8_t* right,
               int64_t right_offset, int64_t length, int64_t out_offset, uint8_t* out) {
  BitmapOp<std::bit_xor, std::bit_xor<bool>>(left, left_offset, right, right_offset,
                                             length, out_offset, out);
}

}  // namespace internal
}  // namespace arrow
