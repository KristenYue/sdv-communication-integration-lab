#pragma once

#include "sdv/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdv {

enum class FaultKind {
    None,
    Drop,
    Duplicate,
    CorruptDlc,
    DelayTimestamp,
    StaleTimestamp,
};

struct FaultPlan {
    FaultKind kind{FaultKind::None};
    std::size_t every_n{1};
    std::int64_t amount_ms{0};
};

class DeterministicFaultInjector {
public:
    explicit DeterministicFaultInjector(FaultPlan plan) : plan_(plan) {}
    std::vector<CanFrame> apply(const CanFrame& frame, std::size_t ordinal) const;

private:
    FaultPlan plan_;
};

}  // namespace sdv

