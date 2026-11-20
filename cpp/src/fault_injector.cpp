#include "sdv/fault_injector.hpp"

#include <stdexcept>

namespace sdv {

std::vector<CanFrame> DeterministicFaultInjector::apply(const CanFrame& frame,
                                                        std::size_t ordinal) const {
    if (plan_.every_n == 0U) {
        throw std::invalid_argument("every_n must be positive");
    }
    if (ordinal == 0U || ordinal % plan_.every_n != 0U || plan_.kind == FaultKind::None) {
        return {frame};
    }

    CanFrame mutated = frame;
    switch (plan_.kind) {
        case FaultKind::Drop:
            return {};
        case FaultKind::Duplicate:
            return {frame, frame};
        case FaultKind::CorruptDlc:
            mutated.dlc = 7U;
            return {mutated};
        case FaultKind::DelayTimestamp:
            mutated.bus_time_ms += plan_.amount_ms;
            return {mutated};
        case FaultKind::StaleTimestamp:
            mutated.bus_time_ms -= plan_.amount_ms;
            return {mutated};
        case FaultKind::None:
            return {frame};
    }
    return {frame};
}

}  // namespace sdv

