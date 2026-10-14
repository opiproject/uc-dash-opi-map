/* SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
 *
 * Tiny SAI helper linked into the lab syncd image. At runtime it dlopens
 * libsai_opi_adapter.so (mounted as /usr/lib/libsai.so) so ObjectStore lives
 * in this process for the lifetime of the container.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t sai_object_id_t;
typedef int32_t sai_status_t;
typedef uint32_t sai_attr_id_t;
typedef uint8_t sai_mac_t[6];
typedef uint32_t sai_ip4_t;

typedef enum {
  SAI_IP_ADDR_FAMILY_IPV4 = 0,
  SAI_IP_ADDR_FAMILY_IPV6 = 1
} sai_ip_addr_family_t;

typedef struct {
  sai_ip_addr_family_t addr_family;
  union {
    sai_ip4_t ip4;
    uint8_t ip6[16];
  } addr;
  union {
    sai_ip4_t ip4;
    uint8_t ip6[16];
  } mask;
} sai_ip_prefix_t;

typedef struct {
  uint32_t count;
  sai_ip_prefix_t* list;
} sai_ip_prefix_list_t;

typedef struct {
  uint32_t count;
  uint8_t* list;
} sai_u8_list_t;

typedef struct {
  uint16_t min;
  uint16_t max;
} sai_u16_range_t;

typedef struct {
  uint32_t count;
  sai_u16_range_t* list;
} sai_u16_range_list_t;

typedef union {
  bool booldata;
  char chardata[32];
  uint32_t u32;
  int32_t s32;
  sai_mac_t mac;
  sai_object_id_t oid;
  sai_ip_prefix_list_t ipprefixlist;
  sai_u8_list_t u8list;
  sai_u16_range_list_t u16rangelist;
} sai_attribute_value_t;

typedef struct {
  sai_attr_id_t id;
  sai_attribute_value_t value;
} sai_attribute_t;

typedef struct {
  sai_object_id_t switch_id;
  sai_ip_prefix_t destination;
  sai_object_id_t outbound_routing_group_id;
} sai_outbound_routing_entry_t;

enum {
  SAI_ENI_ATTR_CPS = 0,
  SAI_ENI_ATTR_ADMIN_STATE = 3,
  SAI_ENI_ATTR_VM_VNI = 6,
  SAI_ENI_ATTR_VNET_ID = 7,
  SAI_ENI_ATTR_VM_MAC = 0x10000000,
  SAI_VNET_ATTR_VNI = 0,
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_DISABLED = 0,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION = 0,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID = 1,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET = 0,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT = 1,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP = 4,
  SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY = 0,
  SAI_DASH_ACL_RULE_ATTR_ACTION = 0,
  SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID = 1,
  SAI_DASH_ACL_RULE_ATTR_DIP = 2,
  SAI_DASH_ACL_RULE_ATTR_SIP = 3,
  SAI_DASH_ACL_RULE_ATTR_PROTOCOL = 4,
  SAI_DASH_ACL_RULE_ATTR_SRC_PORT = 5,
  SAI_DASH_ACL_RULE_ATTR_DST_PORT = 6,
  SAI_DASH_ACL_RULE_ATTR_PRIORITY = 8,
  SAI_DASH_ACL_RULE_ACTION_PERMIT = 0,
  SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE = 1,
  SAI_DASH_ACL_RULE_ACTION_DENY = 2,
  SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE = 3,
  SAI_IP_ADDR_FAMILY_IPV4_VAL = 0
};

typedef sai_status_t (*dash_opi_client_init_fn)(const char*);
typedef void (*dash_opi_client_shutdown_fn)(void);
typedef sai_status_t (*dash_opi_bind_vnet_fn)(sai_object_id_t, const char*,
                                             const char*);
typedef sai_status_t (*sai_create_eni_fn)(sai_object_id_t*, sai_object_id_t,
                                          uint32_t, const sai_attribute_t*);
typedef sai_status_t (*sai_remove_eni_fn)(sai_object_id_t);
typedef sai_status_t (*sai_create_vnet_fn)(sai_object_id_t*, sai_object_id_t,
                                           uint32_t, const sai_attribute_t*);
typedef sai_status_t (*sai_remove_vnet_fn)(sai_object_id_t);
typedef sai_status_t (*sai_create_outbound_routing_group_fn)(
    sai_object_id_t*, sai_object_id_t, uint32_t, const sai_attribute_t*);
typedef sai_status_t (*sai_remove_outbound_routing_group_fn)(sai_object_id_t);
typedef sai_status_t (*sai_create_outbound_routing_entry_fn)(
    const sai_outbound_routing_entry_t*, uint32_t, const sai_attribute_t*);
typedef sai_status_t (*sai_remove_outbound_routing_entry_fn)(
    const sai_outbound_routing_entry_t*);
typedef sai_status_t (*sai_create_dash_acl_group_fn)(sai_object_id_t*,
                                                     sai_object_id_t, uint32_t,
                                                     const sai_attribute_t*);
typedef sai_status_t (*sai_remove_dash_acl_group_fn)(sai_object_id_t);
typedef sai_status_t (*sai_create_dash_acl_rule_fn)(sai_object_id_t*,
                                                    sai_object_id_t, uint32_t,
                                                    const sai_attribute_t*);
typedef sai_status_t (*sai_remove_dash_acl_rule_fn)(sai_object_id_t);

static void* g_lib;
static dash_opi_client_init_fn g_init;
static dash_opi_client_shutdown_fn g_shutdown;
static dash_opi_bind_vnet_fn g_bind;
static sai_create_eni_fn g_create_eni;
static sai_remove_eni_fn g_remove_eni;
static sai_create_vnet_fn g_create_vnet;
static sai_remove_vnet_fn g_remove_vnet;
static sai_create_outbound_routing_group_fn g_create_rt_group;
static sai_remove_outbound_routing_group_fn g_remove_rt_group;
static sai_create_outbound_routing_entry_fn g_create_rt_entry;
static sai_remove_outbound_routing_entry_fn g_remove_rt_entry;
static sai_create_dash_acl_group_fn g_create_acl_group;
static sai_remove_dash_acl_group_fn g_remove_acl_group;
static sai_create_dash_acl_rule_fn g_create_acl_rule;
static sai_remove_dash_acl_rule_fn g_remove_acl_rule;
static int g_ready;

static int parse_ipv4_prefix(const char* cidr, sai_ip_prefix_t* out) {
  unsigned a = 0, b = 0, c = 0, d = 0, plen = 0;
  if (!cidr || !out) {
    return -1;
  }
  if (sscanf(cidr, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &plen) != 5 || plen > 32) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->addr_family = SAI_IP_ADDR_FAMILY_IPV4;
  /* SAI IPv4 is network byte order (big-endian wire). */
  {
    unsigned char bytes[4] = {(unsigned char)a, (unsigned char)b,
                              (unsigned char)c, (unsigned char)d};
    memcpy(&out->addr.ip4, bytes, 4);
  }
  {
    uint32_t host_mask = plen == 0 ? 0u : (0xffffffffu << (32 - plen));
    unsigned char m[4] = {
        (unsigned char)((host_mask >> 24) & 0xff),
        (unsigned char)((host_mask >> 16) & 0xff),
        (unsigned char)((host_mask >> 8) & 0xff),
        (unsigned char)(host_mask & 0xff),
    };
    memcpy(&out->mask.ip4, m, 4);
  }
  return 0;
}

static int load_adapter(void) {
  if (g_ready) {
    return 0;
  }
  const char* paths[] = {"/usr/lib/libsai.so", "/usr/lib/libsai_opi_adapter.so",
                         NULL};
  for (int i = 0; paths[i]; ++i) {
    g_lib = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (g_lib) {
      fprintf(stderr, "syncd-lab: dlopen %s\n", paths[i]);
      break;
    }
  }
  if (!g_lib) {
    fprintf(stderr, "syncd-lab: dlopen failed: %s\n", dlerror());
    fflush(stderr);
    return -1;
  }
  g_init = (dash_opi_client_init_fn)dlsym(g_lib, "dash_opi_client_init");
  g_shutdown = (dash_opi_client_shutdown_fn)dlsym(g_lib, "dash_opi_client_shutdown");
  g_bind = (dash_opi_bind_vnet_fn)dlsym(g_lib, "dash_opi_bind_vnet");
  g_create_eni = (sai_create_eni_fn)dlsym(g_lib, "sai_create_eni");
  g_remove_eni = (sai_remove_eni_fn)dlsym(g_lib, "sai_remove_eni");
  g_create_vnet = (sai_create_vnet_fn)dlsym(g_lib, "sai_create_vnet");
  g_remove_vnet = (sai_remove_vnet_fn)dlsym(g_lib, "sai_remove_vnet");
  g_create_rt_group = (sai_create_outbound_routing_group_fn)dlsym(
      g_lib, "sai_create_outbound_routing_group");
  g_remove_rt_group = (sai_remove_outbound_routing_group_fn)dlsym(
      g_lib, "sai_remove_outbound_routing_group");
  g_create_rt_entry = (sai_create_outbound_routing_entry_fn)dlsym(
      g_lib, "sai_create_outbound_routing_entry");
  g_remove_rt_entry = (sai_remove_outbound_routing_entry_fn)dlsym(
      g_lib, "sai_remove_outbound_routing_entry");
  g_create_acl_group =
      (sai_create_dash_acl_group_fn)dlsym(g_lib, "sai_create_dash_acl_group");
  g_remove_acl_group =
      (sai_remove_dash_acl_group_fn)dlsym(g_lib, "sai_remove_dash_acl_group");
  g_create_acl_rule =
      (sai_create_dash_acl_rule_fn)dlsym(g_lib, "sai_create_dash_acl_rule");
  g_remove_acl_rule =
      (sai_remove_dash_acl_rule_fn)dlsym(g_lib, "sai_remove_dash_acl_rule");
  if (!g_init || !g_bind || !g_create_eni || !g_remove_eni || !g_create_vnet ||
      !g_remove_vnet || !g_create_rt_group || !g_remove_rt_group ||
      !g_create_rt_entry || !g_remove_rt_entry || !g_create_acl_group ||
      !g_remove_acl_group || !g_create_acl_rule || !g_remove_acl_rule) {
    fprintf(stderr, "syncd-lab: missing SAI/OPI symbols in adapter\n");
    return -1;
  }
  g_ready = 1;
  return 0;
}

int lab_sai_init(const char* target) {
  if (load_adapter() != 0) {
    return 12; /* SAI_STATUS_UNINITIALIZED */
  }
  return (int)g_init(target);
}

int lab_sai_bind_vnet(uint64_t vnet_oid, const char* vpc, const char* subnet) {
  if (!g_ready || !g_bind) {
    return 12;
  }
  return (int)g_bind(vnet_oid, vpc, subnet);
}

int lab_sai_create_eni(uint64_t* eni_out, uint64_t switch_id, uint64_t vnet_id,
                       uint32_t cps, const char* mac_colon, int admin,
                       uint32_t vm_vni) {
  if (!g_ready || !g_create_eni) {
    return 12;
  }
  sai_attribute_t attrs[5];
  memset(attrs, 0, sizeof(attrs));
  attrs[0].id = SAI_ENI_ATTR_VNET_ID;
  attrs[0].value.oid = vnet_id;
  attrs[1].id = SAI_ENI_ATTR_CPS;
  attrs[1].value.u32 = cps;
  attrs[2].id = SAI_ENI_ATTR_VM_MAC;
  unsigned a, b, c, d, e, f;
  if (mac_colon && sscanf(mac_colon, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e,
                          &f) == 6) {
    attrs[2].value.mac[0] = (uint8_t)a;
    attrs[2].value.mac[1] = (uint8_t)b;
    attrs[2].value.mac[2] = (uint8_t)c;
    attrs[2].value.mac[3] = (uint8_t)d;
    attrs[2].value.mac[4] = (uint8_t)e;
    attrs[2].value.mac[5] = (uint8_t)f;
  }
  attrs[3].id = SAI_ENI_ATTR_ADMIN_STATE;
  attrs[3].value.booldata = admin ? true : false;
  attrs[4].id = SAI_ENI_ATTR_VM_VNI;
  attrs[4].value.u32 = vm_vni;
  sai_object_id_t eni = 0;
  int st = (int)g_create_eni(&eni, switch_id, 5, attrs);
  if (eni_out) {
    *eni_out = eni;
  }
  return st;
}

int lab_sai_remove_eni(uint64_t eni_id) {
  if (!g_ready || !g_remove_eni) {
    return 12;
  }
  return (int)g_remove_eni(eni_id);
}

int lab_sai_create_vnet(uint64_t* vnet_out, uint64_t switch_id, uint32_t vni) {
  if (!g_ready || !g_create_vnet) {
    return 12;
  }
  sai_attribute_t attrs[1];
  memset(attrs, 0, sizeof(attrs));
  attrs[0].id = SAI_VNET_ATTR_VNI;
  attrs[0].value.u32 = vni;
  sai_object_id_t vnet = 0;
  int st = (int)g_create_vnet(&vnet, switch_id, 1, attrs);
  if (vnet_out) {
    *vnet_out = vnet;
  }
  return st;
}

int lab_sai_remove_vnet(uint64_t vnet_id) {
  if (!g_ready || !g_remove_vnet) {
    return 12;
  }
  return (int)g_remove_vnet(vnet_id);
}

int lab_sai_create_route_group(uint64_t* group_out, uint64_t switch_id,
                               int disabled) {
  if (!g_ready || !g_create_rt_group) {
    return 12;
  }
  sai_attribute_t attrs[1];
  memset(attrs, 0, sizeof(attrs));
  attrs[0].id = SAI_OUTBOUND_ROUTING_GROUP_ATTR_DISABLED;
  attrs[0].value.booldata = disabled ? true : false;
  sai_object_id_t group = 0;
  int st = (int)g_create_rt_group(&group, switch_id, 1, attrs);
  if (group_out) {
    *group_out = group;
  }
  return st;
}

int lab_sai_remove_route_group(uint64_t group_id) {
  if (!g_ready || !g_remove_rt_group) {
    return 12;
  }
  return (int)g_remove_rt_group(group_id);
}

int lab_sai_create_route(uint64_t switch_id, uint64_t group_id,
                         const char* prefix_cidr, int action, uint64_t dst_vnet) {
  if (!g_ready || !g_create_rt_entry) {
    return 12;
  }
  sai_outbound_routing_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.switch_id = switch_id;
  entry.outbound_routing_group_id = group_id;
  if (parse_ipv4_prefix(prefix_cidr, &entry.destination) != 0) {
    return 1; /* SAI_STATUS_INVALID_PARAMETER */
  }
  sai_attribute_t attrs[2];
  memset(attrs, 0, sizeof(attrs));
  uint32_t n = 0;
  attrs[n].id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION;
  attrs[n].value.s32 = action;
  ++n;
  if (dst_vnet != 0) {
    attrs[n].id = SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID;
    attrs[n].value.oid = dst_vnet;
    ++n;
  }
  return (int)g_create_rt_entry(&entry, n, attrs);
}

int lab_sai_remove_route(uint64_t switch_id, uint64_t group_id,
                         const char* prefix_cidr) {
  if (!g_ready || !g_remove_rt_entry) {
    return 12;
  }
  sai_outbound_routing_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.switch_id = switch_id;
  entry.outbound_routing_group_id = group_id;
  if (parse_ipv4_prefix(prefix_cidr, &entry.destination) != 0) {
    return 1;
  }
  return (int)g_remove_rt_entry(&entry);
}

static int parse_port_range(const char* s, uint16_t* lo, uint16_t* hi) {
  unsigned a = 0, b = 0;
  if (!s || !*s) {
    *lo = 0;
    *hi = 65535;
    return 0;
  }
  if (sscanf(s, "%u-%u", &a, &b) == 2) {
    *lo = (uint16_t)a;
    *hi = (uint16_t)b;
    return 0;
  }
  if (sscanf(s, "%u", &a) == 1) {
    *lo = (uint16_t)a;
    *hi = (uint16_t)a;
    return 0;
  }
  return -1;
}

int lab_sai_create_acl_group(uint64_t* group_out, uint64_t switch_id,
                             int ipv6) {
  if (!g_ready || !g_create_acl_group) {
    return 12;
  }
  sai_attribute_t attrs[1];
  memset(attrs, 0, sizeof(attrs));
  attrs[0].id = SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY;
  attrs[0].value.s32 = ipv6 ? 1 : 0; /* SAI_IP_ADDR_FAMILY_IPV6 / IPV4 */
  sai_object_id_t group = 0;
  int st = (int)g_create_acl_group(&group, switch_id, 1, attrs);
  if (group_out) {
    *group_out = group;
  }
  return st;
}

int lab_sai_remove_acl_group(uint64_t group_id) {
  if (!g_ready || !g_remove_acl_group) {
    return 12;
  }
  return (int)g_remove_acl_group(group_id);
}

int lab_sai_create_acl_rule(uint64_t* rule_out, uint64_t switch_id,
                            uint64_t group_id, uint32_t priority, int action,
                            int protocol, const char* sip_cidr,
                            const char* dip_cidr, const char* sport,
                            const char* dport) {
  if (!g_ready || !g_create_acl_rule) {
    return 12;
  }
  sai_ip_prefix_t sip_pfx;
  sai_ip_prefix_t dip_pfx;
  memset(&sip_pfx, 0, sizeof(sip_pfx));
  memset(&dip_pfx, 0, sizeof(dip_pfx));
  if (parse_ipv4_prefix(sip_cidr && *sip_cidr ? sip_cidr : "0.0.0.0/0",
                        &sip_pfx) != 0) {
    return 1;
  }
  if (parse_ipv4_prefix(dip_cidr && *dip_cidr ? dip_cidr : "0.0.0.0/0",
                        &dip_pfx) != 0) {
    return 1;
  }
  uint8_t proto = (uint8_t)(protocol < 0 ? 0 : protocol);
  sai_u16_range_t sport_r = {0, 65535};
  sai_u16_range_t dport_r = {0, 65535};
  if (parse_port_range(sport, &sport_r.min, &sport_r.max) != 0 ||
      parse_port_range(dport, &dport_r.min, &dport_r.max) != 0) {
    return 1;
  }

  sai_attribute_t attrs[8];
  memset(attrs, 0, sizeof(attrs));
  uint32_t n = 0;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_ACTION;
  attrs[n].value.s32 = action;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID;
  attrs[n].value.oid = group_id;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_DIP;
  attrs[n].value.ipprefixlist.count = 1;
  attrs[n].value.ipprefixlist.list = &dip_pfx;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_SIP;
  attrs[n].value.ipprefixlist.count = 1;
  attrs[n].value.ipprefixlist.list = &sip_pfx;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_PROTOCOL;
  attrs[n].value.u8list.count = 1;
  attrs[n].value.u8list.list = &proto;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_SRC_PORT;
  attrs[n].value.u16rangelist.count = 1;
  attrs[n].value.u16rangelist.list = &sport_r;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_DST_PORT;
  attrs[n].value.u16rangelist.count = 1;
  attrs[n].value.u16rangelist.list = &dport_r;
  ++n;
  attrs[n].id = SAI_DASH_ACL_RULE_ATTR_PRIORITY;
  attrs[n].value.u32 = priority;
  ++n;

  sai_object_id_t rule = 0;
  int st = (int)g_create_acl_rule(&rule, switch_id, n, attrs);
  if (rule_out) {
    *rule_out = rule;
  }
  return st;
}

int lab_sai_remove_acl_rule(uint64_t rule_id) {
  if (!g_ready || !g_remove_acl_rule) {
    return 12;
  }
  return (int)g_remove_acl_rule(rule_id);
}

void lab_sai_shutdown(void) {
  if (g_shutdown) {
    g_shutdown();
  }
}
