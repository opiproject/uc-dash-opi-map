#ifndef SAITYPES_H_
#define SAITYPES_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif

typedef uint64_t sai_object_id_t;
typedef uint32_t sai_object_type_t;
typedef uint32_t sai_attr_id_t;
typedef uint32_t sai_uint32_t;
typedef int32_t sai_int32_t;
typedef uint8_t sai_mac_t[6];
typedef uint32_t sai_ip4_t;
typedef uint8_t sai_ip6_t[16];
typedef uint32_t sai_switch_profile_id_t;
typedef int32_t sai_api_t;
typedef int32_t sai_log_level_t;

typedef uint32_t sai_stat_id_t;

#define SAI_NULL_OBJECT_ID 0ULL

#define SAI_OBJECT_TYPE_NULL 0
#define SAI_OBJECT_TYPE_EXTENSIONS_RANGE_BASE 0x20000000

#define SAI_API_UNSPECIFIED 0
#define SAI_API_SWITCH 1
#define SAI_API_EXTENSIONS_RANGE_BASE 0x20000000

#define SAI_LOG_LEVEL_CRITICAL 0
#define SAI_LOG_LEVEL_ERROR 1
#define SAI_LOG_LEVEL_WARN 2
#define SAI_LOG_LEVEL_NOTICE 3
#define SAI_LOG_LEVEL_INFO 4
#define SAI_LOG_LEVEL_DEBUG 5

typedef enum _sai_ip_addr_family_t {
  SAI_IP_ADDR_FAMILY_IPV4,
  SAI_IP_ADDR_FAMILY_IPV6
} sai_ip_addr_family_t;

typedef struct _sai_ip_address_t {
  sai_ip_addr_family_t addr_family;
  union {
    sai_ip4_t ip4;
    sai_ip6_t ip6;
  } addr;
} sai_ip_address_t;

typedef struct _sai_ip_prefix_t {
  sai_ip_addr_family_t addr_family;
  union {
    sai_ip4_t ip4;
    sai_ip6_t ip6;
  } addr;
  union {
    sai_ip4_t ip4;
    sai_ip6_t ip6;
  } mask;
} sai_ip_prefix_t;

typedef struct _sai_ip_prefix_list_t {
  uint32_t count;
  sai_ip_prefix_t* list;
} sai_ip_prefix_list_t;

typedef struct _sai_u8_list_t {
  uint32_t count;
  uint8_t* list;
} sai_u8_list_t;

typedef struct _sai_u16_range_t {
  uint16_t min;
  uint16_t max;
} sai_u16_range_t;

typedef struct _sai_u16_range_list_t {
  uint32_t count;
  sai_u16_range_t* list;
} sai_u16_range_list_t;

typedef union _sai_attribute_value_t {
  bool booldata;
  char chardata[32];
  sai_uint32_t u32;
  sai_int32_t s32;
  sai_mac_t mac;
  sai_object_id_t oid;
  sai_ip_address_t ipaddr;
  sai_ip_prefix_list_t ipprefixlist;
  sai_u8_list_t u8list;
  sai_u16_range_list_t u16rangelist;
} sai_attribute_value_t;

typedef struct _sai_attribute_t {
  sai_attr_id_t id;
  sai_attribute_value_t value;
} sai_attribute_t;

typedef const char* (*sai_profile_get_value_fn)(
    _In_ sai_switch_profile_id_t profile_id,
    _In_ const char* variable);

typedef int (*sai_profile_get_next_value_fn)(
    _In_ sai_switch_profile_id_t profile_id,
    _Out_ const char** variable,
    _Out_ const char** value);

typedef struct _sai_service_method_table_t {
  sai_profile_get_value_fn profile_get_value;
  sai_profile_get_next_value_fn profile_get_next_value;
} sai_service_method_table_t;

#endif
