#include <unity.h>

void run_ubx_parser_tests();
void run_qzss_l1s_tests();
void run_v020_tests();
void run_v021_tests();
void run_startup_logging_tests();
void run_v022_tests();
void run_v023_tests();
void run_v024_tests();
void run_v025_tests();

extern "C" void setUp() {}
extern "C" void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  run_ubx_parser_tests();
  run_qzss_l1s_tests();
  run_v020_tests();
  run_v021_tests();
  run_startup_logging_tests();
  run_v022_tests();
  run_v023_tests();
  run_v024_tests();
  run_v025_tests();
  return UNITY_END();
}
