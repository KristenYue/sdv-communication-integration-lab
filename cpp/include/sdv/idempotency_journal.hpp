#pragma once

#include "sdv/vehicle_service_runtime.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace sdv {

struct RecoveredJournalRequest {
    std::string request_id;
    std::string fingerprint;
    ServiceResponse response;
};

class IdempotencyJournal {
public:
    explicit IdempotencyJournal(std::filesystem::path path);

    bool recover(std::vector<RecoveredJournalRequest>& requests, std::string& error) const;
    bool append_begin(const std::string& request_id, const std::string& fingerprint,
                      std::string& error);
    bool append_complete(const std::string& request_id, const std::string& fingerprint,
                         const ServiceResponse& response, std::string& error);

private:
    std::filesystem::path path_;
    bool append_record(const std::string& content, std::string& error);
};

}  // namespace sdv
