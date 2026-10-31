#include <unity.h>

void run_transport_tests();
void run_gnss_tests();
void run_time_tests();
void run_logging_tests();
void run_bridge_tests();
void run_oled_tests();
void run_receiver_aux_tests();
void run_tim1_capture_tests();

extern "C" void setUp() {}
extern "C" void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  run_transport_tests();
  run_gnss_tests();
  run_time_tests();
  run_logging_tests();
  run_bridge_tests();
  run_oled_tests();
  run_receiver_aux_tests();
  run_tim1_capture_tests();
  return UNITY_END();
}
