/*
 * Simulate SONiC dashorch pushing a DASH ENI through the SAI C ABI.
 *
 *   cmake --build build --target test_sai_create_eni
 *   python tests/opi_server/server.py
 *   ./build/test_sai_create_eni 127.0.0.1:50051
 *
 * SNAP offload (NVMe + IPsec) is optional:
 *   ./build/test_sai_create_eni 172.22.3.2:50051 172.22.3.2:50052
 *   SAI_OPI_SNAP_OFFLOAD=1 ./build/test_sai_create_eni 172.22.3.2:50051
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dash_opi/c_api.h"
#include "sai_abi/sai_create_acl.h"
#include "sai_abi/sai_create_eni.h"
#include "sai_abi/sai_create_route.h"
#include "sai_abi/sai_create_vnet.h"
#include "sai_abi/sai_opi_snap.h"

int main(int argc, char** argv) {
  const char* target = argc > 1 ? argv[1] : "127.0.0.1:50051";
  const char* snap = argc > 2 ? argv[2] : NULL;

  sai_status_t st = dash_opi_client_init_ex(target, snap);
  if (st != SAI_STATUS_SUCCESS) {
    fprintf(stderr, "dash_opi_client_init_ex(%s, %s) failed: 0x%x\n", target,
            snap ? snap : "(derived :50052)", (unsigned)st);
    return 1;
  }

  const sai_object_id_t switch_id = 0x21000000000000ULL;
  sai_attribute_t vnet_attrs[1];
  memset(vnet_attrs, 0, sizeof(vnet_attrs));
  vnet_attrs[0].id = SAI_VNET_ATTR_VNI;
  vnet_attrs[0].value.u32 = 2001;

  sai_object_id_t vnet_id = SAI_NULL_OBJECT_ID;
  st = sai_create_vnet(&vnet_id, switch_id, 1, vnet_attrs);
  printf("orchagent: sai_create_vnet -> status=0x%x vnet_id=0x%llx vni=%u\n",
         (unsigned)st, (unsigned long long)vnet_id, vnet_attrs[0].value.u32);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 1;
  }

  sai_attribute_t group_attrs[1];
  memset(group_attrs, 0, sizeof(group_attrs));
  group_attrs[0].id = SAI_OUTBOUND_ROUTING_GROUP_ATTR_DISABLED;
  group_attrs[0].value.booldata = false;
  sai_object_id_t group_id = SAI_NULL_OBJECT_ID;
  st = sai_create_outbound_routing_group(&group_id, switch_id, 1, group_attrs);
  printf("orchagent: sai_create_outbound_routing_group -> status=0x%x "
         "group_id=0x%llx\n",
         (unsigned)st, (unsigned long long)group_id);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 11;
  }

  sai_outbound_routing_entry_t route_entry;
  memset(&route_entry, 0, sizeof(route_entry));
  route_entry.switch_id = switch_id;
  route_entry.outbound_routing_group_id = group_id;
  route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  {
    const unsigned char addr[4] = {10, 0, 0, 0};
    const unsigned char mask[4] = {255, 255, 0, 0};
    memcpy(&route_entry.destination.addr.ip4, addr, 4);
    memcpy(&route_entry.destination.mask.ip4, mask, 4);
  }
  sai_attribute_t route_attrs[2];
  memset(route_attrs, 0, sizeof(route_attrs));
  route_attrs[0].id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION;
  route_attrs[0].value.s32 = SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET;
  route_attrs[1].id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
  route_attrs[1].value.oid = vnet_id;
  st = sai_create_outbound_routing_entry(&route_entry, 2, route_attrs);
  printf("orchagent: sai_create_outbound_routing_entry -> status=0x%x "
         "prefix=10.0.0.0/16 dst_vnet=0x%llx\n",
         (unsigned)st, (unsigned long long)vnet_id);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 12;
  }

  sai_attribute_t acl_group_attrs[1];
  memset(acl_group_attrs, 0, sizeof(acl_group_attrs));
  acl_group_attrs[0].id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
  acl_group_attrs[0].value.s32 = SAI_IP_ADDR_FAMILY_IPV4;
  sai_object_id_t acl_group_id = SAI_NULL_OBJECT_ID;
  st = sai_create_dash_acl_group(&acl_group_id, switch_id, 1, acl_group_attrs);
  printf("orchagent: sai_create_dash_acl_group -> status=0x%x group_id=0x%llx\n",
         (unsigned)st, (unsigned long long)acl_group_id);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 17;
  }

  sai_ip_prefix_t sip_pfx;
  sai_ip_prefix_t dip_pfx;
  memset(&sip_pfx, 0, sizeof(sip_pfx));
  memset(&dip_pfx, 0, sizeof(dip_pfx));
  sip_pfx.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  dip_pfx.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  {
    const unsigned char sip[4] = {10, 0, 0, 0};
    const unsigned char smask[4] = {255, 0, 0, 0};
    const unsigned char dip[4] = {0, 0, 0, 0};
    const unsigned char dmask[4] = {0, 0, 0, 0};
    memcpy(&sip_pfx.addr.ip4, sip, 4);
    memcpy(&sip_pfx.mask.ip4, smask, 4);
    memcpy(&dip_pfx.addr.ip4, dip, 4);
    memcpy(&dip_pfx.mask.ip4, dmask, 4);
  }
  uint8_t proto = 6;
  sai_u16_range_t sport = {0, 65535};
  sai_u16_range_t dport = {80, 80};
  sai_attribute_t acl_rule_attrs[8];
  memset(acl_rule_attrs, 0, sizeof(acl_rule_attrs));
  uint32_t an = 0;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_ACTION;
  acl_rule_attrs[an].value.s32 = SAI_DASH_ACL_RULE_ACTION_PERMIT;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID;
  acl_rule_attrs[an].value.oid = acl_group_id;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_DIP;
  acl_rule_attrs[an].value.ipprefixlist.count = 1;
  acl_rule_attrs[an].value.ipprefixlist.list = &dip_pfx;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_SIP;
  acl_rule_attrs[an].value.ipprefixlist.count = 1;
  acl_rule_attrs[an].value.ipprefixlist.list = &sip_pfx;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;
  acl_rule_attrs[an].value.u8list.count = 1;
  acl_rule_attrs[an].value.u8list.list = &proto;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
  acl_rule_attrs[an].value.u16rangelist.count = 1;
  acl_rule_attrs[an].value.u16rangelist.list = &sport;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
  acl_rule_attrs[an].value.u16rangelist.count = 1;
  acl_rule_attrs[an].value.u16rangelist.list = &dport;
  ++an;
  acl_rule_attrs[an].id = SAI_DASH_ACL_RULE_ATTR_PRIORITY;
  acl_rule_attrs[an].value.u32 = 10;
  ++an;
  sai_object_id_t acl_rule_id = SAI_NULL_OBJECT_ID;
  st = sai_create_dash_acl_rule(&acl_rule_id, switch_id, an, acl_rule_attrs);
  printf("orchagent: sai_create_dash_acl_rule -> status=0x%x rule_id=0x%llx "
         "priority=10 permit tcp/80\n",
         (unsigned)st, (unsigned long long)acl_rule_id);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 18;
  }

  sai_attribute_t attrs[5];
  memset(attrs, 0, sizeof(attrs));

  attrs[0].id = SAI_ENI_ATTR_VNET_ID;
  attrs[0].value.oid = vnet_id;

  attrs[1].id = SAI_ENI_ATTR_CPS;
  attrs[1].value.u32 = 10000;

  attrs[2].id = SAI_ENI_ATTR_VM_MAC;
  const unsigned char mac[6] = {0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e};
  memcpy(attrs[2].value.mac, mac, sizeof(mac));

  attrs[3].id = SAI_ENI_ATTR_ADMIN_STATE;
  attrs[3].value.booldata = true;

  attrs[4].id = SAI_ENI_ATTR_VM_VNI;
  attrs[4].value.u32 = 2001;

  printf("orchagent: sai_create_eni switch=0x%llx vnet=0x%llx cps=%u "
         "mac=%02x:%02x:%02x:%02x:%02x:%02x vni=%u\n",
         (unsigned long long)switch_id, (unsigned long long)vnet_id,
         attrs[1].value.u32, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
         attrs[4].value.u32);

  sai_object_id_t eni_id = SAI_NULL_OBJECT_ID;
  st = sai_create_eni(&eni_id, switch_id, 5, attrs);

  printf("orchagent: sai_create_eni -> status=0x%x eni_id=0x%llx\n",
         (unsigned)st, (unsigned long long)eni_id);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 2;
  }

  /* FlexCounter / dashorch: sparse SAI_ENI_STAT_* list into counters[]. */
  const sai_stat_id_t ids[] = {
      SAI_ENI_STAT_RX_BYTES,
      SAI_ENI_STAT_RX_PACKETS,
      SAI_ENI_STAT_TX_BYTES,
      SAI_ENI_STAT_TX_PACKETS,
      SAI_ENI_STAT_FORWARDING_DROP_PACKETS,
      SAI_ENI_STAT_POLICY_DROP_PACKETS,
      SAI_ENI_STAT_OTHER_DROP_PACKETS,
      SAI_ENI_STAT_TOTAL_DROP_PACKETS,
  };
  uint64_t counters[8];
  memset(counters, 0xff, sizeof(counters));
  st = sai_get_eni_stats(eni_id, 8, ids, counters);
  printf("orchagent: sai_get_eni_stats -> status=0x%x "
         "rx_bytes=%llu rx_pkts=%llu tx_bytes=%llu tx_pkts=%llu "
         "fwd_drop=%llu policy_drop=%llu other_drop=%llu total_drop=%llu\n",
         (unsigned)st, (unsigned long long)counters[0],
         (unsigned long long)counters[1], (unsigned long long)counters[2],
         (unsigned long long)counters[3], (unsigned long long)counters[4],
         (unsigned long long)counters[5], (unsigned long long)counters[6],
         (unsigned long long)counters[7]);

  /* SNAP storage / IPsec helpers — does not fail the DASH ENI path unless
   * SAI_OPI_SNAP_OFFLOAD_STRICT is set. */
  if (snap != NULL || getenv("SAI_OPI_SNAP_OFFLOAD") != NULL) {
    const sai_status_t nvme_st = sai_opi_create_nvme_controller(NULL, NULL);
    printf("orchagent: sai_opi_create_nvme_controller -> status=0x%x\n",
           (unsigned)nvme_st);
    const sai_status_t ipsec_st = sai_opi_enable_ipsec(NULL);
    printf("orchagent: sai_opi_enable_ipsec -> status=0x%x\n",
           (unsigned)ipsec_st);
    if (getenv("SAI_OPI_SNAP_OFFLOAD_STRICT") != NULL &&
        (nvme_st != SAI_STATUS_SUCCESS || ipsec_st != SAI_STATUS_SUCCESS)) {
      dash_opi_client_shutdown();
      return 5;
    }
  }

  /* Mock server returns a fixed golden set. Hardware sysfs counters do not.
   * Check before remove so DeleteVnic cannot affect the snapshot. */
  if (getenv("SAI_OPI_EXPECT_MOCK_STATS") != NULL) {
    if (st != SAI_STATUS_SUCCESS) {
      dash_opi_client_shutdown();
      return 3;
    }
    if (counters[0] != 1000ULL || counters[1] != 10ULL ||
        counters[2] != 2000ULL || counters[3] != 20ULL || counters[4] != 4ULL ||
        counters[5] != 3ULL || counters[6] != 3ULL || counters[7] != 10ULL) {
      fprintf(stderr, "sai_get_eni_stats: unexpected mock counter values\n");
      dash_opi_client_shutdown();
      return 4;
    }
  } else if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 3;
  }

  st = sai_remove_eni(eni_id);
  printf("orchagent: sai_remove_eni -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 6;
  }

  st = sai_get_eni_stats(eni_id, 8, ids, counters);
  printf("orchagent: sai_get_eni_stats after remove -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr, "sai_get_eni_stats after remove: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 7;
  }

  st = sai_remove_eni(eni_id);
  printf("orchagent: sai_remove_eni (again) -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr, "sai_remove_eni twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 8;
  }

  st = sai_remove_outbound_routing_entry(&route_entry);
  printf("orchagent: sai_remove_outbound_routing_entry -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 13;
  }

  st = sai_remove_outbound_routing_entry(&route_entry);
  printf("orchagent: sai_remove_outbound_routing_entry (again) -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr,
            "sai_remove_outbound_routing_entry twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 14;
  }

  st = sai_remove_outbound_routing_group(group_id);
  printf("orchagent: sai_remove_outbound_routing_group -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 15;
  }

  st = sai_remove_outbound_routing_group(group_id);
  printf("orchagent: sai_remove_outbound_routing_group (again) -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr,
            "sai_remove_outbound_routing_group twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 16;
  }

  st = sai_remove_dash_acl_rule(acl_rule_id);
  printf("orchagent: sai_remove_dash_acl_rule -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 19;
  }

  st = sai_remove_dash_acl_rule(acl_rule_id);
  printf("orchagent: sai_remove_dash_acl_rule (again) -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr, "sai_remove_dash_acl_rule twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 20;
  }

  st = sai_remove_dash_acl_group(acl_group_id);
  printf("orchagent: sai_remove_dash_acl_group -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 21;
  }

  st = sai_remove_dash_acl_group(acl_group_id);
  printf("orchagent: sai_remove_dash_acl_group (again) -> status=0x%x\n",
         (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr, "sai_remove_dash_acl_group twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 22;
  }

  st = sai_remove_vnet(vnet_id);
  printf("orchagent: sai_remove_vnet -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_SUCCESS) {
    dash_opi_client_shutdown();
    return 9;
  }

  st = sai_remove_vnet(vnet_id);
  printf("orchagent: sai_remove_vnet (again) -> status=0x%x\n", (unsigned)st);
  if (st != SAI_STATUS_ITEM_NOT_FOUND) {
    fprintf(stderr, "sai_remove_vnet twice: expected ITEM_NOT_FOUND\n");
    dash_opi_client_shutdown();
    return 10;
  }

  dash_opi_client_shutdown();
  return 0;
}
