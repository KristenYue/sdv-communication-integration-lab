#include "sdv/diagnostic_manager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

sdv::VehicleSnapshot snapshot(double speed = 88.5) {
    sdv::VehicleSnapshot value;
    value.speed_kph = speed;
    value.battery_percent = 72;
    value.door_open = true;
    value.tire_kpa = {230, 231, 229, 232};
    value.door_open_mask = 1U;
    value.door_lock_mask = 14U;
    value.wiper_state = 2U;
    value.cloud_connected = true;
    value.signal_quality_percent = 85;
    value.upload_backlog = 4;
    return value;
}

std::filesystem::path temporary_journal(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void unlock(sdv::DiagnosticManager& manager, const std::string& client, std::int64_t now) {
    check(manager.start_session(client, sdv::DiagnosticSession::Extended, now, "session").accepted,
          "extended session entered");
    const auto seed = manager.request_security_seed(client, now + 1, "seed");
    check(seed.accepted, "security seed issued");
    check(manager
              .submit_security_key(client, seed.seed,
                                   sdv::DiagnosticManager::calculate_test_key(seed.seed), now + 2,
                                   "key")
              .accepted,
          "security key accepted");
}

void test_dtc_lifecycle_and_freeze_frame() {
    sdv::DiagnosticManager manager;
    check(manager.register_dtc(0xC10001U, "TPMS_FRONT_LEFT_LOW", 2U), "DTC registered");
    check(manager.begin_operation_cycle(1U, 100, "cycle-1").accepted, "cycle one started");
    check(manager.report_test_result(0xC10001U, true, snapshot(), 110, "fail-1").accepted,
          "first failure accepted");
    auto dtc = manager.all_dtcs().front();
    check(dtc.pending && !dtc.confirmed && dtc.occurrence_count == 1U,
          "first failed cycle creates pending DTC");
    check(dtc.freeze_frame.valid && dtc.freeze_frame.vehicle.speed_kph == 88.5,
          "first failure captures freeze frame");
    check(manager.report_test_result(0xC10001U, true, snapshot(99.0), 111, "repeat").accepted,
          "same-cycle repeated failure accepted idempotently");
    dtc = manager.all_dtcs().front();
    check(dtc.occurrence_count == 1U && dtc.freeze_frame.vehicle.speed_kph == 88.5,
          "same cycle does not inflate occurrence or replace freeze frame");

    check(manager.begin_operation_cycle(2U, 200, "cycle-2").accepted, "cycle two started");
    check(manager.report_test_result(0xC10001U, true, snapshot(100.0), 210, "fail-2").accepted,
          "second failed cycle accepted");
    dtc = manager.all_dtcs().front();
    check(dtc.confirmed && dtc.occurrence_count == 2U,
          "second consecutive failed cycle confirms DTC");

    for (std::uint64_t cycle = 3U; cycle <= 5U; ++cycle) {
        check(manager.begin_operation_cycle(cycle, static_cast<std::int64_t>(cycle * 100U),
                                            "pass-cycle")
                  .accepted,
              "passing cycle started");
        check(manager.report_test_result(0xC10001U, false, snapshot(),
                                         static_cast<std::int64_t>(cycle * 100U + 10U), "pass")
                  .accepted,
              "passing result accepted");
    }
    dtc = manager.all_dtcs().front();
    check(!dtc.pending && !dtc.confirmed && !dtc.test_failed,
          "three passing cycles age confirmed DTC inactive");
    check(manager.active_dtcs().empty(), "aged DTC absent from active view");
}

void test_security_lockout_and_session_timeout() {
    sdv::DiagnosticManager manager;
    check(manager.start_session("tool", sdv::DiagnosticSession::Extended, 1, "session").accepted,
          "security test enters extended session");
    const auto seed = manager.request_security_seed("tool", 2, "seed");
    check(seed.accepted, "security test seed issued");
    check(manager.submit_security_key("tool", seed.seed, 0U, 3, "bad-1").code == "INVALID_KEY",
          "first invalid key rejected");
    check(manager.submit_security_key("tool", seed.seed, 0U, 4, "bad-2").code == "INVALID_KEY",
          "second invalid key rejected");
    check(manager.submit_security_key("tool", seed.seed, 0U, 5, "bad-3").code ==
              "SECURITY_LOCKED",
          "third invalid key triggers lockout");
    check(manager.request_security_seed("tool", 100, "locked").code == "SECURITY_LOCKED",
          "seed denied during lockout");
    check(manager.start_session("tool", sdv::DiagnosticSession::Extended, 10'006, "new-session")
              .accepted,
          "extended session restored after lockout");
    const auto recovered_seed = manager.request_security_seed("tool", 10'007, "new-seed");
    check(recovered_seed.accepted, "seed issued after lockout");
    check(manager
              .submit_security_key(
                  "tool", recovered_seed.seed,
                  sdv::DiagnosticManager::calculate_test_key(recovered_seed.seed), 10'008,
                  "valid-key")
              .accepted,
          "valid key accepted after lockout");
    check(manager.clear_all("tool", 16'000, "expired-clear").code == "SESSION_DENIED",
          "extended session expires after inactivity");
}

void test_persistence_clear_and_corruption() {
    const auto path = temporary_journal("sdv-diagnostic-manager-test.journal");
    {
        sdv::DiagnosticManager manager(path);
        check(manager.register_dtc(0xC20001U, "TBOX_OFFLINE", 3U), "persistent DTC registered");
        check(manager.begin_operation_cycle(10U, 1'000, "cycle-10").accepted,
              "persistent cycle ten started");
        check(manager.report_test_result(0xC20001U, true, snapshot(), 1'010, "failure-10")
                  .accepted,
              "persistent first failure stored");
        check(manager.begin_operation_cycle(11U, 1'100, "cycle-11").accepted,
              "persistent cycle eleven started");
        check(manager.report_test_result(0xC20001U, true, snapshot(), 1'110, "failure-11")
                  .accepted,
              "persistent confirmation stored");
    }
    {
        sdv::DiagnosticManager recovered(path);
        check(recovered.register_dtc(0xC20001U, "TBOX_OFFLINE", 3U),
              "recovered DTC definition matches");
        check(recovered.active_dtcs().size() == 1U && recovered.active_dtcs().front().confirmed,
              "confirmed DTC recovered after restart");
        check(recovered.clear_all("tool", 2'000, "unauthorized").code == "SESSION_DENIED",
              "security session is not persisted across restart");
        unlock(recovered, "tool", 2'001);
        check(recovered.clear_all("tool", 2'010, "clear").accepted,
              "authorized clear persisted");
    }
    {
        sdv::DiagnosticManager recovered(path);
        check(recovered.active_dtcs().empty(), "cleared status recovered after second restart");
    }
    {
        std::ofstream output(path, std::ios::app);
        output << "corrupt-record\n";
    }
    bool corruption_rejected = false;
    try {
        sdv::DiagnosticManager corrupted(path);
    } catch (const std::runtime_error&) {
        corruption_rejected = true;
    }
    check(corruption_rejected, "corrupt diagnostic journal fails closed");
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int main() {
    test_dtc_lifecycle_and_freeze_frame();
    test_security_lockout_and_session_timeout();
    test_persistence_clear_and_corruption();
    if (failures == 0) {
        std::cout << "All diagnostic manager tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " diagnostic manager test(s) failed\n";
    return EXIT_FAILURE;
}
