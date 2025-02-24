// Copyright 2024, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/types/span.h>

#include <optional>

#include "src/facade/facade_types.h"

namespace dfly {

class EngineShard;
class Transaction;

using DbIndex = uint16_t;
using ShardId = uint16_t;
using LockFp = uint64_t;  // a key fingerprint used by the LockTable.

using ArgSlice = absl::Span<const std::string_view>;

constexpr DbIndex kInvalidDbId = DbIndex(-1);
constexpr ShardId kInvalidSid = ShardId(-1);
constexpr DbIndex kMaxDbId = 1024;  // Reasonable starting point.

struct KeyLockArgs {
  DbIndex db_index = 0;
  absl::Span<const LockFp> fps;
};

// Describes key indices.
struct KeyIndex {
  unsigned start;
  unsigned end;   // does not include this index (open limit).
  unsigned step;  // 1 for commands like mget. 2 for commands like mset.

  // if index is non-zero then adds another key index (usually 0).
  // relevant for for commands like ZUNIONSTORE/ZINTERSTORE for destination key.
  std::optional<uint16_t> bonus{};

  KeyIndex(unsigned s = 0, unsigned e = 0, unsigned step = 0) : start(s), end(e), step(step) {
  }

  static KeyIndex Range(unsigned start, unsigned end, unsigned step = 1) {
    return KeyIndex{start, end, step};
  }

  bool HasSingleKey() const {
    return !bonus && (start + step >= end);
  }

  unsigned num_args() const {
    return end - start + bool(bonus);
  }
};

struct DbContext {
  DbIndex db_index = 0;
  uint64_t time_now_ms = 0;
};

struct OpArgs {
  EngineShard* shard;
  const Transaction* tx;
  DbContext db_cntx;

  OpArgs() : shard(nullptr), tx(nullptr) {
  }

  OpArgs(EngineShard* s, const Transaction* tx, const DbContext& cntx)
      : shard(s), tx(tx), db_cntx(cntx) {
  }
};

// A strong type for a lock tag. Helps to disambiguate between keys and the parts of the
// keys that are used for locking.
class LockTag {
  std::string_view str_;

 public:
  using is_stackonly = void;  // marks that this object does not use heap.

  LockTag() = default;
  explicit LockTag(std::string_view key);

  explicit operator std::string_view() const {
    return str_;
  }

  LockFp Fingerprint() const;

  // To make it hashable.
  template <typename H> friend H AbslHashValue(H h, const LockTag& tag) {
    return H::combine(std::move(h), tag.str_);
  }

  bool operator==(const LockTag& o) const {
    return str_ == o.str_;
  }
};

// Checks whether the touched key is valid for a blocking transaction watching it.
using KeyReadyChecker =
    std::function<bool(EngineShard*, const DbContext& context, Transaction* tx, std::string_view)>;

// References arguments in another array.
using IndexSlice = std::pair<uint32_t, uint32_t>;  // [begin, end)

// ShardArgs - hold a span to full arguments and a span of sub-ranges
// referencing those arguments.
class ShardArgs {
  using ArgsIndexPair = std::pair<facade::CmdArgList, absl::Span<const IndexSlice>>;
  ArgsIndexPair slice_;

 public:
  class Iterator {
    facade::CmdArgList arglist_;
    absl::Span<const IndexSlice>::const_iterator index_it_;
    uint32_t delta_ = 0;

   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = std::string_view;
    using difference_type = ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;

    // First version, corresponds to spans over arguments.
    Iterator(facade::CmdArgList list, absl::Span<const IndexSlice>::const_iterator it)
        : arglist_(list), index_it_(it) {
    }

    bool operator==(const Iterator& o) const {
      return arglist_ == o.arglist_ && index_it_ == o.index_it_ && delta_ == o.delta_;
    }

    bool operator!=(const Iterator& o) const {
      return !(*this == o);
    }

    std::string_view operator*() const {
      return facade::ArgS(arglist_, index());
    }

    Iterator& operator++() {
      ++delta_;
      if (index() >= index_it_->second) {
        ++index_it_;
        ++delta_ = 0;
      }
      return *this;
    }

    size_t index() const {
      return index_it_->first + delta_;
    }
  };

  ShardArgs(facade::CmdArgList fa, absl::Span<const IndexSlice> s) : slice_(ArgsIndexPair(fa, s)) {
  }

  ShardArgs() : slice_(ArgsIndexPair{}) {
  }

  size_t Size() const;

  Iterator cbegin() const {
    return Iterator{slice_.first, slice_.second.begin()};
  }

  Iterator cend() const {
    return Iterator{slice_.first, slice_.second.end()};
  }

  Iterator begin() const {
    return cbegin();
  }

  Iterator end() const {
    return cend();
  }

  bool Empty() const {
    return slice_.second.empty();
  }

  std::string_view Front() const {
    return *cbegin();
  }
};

// Record non auto journal command with own txid and dbid.
void RecordJournal(const OpArgs& op_args, std::string_view cmd, const ShardArgs& args,
                   uint32_t shard_cnt = 1, bool multi_commands = false);
void RecordJournal(const OpArgs& op_args, std::string_view cmd, ArgSlice args,
                   uint32_t shard_cnt = 1, bool multi_commands = false);

// Record non auto journal command finish. Call only when command translates to multi commands.
void RecordJournalFinish(const OpArgs& op_args, uint32_t shard_cnt);

// Record expiry in journal with independent transaction. Must be called from shard thread holding
// key.
void RecordExpiry(DbIndex dbid, std::string_view key);

// Trigger journal write to sink, no journal record will be added to journal.
// Must be called from shard thread of journal to sink.
void TriggerJournalWriteToSink();

std::ostream& operator<<(std::ostream& os, ArgSlice list);

}  // namespace dfly
