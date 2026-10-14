// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
//
// dpu_doca_flowd — DOCA Flow switch/eSwitch sidecar for CloudInfra.
// Speaks JSON lines on a Unix socket. The Go daemon and the host adapter
// never link libdoca; DPDK EAL and hugepages live here.
//
//   make                 # stub: acks RPCs, no hardware (protocol + lab bring-up)
//   make DOCA=1          # DOCA 2.7+ opaque cfg setters (must run on the DPU)
//   ./dpu_doca_flowd -s /var/run/opi-doca.sock
//
// Hardware pipeline (one root CONTROL pipe, OVS-equivalent):
//   ACL deny  → drop (5-tuple / prefix)
//   L3 route  → IPv4 dst prefix → VXLAN encap + VNI → uplink
//   overlay   → VNI/MAC decap to representor; encap from representor to uplink
// ACL permit is default-allow (no entry); miss falls through to L3/overlay.
//
// Default CloudInfra stays on OVS. Start this, then:
//   dpu_cloudinfra_bridge -datapath doca

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#ifdef OPI_DOCA
#include <doca_error.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_dev.h>
#ifdef __has_include
#if __has_include(<doca_dpdk.h>)
#include <doca_dpdk.h>
#define OPI_HAVE_DOCA_DPDK_H 1
#endif
#if __has_include(<rte_eal.h>)
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#define OPI_HAVE_DPDK 1
#endif
#endif
#ifndef DOCA_DEVINFO_PCI_ADDR_SIZE
#define DOCA_DEVINFO_PCI_ADDR_SIZE 32
#endif
#ifndef OPI_HAVE_DOCA_DPDK_H
doca_error_t doca_dpdk_port_probe(struct doca_dev *dev, const char *extra);
#endif
#endif

#ifndef OPI_BE16
#define OPI_BE16 htons
#endif
#ifndef OPI_BE32
#define OPI_BE32 htonl
#endif

#define MAX_LINE 4096
#define MAX_ENTS 256
#define MAX_PORTS 64

static const char *g_sock = "/var/run/opi-doca.sock";
static const char *g_pci = NULL;
static const char *g_eal = NULL;
static int g_listen = -1;

struct opi_ent {
	char name[128];
	char kind[16];
#ifdef OPI_DOCA
	struct doca_flow_pipe_entry *hw;
	struct doca_flow_pipe_entry *hw2;
#endif
};

static struct opi_ent g_ents[MAX_ENTS];
static int g_nent;

static char g_port_name[MAX_PORTS][64];
static uint16_t g_nports;
static uint16_t g_uplink_id;

#ifdef OPI_DOCA
static struct doca_flow_pipe *g_pipe;
static struct doca_flow_port *g_sw_port;
static struct doca_dev *g_dev;
static int g_hw_ready;
#ifdef OPI_HAVE_DPDK
static struct rte_mempool *g_mbuf_pool;
#endif
#ifndef OPI_CONTROL_ADD_TAIL
#include "doca_compat.inc"
#endif
#endif

static void json_ok(FILE *fp) { fprintf(fp, "{\"ok\":true}\n"); fflush(fp); }
static void json_err(FILE *fp, const char *e)
{
	fprintf(fp, "{\"ok\":false,\"error\":\"%s\"}\n", e ? e : "error");
	fflush(fp);
}

static int json_str(const char *j, const char *key, char *out, size_t n)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(j, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	if (!p)
		return 0;
	p++;
	while (*p == ' ')
		p++;
	if (*p != '"')
		return 0;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n)
		out[i++] = *p++;
	out[i] = 0;
	return 1;
}

static int json_long(const char *j, const char *key, long *out)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(j, pat);
	if (!p)
		return 0;
	p = strchr(p + strlen(pat), ':');
	if (!p)
		return 0;
	*out = strtol(p + 1, NULL, 10);
	return 1;
}

static int parse_cidr(const char *s, uint32_t *addr, uint32_t *mask)
{
	char ip[64] = {0};
	int plen = 32;
	const char *slash;

	*addr = 0;
	*mask = 0;
	if (!s || !s[0] || strcmp(s, "0.0.0.0/0") == 0 || strcmp(s, "0.0.0.0") == 0)
		return 0;
	slash = strchr(s, '/');
	if (slash) {
		size_t n = (size_t)(slash - s);
		if (n >= sizeof(ip))
			n = sizeof(ip) - 1;
		memcpy(ip, s, n);
		ip[n] = 0;
		plen = atoi(slash + 1);
	} else {
		snprintf(ip, sizeof(ip), "%s", s);
	}
	if (plen < 0)
		plen = 0;
	if (plen > 32)
		plen = 32;
	if (inet_pton(AF_INET, ip, addr) != 1)
		return -1;
	if (plen == 0)
		*mask = 0;
	else
		*mask = OPI_BE32(0xFFFFFFFFu << (32 - plen));
	return 1;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
	unsigned a[6];
	int i;
	if (!s || !s[0])
		return 0;
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6 &&
	    sscanf(s, "%x-%x-%x-%x-%x-%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
		return 0;
	for (i = 0; i < 6; i++)
		mac[i] = (uint8_t)a[i];
	return 1;
}

static int sysfs_oneline(const char *path, char *out, size_t n)
{
	FILE *f;
	size_t L;

	if (!path || !out || n == 0)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	L = strlen(out);
	while (L && (out[L - 1] == '\n' || out[L - 1] == '\r' || out[L - 1] == ' '))
		out[--L] = 0;
	return L ? 0 : -1;
}

static int name_matches_dev(const char *dpdk_name, const char *dev)
{
	if (!dpdk_name || !dev || !dpdk_name[0] || !dev[0])
		return 0;
	if (strcmp(dpdk_name, dev) == 0)
		return 1;
	return strstr(dpdk_name, dev) != NULL;
}

/* Map kernel netdev (pf0vf1) to a real DPDK port id. Do not invent ids. */
static int lookup_port(const char *dev, uint16_t *out)
{
	uint16_t i;
	char phys[64] = {0};
	char path[256];

	if (!dev || !dev[0] || !out)
		return 0;
	for (i = 0; i < MAX_PORTS; i++) {
		if (g_port_name[i][0] && strcmp(g_port_name[i], dev) == 0) {
			*out = i;
			return 1;
		}
	}
#ifdef OPI_HAVE_DPDK
	{
		uint16_t p;
		char name[RTE_ETH_NAME_MAX_LEN];

		if (rte_eth_dev_get_port_by_name(dev, &p) == 0 && p < MAX_PORTS) {
			*out = p;
			return 1;
		}
		RTE_ETH_FOREACH_DEV(p) {
			if (p >= MAX_PORTS)
				continue;
			if (rte_eth_dev_get_name_by_port(p, name) == 0 &&
			    name_matches_dev(name, dev)) {
				*out = p;
				return 1;
			}
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/phys_port_name", dev);
		if (sysfs_oneline(path, phys, sizeof(phys)) == 0) {
			RTE_ETH_FOREACH_DEV(p) {
				if (p >= MAX_PORTS)
					continue;
				if (rte_eth_dev_get_name_by_port(p, name) == 0 &&
				    name_matches_dev(name, phys)) {
					*out = p;
					return 1;
				}
			}
		}
	}
#else
	(void)phys;
	(void)path;
#endif
	return 0;
}

static struct opi_ent *ent_find(const char *name)
{
	int i;
	for (i = 0; i < g_nent; i++) {
		if (strcmp(g_ents[i].name, name) == 0)
			return &g_ents[i];
	}
	return NULL;
}

static struct opi_ent *ent_add(const char *name, const char *kind)
{
	struct opi_ent *e = ent_find(name);
	if (e)
		return e;
	if (g_nent >= MAX_ENTS)
		return NULL;
	e = &g_ents[g_nent++];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name);
	snprintf(e->kind, sizeof(e->kind), "%s", kind);
	return e;
}

static void ent_del(const char *name)
{
	int i;
	for (i = 0; i < g_nent; i++) {
		if (strcmp(g_ents[i].name, name) == 0) {
			g_ents[i] = g_ents[g_nent - 1];
			g_nent--;
			return;
		}
	}
}

#ifdef OPI_DOCA
static void doca_errmsg(char *err, size_t errlen, const char *what, doca_error_t rc)
{
	const char *name = doca_error_get_name(rc);
	const char *descr = doca_error_get_descr(rc);
	snprintf(err, errlen, "%s: %s (%s/%d)", what, descr ? descr : "?",
		 name ? name : "?", (int)rc);
}

static int pci_same(const char *a, const char *b)
{
	if (!a || !b || !a[0] || !b[0])
		return 0;
	if (strncmp(a, "0000:", 5) == 0)
		a += 5;
	if (strncmp(b, "0000:", 5) == 0)
		b += 5;
	return strcmp(a, b) == 0;
}

static int pci_for_netdev(const char *ifname, char *out, size_t n)
{
	char path[256], buf[256];
	ssize_t r;
	const char *slash;

	if (!ifname || !ifname[0])
		return -1;
	snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
	r = readlink(path, buf, sizeof(buf) - 1);
	if (r < 0)
		return -1;
	buf[r] = 0;
	slash = strrchr(buf, '/');
	{
		const char *bdf = slash ? slash + 1 : buf;
		if (strlen(bdf) >= n)
			return -1;
		memcpy(out, bdf, strlen(bdf) + 1);
	}
	return 0;
}

static int eal_start(char *err, size_t errlen)
{
#ifdef OPI_HAVE_DPDK
	char *argv[24];
	char extra[256];
	int argc = 0;
	int user_a = (g_eal && g_eal[0] && strstr(g_eal, "-a") != NULL);

	argv[argc++] = "dpu_doca_flowd";
	argv[argc++] = "--file-prefix";
	argv[argc++] = "opi-doca";
	argv[argc++] = "-l";
	argv[argc++] = "0-1";
	/* Empty allow-list so mlx5 is not auto-probed; doca_dpdk_port_probe owns it. */
	if (!user_a) {
		argv[argc++] = "-a";
		argv[argc++] = "00:00.0";
	}
	if (g_eal && g_eal[0]) {
		char *tok;
		snprintf(extra, sizeof(extra), "%s", g_eal);
		tok = strtok(extra, " ");
		while (tok && argc < 22) {
			argv[argc++] = tok;
			tok = strtok(NULL, " ");
		}
	}
	if (rte_eal_init(argc, argv) < 0) {
		if (!user_a) {
			char *argv2[] = {"dpu_doca_flowd", "--file-prefix", "opi-doca", "-l", "0-1", NULL};
			if (rte_eal_init(5, argv2) >= 0)
				return 0;
		}
		snprintf(err, errlen, "rte_eal_init failed (hugepages mounted at /mnt/huge?)");
		return -1;
	}
#else
	(void)err;
	(void)errlen;
#endif
	return 0;
}

static int open_pf_dev(const char *uplink, char *err, size_t errlen)
{
	struct doca_devinfo **list = NULL;
	uint32_t n = 0, i;
	char want[64] = {0};
	doca_error_t rc;

	if (g_pci && g_pci[0]) {
		snprintf(want, sizeof(want), "%s", g_pci);
		{
			char *comma = strchr(want, ',');
			if (comma)
				*comma = 0;
		}
	} else if (pci_for_netdev(uplink, want, sizeof(want)) != 0)
		(void)pci_for_netdev("p0", want, sizeof(want));

	rc = doca_devinfo_create_list(&list, &n);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_devinfo_create_list", rc);
		return -1;
	}
	for (i = 0; i < n; i++) {
		char addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};
		(void)doca_devinfo_get_pci_addr_str(list[i], addr);
		if (want[0] && !pci_same(addr, want))
			continue;
#ifdef OPI_DEV_OPEN_DEV_FIRST
		rc = doca_dev_open(&g_dev, list[i]);
#else
		rc = doca_dev_open(list[i], &g_dev);
#endif
		if (rc == DOCA_SUCCESS) {
			fprintf(stderr, "dpu-doca: opened mlx5 %s (want=%s uplink=%s)\n",
				addr, want, uplink ? uplink : "");
			break;
		}
		g_dev = NULL;
	}
	if (!g_dev && n > 0) {
		char addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};
		(void)doca_devinfo_get_pci_addr_str(list[0], addr);
		fprintf(stderr, "dpu-doca: no PCI match for %s; opening first mlx5 %s\n",
			want[0] ? want : "(none)", addr);
#ifdef OPI_DEV_OPEN_DEV_FIRST
		rc = doca_dev_open(&g_dev, list[0]);
#else
		rc = doca_dev_open(list[0], &g_dev);
#endif
	}
	doca_devinfo_destroy_list(list);
	if (!g_dev) {
		snprintf(err, errlen,
			 "no mlx5 doca_dev (pci=%s uplink=%s); set DOCA_PCI=BB:DD.F",
			 want[0] ? want : "(none)", uplink ? uplink : "");
		return -1;
	}
	return 0;
}

static const char *probe_args(int with_reps)
{
	static const char *pf_only =
		"dv_flow_en=2,fdb_def_rule_en=0,vport_match=1,repr_matching_en=0,dv_xmeta_en=4";
	static const char *with_vf =
		"dv_flow_en=2,fdb_def_rule_en=0,vport_match=1,repr_matching_en=0,dv_xmeta_en=4,"
		"representor=pf[0-1]vf[0-7],representor=pf[0-1]sf[0-7]";
	const char *env = getenv("DOCA_PROBE_ARGS");

	if (env && env[0])
		return env;
	return with_reps ? with_vf : pf_only;
}

#define OPI_ETH_QUEUES 2

#ifdef OPI_HAVE_DPDK
/* Switch manager PF only — do not configure VF/SF representors (DOCA 2.7). */
static int eth_start(uint16_t port_id, char *err, size_t errlen)
{
	struct rte_eth_conf conf = {0};
	struct rte_eth_dev_info info;
	uint16_t nq = OPI_ETH_QUEUES, i, nb_rx = 256, nb_tx = 256;
	int sid, rc;

	memset(&info, 0, sizeof(info));
	if (!g_mbuf_pool) {
		g_mbuf_pool = rte_pktmbuf_pool_create("opi_mbuf", 8192, 256, 0,
						      RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
		if (!g_mbuf_pool)
			g_mbuf_pool = rte_mempool_lookup("opi_mbuf");
		if (!g_mbuf_pool) {
			snprintf(err, errlen, "rte_pktmbuf_pool_create failed");
			return -1;
		}
	}
	if (rte_eth_dev_info_get(port_id, &info) == 0) {
		if (info.max_rx_queues && nq > info.max_rx_queues)
			nq = info.max_rx_queues;
		if (info.max_tx_queues && nq > info.max_tx_queues)
			nq = info.max_tx_queues;
	}
	if (nq < 1)
		nq = 1;
	rc = rte_eth_dev_configure(port_id, nq, nq, &conf);
	if (rc < 0) {
		snprintf(err, errlen, "rte_eth_dev_configure(%u) rc=%d", (unsigned)port_id, rc);
		return -1;
	}
	(void)rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rx, &nb_tx);
	sid = rte_eth_dev_socket_id(port_id);
	if (sid < 0)
		sid = (int)SOCKET_ID_ANY;
	for (i = 0; i < nq; i++) {
		rc = rte_eth_rx_queue_setup(port_id, i, nb_rx, (unsigned)sid, NULL, g_mbuf_pool);
		if (rc < 0) {
			snprintf(err, errlen, "rte_eth_rx_queue_setup(%u,%u) rc=%d",
				 (unsigned)port_id, i, rc);
			return -1;
		}
		rc = rte_eth_tx_queue_setup(port_id, i, nb_tx, (unsigned)sid, NULL);
		if (rc < 0) {
			snprintf(err, errlen, "rte_eth_tx_queue_setup(%u,%u) rc=%d",
				 (unsigned)port_id, i, rc);
			return -1;
		}
	}
	rc = rte_eth_dev_start(port_id);
	if (rc < 0) {
		snprintf(err, errlen, "rte_eth_dev_start(%u) rc=%d", (unsigned)port_id, rc);
		return -1;
	}
	(void)rte_eth_promiscuous_enable(port_id);
	fprintf(stderr, "dpu-doca: DPDK ethdev port %u started queues=%u\n",
		(unsigned)port_id, (unsigned)nq);
	return 0;
}
#endif

/* DOCA 2.7+: opaque cfg. set_dev is mandatory; set_port_id when the header has it. */
static doca_error_t start_port(uint16_t port_id, struct doca_dev *dev, struct doca_flow_port **out)
{
	struct doca_flow_port_cfg *pcfg = NULL;
	char id[16];
	doca_error_t rc;

	snprintf(id, sizeof(id), "%u", (unsigned)port_id);
	rc = doca_flow_port_cfg_create(&pcfg);
	if (rc != DOCA_SUCCESS)
		return rc;
	(void)doca_flow_port_cfg_set_devargs(pcfg, id);
#ifdef OPI_HAVE_SET_PORT_ID
	(void)doca_flow_port_cfg_set_port_id(pcfg, port_id);
#endif
	if (dev) {
		rc = doca_flow_port_cfg_set_dev(pcfg, dev);
		if (rc != DOCA_SUCCESS) {
			doca_flow_port_cfg_destroy(pcfg);
			return rc;
		}
	}
	fprintf(stderr, "dpu-doca: port_start id=%u has_dev=%d\n", (unsigned)port_id, dev ? 1 : 0);
	rc = doca_flow_port_start(pcfg, out);
	doca_flow_port_cfg_destroy(pcfg);
	return rc;
}

static void fill_vxlan_encap(struct doca_flow_actions *act, long vni, const char *vtep)
{
	act->encap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	act->encap_cfg.is_l2 = 1;
	act->encap_cfg.encap.tun.type = DOCA_FLOW_TUN_VXLAN;
	act->encap_cfg.encap.tun.vxlan_tun_id = (doca_be32_t)vni;
	act->encap_cfg.encap.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	act->encap_cfg.encap.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
#ifdef DOCA_FLOW_VXLAN_DEFAULT_PORT
	act->encap_cfg.encap.outer.udp.l4_port.dst_port = OPI_BE16(DOCA_FLOW_VXLAN_DEFAULT_PORT);
#else
	act->encap_cfg.encap.outer.udp.l4_port.dst_port = OPI_BE16(4789);
#endif
	if (vtep && vtep[0])
		inet_pton(AF_INET, vtep, &act->encap_cfg.encap.outer.ip4.dst_ip);
}

static int hw_init(const char *uplink, const char *vtep, char *err, size_t errlen)
{
	struct doca_flow_cfg *cfg = NULL;
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	doca_error_t rc;
	uint16_t port_id = 0;

	if (eal_start(err, errlen) != 0)
		return -1;
	if (open_pf_dev(uplink, err, errlen) != 0)
		return -1;

	rc = doca_dpdk_port_probe(g_dev, probe_args(1));
	if (rc != DOCA_SUCCESS) {
		const char *env = getenv("DOCA_PROBE_ARGS");

		fprintf(stderr, "dpu-doca: probe with representors failed: %s\n",
			doca_error_get_descr(rc));
		if (env && env[0]) {
			doca_errmsg(err, errlen, "doca_dpdk_port_probe", rc);
			return -1;
		}
		rc = doca_dpdk_port_probe(g_dev, probe_args(0));
		if (rc != DOCA_SUCCESS) {
			doca_errmsg(err, errlen, "doca_dpdk_port_probe", rc);
			return -1;
		}
		fprintf(stderr, "dpu-doca: probed PF only (no representor DPDK ports)\n");
	}
#ifdef OPI_HAVE_DPDK
	{
		uint16_t n = rte_eth_dev_count_avail();
		uint16_t p;
		char name[RTE_ETH_NAME_MAX_LEN];

		fprintf(stderr, "dpu-doca: DPDK ports after probe=%u\n", (unsigned)n);
		if (n == 0) {
			snprintf(err, errlen, "doca_dpdk_port_probe left 0 DPDK ports");
			return -1;
		}
		RTE_ETH_FOREACH_DEV(p) {
			memset(name, 0, sizeof(name));
			rte_eth_dev_get_name_by_port(p, name);
			fprintf(stderr, "dpu-doca: dpdk port %u name=%s%s\n", (unsigned)p,
				name[0] ? name : "?", p == 0 ? " (likely switch PF)" : "");
		}
	}
#ifdef OPI_HAVE_GET_FIRST_PORT_ID
	if (doca_dpdk_get_first_port_id(g_dev, &port_id) != DOCA_SUCCESS)
		port_id = 0;
#endif
	if (eth_start(port_id, err, errlen) != 0)
		return -1;
#endif

	rc = doca_flow_cfg_create(&cfg);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_flow_cfg_create", rc);
		return -1;
	}
	(void)doca_flow_cfg_set_pipe_queues(cfg, OPI_ETH_QUEUES);
	(void)doca_flow_cfg_set_mode_args(cfg, "switch,hws");
	(void)doca_flow_cfg_set_nr_counters(cfg, 2048);
	rc = doca_flow_init(cfg);
	doca_flow_cfg_destroy(cfg);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_flow_init", rc);
		return -1;
	}

	rc = start_port(port_id, g_dev, &g_sw_port);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_flow_port_start", rc);
		return -1;
	}
	memset(g_port_name, 0, sizeof(g_port_name));
	g_uplink_id = port_id;
	if (port_id < MAX_PORTS)
		snprintf(g_port_name[port_id], sizeof(g_port_name[0]), "%s",
			 uplink && uplink[0] ? uplink : "p0");
	g_nports = (uint16_t)(port_id + 1);

#ifdef OPI_HAVE_DPDK
	{
		uint16_t p;
		char name[RTE_ETH_NAME_MAX_LEN];

		RTE_ETH_FOREACH_DEV(p) {
			if (p >= MAX_PORTS)
				continue;
			memset(name, 0, sizeof(name));
			rte_eth_dev_get_name_by_port(p, name);
			if (!(p == port_id && g_port_name[p][0]))
				snprintf(g_port_name[p], sizeof(g_port_name[0]), "%s",
					 name[0] ? name : "");
			if (!g_port_name[p][0])
				snprintf(g_port_name[p], sizeof(g_port_name[0]), "%u", p);
			if ((uint16_t)(p + 1) > g_nports)
				g_nports = (uint16_t)(p + 1);
		}
	}
#endif

	rc = doca_flow_pipe_cfg_create(&pipe_cfg, g_sw_port);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_flow_pipe_cfg_create", rc);
		return -1;
	}
	(void)doca_flow_pipe_cfg_set_name(pipe_cfg, "opi_root");
	(void)doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	(void)doca_flow_pipe_cfg_set_is_root(pipe_cfg, 1);
	rc = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &g_pipe);
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	if (rc != DOCA_SUCCESS) {
		doca_errmsg(err, errlen, "doca_flow_pipe_create", rc);
		return -1;
	}
	g_hw_ready = 1;
	fprintf(stderr,
		"dpu-doca: eSwitch control pipe ready uplink=%s vtep=%s ports=%u\n",
		uplink ? uplink : "", vtep ? vtep : "", (unsigned)g_nports);
	return 0;
}

static int hw_add(uint32_t pri, const struct doca_flow_match *match,
		  const struct doca_flow_match *mask, const struct doca_flow_actions *act,
		  const struct doca_flow_fwd *fwd, struct doca_flow_pipe_entry **out)
{
	/* Installed 2.7.0085 adds flags/usr_ctx after fwd; tail comes from doca_compat.inc. */
	return doca_flow_pipe_control_add_entry(0, pri, g_pipe, match, mask, NULL, act, NULL,
						NULL, NULL, fwd OPI_CONTROL_ADD_TAIL, out);
}

static void fill_ipv4_tuple(struct doca_flow_match *m, struct doca_flow_match *mask,
			    uint32_t sip, uint32_t smask, uint32_t dip, uint32_t dmask,
			    long proto, long sport, long dport)
{
	memset(m, 0, sizeof(*m));
	memset(mask, 0, sizeof(*mask));
	m->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	mask->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	if (smask) {
		m->outer.ip4.src_ip = sip;
		mask->outer.ip4.src_ip = smask;
	}
	if (dmask) {
		m->outer.ip4.dst_ip = dip;
		mask->outer.ip4.dst_ip = dmask;
	}
	if (proto > 0) {
		m->outer.ip4.next_proto = (uint8_t)proto;
		mask->outer.ip4.next_proto = 0xff;
		if (proto == 6) {
			m->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
			mask->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
			if (sport > 0) {
				m->outer.tcp.l4_port.src_port = OPI_BE16((uint16_t)sport);
				mask->outer.tcp.l4_port.src_port = 0xffff;
			}
			if (dport > 0) {
				m->outer.tcp.l4_port.dst_port = OPI_BE16((uint16_t)dport);
				mask->outer.tcp.l4_port.dst_port = 0xffff;
			}
		} else if (proto == 17) {
			m->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
			mask->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
			if (sport > 0) {
				m->outer.udp.l4_port.src_port = OPI_BE16((uint16_t)sport);
				mask->outer.udp.l4_port.src_port = 0xffff;
			}
			if (dport > 0) {
				m->outer.udp.l4_port.dst_port = OPI_BE16((uint16_t)dport);
				mask->outer.udp.l4_port.dst_port = 0xffff;
			}
		}
	}
}

static int hw_acl(struct opi_ent *e, const char *src, const char *dst, long proto,
		  long sport, long dport, long action, long pri)
{
	struct doca_flow_match m, mask;
	struct doca_flow_fwd fwd = {0};
	uint32_t sip = 0, sm = 0, dip = 0, dm = 0;
	uint32_t prio = (uint32_t)(400 + (pri < 0 ? 0 : (pri > 999 ? 0 : 1000 - pri)));

	parse_cidr(src, &sip, &sm);
	parse_cidr(dst, &dip, &dm);
	fill_ipv4_tuple(&m, &mask, sip, sm, dip, dm, proto, sport, dport);
	if (action == 2) {
		fwd.type = DOCA_FLOW_FWD_DROP;
		return hw_add(prio, &m, &mask, NULL, &fwd, &e->hw);
	}
	/* Permit = default-allow: no hardware entry (packet continues to L3/overlay). */
	(void)e;
	return DOCA_SUCCESS;
}

static int hw_route(struct opi_ent *e, const char *prefix, long vni, const char *vtep, long cls)
{
	struct doca_flow_match m, mask;
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_actions act = {0};
	uint32_t dip = 0, dm = 0;
	uint32_t prio = 300;
	int has = parse_cidr(prefix, &dip, &dm);

	memset(&m, 0, sizeof(m));
	memset(&mask, 0, sizeof(mask));
	m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	if (has > 0 && dm) {
		m.outer.ip4.dst_ip = dip;
		mask.outer.ip4.dst_ip = dm;
		/* longer prefix → higher priority, same idea as OVS table 1 */
		prio = 300;
	}
	if (cls == 0) {
		fwd.type = DOCA_FLOW_FWD_DROP;
		return hw_add(prio, &m, &mask, NULL, &fwd, &e->hw);
	}
	fill_vxlan_encap(&act, vni, vtep);
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = g_uplink_id;
	return hw_add(prio, &m, &mask, &act, &fwd, &e->hw);
}

static int hw_overlay(struct opi_ent *e, const char *dev, const char *mac, long vni,
		      const char *vtep)
{
	struct doca_flow_match m, mask;
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_actions act = {0};
	uint8_t hwaddr[6] = {0};
	uint16_t rep = g_uplink_id;
	int have_mac = parse_mac(mac, hwaddr);
	int have_rep = lookup_port(dev, &rep);
	doca_error_t rc;

	/* Never FWD to a port_id DPDK did not probe (that used to invent port 1). */
	if (!have_rep || (rep == g_uplink_id && dev && dev[0] &&
			  strcmp(dev, g_port_name[g_uplink_id]) != 0)) {
		fprintf(stderr,
			"dpu-doca: overlay %s: no DPDK representor for %s (uplink=%u); "
			"encap to PF only\n",
			e->name, dev ? dev : "", (unsigned)g_uplink_id);
		have_rep = 0;
	}

	/* Decap: VXLAN VNI (+ inner/outer dst MAC) → representor */
	if (have_rep) {
		memset(&m, 0, sizeof(m));
		memset(&mask, 0, sizeof(mask));
		m.tun.type = DOCA_FLOW_TUN_VXLAN;
		mask.tun.type = DOCA_FLOW_TUN_VXLAN;
		m.tun.vxlan_tun_id = (doca_be32_t)vni;
		mask.tun.vxlan_tun_id = OPI_BE32(0x00ffffff);
		if (have_mac) {
			memcpy(m.inner.eth.dst_mac, hwaddr, 6);
			memset(mask.inner.eth.dst_mac, 0xff, 6);
		}
		act.decap_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
		act.decap_cfg.is_l2 = 1;
		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = rep;
		rc = hw_add(220, &m, &mask, &act, &fwd, &e->hw);
		if (rc != DOCA_SUCCESS)
			return rc;
	}

	/* Encap: from representor → VXLAN VNI → uplink */
	memset(&m, 0, sizeof(m));
	memset(&mask, 0, sizeof(mask));
	memset(&act, 0, sizeof(act));
	memset(&fwd, 0, sizeof(fwd));
	m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	if (have_mac) {
		memcpy(m.outer.eth.src_mac, hwaddr, 6);
		memset(mask.outer.eth.src_mac, 0xff, 6);
	}
	fill_vxlan_encap(&act, vni, vtep);
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = g_uplink_id;
	return hw_add(200, &m, &mask, &act, &fwd, &e->hw2);
}

static void hw_del(struct opi_ent *e)
{
	if (!e || !g_pipe)
		return;
	if (e->hw)
		(void)doca_flow_pipe_rm_entry(0, DOCA_FLOW_NO_WAIT, e->hw);
	if (e->hw2)
		(void)doca_flow_pipe_rm_entry(0, DOCA_FLOW_NO_WAIT, e->hw2);
	e->hw = NULL;
	e->hw2 = NULL;
}
#endif /* OPI_DOCA */

static void handle(const char *line, FILE *fp)
{
	char op[32] = {0};
	char name[128] = {0};
	char dev[64] = {0};
	char prefix[64] = {0};
	char src[64] = {0};
	char dst[64] = {0};
	char mac[32] = {0};
	char vtep[64] = {0};
	char uplink[64] = {0};
	long vni = 0, cls = 0, pri = 0, action = 0, proto = 0, sport = 0, dport = 0;

	json_str(line, "op", op, sizeof(op));
	json_str(line, "name", name, sizeof(name));
	json_str(line, "dev", dev, sizeof(dev));
	json_str(line, "prefix", prefix, sizeof(prefix));
	json_str(line, "src", src, sizeof(src));
	json_str(line, "dst", dst, sizeof(dst));
	json_str(line, "mac", mac, sizeof(mac));
	json_str(line, "vtep", vtep, sizeof(vtep));
	json_str(line, "uplink", uplink, sizeof(uplink));
	json_long(line, "vni", &vni);
	json_long(line, "class", &cls);
	json_long(line, "priority", &pri);
	json_long(line, "action", &action);
	json_long(line, "proto", &proto);
	json_long(line, "sport", &sport);
	json_long(line, "dport", &dport);

	if (strcmp(op, "ping") == 0) {
		json_ok(fp);
		return;
	}

	if (strcmp(op, "list") == 0) {
		int i;
		fprintf(fp, "{\"ok\":true,\"hw\":%s,\"entries\":[",
#ifdef OPI_DOCA
			g_hw_ready ? "true" : "false"
#else
			"false"
#endif
		);
		for (i = 0; i < g_nent; i++) {
			fprintf(fp, "%s{\"name\":\"%s\",\"kind\":\"%s\"}", i ? "," : "",
				g_ents[i].name, g_ents[i].kind);
		}
		fprintf(fp, "]}\n");
		fflush(fp);
		return;
	}

	if (strcmp(op, "init") == 0) {
#ifdef OPI_DOCA
		char err[256];
		if (hw_init(uplink, vtep, err, sizeof(err)) != 0) {
			json_err(fp, err);
			return;
		}
#else
		fprintf(stderr, "dpu-doca: stub init uplink=%s vtep=%s (rebuild with make DOCA=1 for eSwitch)\n",
			uplink, vtep);
#endif
		json_ok(fp);
		return;
	}

	if (strcmp(op, "attach") == 0) {
		uint16_t p = 0;

		if (dev[0] && lookup_port(dev, &p)) {
			if (p < MAX_PORTS)
				snprintf(g_port_name[p], sizeof(g_port_name[0]), "%s", dev);
			fprintf(stderr, "dpu-doca: attach %s → dpdk port %u\n", dev,
				(unsigned)p);
		} else {
			fprintf(stderr,
				"dpu-doca: attach %s: no DPDK representor yet (not inventing port_id)\n",
				dev);
		}
		json_ok(fp);
		return;
	}

	if (strcmp(op, "overlay_add") == 0 || strcmp(op, "route_add") == 0 ||
	    strcmp(op, "acl_add") == 0) {
		const char *kind = op;
		if (!name[0]) {
			json_err(fp, "name required");
			return;
		}
		struct opi_ent *e = ent_add(name, kind);
		if (!e) {
			json_err(fp, "entry table full");
			return;
		}
#ifdef OPI_DOCA
		if (g_hw_ready && g_pipe) {
			doca_error_t rc = DOCA_SUCCESS;
			if (strcmp(op, "acl_add") == 0)
				rc = hw_acl(e, src, dst, proto, sport, dport, action, pri);
			else if (strcmp(op, "route_add") == 0)
				rc = hw_route(e, prefix, vni, vtep, cls);
			else
				rc = hw_overlay(e, dev, mac, vni, vtep);
			if (rc != DOCA_SUCCESS) {
				char buf[128];
				snprintf(buf, sizeof(buf), "%s: %s", op, doca_error_get_descr(rc));
				json_err(fp, buf);
				return;
			}
		}
#endif
		fprintf(stderr,
			"dpu-doca: %s name=%s dev=%s vni=%ld prefix=%s src=%s dst=%s proto=%ld dport=%ld action=%ld\n",
			op, name, dev, vni, prefix, src, dst, proto, dport, action);
		json_ok(fp);
		return;
	}

	if (strcmp(op, "overlay_del") == 0 || strcmp(op, "route_del") == 0 ||
	    strcmp(op, "acl_del") == 0) {
#ifdef OPI_DOCA
		hw_del(ent_find(name));
#endif
		ent_del(name);
		fprintf(stderr, "dpu-doca: %s name=%s\n", op, name);
		json_ok(fp);
		return;
	}

	json_err(fp, "unknown op");
}

static int serve(void)
{
	struct sockaddr_un addr;
	unlink(g_sock);
	g_listen = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_listen < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock);
	if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	chmod(g_sock, 0660);
	if (listen(g_listen, 16) < 0) {
		perror("listen");
		return 1;
	}
	fprintf(stderr, "dpu-doca: listening %s (%s)\n", g_sock,
#ifdef OPI_DOCA
		"DOCA Flow eSwitch"
#else
		"stub"
#endif
	);
	for (;;) {
		int fd = accept(g_listen, NULL, NULL);
		if (fd < 0)
			continue;
		FILE *fp = fdopen(fd, "r+");
		if (!fp) {
			close(fd);
			continue;
		}
		char line[MAX_LINE];
		if (fgets(line, sizeof(line), fp))
			handle(line, fp);
		fclose(fp);
	}
}

int main(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-socket") == 0) && i + 1 < argc)
			g_sock = argv[++i];
		else if (strcmp(argv[i], "-pci") == 0 && i + 1 < argc)
			g_pci = argv[++i];
		else if (strcmp(argv[i], "-eal") == 0 && i + 1 < argc)
			g_eal = argv[++i];
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			fprintf(stderr, "usage: %s [-s /var/run/opi-doca.sock] [-pci BDF] [-eal ARGS]\n",
				argv[0]);
			return 0;
		}
	}
#ifndef OPI_DOCA
	(void)g_pci;
	(void)g_eal;
#endif
	return serve();
}
