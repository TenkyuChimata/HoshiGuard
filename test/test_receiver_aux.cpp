#include "test_support.h"
#include "receiver_aux.h"

using namespace auxiliary;
namespace {
void action(RecoveryAction expected, RecoveryAction actual) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}
void test_led_patterns_and_gnss_staleness() {
  StatusLed led;
  TEST_ASSERT_TRUE(led.tick(0U, true, false, false));
  TEST_ASSERT_FALSE(led.tick(250U, true, false, false));
  led.noteUbx();
  TEST_ASSERT_TRUE(led.tick(500U, false, false, false));
  led.noteUbx();
  TEST_ASSERT_FALSE(led.tick(550U, false, false, false));
  // L1S の通報・衛星データがなくても、UBX があれば正常。
  for (uint32_t now = 1000U; now <= 60000U; now += 500U) {
    led.noteUbx();
    (void)led.tick(now, false, false, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedState::Running), static_cast<int>(led.state()));
  }
  TEST_ASSERT_TRUE(led.tick(70000U, false, false, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LedState::Warning), static_cast<int>(led.state()));
  TEST_ASSERT_FALSE(led.tick(70100U, false, false, false));
  TEST_ASSERT_TRUE(led.tick(70200U, false, false, false));
  TEST_ASSERT_FALSE(led.tick(70300U, false, false, false));
  led.noteUbx();
  (void)led.tick(70400U, false, false, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LedState::Running), static_cast<int>(led.state()));
}
void test_led_fatal_config_error_and_rollover() {
  StatusLed led;
  led.noteUbx();
  (void)led.tick(UINT32_MAX - 30U, false, false, false);
  TEST_ASSERT_FALSE(led.tick(19U, false, false, false));
  TEST_ASSERT_TRUE(led.tick(20U, false, false, true));
  TEST_ASSERT_TRUE(led.tick(21U, false, true, false));
  TEST_ASSERT_FALSE(led.tick(121U, false, true, false));
  TEST_ASSERT_TRUE(led.tick(221U, false, true, false));
  TEST_ASSERT_TRUE(led.tick(421U, false, true, false));
  TEST_ASSERT_FALSE(led.tick(521U, false, true, false));
  (void)led.tick(UINT32_MAX - 10U, false, false, false);
  (void)led.tick(20U, false, false, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LedState::Warning), static_cast<int>(led.state()));
}
void test_oled_boot_switch_is_fail_closed_and_latched() {
  OledBootSwitch off;
  off.sample(false);
  TEST_ASSERT_FALSE(off.disabled());
  OledBootSwitch on;
  on.sample(true);
  on.sample(false);
  TEST_ASSERT_TRUE(on.disabled());
  // 他の OPTIONS、BAUDRATE、SAMPLERATE の組み合わせはこの入力を変更しない。
}
void test_recovery_software_first_pulse_boot_and_one_reset_only() {
  StartupRecovery recovery;
  const RecoveryInput failed = {true, false, false, false};
  action(RecoveryAction::None, recovery.tick(29999U, failed));
  action(RecoveryAction::RestartSoftware, recovery.tick(30000U, failed));
  action(RecoveryAction::None, recovery.tick(149999U, failed));
  action(RecoveryAction::AssertReset, recovery.tick(150000U, failed));
  TEST_ASSERT_TRUE(recovery.holdingReset());
  TEST_ASSERT_TRUE(recovery.suspendsReception());
  action(RecoveryAction::None, recovery.tick(150009U, failed));
  // 保持中の入力でも RESET は必ず解除される。
  const RecoveryInput noise = {false, true, true, true};
  action(RecoveryAction::ReleaseReset, recovery.tick(150010U, noise));
  TEST_ASSERT_FALSE(recovery.holdingReset());
  TEST_ASSERT_TRUE(recovery.suspendsReception());
  action(RecoveryAction::None, recovery.tick(151009U, failed));
  action(RecoveryAction::RestartSoftware, recovery.tick(151010U, failed));
  TEST_ASSERT_FALSE(recovery.suspendsReception());
  for (uint32_t now = 200000U; now < 100000000U; now += 100000U)
    action(RecoveryAction::None, recovery.tick(now, failed));
  TEST_ASSERT_EQUAL_UINT8(1U, recovery.hardwareResets());
}
void test_recovery_never_interrupts_initialization_or_healthy_receiver() {
  for (unsigned evidence = 0U; evidence < 3U; ++evidence) {
    StartupRecovery recovery;
    RecoveryInput input = {false, evidence == 1U, evidence == 2U, false};
    action(RecoveryAction::None, recovery.tick(30000U, input));
    action(RecoveryAction::None, recovery.tick(200000U, input));
    if (evidence != 0U) {
      input = {true, false, false, false};
      action(RecoveryAction::None, recovery.tick(400000U, input));
      TEST_ASSERT_EQUAL_UINT8(0U, recovery.hardwareResets());
    }
  }
}
void test_recovery_cancels_after_software_success_or_pps() {
  for (unsigned evidence = 0U; evidence < 2U; ++evidence) {
    StartupRecovery recovery;
    RecoveryInput input = {true, false, false, false};
    action(RecoveryAction::RestartSoftware, recovery.tick(30000U, input));
    input.validUbxSeen = evidence == 0U;
    input.ppsSeen = evidence == 1U;
    action(RecoveryAction::None, recovery.tick(150000U, input));
    input = {true, false, false, false};
    action(RecoveryAction::None, recovery.tick(300000U, input));
    TEST_ASSERT_EQUAL_UINT8(0U, recovery.hardwareResets());
  }
}
void test_recovery_requires_uart_silence_and_exhausted_software() {
  StartupRecovery recovery;
  RecoveryInput input = {true, false, false, true};
  action(RecoveryAction::RestartSoftware, recovery.tick(30000U, input));
  action(RecoveryAction::None, recovery.tick(150000U, input));
  input.rxBytes = false;
  action(RecoveryAction::None, recovery.tick(179999U, input));
  input.initFailed = false;
  action(RecoveryAction::None, recovery.tick(180000U, input));
  input.initFailed = true;
  action(RecoveryAction::AssertReset, recovery.tick(180001U, input));
}
void test_recovery_deadlines_across_wrap_do_not_rearm() {
  StartupRecovery recovery;
  const RecoveryInput failed = {true, false, false, false};
  action(RecoveryAction::RestartSoftware, recovery.tick(UINT32_MAX - 100U, failed));
  action(RecoveryAction::None, recovery.tick(119898U, failed));
  action(RecoveryAction::AssertReset, recovery.tick(119899U, failed));
  action(RecoveryAction::ReleaseReset, recovery.tick(119909U, failed));
  action(RecoveryAction::RestartSoftware, recovery.tick(120909U, failed));
  action(RecoveryAction::None, recovery.tick(UINT32_MAX - 10U, failed));
  action(RecoveryAction::None, recovery.tick(10U, failed));
  TEST_ASSERT_EQUAL_UINT8(1U, recovery.hardwareResets());
}
}
void run_receiver_aux_tests() {
  RUN_TEST(test_led_patterns_and_gnss_staleness);
  RUN_TEST(test_led_fatal_config_error_and_rollover);
  RUN_TEST(test_oled_boot_switch_is_fail_closed_and_latched);
  RUN_TEST(test_recovery_software_first_pulse_boot_and_one_reset_only);
  RUN_TEST(test_recovery_never_interrupts_initialization_or_healthy_receiver);
  RUN_TEST(test_recovery_cancels_after_software_success_or_pps);
  RUN_TEST(test_recovery_requires_uart_silence_and_exhausted_software);
  RUN_TEST(test_recovery_deadlines_across_wrap_do_not_rearm);
}
