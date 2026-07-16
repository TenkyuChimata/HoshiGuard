#include "startup_metadata.h"

SafeFormatResult formatStartupMetadata(const StartupMetadata &m,
                                       char *buffer, size_t capacity) {
  return safeSnprintf(
      buffer, capacity,
      "META firmware=%s build_date=%s build_time=%s git_commit=%s "
      "config_layer=%s antenna=%s region=%s sky=%s nav_coordinates=%s",
      m.firmware, m.buildDate, m.buildTime, m.gitCommit, m.configLayer,
      m.antenna, m.region, m.skyView,
      m.navCoordinatesEnabled ? "enabled" : "disabled");
}

SafeFormatResult formatPrivacyMetadata(bool privacyMode,
                                       bool navPvtRawEnabled,
                                       char *buffer, size_t capacity) {
  return safeSnprintf(
      buffer, capacity,
      "META_PRIVACY privacy_mode=%s nav_pvt_raw=%s warning=%s",
      privacyMode ? "enabled" : "disabled",
      navPvtRawEnabled ? "enabled" : "disabled",
      navPvtRawEnabled ? "location_data_present" : "none");
}
