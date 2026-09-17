/*
 * Access control lists in the ingress field processor.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A rule is one property:
 *
 *     acl_<seq>=<permit|deny> [in <port>] [proto <name|number>]
 *                             [src <a.b.c.d[/len]>] [dst <a.b.c.d[/len]>]
 *                             [sport <n>] [dport <n>]
 *
 * Rules are evaluated lowest sequence number first, the first match decides,
 * and a packet no rule matches is forwarded -- an ACL here narrows what the
 * switch does, it does not replace it. Every rule counts what it matched,
 * whichever way it decided, because "the rule is in the chip and matched
 * nothing" and "the rule is in the chip and matched the wrong thing" are
 * different faults and a counter tells them apart.
 *
 * THIS IS ONE FIELD GROUP, AND THE SDK DECIDES ITS SHAPE. The group asks for
 * the ingress port, the IP protocol, both addresses and both L4 ports at once,
 * and the SDK picks the slice layout and field selectors that carry them. On
 * the AS5610's Trident+ that came out single-wide, 1792 entries. Nothing here
 * chooses slices or selectors, and nothing should: the previous attempt at
 * ACLs on this chip wrote the TCAM by hand from a capture of Cumulus doing it,
 * got every register to match, and never saw a packet, because the SDK's own
 * initialisation is what arms the lookup and no register diff finds a
 * sequence. Under this SDK's bring-up the lookup fires: the first thing
 * checked on this board was the control plane's own punt rule, which counted
 * exactly the twenty echo replies sent through it.
 *
 * THE INGRESS PORT IS IN THE KEY, NOT IN THE PORT GATE. The SDK's natural
 * qualifier for "arrived on this port" is InPorts, a port bitmap that on this
 * family lives in FP_GLOBAL_MASK_TCAM, a gate in front of the slice rather
 * than a field in the key. It does not work on the AS5610, and it fails open:
 * a rule scoped to swp6 counted the swp51 neighbour's echo replies, and a
 * deny of OSPF on swp6 took down all three adjacencies. The chip has two
 * ingress pipelines with a copy of that gate each. Read back after an
 * install, the Y copy held the port and its care bits and the X copy was all
 * zeros -- and a zero mask matches everything. The SDK writes the gate through
 * the aggregate view and expects both copies to take it; on this board, with
 * this S-Channel, only one does. The main TCAM is not affected, or the OSPF
 * deny could not have reached the other two ports. So the port is qualified
 * as SrcPort, (module, port) in the F3 selector of the key itself, which the
 * SDK places and the TCAM holds like any other field. It costs part of the
 * F3 field and nothing else. Whether the aggregate write is the SDK's fault or the
 * S-Channel bring-up's is recorded on the board's todo, not solved here.
 *
 * DENY IS DROP AND ALSO "DO NOT PUNT". The control plane's own field rules --
 * OSPF to the CPU, our own addresses to the CPU -- live in another group, and
 * on this family the actions of two groups combine: a packet dropped here
 * would still be copied up by them. Cumulus resolves that by pairing DROP with
 * SwitchToCpuCancel, and this does the same. A denied packet is gone.
 *
 * RULES ARE RE-READ WHILE RUNNING. Once a second the property files are read
 * again and, if the acl_ lines differ from what is in the chip, every entry is
 * torn down and the set is installed afresh. That is not atomic: for the
 * milliseconds between, no rule is in force. Cumulus keeps half the TCAM as a
 * standby copy to avoid that window and pays for it in capacity. This does
 * not, yet, and says so rather than pretending otherwise.
 *
 * IPv4 only. A second group carries the v6 keys -- DstIp6 is 128 bits and does
 * not share a key with DstIp -- and it is not written until something needs it.
 */
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

#include <sal/types.h>
#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/field.h>

#include <bcm/stack.h>

#include "acl.h"
#include "props.h"
#include "tapbridge.h"

#define MAX_RULES  256
#define MAX_TEXT   160
#define MAX_ERR    72

/* Sequence numbers are the operator's ordering; the chip wants a priority
 * where larger wins. Anything in this range maps one to one. */
#define MAX_SEQ    999999

enum { ACT_PERMIT, ACT_DENY };

struct rule {
	int      seq;
	char     text[MAX_TEXT];    /* as written, for showing it back */
	int      action;
	int      in_port;           /* logical port, or -1 for any */
	int      proto;             /* IP protocol, or -1 for any */
	int      have_src, have_dst;
	uint32_t src, src_mask, dst, dst_mask;
	int      sport, dport;      /* L4 ports, or -1 for any */

	bcm_field_entry_t ent;
	int      stat;
	int      installed;
	char     err[MAX_ERR];      /* why not, when not installed */
};

static int acl_unit;
static bcm_field_group_t acl_grp = -1;
static int acl_ready;
static bcm_module_t acl_modid;   /* this switch, for the source-port key */

static struct rule rules[MAX_RULES];
static int nrules;

/* Everything above is read by the query thread and written by the periodic
 * one. Coarse, and it does not need to be finer: a reload is rare and a
 * query is a person typing. */
static pthread_mutex_t acl_lock = PTHREAD_MUTEX_INITIALIZER;

/* The acl_ lines as last applied, joined, so a change is one memcmp. */
static char *applied_blob;

/* ----------------------------------------------------------------- reading */

/*
 * The acl_ properties, from the same two directories in the same order the
 * daemon read everything else at start: the image's /etc/nosaic, then the
 * switch's own configuration, later files winning. Read here rather than
 * through props.c because this happens again while the daemon runs, and the
 * property store is loaded once and never re-read.
 */
struct line {
	int  seq;
	char text[MAX_TEXT];
};

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void take_line(struct line *lines, int *n, int seq, const char *text)
{
	int i;

	for (i = 0; i < *n; i++) {
		if (lines[i].seq == seq) {
			snprintf(lines[i].text, MAX_TEXT, "%s", text);
			return;
		}
	}
	if (*n >= MAX_RULES)
		return;
	lines[*n].seq = seq;
	snprintf(lines[*n].text, MAX_TEXT, "%s", text);
	(*n)++;
}

static void read_file(const char *path, struct line *lines, int *n)
{
	char buf[512];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return;
	while (fgets(buf, sizeof(buf), f) != NULL) {
		char *hash = strchr(buf, '#');
		char *eq, *name, *val, *end;
		long seq;

		if (hash != NULL)
			*hash = '\0';
		if ((eq = strchr(buf, '=')) == NULL)
			continue;
		*eq = '\0';
		name = buf;
		while (isspace((unsigned char)*name))
			name++;
		end = name + strlen(name);
		while (end > name && isspace((unsigned char)end[-1]))
			*--end = '\0';
		if (strncmp(name, "acl_", 4) != 0)
			continue;
		seq = strtol(name + 4, &end, 10);
		if (*end != '\0' || seq < 1 || seq > MAX_SEQ)
			continue;
		val = eq + 1;
		while (isspace((unsigned char)*val))
			val++;
		end = val + strlen(val);
		while (end > val && isspace((unsigned char)end[-1]))
			*--end = '\0';
		take_line(lines, n, (int)seq, val);
	}
	fclose(f);
}

static void read_dir(const char *dir, struct line *lines, int *n)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char *names[256];
	int nn = 0, i;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && nn < 256) {
		size_t len = strlen(e->d_name);

		if (len < 6 || strcmp(e->d_name + len - 5, ".conf") != 0)
			continue;
		names[nn++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, nn, sizeof(names[0]), cmp_name);
	for (i = 0; i < nn; i++) {
		char path[512];

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		read_file(path, lines, n);
		free(names[i]);
	}
}

static int cmp_seq(const void *a, const void *b)
{
	return ((const struct line *)a)->seq - ((const struct line *)b)->seq;
}

/* All the acl_ lines, by sequence number, and the blob that identifies the
 * set. An empty value is a rule removed. */
static int read_rules(struct line *lines, char **blob)
{
	int n = 0, i, kept = 0;
	size_t len = 0;
	char *b;

	read_dir("/etc/nosaic", lines, &n);
	read_dir(NOSAIC_CONFIG_DIR, lines, &n);
	qsort(lines, n, sizeof(lines[0]), cmp_seq);
	for (i = 0; i < n; i++) {
		if (lines[i].text[0] == '\0')
			continue;
		lines[kept++] = lines[i];
	}
	for (i = 0; i < kept; i++)
		len += strlen(lines[i].text) + 16;
	if ((b = malloc(len + 1)) == NULL)
		return -1;
	b[0] = '\0';
	for (i = 0; i < kept; i++) {
		char head[16];

		snprintf(head, sizeof(head), "%d=", lines[i].seq);
		strcat(b, head);
		strcat(b, lines[i].text);
		strcat(b, "\n");
	}
	*blob = b;
	return kept;
}

/* ----------------------------------------------------------------- parsing */

static int proto_number(const char *s)
{
	static const struct { const char *name; int n; } known[] = {
		{ "icmp", 1 }, { "igmp", 2 }, { "tcp", 6 }, { "udp", 17 },
		{ "gre", 47 }, { "esp", 50 }, { "ah", 51 }, { "ospf", 89 },
		{ "vrrp", 112 }, { "sctp", 132 },
	};
	char *end;
	long n;
	size_t i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (strcmp(s, known[i].name) == 0)
			return known[i].n;
	n = strtol(s, &end, 10);
	if (*end != '\0' || n < 0 || n > 255)
		return -1;
	return (int)n;
}

static int parse_prefix(const char *s, uint32_t *addr, uint32_t *mask)
{
	char buf[40], *slash, *end;
	struct in_addr a;
	long len = 32;

	snprintf(buf, sizeof(buf), "%s", s);
	if ((slash = strchr(buf, '/')) != NULL) {
		*slash = '\0';
		len = strtol(slash + 1, &end, 10);
		if (*end != '\0' || len < 0 || len > 32)
			return -1;
	}
	if (inet_pton(AF_INET, buf, &a) != 1)
		return -1;
	*mask = len == 0 ? 0 : 0xffffffffu << (32 - len);
	*addr = ntohl(a.s_addr) & *mask;
	return 0;
}

static int port_number(const char *s)
{
	char *end;
	long n = strtol(s, &end, 10);

	if (*end != '\0' || n < 0 || n > 65535)
		return -1;
	return (int)n;
}

static int port_by_name(const char *name)
{
	int i;

	for (i = 0; i < nosaic_tap_count(); i++) {
		const char *n = NULL;
		unsigned char mac[6];
		int port, vlan, mtu;

		if (nosaic_tap_info(i, &n, &port, &vlan, &mtu, mac) != 0)
			continue;
		if (n != NULL && strcmp(n, name) == 0)
			return port;
	}
	return -1;
}

/* Fill in a rule from its text. 0, or -1 with r->err saying what was wrong.
 * The error names the word, because "bad rule" sends someone to read the
 * grammar when what they need is the one token they mistyped. */
static int parse_rule(struct rule *r, int seq, const char *text)
{
	char buf[MAX_TEXT];
	char *save = NULL, *tok;

	memset(r, 0, sizeof(*r));
	r->seq = seq;
	snprintf(r->text, sizeof(r->text), "%s", text);
	r->in_port = r->proto = r->sport = r->dport = -1;
	r->ent = -1;
	r->stat = -1;

	snprintf(buf, sizeof(buf), "%s", text);
	tok = strtok_r(buf, " \t", &save);
	if (tok == NULL) {
		snprintf(r->err, sizeof(r->err), "empty rule");
		return -1;
	}
	if (strcmp(tok, "permit") == 0)
		r->action = ACT_PERMIT;
	else if (strcmp(tok, "deny") == 0)
		r->action = ACT_DENY;
	else {
		snprintf(r->err, sizeof(r->err), "'%s': expected permit or deny", tok);
		return -1;
	}

	while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
		char *arg = strtok_r(NULL, " \t", &save);

		if (arg == NULL) {
			snprintf(r->err, sizeof(r->err), "'%s' needs a value", tok);
			return -1;
		}
		if (strcmp(tok, "in") == 0) {
			if ((r->in_port = port_by_name(arg)) < 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not a port on this switch", arg);
				return -1;
			}
		} else if (strcmp(tok, "proto") == 0) {
			if ((r->proto = proto_number(arg)) < 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not an IP protocol", arg);
				return -1;
			}
		} else if (strcmp(tok, "src") == 0) {
			if (parse_prefix(arg, &r->src, &r->src_mask) != 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not an IPv4 prefix", arg);
				return -1;
			}
			r->have_src = 1;
		} else if (strcmp(tok, "dst") == 0) {
			if (parse_prefix(arg, &r->dst, &r->dst_mask) != 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not an IPv4 prefix", arg);
				return -1;
			}
			r->have_dst = 1;
		} else if (strcmp(tok, "sport") == 0) {
			if ((r->sport = port_number(arg)) < 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not an L4 port", arg);
				return -1;
			}
		} else if (strcmp(tok, "dport") == 0) {
			if ((r->dport = port_number(arg)) < 0) {
				snprintf(r->err, sizeof(r->err),
					 "'%s' is not an L4 port", arg);
				return -1;
			}
		} else {
			snprintf(r->err, sizeof(r->err), "'%s': not a match keyword", tok);
			return -1;
		}
	}
	if ((r->sport >= 0 || r->dport >= 0) && r->proto != 6 && r->proto != 17) {
		/* The chip reads L4 ports out of whatever follows the IP header.
		 * Without a protocol to say it is TCP or UDP, that is a match on
		 * bytes of something else. */
		snprintf(r->err, sizeof(r->err), "sport/dport need proto tcp or udp");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- programming */

static const char *bcm_err(int rv)
{
	return bcm_errmsg(rv);
}

static void uninstall(struct rule *r)
{
	if (r->stat >= 0) {
		if (r->ent >= 0)
			bcm_field_entry_stat_detach(acl_unit, r->ent, r->stat);
		bcm_field_stat_destroy(acl_unit, r->stat);
		r->stat = -1;
	}
	if (r->ent >= 0) {
		bcm_field_entry_destroy(acl_unit, r->ent);
		r->ent = -1;
	}
	r->installed = 0;
}

static int install(struct rule *r)
{
	bcm_field_stat_t st[1] = { bcmFieldStatPackets };
	int rv;

	rv = bcm_field_entry_create(acl_unit, acl_grp, &r->ent);
	if (rv != BCM_E_NONE) {
		r->ent = -1;
		snprintf(r->err, sizeof(r->err), "entry_create: %s", bcm_err(rv));
		return -1;
	}
	/* Lower sequence numbers win, so they get the higher priority. */
	rv = bcm_field_entry_prio_set(acl_unit, r->ent, MAX_SEQ + 1 - r->seq);
	if (rv != BCM_E_NONE) {
		snprintf(r->err, sizeof(r->err), "prio_set: %s", bcm_err(rv));
		goto fail;
	}

	/* IPv4 and nothing else. The address fields are bytes at offsets in the
	 * key, and without this a non-IP frame whose bytes happen to line up
	 * would match an address it does not carry. Cumulus qualifies every
	 * rule on IpType for the same reason. */
	rv = bcm_field_qualify_IpType(acl_unit, r->ent, bcmFieldIpTypeIpv4Any);
	if (rv != BCM_E_NONE) {
		snprintf(r->err, sizeof(r->err), "IpType: %s", bcm_err(rv));
		goto fail;
	}
	if (r->in_port >= 0) {
		/* The ingress port as (module, port) in the key. See the note at
		 * the top on why this and not the InPorts bitmap. */
		rv = bcm_field_qualify_SrcPort(acl_unit, r->ent, acl_modid, -1,
					       (bcm_port_t)r->in_port, -1);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "SrcPort: %s", bcm_err(rv));
			goto fail;
		}
	}
	if (r->proto >= 0) {
		rv = bcm_field_qualify_IpProtocol(acl_unit, r->ent, (uint8)r->proto, 0xff);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "IpProtocol: %s", bcm_err(rv));
			goto fail;
		}
	}
	if (r->have_src) {
		rv = bcm_field_qualify_SrcIp(acl_unit, r->ent, (bcm_ip_t)r->src,
					     (bcm_ip_t)r->src_mask);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "SrcIp: %s", bcm_err(rv));
			goto fail;
		}
	}
	if (r->have_dst) {
		rv = bcm_field_qualify_DstIp(acl_unit, r->ent, (bcm_ip_t)r->dst,
					     (bcm_ip_t)r->dst_mask);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "DstIp: %s", bcm_err(rv));
			goto fail;
		}
	}
	if (r->sport >= 0) {
		rv = bcm_field_qualify_L4SrcPort(acl_unit, r->ent,
						 (bcm_l4_port_t)r->sport, 0xffff);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "L4SrcPort: %s", bcm_err(rv));
			goto fail;
		}
	}
	if (r->dport >= 0) {
		rv = bcm_field_qualify_L4DstPort(acl_unit, r->ent,
						 (bcm_l4_port_t)r->dport, 0xffff);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "L4DstPort: %s", bcm_err(rv));
			goto fail;
		}
	}

	if (r->action == ACT_DENY) {
		rv = bcm_field_action_add(acl_unit, r->ent, bcmFieldActionDrop, 0, 0);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "Drop: %s", bcm_err(rv));
			goto fail;
		}
		/* See the note at the top: dropped is also not punted. */
		rv = bcm_field_action_add(acl_unit, r->ent,
					  bcmFieldActionCopyToCpuCancel, 0, 0);
		if (rv != BCM_E_NONE) {
			snprintf(r->err, sizeof(r->err), "CopyToCpuCancel: %s", bcm_err(rv));
			goto fail;
		}
	}
	/* A permit has no action. It is still the match that wins, which is what
	 * stops a later deny from applying -- and it still counts. */

	rv = bcm_field_stat_create(acl_unit, acl_grp, 1, st, &r->stat);
	if (rv != BCM_E_NONE) {
		/* Not fatal: a rule that works and does not count is still a
		 * rule. Say so in the status rather than refusing it. */
		r->stat = -1;
		snprintf(r->err, sizeof(r->err), "no counter: %s", bcm_err(rv));
	} else {
		rv = bcm_field_entry_stat_attach(acl_unit, r->ent, r->stat);
		if (rv != BCM_E_NONE) {
			bcm_field_stat_destroy(acl_unit, r->stat);
			r->stat = -1;
			snprintf(r->err, sizeof(r->err), "no counter: %s", bcm_err(rv));
		}
	}

	rv = bcm_field_entry_install(acl_unit, r->ent);
	if (rv != BCM_E_NONE) {
		snprintf(r->err, sizeof(r->err), "install: %s", bcm_err(rv));
		goto fail;
	}
	r->installed = 1;
	return 0;
fail:
	uninstall(r);
	return -1;
}

/* Replace what is in the chip with the given lines. Caller holds the lock. */
static void program(const struct line *lines, int n)
{
	int i, ok = 0;

	for (i = 0; i < nrules; i++)
		uninstall(&rules[i]);
	nrules = 0;

	for (i = 0; i < n && nrules < MAX_RULES; i++) {
		struct rule *r = &rules[nrules++];

		if (parse_rule(r, lines[i].seq, lines[i].text) != 0) {
			fprintf(stderr, "acl: rule %d not installed: %s\n",
				r->seq, r->err);
			continue;
		}
		if (!acl_ready) {
			snprintf(r->err, sizeof(r->err), "no field group on this chip");
			continue;
		}
		if (install(r) != 0) {
			fprintf(stderr, "acl: rule %d not installed: %s\n",
				r->seq, r->err);
			continue;
		}
		ok++;
	}
	printf("acl: %d rule(s) configured, %d in the chip\n", nrules, ok);
	fflush(stdout);
}

/* Read, compare, and reprogram only on a change. */
static void reload(int force)
{
	struct line lines[MAX_RULES];
	char *blob = NULL;
	int n;

	n = read_rules(lines, &blob);
	if (n < 0)
		return;
	pthread_mutex_lock(&acl_lock);
	if (force || applied_blob == NULL || strcmp(applied_blob, blob) != 0) {
		program(lines, n);
		free(applied_blob);
		applied_blob = blob;
		blob = NULL;
	}
	pthread_mutex_unlock(&acl_lock);
	free(blob);
}

/* --------------------------------------------------------------- interface */

int nosaic_acl_start(int unit)
{
	bcm_field_qset_t q;
	bcm_field_group_mode_t mode = bcmFieldGroupModeSingle;
	int rv;

	acl_unit = unit;
	if (bcm_stk_my_modid_get(unit, &acl_modid) != BCM_E_NONE)
		acl_modid = 0;

	BCM_FIELD_QSET_INIT(q);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyStageIngress);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyIpType);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifySrcPort);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyIpProtocol);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifySrcIp);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyDstIp);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyL4SrcPort);
	BCM_FIELD_QSET_ADD(q, bcmFieldQualifyL4DstPort);

	/*
	 * Above the control plane's punt groups, which take whatever priority
	 * the SDK hands out. When a deny here and a copy-to-CPU there both
	 * match, the higher-priority group's answer to "copy to CPU?" stands,
	 * and it has to be this one's for a deny to mean gone.
	 */
	rv = bcm_field_group_create(unit, q, 100, &acl_grp);
	if (rv != BCM_E_NONE) {
		fprintf(stderr, "acl: group_create: %s -- no ACLs on this chip\n",
			bcm_err(rv));
		acl_grp = -1;
		acl_ready = 0;
		reload(1);
		return -1;
	}
	acl_ready = 1;
	bcm_field_group_mode_get(unit, acl_grp, &mode);
	printf("acl: field group %d, %s-wide\n", acl_grp,
	       mode == bcmFieldGroupModeDouble ? "double" :
	       mode == bcmFieldGroupModeTriple ? "triple" :
	       mode == bcmFieldGroupModeSingle ? "single" : "?");
	fflush(stdout);
	reload(1);
	return 0;
}

void nosaic_acl_poll(void)
{
	reload(0);
}

void nosaic_acl_capability(int *available, int *entries_total, int *entries_free)
{
	bcm_field_group_status_t st;

	*available = acl_ready;
	*entries_total = *entries_free = 0;
	if (!acl_ready)
		return;
	if (bcm_field_group_status_get(acl_unit, acl_grp, &st) == BCM_E_NONE) {
		*entries_total = st.entries_total;
		*entries_free = st.entries_free;
	}
}

/* The text is an operator's, and a quote in it would end the JSON string
 * early and leave the CLI reading the wrong fields. */
static void jput(FILE *out, const char *s)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', out);
		if ((unsigned char)*s < 0x20)
			continue;
		fputc(*s, out);
	}
}

void nosaic_acl_query(FILE *out)
{
	int total, freen, avail, i;

	nosaic_acl_capability(&avail, &total, &freen);
	pthread_mutex_lock(&acl_lock);
	fprintf(out, "{\"ok\":true,\"result\":{\"Available\":%s,"
		"\"Total\":%d,\"Free\":%d,\"Rules\":[",
		avail ? "true" : "false", total, freen);
	for (i = 0; i < nrules; i++) {
		struct rule *r = &rules[i];
		unsigned long long pkts = 0;
		uint64 v;

		if (r->stat >= 0 &&
		    bcm_field_stat_get(acl_unit, r->stat, bcmFieldStatPackets, &v) == BCM_E_NONE)
			pkts = (unsigned long long)COMPILER_64_LO(v) |
			       ((unsigned long long)COMPILER_64_HI(v) << 32);
		fprintf(out, "%s{\"Seq\":%d,\"Rule\":\"", i ? "," : "", r->seq);
		jput(out, r->text);
		fprintf(out, "\",\"Installed\":%s,\"Packets\":%llu,\"Error\":\"",
			r->installed ? "true" : "false", pkts);
		jput(out, r->err);
		fprintf(out, "\"}");
	}
	fprintf(out, "]}}\n");
	pthread_mutex_unlock(&acl_lock);
}
