// SPDX-License-Identifier: GPL-2.0-only
/*
 * luci.client-rates - per-host traffic survey rpcd plugin
 *
 * Serves per-client upload/download rates and per-MAC total traffic over
 * ubus, derived from conntrack accounting (ctnetlink dump).
 *
 * Accurate with MediaTek hardware NAT offload: the kernel merges PPE
 * per-entry counters back into conntrack when 'hnat counter update to
 * nf_conntrack' is enabled (hnat_setting 7 1), which the companion init
 * script does at boot. So offloaded flows are fully accounted.
 *
 * Protocol:
 *   ubus call luci.client-rates get '{"addresses":["192.168.0.10",...]}'
 *   -> {"rates":{"<ip>":{"upload":Bps,"download":Bps,"mac":"..","ready":1}},
 *       "totals":{"<MAC>":bytes},
 *       "window_ms":N,"interval_ms":N,"source":"conntrack",
 *       "accounting_disabled":0|1,"netlink_unavailable":0|1}
 *
 * Copyright (C) 2026 Become-ILLUSORY
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <linux/netlink.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubox/avl.h>
#include <libubox/avl-cmp.h>

#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>

#define INTERVAL_MS   2000
#define FLOW_LIMIT    65536
#define DUMP_TIMEOUT  8          /* seconds */
#define HASH_BITS     10         /* 1024 client buckets */

/* ------------------------------------------------------------------ */

struct flow_key {
	uint8_t  family;	/* AF_INET / AF_INET6 */
	uint8_t  l4proto;
	uint16_t sport, dport;
	uint8_t  src[16];
	uint8_t  dst[16];
};

struct flow {
	struct flow_key k;
	uint64_t up;		/* original-direction bytes (client->server) */
	uint64_t down;		/* reply-direction bytes   (server->client) */
	struct flow *next;
};

/* per-IP accounting snapshot */
struct ipacct {
	struct ipacct *next;
	uint8_t  addr[16];
	uint8_t  family;
	uint64_t up, down;	/* accumulated bytes this window */
};

/* per-MAC totals (persistent across runtime, saved to state file) */
struct mactot {
	struct mactot *next;
	char     mac[18];
	uint64_t bytes;
};

static struct ipacct  *cur[1 << HASH_BITS];
static struct ipacct  *prev[1 << HASH_BITS];
static struct mactot  *totals;
static struct ubus_context *ubus_ctx;
static struct uloop_timeout sampler;
static uint64_t last_ms;
static uint32_t flow_count;
static bool warming_up = true;

static const char *state_path = "/tmp/luci-client-traffic.state";
static const char *state_tmp  = "/tmp/luci-client-traffic.state.new";

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned iphash(const uint8_t *a)
{
	unsigned h = 5381;
	for (int i = 0; i < 16; i++)
		h = ((h << 5) + h) ^ a[i];
	return h & ((1u << HASH_BITS) - 1);
}

static bool ip_equal(const uint8_t *a, const uint8_t *b, uint8_t fam)
{
	int n = (fam == AF_INET) ? 4 : 16;
	return memcmp(a, b, n) == 0;
}

static void ip_to_str(uint8_t fam, const uint8_t *a, char *buf, size_t len)
{
	if (fam == AF_INET)
		snprintf(buf, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
	else
		inet_ntop(AF_INET6, a, buf, len);
}

/* parse "1.2.3.4" or v6 into 16-byte (v4 in first 4) */
static bool str_to_ip(const char *s, uint8_t fam, uint8_t *out)
{
	memset(out, 0, 16);
	if (fam == AF_INET) {
		unsigned b[4];
		if (sscanf(s, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4)
			return false;
		for (int i = 0; i < 4; i++) { if (b[i] > 255) return false; out[i] = b[i]; }
		return true;
	}
	return inet_pton(AF_INET6, s, out) == 1;
}

/* ------------------------------------------------------------------ */
/* conntrack dump via ctnetlink                                        */

static int ct_cb(const struct nlmsghdr *nlh, void *data)
{
	struct nf_conntrack *ct = nfct_new();
	if (!ct)
		return MNL_CB_OK;

	if (nfct_nlmsg_parse(nlh, ct) < 0)
		goto out;

	uint8_t fam  = nfct_get_attr_u8(ct, ATTR_L3PROTO);
	if (fam != AF_INET && fam != AF_INET6)
		goto out;

	/* original direction = upload, reply = download */
	uint64_t b_orig = nfct_get_attr_u64(ct, ATTR_ORIG_COUNTER_BYTES);
	uint64_t b_repl = nfct_get_attr_u64(ct, ATTR_REPL_COUNTER_BYTES);

	uint8_t src[16] = {0}, dst[16] = {0};
	if (fam == AF_INET) {
		uint32_t s = nfct_get_attr_u32(ct, ATTR_IPV4_SRC);
		uint32_t d = nfct_get_attr_u32(ct, ATTR_IPV4_DST);
		memcpy(src, &s, 4); memcpy(dst, &d, 4);
	} else {
		memcpy(src, nfct_get_attr(ct, ATTR_IPV6_SRC), 16);
		memcpy(dst, nfct_get_attr(ct, ATTR_IPV6_DST), 16);
	}

	if (flow_count >= FLOW_LIMIT)
		goto out;
	flow_count++;

	/* aggregate into current window by source (client) IP */
	unsigned h = iphash(src);
	struct ipacct *e;
	for (e = cur[h]; e; e = e->next)
		if (e->family == fam && ip_equal(e->addr, src, fam))
			break;
	if (!e) {
		e = calloc(1, sizeof(*e));
		if (!e) goto out;
		e->family = fam;
		memcpy(e->addr, src, 16);
		e->next = cur[h];
		cur[h] = e;
	}
	e->up   += b_orig;
	e->down += b_repl;
out:
	nfct_destroy(ct);
	return MNL_CB_OK;
}

static void clear_table(struct ipacct **t)
{
	for (int i = 0; i < (1 << HASH_BITS); i++) {
		struct ipacct *e = t[i];
		while (e) { struct ipacct *n = e->next; free(e); e = n; }
		t[i] = NULL;
	}
}

/* dump conntrack into `cur`; returns 0 ok, -1 netlink unavailable */
static int dump_conntrack(void)
{
	clear_table(cur);
	flow_count = 0;

	struct mnl_socket *nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!nl)
		return -1;
	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(nl);
		return -1;
	}

	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type  = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

	uint32_t portid = mnl_socket_get_portid(nl);
	uint32_t seq = (uint32_t)now_ms();
	nlh->nlmsg_seq = seq;

	struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
	nfg->nfgen_family = AF_UNSPEC;
	nfg->version      = NFNETLINK_V0;
	nfg->res_id       = htons(0);

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		mnl_socket_close(nl);
		return -1;
	}

	/* receive until NLMSG_DONE */
	int ret = 0;
	time_t deadline = time(NULL) + DUMP_TIMEOUT;
	for (;;) {
		if (time(NULL) > deadline) { ret = -1; break; }
		ssize_t n = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (n <= 0)
			break;
		int r = mnl_cb_run(buf, n, seq, portid, ct_cb, NULL);
		if (r <= MNL_CB_STOP)
			break;
	}
	mnl_socket_close(nl);
	return ret;
}

/* ------------------------------------------------------------------ */
/* per-MAC totals persistence (LCTRAF1 plain-text state)               */

static void totals_add(const char *mac, uint64_t bytes)
{
	struct mactot *m;
	for (m = totals; m; m = m->next)
		if (!strcmp(m->mac, mac)) { m->bytes += bytes; return; }
	m = calloc(1, sizeof(*m));
	if (!m) return;
	strncpy(m->mac, mac, sizeof(m->mac) - 1);
	m->bytes = bytes;
	m->next = totals;
	totals = m;
}

static void totals_save(void)
{
	FILE *f = fopen(state_tmp, "w");
	if (!f) return;
	fputs("LCTRAF1\n", f);
	for (struct mactot *m = totals; m; m = m->next)
		fprintf(f, "%s %llu\n", m->mac, (unsigned long long)m->bytes);
	fclose(f);
	rename(state_tmp, state_path);
}

static void totals_load(void)
{
	FILE *f = fopen(state_path, "r");
	if (!f) return;
	char line[64], mac[18];
	unsigned long long b;
	if (!fgets(line, sizeof(line), f) || strncmp(line, "LCTRAF1", 7)) { fclose(f); return; }
	while (fscanf(f, "%17s %llu", mac, &b) == 2)
		totals_add(mac, b);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* MAC lookup from ARP/neigh                                           */

static bool mac_for_ip(const char *ip, char *mac, size_t mlen)
{
	FILE *f = fopen("/proc/net/arp", "r");
	if (f) {
		char l[256], ia[64], hw[64], ma[64];
		if (fgets(l, sizeof(l), f)) /* header */
		while (fgets(l, sizeof(l), f)) {
			if (sscanf(l, "%63s %63s %*s %63s", ia, hw, ma) == 3 &&
			    !strcmp(ia, ip) && strcmp(ma, "00:00:00:00:00:00")) {
				strncpy(mac, ma, mlen - 1);
				for (char *p = mac; *p; p++) *p = toupper(*p);
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* periodic sampler: rotate window, accumulate totals                  */

#define PROBE_MAX_HOSTS  256
static void probe_submit(const char *ips[], int n);

static void sample_cb(struct uloop_timeout *t)
{
	uint64_t t0 = now_ms();

	if (dump_conntrack() == 0) {
		/* accumulate per-MAC totals from delta(cur, prev) */
		if (!warming_up) {
			for (int i = 0; i < (1 << HASH_BITS); i++) {
				for (struct ipacct *c = cur[i]; c; c = c->next) {
					struct ipacct *p;
					uint64_t du = 0, dd = 0;
					for (p = prev[iphash(c->addr)]; p; p = p->next)
						if (p->family == c->family && ip_equal(p->addr, c->addr, c->family)) {
							du = c->up > p->up ? c->up - p->up : 0;
							dd = c->down > p->down ? c->down - p->down : 0;
							break;
						}
					if (du + dd) {
						char ip[64], mac[18];
						ip_to_str(c->family, c->addr, ip, sizeof(ip));
						if (mac_for_ip(ip, mac, sizeof(mac)))
							totals_add(mac, du + dd);
					}
				}
			}
		}

		/* feed online IPs to the web-port prober */
		{
			static const char *ips[PROBE_MAX_HOSTS];
			static char buf[PROBE_MAX_HOSTS][64];
			int n = 0;
			for (int i = 0; i < (1 << HASH_BITS) && n < PROBE_MAX_HOSTS; i++)
				for (struct ipacct *c = cur[i]; c && n < PROBE_MAX_HOSTS; c = c->next) {
					if (c->family != AF_INET) continue; /* probe v4 web UIs only */
					ip_to_str(c->family, c->addr, buf[n], sizeof(buf[n]));
					ips[n] = buf[n]; n++;
				}
			if (n) probe_submit(ips, n);
		}

		/* rotate */
		memcpy(prev, cur, sizeof(cur));
		memset(cur, 0, sizeof(cur));
		warming_up = false;
	}

	last_ms = t0;
	totals_save();
	uloop_timeout_set(t, INTERVAL_MS);
}

/* ------------------------------------------------------------------ */
/* web port prober: background thread scans common web mgmt ports and   */
/* records which hosts expose a web UI, so the frontend can render a    */
/* clickable link. Non-blocking connect, small concurrency.             */

#define PROBE_TIMEOUT_MS 600

static const uint16_t probe_ports[] = { 80, 443, 8080, 8443, 81, 8000, 5000, 9000, 5666, 5667 };
#define N_PROBE_PORTS (sizeof(probe_ports)/sizeof(probe_ports[0]))

struct probe_result {
	char     ip[64];
	uint16_t port;        /* first open web port found, 0 = none */
	uint64_t ts;          /* when probed */
};

static struct probe_result probe_table[PROBE_MAX_HOSTS];
static int    probe_n = 0;
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;

/* pending scan queue fed by sampler */
static char probe_queue[PROBE_MAX_HOSTS][64];
static int  probe_qn = 0;
static bool probe_dirty = false;

static void probe_submit(const char *ips[], int n)
{
	pthread_mutex_lock(&probe_lock);
	probe_qn = n > PROBE_MAX_HOSTS ? PROBE_MAX_HOSTS : n;
	for (int i = 0; i < probe_qn; i++)
		snprintf(probe_queue[i], sizeof(probe_queue[i]), "%s", ips[i]);
	probe_dirty = true;
	pthread_mutex_unlock(&probe_lock);
}

/* probe one ip:port with a short non-blocking connect */
static bool port_open(const char *ip, uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;

	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { close(fd); return false; }

	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (r < 0 && errno != EINPROGRESS) { close(fd); return false; }

	if (r < 0) {
		fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
		struct timeval tv = { .tv_sec = 0, .tv_usec = PROBE_TIMEOUT_MS * 1000 };
		if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) { close(fd); return false; }
		int err = 0; socklen_t l = sizeof(err);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
		if (err) { close(fd); return false; }
	}
	close(fd);
	return true;
}

static void *probe_thread(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&probe_lock);
		bool work = probe_dirty;
		char local[PROBE_MAX_HOSTS][64];
		int n = probe_qn;
		if (work) {
			for (int i = 0; i < n; i++)
				snprintf(local[i], sizeof(local[i]), "%s", probe_queue[i]);
			probe_dirty = false;
		}
		pthread_mutex_unlock(&probe_lock);

		if (work && n > 0) {
			for (int i = 0; i < n; i++) {
				uint16_t found = 0;
				/* skip if probed recently (within 5 min) */
				uint64_t now = now_ms();
				bool fresh = false;
				pthread_mutex_lock(&probe_lock);
				for (int j = 0; j < probe_n; j++)
					if (!strcmp(probe_table[j].ip, local[i]) &&
					    now - probe_table[j].ts < 300000) {
						fresh = true; break;
					}
				pthread_mutex_unlock(&probe_lock);
				if (fresh) continue;

				for (size_t p = 0; p < N_PROBE_PORTS && !found; p++)
					if (port_open(local[i], probe_ports[p]))
						found = probe_ports[p];

				pthread_mutex_lock(&probe_lock);
				int slot = -1;
				for (int j = 0; j < probe_n; j++)
					if (!strcmp(probe_table[j].ip, local[i])) { slot = j; break; }
				if (slot < 0 && probe_n < PROBE_MAX_HOSTS)
					slot = probe_n++;
				if (slot >= 0) {
					snprintf(probe_table[slot].ip, sizeof(probe_table[slot].ip), "%s", local[i]);
					probe_table[slot].port = found;
					probe_table[slot].ts = now;
				}
				pthread_mutex_unlock(&probe_lock);
			}
		}
		usleep(500000); /* 0.5s idle */
	}
	return NULL;
}

/* look up probed web port for an ip (0 = none) */
static uint16_t probe_web_port(const char *ip)
{
	uint16_t port = 0;
	pthread_mutex_lock(&probe_lock);
	for (int j = 0; j < probe_n; j++)
		if (!strcmp(probe_table[j].ip, ip)) { port = probe_table[j].port; break; }
	pthread_mutex_unlock(&probe_lock);
	return port;
}

/* ------------------------------------------------------------------ */
/* ubus method: get                                                    */

enum { GET_ADDRESSES, __GET_MAX };
static const struct blobmsg_policy get_policy[__GET_MAX] = {
	[GET_ADDRESSES] = { "addresses", BLOBMSG_TYPE_ARRAY },
};

static int ubus_get(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__GET_MAX];
	blobmsg_parse(get_policy, __GET_MAX, tb, blob_data(msg), blob_len(msg));

	static struct blob_buf b;
	blob_buf_init(&b, 0);

	/* acct disabled? */
	int fd = open("/proc/sys/net/netfilter/nf_conntrack_acct", O_RDONLY);
	char acct = '1';
	if (fd >= 0) { if (read(fd, &acct, 1) != 1) acct = '0'; close(fd); }
	bool acct_off = (acct != '1');

	void *rates = blobmsg_open_table(&b, "rates");
	uint64_t win = warming_up ? 0 : INTERVAL_MS;

	if (tb[GET_ADDRESSES] && !acct_off && !warming_up) {
		struct blob_attr *cur_a;
		size_t rem;
		blobmsg_for_each_attr(cur_a, tb[GET_ADDRESSES], rem) {
			const char *ips = blobmsg_get_string(cur_a);
			if (!ips) continue;
			uint8_t fam = strchr(ips, ':') ? AF_INET6 : AF_INET;
			uint8_t addr[16];
			if (!str_to_ip(ips, fam, addr)) continue;

			uint64_t up = 0, down = 0;
			for (struct ipacct *e = cur[iphash(addr)]; e; e = e->next)
				if (e->family == fam && ip_equal(e->addr, addr, fam)) {
					struct ipacct *p;
					for (p = prev[iphash(addr)]; p; p = p->next)
						if (p->family == fam && ip_equal(p->addr, addr, fam)) {
							up   = e->up   > p->up   ? e->up   - p->up   : 0;
							down = e->down > p->down ? e->down - p->down : 0;
							break;
						}
					break;
				}
			/* to bytes/sec */
			up   = win ? up   * 1000 / win : 0;
			down = win ? down * 1000 / win : 0;

			char mac[18] = "";
			mac_for_ip(ips, mac, sizeof(mac));

			void *r = blobmsg_open_table(&b, ips);
			blobmsg_add_u64(&b, "upload",   up);
			blobmsg_add_u64(&b, "download", down);
			blobmsg_add_string(&b, "mac", mac);
			blobmsg_add_u8(&b, "ready", 1);
			blobmsg_add_u32(&b, "web_port", probe_web_port(ips));
			blobmsg_close_table(&b, r);
		}
	}
	blobmsg_close_table(&b, rates);

	void *tot = blobmsg_open_table(&b, "totals");
	for (struct mactot *m = totals; m; m = m->next)
		blobmsg_add_u64(&b, m->mac, m->bytes);
	blobmsg_close_table(&b, tot);

	blobmsg_add_u64(&b, "window_ms",   win);
	blobmsg_add_u64(&b, "interval_ms", INTERVAL_MS);
	blobmsg_add_string(&b, "source",   "conntrack");
	blobmsg_add_u8(&b, "accounting_disabled", acct_off ? 1 : 0);
	blobmsg_add_u8(&b, "netlink_unavailable", 0);

	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

static const struct ubus_method rates_methods[] = {
	UBUS_METHOD("get", ubus_get, get_policy),
};

static struct ubus_object_type rates_type =
	UBUS_OBJECT_TYPE("luci-client-rates", rates_methods);

static struct ubus_object rates_obj = {
	.name = "luci.client-rates",
	.type = &rates_type,
	.methods = rates_methods,
	.n_methods = ARRAY_SIZE(rates_methods),
};

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	uloop_init();
	totals_load();

	ubus_ctx = ubus_connect(NULL);
	if (!ubus_ctx) {
		fprintf(stderr, "ubus connect failed\n");
		return 1;
	}
	ubus_add_uloop(ubus_ctx);

	if (ubus_add_object(ubus_ctx, &rates_obj)) {
		fprintf(stderr, "ubus add object failed\n");
		return 1;
	}

	/* web port prober thread */
	pthread_t pt;
	pthread_create(&pt, NULL, probe_thread, NULL);
	pthread_detach(pt);

	/* first sample immediately, then periodically */
	sampler.cb = sample_cb;
	uloop_timeout_set(&sampler, 100);

	uloop_run();

	ubus_free(ubus_ctx);
	uloop_done();
	return 0;
}
