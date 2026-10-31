#include <unity.h>

void run_transport_tests();
void run_gnss_tests();
void run_time_tests();
void run_logging_tests();
void run_bridge_tests();

extern "C" void setUp() {}
extern "C" void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  run_transport_tests();
  run_gnss_tests();
  run_time_tests();
  run_logging_tests();
  run_bridge_tests();
  return UNITY_END();
}
