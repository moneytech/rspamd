#ifndef RSPAMD_SPF_H
#define RSPAMD_SPF_H

#include "config.h"

struct worker_task;
struct spf_record;

typedef void (*spf_cb_t)(struct spf_record *record, struct worker_task *task);

typedef enum spf_mech_e {
	SPF_FAIL,
	SPF_SOFT_FAIL,
	SPF_PASS,
	SPF_NEUTRAL
} spf_mech_t;

typedef enum spf_action_e {
	SPF_RESOLVE_MX,
	SPF_RESOLVE_A,
	SPF_RESOLVE_PTR,
	SPF_RESOLVE_REDIRECT,
	SPF_RESOLVE_INCLUDE,
	SPF_RESOLVE_EXISTS,
	SPF_RESOLVE_EXP
} spf_action_t;

struct spf_addr {
	guint32 addr;
	guint32 mask;
	spf_mech_t mech;
	gchar *spf_string;
};

struct spf_record {
	gchar **elts;

	gchar *cur_elt;
	gint elt_num;
	gint nested;
	gint dns_requests;

	GList *addrs;
	gchar *cur_domain;
	gchar *sender;
	gchar *sender_domain;
	gchar *local_part;
	struct worker_task *task;
	spf_cb_t callback;

	gboolean in_include;
};


gboolean resolve_spf (struct worker_task *task, spf_cb_t callback);


#endif
