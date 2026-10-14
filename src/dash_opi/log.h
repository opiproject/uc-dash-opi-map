#pragma once

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <syslog.h>

namespace dash_opi {

// Translation traces for verify_dash_opi_flow.sh (prefix "sai-opi:").
inline void LogXlate(const std::string& message) {
  openlog("sai-opi-adapter", LOG_PID | LOG_NDELAY, LOG_DAEMON);
  syslog(LOG_INFO, "%s", message.c_str());

  std::cerr << "sai-opi: " << message << std::endl;

  const char* path = std::getenv("SAI_OPI_LOG");
  if (path == nullptr || path[0] == '\0') {
    path = "/var/log/sai-opi-adapter.log";
  }

  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  std::ofstream out(path, std::ios::app);
  if (!out) {
    out.open("/tmp/sai-opi-adapter.log", std::ios::app);
  }
  if (!out) {
    return;
  }
  const auto now = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  out << std::put_time(std::gmtime(&now), "%Y-%m-%dT%H:%M:%SZ") << " sai-opi: "
      << message << '\n';
}

inline std::string BytesToMac(const std::string& raw) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (size_t i = 0; i < raw.size(); ++i) {
    if (i != 0) {
      os << ':';
    }
    os << std::setw(2)
       << static_cast<unsigned>(static_cast<unsigned char>(raw[i]));
  }
  return os.str();
}

}  // namespace dash_opi
