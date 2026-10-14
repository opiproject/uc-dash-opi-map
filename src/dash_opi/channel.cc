#include "dash_opi/channel.h"

#include "dash_opi/log.h"

#include <cstdlib>
#include <fstream>

namespace dash_opi {
namespace {

std::string Trim(std::string s) {
  const auto begin = s.find_first_not_of(" \t\r\n\"'");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = s.find_last_not_of(" \t\r\n\"'");
  return s.substr(begin, end - begin + 1);
}

std::string KeyFromEnvFile(const char* path, const char* key) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind(prefix, 0) == 0) {
      return Trim(line.substr(prefix.size()));
    }
  }
  return {};
}

}  // namespace

bool EnvFlagSet(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return false;
  }
  return v[0] != '0' && v[0] != 'n' && v[0] != 'N' && v[0] != 'f' &&
         v[0] != 'F';
}

std::string ConfigValue(const char* key,
                        const sai_service_method_table_t* services) {
  if (const char* env = std::getenv(key)) {
    if (env[0] != '\0') {
      return env;
    }
  }
  for (const char* path : {"/etc/sonic/opi.env", "/etc/sai.d/opi.env"}) {
    std::string v = KeyFromEnvFile(path, key);
    if (!v.empty()) {
      return v;
    }
  }
  if (services && services->profile_get_value) {
    if (const char* p = services->profile_get_value(0, key)) {
      if (p[0] != '\0') {
        return p;
      }
    }
  }
  return {};
}

std::string SiblingHostPort(const std::string& hostport, const char* port) {
  if (hostport.empty() || port == nullptr || port[0] == '\0') {
    return {};
  }
  if (hostport.front() == '[') {
    const auto close = hostport.find(']');
    if (close != std::string::npos) {
      return hostport.substr(0, close + 1) + ":" + port;
    }
  }
  const auto colon = hostport.rfind(':');
  if (colon == std::string::npos) {
    return hostport + ":" + port;
  }
  return hostport.substr(0, colon + 1) + port;
}

std::string ResolveSnapTarget(const std::string& cloudinfra,
                              const sai_service_method_table_t* services) {
  std::string snap = ConfigValue("OPI_SNAP_ADDRESS", services);
  if (!snap.empty()) {
    return snap;
  }
  return SiblingHostPort(cloudinfra, "50052");
}

OpiEndpoints ResolveOpiEndpoints(const sai_service_method_table_t* services) {
  OpiEndpoints eps;
  eps.cloudinfra = ConfigValue("OPI_SERVER_ADDRESS", services);
  if (eps.cloudinfra.empty()) {
    eps.cloudinfra = "127.0.0.1:50051";
  }
  eps.snap = ResolveSnapTarget(eps.cloudinfra, services);
  return eps;
}

std::shared_ptr<grpc::Channel> OpenInsecureChannel(
    const std::string& target, std::chrono::milliseconds wait) {
  if (target.empty()) {
    return nullptr;
  }
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

  auto channel = grpc::CreateCustomChannel(
      target, grpc::InsecureChannelCredentials(), args);
  const auto deadline = std::chrono::system_clock::now() + wait;
  if (!channel->WaitForConnected(deadline)) {
    LogXlate("channel wait failed target=" + target);
    return nullptr;
  }
  return channel;
}

}  // namespace dash_opi
