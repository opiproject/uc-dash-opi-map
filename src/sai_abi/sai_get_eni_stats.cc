#include "sai_abi/sai_create_eni.h"

#include "dash_opi/adapter.h"
#include "dash_opi/grpc_bridge.h"
#include "dash_opi/log.h"

#include "telemetry/v1/telemetry.grpc.pb.h"

#include <cstdint>
#include <sstream>
#include <string>

extern "C" {
#include <saiexperimentaldasheni.h>
}

namespace tel = opi_api::telemetry::v1;

namespace {

// FlexCounter / dashorch pass a sparse list of SAI_ENI_STAT_* ids. Fill the
// corresponding counters[] slot; unmapped ids stay 0 so the poller still
// receives a complete array (never an unknown SAI status code).
uint64_t MapEniStat(sai_stat_id_t id, const tel::VnicMetrics& m) {
  switch (id) {
    case SAI_ENI_STAT_RX_BYTES:
      return m.rx_bytes();
    case SAI_ENI_STAT_RX_PACKETS:
      return m.rx_packets();
    case SAI_ENI_STAT_TX_BYTES:
      return m.tx_bytes();
    case SAI_ENI_STAT_TX_PACKETS:
      return m.tx_packets();
    case SAI_ENI_STAT_POLICY_DROP_PACKETS:
      return m.policy_drops();
    case SAI_ENI_STAT_FORWARDING_DROP_PACKETS:
      return m.forwarding_drops();
    case SAI_ENI_STAT_OTHER_DROP_PACKETS:
      return m.rx_drops() + m.tx_drops();
    case SAI_ENI_STAT_TOTAL_DROP_PACKETS:
      return m.rx_drops() + m.tx_drops() + m.policy_drops() +
             m.forwarding_drops();
    default:
      // Valid ENI stat that OPI telemetry does not expose — leave 0 so
      // FlexCounter still gets a full array (do not return an unknown code).
      return 0;
  }
}

}  // namespace

extern "C" sai_status_t sai_get_eni_stats(
    sai_object_id_t eni_id,
    uint32_t number_of_counters,
    const sai_stat_id_t* counter_ids,
    uint64_t* counters) {
  if (eni_id == SAI_NULL_OBJECT_ID ||
      (number_of_counters > 0 && (counter_ids == nullptr || counters == nullptr))) {
    return SAI_STATUS_INVALID_PARAMETER;
  }

  auto& adapter = dash_opi::Adapter::Instance();
  tel::TelemetryService::Stub* tel_stub = adapter.telemetry();
  if (tel_stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const auto vnic_name = adapter.store().Name(eni_id);
  if (!vnic_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }

  tel::GetVnicMetricsRequest req;
  req.set_name(*vnic_name);
  tel::VnicMetrics metrics;
  const sai_status_t st = dash_opi::BlockingUnary(
      tel_stub, &tel::TelemetryService::Stub::GetVnicMetrics, req, &metrics,
      adapter.rpc_deadline());
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi::LogXlate("GetVnicMetrics name=" + *vnic_name +
                       " grpc_status=" + std::to_string(st));
    return st;
  }

  for (uint32_t i = 0; i < number_of_counters; ++i) {
    counters[i] = MapEniStat(counter_ids[i], metrics);
  }

  std::ostringstream os;
  os << "GetVnicMetrics name=" << *vnic_name
     << " rx_bytes=" << metrics.rx_bytes()
     << " rx_packets=" << metrics.rx_packets()
     << " tx_bytes=" << metrics.tx_bytes()
     << " tx_packets=" << metrics.tx_packets()
     << " drops=" << (metrics.rx_drops() + metrics.tx_drops() +
                      metrics.policy_drops() + metrics.forwarding_drops())
     << " ncounters=" << number_of_counters;
  dash_opi::LogXlate(os.str());
  return SAI_STATUS_SUCCESS;
}
