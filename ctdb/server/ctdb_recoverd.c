/* 
   ctdb recovery daemon

   Copyright (C) Ronnie Sahlberg  2007

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/filesys.h"
#include "system/time.h"
#include "system/network.h"
#include "system/wait.h"

#include <popt.h>
#include <talloc.h>
#include <tevent.h>
#include <tdb.h>

#include "lib/tdb_wrap/tdb_wrap.h"
#include "lib/util/dlinklist.h"
#include "lib/util/debug.h"
#include "lib/util/samba_util.h"
#include "lib/util/util_process.h"

#include "ctdb_private.h"
#include "ctdb_client.h"

#include "common/system.h"
#include "common/cmdline.h"
#include "common/common.h"
#include "common/logging.h"


/* List of SRVID requests that need to be processed */
struct srvid_list {
	struct srvid_list *next, *prev;
	struct ctdb_srvid_message *request;
};

struct srvid_requests {
	struct srvid_list *requests;
};

static void srvid_request_reply(struct ctdb_context *ctdb,
				struct ctdb_srvid_message *request,
				TDB_DATA result)
{
	/* Someone that sent srvid==0 does not want a reply */
	if (request->srvid == 0) {
		talloc_free(request);
		return;
	}

	if (ctdb_client_send_message(ctdb, request->pnn, request->srvid,
				     result) == 0) {
		DEBUG(DEBUG_INFO,("Sent SRVID reply to %u:%llu\n",
				  (unsigned)request->pnn,
				  (unsigned long long)request->srvid));
	} else {
		DEBUG(DEBUG_ERR,("Failed to send SRVID reply to %u:%llu\n",
				 (unsigned)request->pnn,
				 (unsigned long long)request->srvid));
	}

	talloc_free(request);
}

static void srvid_requests_reply(struct ctdb_context *ctdb,
				 struct srvid_requests **requests,
				 TDB_DATA result)
{
	struct srvid_list *r;

	for (r = (*requests)->requests; r != NULL; r = r->next) {
		srvid_request_reply(ctdb, r->request, result);
	}

	/* Free the list structure... */
	TALLOC_FREE(*requests);
}

static void srvid_request_add(struct ctdb_context *ctdb,
			      struct srvid_requests **requests,
			      struct ctdb_srvid_message *request)
{
	struct srvid_list *t;
	int32_t ret;
	TDB_DATA result;

	if (*requests == NULL) {
		*requests = talloc_zero(ctdb, struct srvid_requests);
		if (*requests == NULL) {
			goto nomem;
		}
	}

	t = talloc_zero(*requests, struct srvid_list);
	if (t == NULL) {
		/* If *requests was just allocated above then free it */
		if ((*requests)->requests == NULL) {
			TALLOC_FREE(*requests);
		}
		goto nomem;
	}

	t->request = (struct ctdb_srvid_message *)talloc_steal(t, request);
	DLIST_ADD((*requests)->requests, t);

	return;

nomem:
	/* Failed to add the request to the list.  Send a fail. */
	DEBUG(DEBUG_ERR, (__location__
			  " Out of memory, failed to queue SRVID request\n"));
	ret = -ENOMEM;
	result.dsize = sizeof(ret);
	result.dptr = (uint8_t *)&ret;
	srvid_request_reply(ctdb, request, result);
}

/* An abstraction to allow an operation (takeover runs, recoveries,
 * ...) to be disabled for a given timeout */
struct ctdb_op_state {
	struct tevent_timer *timer;
	bool in_progress;
	const char *name;
};

static struct ctdb_op_state *ctdb_op_init(TALLOC_CTX *mem_ctx, const char *name)
{
	struct ctdb_op_state *state = talloc_zero(mem_ctx, struct ctdb_op_state);

	if (state != NULL) {
		state->in_progress = false;
		state->name = name;
	}

	return state;
}

static bool ctdb_op_is_disabled(struct ctdb_op_state *state)
{
	return state->timer != NULL;
}

static bool ctdb_op_begin(struct ctdb_op_state *state)
{
	if (ctdb_op_is_disabled(state)) {
		DEBUG(DEBUG_NOTICE,
		      ("Unable to begin - %s are disabled\n", state->name));
		return false;
	}

	state->in_progress = true;
	return true;
}

static bool ctdb_op_end(struct ctdb_op_state *state)
{
	return state->in_progress = false;
}

static bool ctdb_op_is_in_progress(struct ctdb_op_state *state)
{
	return state->in_progress;
}

static void ctdb_op_enable(struct ctdb_op_state *state)
{
	TALLOC_FREE(state->timer);
}

static void ctdb_op_timeout_handler(struct tevent_context *ev,
				    struct tevent_timer *te,
				    struct timeval yt, void *p)
{
	struct ctdb_op_state *state =
		talloc_get_type(p, struct ctdb_op_state);

	DEBUG(DEBUG_NOTICE,("Reenabling %s after timeout\n", state->name));
	ctdb_op_enable(state);
}

static int ctdb_op_disable(struct ctdb_op_state *state,
			   struct tevent_context *ev,
			   uint32_t timeout)
{
	if (timeout == 0) {
		DEBUG(DEBUG_NOTICE,("Reenabling %s\n", state->name));
		ctdb_op_enable(state);
		return 0;
	}

	if (state->in_progress) {
		DEBUG(DEBUG_ERR,
		      ("Unable to disable %s - in progress\n", state->name));
		return -EAGAIN;
	}

	DEBUG(DEBUG_NOTICE,("Disabling %s for %u seconds\n",
			    state->name, timeout));

	/* Clear any old timers */
	talloc_free(state->timer);

	/* Arrange for the timeout to occur */
	state->timer = tevent_add_timer(ev, state,
					timeval_current_ofs(timeout, 0),
					ctdb_op_timeout_handler, state);
	if (state->timer == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to setup timer\n"));
		return -ENOMEM;
	}

	return 0;
}

struct ctdb_banning_state {
	uint32_t count;
	struct timeval last_reported_time;
};

/*
  private state of recovery daemon
 */
struct ctdb_recoverd {
	struct ctdb_context *ctdb;
	uint32_t recmaster;
	uint32_t last_culprit_node;
	struct ctdb_node_map_old *nodemap;
	struct timeval priority_time;
	bool need_takeover_run;
	bool need_recovery;
	uint32_t node_flags;
	struct tevent_timer *send_election_te;
	struct tevent_timer *election_timeout;
	struct srvid_requests *reallocate_requests;
	struct ctdb_op_state *takeover_run;
	struct ctdb_op_state *recovery;
	struct ctdb_iface_list_old *ifaces;
	uint32_t *force_rebalance_nodes;
	struct ctdb_node_capabilities *caps;
	bool frozen_on_inactive;
};

#define CONTROL_TIMEOUT() timeval_current_ofs(ctdb->tunable.recover_timeout, 0)
#define MONITOR_TIMEOUT() timeval_current_ofs(ctdb->tunable.recover_interval, 0)

static void ctdb_restart_recd(struct tevent_context *ev,
			      struct tevent_timer *te, struct timeval t,
			      void *private_data);

/*
  ban a node for a period of time
 */
static void ctdb_ban_node(struct ctdb_recoverd *rec, uint32_t pnn, uint32_t ban_time)
{
	int ret;
	struct ctdb_context *ctdb = rec->ctdb;
	struct ctdb_ban_state bantime;

	if (!ctdb_validate_pnn(ctdb, pnn)) {
		DEBUG(DEBUG_ERR,("Bad pnn %u in ctdb_ban_node\n", pnn));
		return;
	}

	DEBUG(DEBUG_NOTICE,("Banning node %u for %u seconds\n", pnn, ban_time));

	bantime.pnn  = pnn;
	bantime.time = ban_time;

	ret = ctdb_ctrl_set_ban(ctdb, CONTROL_TIMEOUT(), pnn, &bantime);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to ban node %d\n", pnn));
		return;
	}

}

enum monitor_result { MONITOR_OK, MONITOR_RECOVERY_NEEDED, MONITOR_ELECTION_NEEDED, MONITOR_FAILED};


/*
  remember the trouble maker
 */
static void ctdb_set_culprit_count(struct ctdb_recoverd *rec, uint32_t culprit, uint32_t count)
{
	struct ctdb_context *ctdb = talloc_get_type(rec->ctdb, struct ctdb_context);
	struct ctdb_banning_state *ban_state;

	if (culprit > ctdb->num_nodes) {
		DEBUG(DEBUG_ERR,("Trying to set culprit %d but num_nodes is %d\n", culprit, ctdb->num_nodes));
		return;
	}

	/* If we are banned or stopped, do not set other nodes as culprits */
	if (rec->node_flags & NODE_FLAGS_INACTIVE) {
		DEBUG(DEBUG_NOTICE, ("This node is INACTIVE, cannot set culprit node %d\n", culprit));
		return;
	}

	if (ctdb->nodes[culprit]->ban_state == NULL) {
		ctdb->nodes[culprit]->ban_state = talloc_zero(ctdb->nodes[culprit], struct ctdb_banning_state);
		CTDB_NO_MEMORY_VOID(ctdb, ctdb->nodes[culprit]->ban_state);

		
	}
	ban_state = ctdb->nodes[culprit]->ban_state;
	if (timeval_elapsed(&ban_state->last_reported_time) > ctdb->tunable.recovery_grace_period) {
		/* this was the first time in a long while this node
		   misbehaved so we will forgive any old transgressions.
		*/
		ban_state->count = 0;
	}

	ban_state->count += count;
	ban_state->last_reported_time = timeval_current();
	rec->last_culprit_node = culprit;
}

/*
  remember the trouble maker
 */
static void ctdb_set_culprit(struct ctdb_recoverd *rec, uint32_t culprit)
{
	ctdb_set_culprit_count(rec, culprit, 1);
}


/* this callback is called for every node that failed to execute the
   recovered event
*/
static void recovered_fail_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(callback_data, struct ctdb_recoverd);

	DEBUG(DEBUG_ERR, (__location__ " Node %u failed the recovered event. Setting it as recovery fail culprit\n", node_pnn));

	ctdb_set_culprit(rec, node_pnn);
}

/*
  run the "recovered" eventscript on all nodes
 */
static int run_recovered_eventscript(struct ctdb_recoverd *rec, struct ctdb_node_map_old *nodemap, const char *caller)
{
	TALLOC_CTX *tmp_ctx;
	uint32_t *nodes;
	struct ctdb_context *ctdb = rec->ctdb;

	tmp_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY(ctdb, tmp_ctx);

	nodes = list_of_active_nodes(ctdb, nodemap, tmp_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_END_RECOVERY,
					nodes, 0,
					CONTROL_TIMEOUT(), false, tdb_null,
					NULL, recovered_fail_callback,
					rec) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to run the 'recovered' event when called from %s\n", caller));

		talloc_free(tmp_ctx);
		return -1;
	}

	talloc_free(tmp_ctx);
	return 0;
}

/* this callback is called for every node that failed to execute the
   start recovery event
*/
static void startrecovery_fail_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(callback_data, struct ctdb_recoverd);

	DEBUG(DEBUG_ERR, (__location__ " Node %u failed the startrecovery event. Setting it as recovery fail culprit\n", node_pnn));

	ctdb_set_culprit(rec, node_pnn);
}

/*
  run the "startrecovery" eventscript on all nodes
 */
static int run_startrecovery_eventscript(struct ctdb_recoverd *rec, struct ctdb_node_map_old *nodemap)
{
	TALLOC_CTX *tmp_ctx;
	uint32_t *nodes;
	struct ctdb_context *ctdb = rec->ctdb;

	tmp_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY(ctdb, tmp_ctx);

	nodes = list_of_active_nodes(ctdb, nodemap, tmp_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_START_RECOVERY,
					nodes, 0,
					CONTROL_TIMEOUT(), false, tdb_null,
					NULL,
					startrecovery_fail_callback,
					rec) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to run the 'startrecovery' event. Recovery failed.\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	talloc_free(tmp_ctx);
	return 0;
}

/*
  Retrieve capabilities from all connected nodes
 */
static int update_capabilities(struct ctdb_recoverd *rec,
			       struct ctdb_node_map_old *nodemap)
{
	uint32_t *capp;
	TALLOC_CTX *tmp_ctx;
	struct ctdb_node_capabilities *caps;
	struct ctdb_context *ctdb = rec->ctdb;

	tmp_ctx = talloc_new(rec);
	CTDB_NO_MEMORY(ctdb, tmp_ctx);

	caps = ctdb_get_capabilities(ctdb, tmp_ctx,
				     CONTROL_TIMEOUT(), nodemap);

	if (caps == NULL) {
		DEBUG(DEBUG_ERR,
		      (__location__ " Failed to get node capabilities\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	capp = ctdb_get_node_capabilities(caps, ctdb_get_pnn(ctdb));
	if (capp == NULL) {
		DEBUG(DEBUG_ERR,
		      (__location__
		       " Capabilities don't include current node.\n"));
		talloc_free(tmp_ctx);
		return -1;
	}
	ctdb->capabilities = *capp;

	TALLOC_FREE(rec->caps);
	rec->caps = talloc_steal(rec, caps);

	talloc_free(tmp_ctx);
	return 0;
}

static void set_recmode_fail_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(callback_data, struct ctdb_recoverd);

	DEBUG(DEBUG_ERR,("Failed to freeze node %u during recovery. Set it as ban culprit for %d credits\n", node_pnn, rec->nodemap->num));
	ctdb_set_culprit_count(rec, node_pnn, rec->nodemap->num);
}

static void transaction_start_fail_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(callback_data, struct ctdb_recoverd);

	DEBUG(DEBUG_ERR,("Failed to start recovery transaction on node %u. Set it as ban culprit for %d credits\n", node_pnn, rec->nodemap->num));
	ctdb_set_culprit_count(rec, node_pnn, rec->nodemap->num);
}

/*
  change recovery mode on all nodes
 */
static int set_recovery_mode(struct ctdb_context *ctdb,
			     struct ctdb_recoverd *rec,
			     struct ctdb_node_map_old *nodemap,
			     uint32_t rec_mode, bool freeze)
{
	TDB_DATA data;
	uint32_t *nodes;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY(ctdb, tmp_ctx);

	nodes = list_of_active_nodes(ctdb, nodemap, tmp_ctx, true);

	data.dsize = sizeof(uint32_t);
	data.dptr = (unsigned char *)&rec_mode;

	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_SET_RECMODE,
					nodes, 0,
					CONTROL_TIMEOUT(),
					false, data,
					NULL, NULL,
					NULL) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set recovery mode. Recovery failed.\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	/* freeze all nodes */
	if (freeze && rec_mode == CTDB_RECOVERY_ACTIVE) {
		int i;

		for (i=1; i<=NUM_DB_PRIORITIES; i++) {
			if (ctdb_client_async_control(ctdb, CTDB_CONTROL_FREEZE,
						nodes, i,
						CONTROL_TIMEOUT(),
						false, tdb_null,
						NULL,
						set_recmode_fail_callback,
						rec) != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to freeze nodes. Recovery failed.\n"));
				talloc_free(tmp_ctx);
				return -1;
			}
		}
	}

	talloc_free(tmp_ctx);
	return 0;
}

/* update all remote nodes to use the same db priority that we have
   this can fail if the remove node has not yet been upgraded to 
   support this function, so we always return success and never fail
   a recovery if this call fails.
*/
static int update_db_priority_on_remote_nodes(struct ctdb_context *ctdb,
	struct ctdb_node_map_old *nodemap, 
	uint32_t pnn, struct ctdb_dbid_map_old *dbmap, TALLOC_CTX *mem_ctx)
{
	int db;

	/* step through all local databases */
	for (db=0; db<dbmap->num;db++) {
		struct ctdb_db_priority db_prio;
		int ret;

		db_prio.db_id     = dbmap->dbs[db].db_id;
		ret = ctdb_ctrl_get_db_priority(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, dbmap->dbs[db].db_id, &db_prio.priority);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to read database priority from local node for db 0x%08x\n", dbmap->dbs[db].db_id));
			continue;
		}

		DEBUG(DEBUG_INFO,("Update DB priority for db 0x%08x to %u\n", dbmap->dbs[db].db_id, db_prio.priority)); 

		ret = ctdb_ctrl_set_db_priority(ctdb, CONTROL_TIMEOUT(),
						CTDB_CURRENT_NODE, &db_prio);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to set DB priority for 0x%08x\n",
					 db_prio.db_id));
		}
	}

	return 0;
}			

/*
  ensure all other nodes have attached to any databases that we have
 */
static int create_missing_remote_databases(struct ctdb_context *ctdb, struct ctdb_node_map_old *nodemap, 
					   uint32_t pnn, struct ctdb_dbid_map_old *dbmap, TALLOC_CTX *mem_ctx)
{
	int i, j, db, ret;
	struct ctdb_dbid_map_old *remote_dbmap;

	/* verify that all other nodes have all our databases */
	for (j=0; j<nodemap->num; j++) {
		/* we don't need to ourself ourselves */
		if (nodemap->nodes[j].pnn == pnn) {
			continue;
		}
		/* don't check nodes that are unavailable */
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		ret = ctdb_ctrl_getdbmap(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, 
					 mem_ctx, &remote_dbmap);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Unable to get dbids from node %u\n", pnn));
			return -1;
		}

		/* step through all local databases */
		for (db=0; db<dbmap->num;db++) {
			const char *name;


			for (i=0;i<remote_dbmap->num;i++) {
				if (dbmap->dbs[db].db_id == remote_dbmap->dbs[i].db_id) {
					break;
				}
			}
			/* the remote node already have this database */
			if (i!=remote_dbmap->num) {
				continue;
			}
			/* ok so we need to create this database */
			ret = ctdb_ctrl_getdbname(ctdb, CONTROL_TIMEOUT(), pnn,
						  dbmap->dbs[db].db_id, mem_ctx,
						  &name);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to get dbname from node %u\n", pnn));
				return -1;
			}
			ret = ctdb_ctrl_createdb(ctdb, CONTROL_TIMEOUT(),
						 nodemap->nodes[j].pnn,
						 mem_ctx, name,
						 dbmap->dbs[db].flags & CTDB_DB_FLAGS_PERSISTENT);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to create remote db:%s\n", name));
				return -1;
			}
		}
	}

	return 0;
}


/*
  ensure we are attached to any databases that anyone else is attached to
 */
static int create_missing_local_databases(struct ctdb_context *ctdb, struct ctdb_node_map_old *nodemap, 
					  uint32_t pnn, struct ctdb_dbid_map_old **dbmap, TALLOC_CTX *mem_ctx)
{
	int i, j, db, ret;
	struct ctdb_dbid_map_old *remote_dbmap;

	/* verify that we have all database any other node has */
	for (j=0; j<nodemap->num; j++) {
		/* we don't need to ourself ourselves */
		if (nodemap->nodes[j].pnn == pnn) {
			continue;
		}
		/* don't check nodes that are unavailable */
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		ret = ctdb_ctrl_getdbmap(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, 
					 mem_ctx, &remote_dbmap);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Unable to get dbids from node %u\n", pnn));
			return -1;
		}

		/* step through all databases on the remote node */
		for (db=0; db<remote_dbmap->num;db++) {
			const char *name;

			for (i=0;i<(*dbmap)->num;i++) {
				if (remote_dbmap->dbs[db].db_id == (*dbmap)->dbs[i].db_id) {
					break;
				}
			}
			/* we already have this db locally */
			if (i!=(*dbmap)->num) {
				continue;
			}
			/* ok so we need to create this database and
			   rebuild dbmap
			 */
			ctdb_ctrl_getdbname(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, 
					    remote_dbmap->dbs[db].db_id, mem_ctx, &name);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to get dbname from node %u\n", 
					  nodemap->nodes[j].pnn));
				return -1;
			}
			ctdb_ctrl_createdb(ctdb, CONTROL_TIMEOUT(), pnn, mem_ctx, name, 
					   remote_dbmap->dbs[db].flags & CTDB_DB_FLAGS_PERSISTENT);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to create local db:%s\n", name));
				return -1;
			}
			ret = ctdb_ctrl_getdbmap(ctdb, CONTROL_TIMEOUT(), pnn, mem_ctx, dbmap);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to reread dbmap on node %u\n", pnn));
				return -1;
			}
		}
	}

	return 0;
}


/*
  pull the remote database contents from one node into the recdb
 */
static int pull_one_remote_database(struct ctdb_context *ctdb, uint32_t srcnode,
				    struct tdb_wrap *recdb, uint32_t dbid)
{
	int ret;
	TDB_DATA outdata;
	struct ctdb_marshall_buffer *reply;
	struct ctdb_rec_data_old *recdata;
	int i;
	TALLOC_CTX *tmp_ctx = talloc_new(recdb);

	ret = ctdb_ctrl_pulldb(ctdb, srcnode, dbid, CTDB_LMASTER_ANY, tmp_ctx,
			       CONTROL_TIMEOUT(), &outdata);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to copy db from node %u\n", srcnode));
		talloc_free(tmp_ctx);
		return -1;
	}

	reply = (struct ctdb_marshall_buffer *)outdata.dptr;

	if (outdata.dsize < offsetof(struct ctdb_marshall_buffer, data)) {
		DEBUG(DEBUG_ERR,(__location__ " invalid data in pulldb reply\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	recdata = (struct ctdb_rec_data_old *)&reply->data[0];

	for (i=0;
	     i<reply->count;
	     recdata = (struct ctdb_rec_data_old *)(recdata->length + (uint8_t *)recdata), i++) {
		TDB_DATA key, data;
		struct ctdb_ltdb_header *hdr;
		TDB_DATA existing;

		key.dptr = &recdata->data[0];
		key.dsize = recdata->keylen;
		data.dptr = &recdata->data[key.dsize];
		data.dsize = recdata->datalen;

		hdr = (struct ctdb_ltdb_header *)data.dptr;

		if (data.dsize < sizeof(struct ctdb_ltdb_header)) {
			DEBUG(DEBUG_CRIT,(__location__ " bad ltdb record\n"));
			talloc_free(tmp_ctx);
			return -1;
		}

		/* fetch the existing record, if any */
		existing = tdb_fetch(recdb->tdb, key);

		if (existing.dptr != NULL) {
			struct ctdb_ltdb_header header;
			if (existing.dsize < sizeof(struct ctdb_ltdb_header)) {
				DEBUG(DEBUG_CRIT,(__location__ " Bad record size %u from node %u\n",
					 (unsigned)existing.dsize, srcnode));
				free(existing.dptr);
				talloc_free(tmp_ctx);
				return -1;
			}
			header = *(struct ctdb_ltdb_header *)existing.dptr;
			free(existing.dptr);
			if (!(header.rsn < hdr->rsn ||
			      (header.dmaster != ctdb_get_pnn(ctdb) &&
			       header.rsn == hdr->rsn))) {
				continue;
			}
		}

		if (tdb_store(recdb->tdb, key, data, TDB_REPLACE) != 0) {
			DEBUG(DEBUG_CRIT,(__location__ " Failed to store record\n"));
			talloc_free(tmp_ctx);
			return -1;
		}
	}

	talloc_free(tmp_ctx);

	return 0;
}


struct pull_seqnum_cbdata {
	int failed;
	uint32_t pnn;
	uint64_t seqnum;
};

static void pull_seqnum_cb(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct pull_seqnum_cbdata *cb_data = talloc_get_type(callback_data, struct pull_seqnum_cbdata);
	uint64_t seqnum;

	if (cb_data->failed != 0) {
		DEBUG(DEBUG_ERR, ("Got seqnum from node %d but we have already failed the entire operation\n", node_pnn));
		return;
	}

	if (res != 0) {
		DEBUG(DEBUG_ERR, ("Error when pulling seqnum from node %d\n", node_pnn));
		cb_data->failed = 1;
		return;
	}

	if (outdata.dsize != sizeof(uint64_t)) {
		DEBUG(DEBUG_ERR, ("Error when reading pull seqnum from node %d, got %d bytes but expected %d\n", node_pnn, (int)outdata.dsize, (int)sizeof(uint64_t)));
		cb_data->failed = -1;
		return;
	}

	seqnum = *((uint64_t *)outdata.dptr);

	if (seqnum > cb_data->seqnum ||
	    (cb_data->pnn == -1 && seqnum == 0)) {
		cb_data->seqnum = seqnum;
		cb_data->pnn = node_pnn;
	}
}

static void pull_seqnum_fail_cb(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct pull_seqnum_cbdata *cb_data = talloc_get_type(callback_data, struct pull_seqnum_cbdata);

	DEBUG(DEBUG_ERR, ("Failed to pull db seqnum from node %d\n", node_pnn));
	cb_data->failed = 1;
}

static int pull_highest_seqnum_pdb(struct ctdb_context *ctdb,
				struct ctdb_recoverd *rec, 
				struct ctdb_node_map_old *nodemap, 
				struct tdb_wrap *recdb, uint32_t dbid)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	uint32_t *nodes;
	TDB_DATA data;
	uint32_t outdata[2];
	struct pull_seqnum_cbdata *cb_data;

	DEBUG(DEBUG_NOTICE, ("Scan for highest seqnum pdb for db:0x%08x\n", dbid));

	outdata[0] = dbid;
	outdata[1] = 0;

	data.dsize = sizeof(outdata);
	data.dptr  = (uint8_t *)&outdata[0];

	cb_data = talloc(tmp_ctx, struct pull_seqnum_cbdata);
	if (cb_data == NULL) {
		DEBUG(DEBUG_ERR, ("Failed to allocate pull highest seqnum cb_data structure\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	cb_data->failed = 0;
	cb_data->pnn    = -1;
	cb_data->seqnum = 0;
	
	nodes = list_of_active_nodes(ctdb, nodemap, tmp_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_GET_DB_SEQNUM,
					nodes, 0,
					CONTROL_TIMEOUT(), false, data,
					pull_seqnum_cb,
					pull_seqnum_fail_cb,
					cb_data) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to run async GET_DB_SEQNUM\n"));

		talloc_free(tmp_ctx);
		return -1;
	}

	if (cb_data->failed != 0) {
		DEBUG(DEBUG_NOTICE, ("Failed to pull sequence numbers for DB 0x%08x\n", dbid));
		talloc_free(tmp_ctx);
		return -1;
	}

	if (cb_data->pnn == -1) {
		DEBUG(DEBUG_NOTICE, ("Failed to find a node with highest sequence numbers for DB 0x%08x\n", dbid));
		talloc_free(tmp_ctx);
		return -1;
	}

	DEBUG(DEBUG_NOTICE, ("Pull persistent db:0x%08x from node %d with highest seqnum:%lld\n", dbid, cb_data->pnn, (long long)cb_data->seqnum)); 

	if (pull_one_remote_database(ctdb, cb_data->pnn, recdb, dbid) != 0) {
		DEBUG(DEBUG_ERR, ("Failed to pull higest seqnum database 0x%08x from node %d\n", dbid, cb_data->pnn));
		talloc_free(tmp_ctx);
		return -1;
	}

	talloc_free(tmp_ctx);
	return 0;
}


/*
  pull all the remote database contents into the recdb
 */
static int pull_remote_database(struct ctdb_context *ctdb,
				struct ctdb_recoverd *rec, 
				struct ctdb_node_map_old *nodemap, 
				struct tdb_wrap *recdb, uint32_t dbid,
				bool persistent)
{
	int j;

	if (persistent && ctdb->tunable.recover_pdb_by_seqnum != 0) {
		int ret;
		ret = pull_highest_seqnum_pdb(ctdb, rec, nodemap, recdb, dbid);
		if (ret == 0) {
			return 0;
		}
	}

	/* pull all records from all other nodes across onto this node
	   (this merges based on rsn)
	*/
	for (j=0; j<nodemap->num; j++) {
		/* don't merge from nodes that are unavailable */
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		if (pull_one_remote_database(ctdb, nodemap->nodes[j].pnn, recdb, dbid) != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to pull remote database from node %u\n", 
				 nodemap->nodes[j].pnn));
			ctdb_set_culprit_count(rec, nodemap->nodes[j].pnn, nodemap->num);
			return -1;
		}
	}
	
	return 0;
}


/*
  update flags on all active nodes
 */
static int update_flags_on_all_nodes(struct ctdb_context *ctdb, struct ctdb_node_map_old *nodemap, uint32_t pnn, uint32_t flags)
{
	int ret;

	ret = ctdb_ctrl_modflags(ctdb, CONTROL_TIMEOUT(), pnn, flags, ~flags);
		if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to update nodeflags on remote nodes\n"));
		return -1;
	}

	return 0;
}

/*
  ensure all nodes have the same vnnmap we do
 */
static int update_vnnmap_on_all_nodes(struct ctdb_context *ctdb, struct ctdb_node_map_old *nodemap, 
				      uint32_t pnn, struct ctdb_vnn_map *vnnmap, TALLOC_CTX *mem_ctx)
{
	int j, ret;

	/* push the new vnn map out to all the nodes */
	for (j=0; j<nodemap->num; j++) {
		/* don't push to nodes that are unavailable */
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		ret = ctdb_ctrl_setvnnmap(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, mem_ctx, vnnmap);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Unable to set vnnmap for node %u\n", pnn));
			return -1;
		}
	}

	return 0;
}


/*
  called when a vacuum fetch has completed - just free it and do the next one
 */
static void vacuum_fetch_callback(struct ctdb_client_call_state *state)
{
	talloc_free(state);
}


/**
 * Process one elements of the vacuum fetch list:
 * Migrate it over to us with the special flag
 * CTDB_CALL_FLAG_VACUUM_MIGRATION.
 */
static bool vacuum_fetch_process_one(struct ctdb_db_context *ctdb_db,
				     uint32_t pnn,
				     struct ctdb_rec_data_old *r)
{
	struct ctdb_client_call_state *state;
	TDB_DATA data;
	struct ctdb_ltdb_header *hdr;
	struct ctdb_call call;

	ZERO_STRUCT(call);
	call.call_id = CTDB_NULL_FUNC;
	call.flags = CTDB_IMMEDIATE_MIGRATION;
	call.flags |= CTDB_CALL_FLAG_VACUUM_MIGRATION;

	call.key.dptr = &r->data[0];
	call.key.dsize = r->keylen;

	/* ensure we don't block this daemon - just skip a record if we can't get
	   the chainlock */
	if (tdb_chainlock_nonblock(ctdb_db->ltdb->tdb, call.key) != 0) {
		return true;
	}

	data = tdb_fetch(ctdb_db->ltdb->tdb, call.key);
	if (data.dptr == NULL) {
		tdb_chainunlock(ctdb_db->ltdb->tdb, call.key);
		return true;
	}

	if (data.dsize < sizeof(struct ctdb_ltdb_header)) {
		free(data.dptr);
		tdb_chainunlock(ctdb_db->ltdb->tdb, call.key);
		return true;
	}

	hdr = (struct ctdb_ltdb_header *)data.dptr;
	if (hdr->dmaster == pnn) {
		/* its already local */
		free(data.dptr);
		tdb_chainunlock(ctdb_db->ltdb->tdb, call.key);
		return true;
	}

	free(data.dptr);

	state = ctdb_call_send(ctdb_db, &call);
	tdb_chainunlock(ctdb_db->ltdb->tdb, call.key);
	if (state == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to setup vacuum fetch call\n"));
		return false;
	}
	state->async.fn = vacuum_fetch_callback;
	state->async.private_data = NULL;

	return true;
}


/*
  handler for vacuum fetch
*/
static void vacuum_fetch_handler(uint64_t srvid, TDB_DATA data,
				 void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	struct ctdb_marshall_buffer *recs;
	int ret, i;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	const char *name;
	struct ctdb_dbid_map_old *dbmap=NULL;
	bool persistent = false;
	struct ctdb_db_context *ctdb_db;
	struct ctdb_rec_data_old *r;

	recs = (struct ctdb_marshall_buffer *)data.dptr;

	if (recs->count == 0) {
		goto done;
	}

	/* work out if the database is persistent */
	ret = ctdb_ctrl_getdbmap(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, tmp_ctx, &dbmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to get dbids from local node\n"));
		goto done;
	}

	for (i=0;i<dbmap->num;i++) {
		if (dbmap->dbs[i].db_id == recs->db_id) {
			persistent = dbmap->dbs[i].flags & CTDB_DB_FLAGS_PERSISTENT;
			break;
		}
	}
	if (i == dbmap->num) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to find db_id 0x%x on local node\n", recs->db_id));
		goto done;
	}

	/* find the name of this database */
	if (ctdb_ctrl_getdbname(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, recs->db_id, tmp_ctx, &name) != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to get name of db 0x%x\n", recs->db_id));
		goto done;
	}

	/* attach to it */
	ctdb_db = ctdb_attach(ctdb, CONTROL_TIMEOUT(), name, persistent, 0);
	if (ctdb_db == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to attach to database '%s'\n", name));
		goto done;
	}

	r = (struct ctdb_rec_data_old *)&recs->data[0];
	while (recs->count) {
		bool ok;

		ok = vacuum_fetch_process_one(ctdb_db, rec->ctdb->pnn, r);
		if (!ok) {
			break;
		}

		r = (struct ctdb_rec_data_old *)(r->length + (uint8_t *)r);
		recs->count--;
	}

done:
	talloc_free(tmp_ctx);
}


/*
 * handler for database detach
 */
static void detach_database_handler(uint64_t srvid, TDB_DATA data,
				    void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	uint32_t db_id;
	struct ctdb_db_context *ctdb_db;

	if (data.dsize != sizeof(db_id)) {
		return;
	}
	db_id = *(uint32_t *)data.dptr;

	ctdb_db = find_ctdb_db(ctdb, db_id);
	if (ctdb_db == NULL) {
		/* database is not attached */
		return;
	}

	DLIST_REMOVE(ctdb->db_list, ctdb_db);

	DEBUG(DEBUG_NOTICE, ("Detached from database '%s'\n",
			     ctdb_db->db_name));
	talloc_free(ctdb_db);
}

/*
  called when ctdb_wait_timeout should finish
 */
static void ctdb_wait_handler(struct tevent_context *ev,
			      struct tevent_timer *te,
			      struct timeval yt, void *p)
{
	uint32_t *timed_out = (uint32_t *)p;
	(*timed_out) = 1;
}

/*
  wait for a given number of seconds
 */
static void ctdb_wait_timeout(struct ctdb_context *ctdb, double secs)
{
	uint32_t timed_out = 0;
	time_t usecs = (secs - (time_t)secs) * 1000000;
	tevent_add_timer(ctdb->ev, ctdb, timeval_current_ofs(secs, usecs),
			 ctdb_wait_handler, &timed_out);
	while (!timed_out) {
		tevent_loop_once(ctdb->ev);
	}
}

/*
  called when an election times out (ends)
 */
static void ctdb_election_timeout(struct tevent_context *ev,
				  struct tevent_timer *te,
				  struct timeval t, void *p)
{
	struct ctdb_recoverd *rec = talloc_get_type(p, struct ctdb_recoverd);
	rec->election_timeout = NULL;
	fast_start = false;

	DEBUG(DEBUG_WARNING,("Election period ended\n"));
}


/*
  wait for an election to finish. It finished election_timeout seconds after
  the last election packet is received
 */
static void ctdb_wait_election(struct ctdb_recoverd *rec)
{
	struct ctdb_context *ctdb = rec->ctdb;
	while (rec->election_timeout) {
		tevent_loop_once(ctdb->ev);
	}
}

/*
  Update our local flags from all remote connected nodes. 
  This is only run when we are or we belive we are the recovery master
 */
static int update_local_flags(struct ctdb_recoverd *rec, struct ctdb_node_map_old *nodemap)
{
	int j;
	struct ctdb_context *ctdb = rec->ctdb;
	TALLOC_CTX *mem_ctx = talloc_new(ctdb);

	/* get the nodemap for all active remote nodes and verify
	   they are the same as for this node
	 */
	for (j=0; j<nodemap->num; j++) {
		struct ctdb_node_map_old *remote_nodemap=NULL;
		int ret;

		if (nodemap->nodes[j].flags & NODE_FLAGS_DISCONNECTED) {
			continue;
		}
		if (nodemap->nodes[j].pnn == ctdb->pnn) {
			continue;
		}

		ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, 
					   mem_ctx, &remote_nodemap);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Unable to get nodemap from remote node %u\n", 
				  nodemap->nodes[j].pnn));
			ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
			talloc_free(mem_ctx);
			return MONITOR_FAILED;
		}
		if (nodemap->nodes[j].flags != remote_nodemap->nodes[j].flags) {
			/* We should tell our daemon about this so it
			   updates its flags or else we will log the same 
			   message again in the next iteration of recovery.
			   Since we are the recovery master we can just as
			   well update the flags on all nodes.
			*/
			ret = ctdb_ctrl_modflags(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, remote_nodemap->nodes[j].flags, ~remote_nodemap->nodes[j].flags);
			if (ret != 0) {
				DEBUG(DEBUG_ERR, (__location__ " Unable to update nodeflags on remote nodes\n"));
				return -1;
			}

			/* Update our local copy of the flags in the recovery
			   daemon.
			*/
			DEBUG(DEBUG_NOTICE,("Remote node %u had flags 0x%x, local had 0x%x - updating local\n",
				 nodemap->nodes[j].pnn, remote_nodemap->nodes[j].flags,
				 nodemap->nodes[j].flags));
			nodemap->nodes[j].flags = remote_nodemap->nodes[j].flags;
		}
		talloc_free(remote_nodemap);
	}
	talloc_free(mem_ctx);
	return MONITOR_OK;
}


/* Create a new random generation id.
   The generation id can not be the INVALID_GENERATION id
*/
static uint32_t new_generation(void)
{
	uint32_t generation;

	while (1) {
		generation = random();

		if (generation != INVALID_GENERATION) {
			break;
		}
	}

	return generation;
}


/*
  create a temporary working database
 */
static struct tdb_wrap *create_recdb(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx)
{
	char *name;
	struct tdb_wrap *recdb;
	unsigned tdb_flags;

	/* open up the temporary recovery database */
	name = talloc_asprintf(mem_ctx, "%s/recdb.tdb.%u",
			       ctdb->db_directory_state,
			       ctdb->pnn);
	if (name == NULL) {
		return NULL;
	}
	unlink(name);

	tdb_flags = TDB_NOLOCK;
	if (ctdb->valgrinding) {
		tdb_flags |= TDB_NOMMAP;
	}
	tdb_flags |= (TDB_INCOMPATIBLE_HASH | TDB_DISALLOW_NESTING);

	recdb = tdb_wrap_open(mem_ctx, name, ctdb->tunable.database_hash_size, 
			      tdb_flags, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (recdb == NULL) {
		DEBUG(DEBUG_CRIT,(__location__ " Failed to create temp recovery database '%s'\n", name));
	}

	talloc_free(name);

	return recdb;
}


/* 
   a traverse function for pulling all relevant records from recdb
 */
struct recdb_data {
	struct ctdb_context *ctdb;
	struct ctdb_marshall_buffer *recdata;
	uint32_t len;
	uint32_t allocated_len;
	bool failed;
	bool persistent;
};

static int traverse_recdb(struct tdb_context *tdb, TDB_DATA key, TDB_DATA data, void *p)
{
	struct recdb_data *params = (struct recdb_data *)p;
	struct ctdb_rec_data_old *recdata;
	struct ctdb_ltdb_header *hdr;

	/*
	 * skip empty records - but NOT for persistent databases:
	 *
	 * The record-by-record mode of recovery deletes empty records.
	 * For persistent databases, this can lead to data corruption
	 * by deleting records that should be there:
	 *
	 * - Assume the cluster has been running for a while.
	 *
	 * - A record R in a persistent database has been created and
	 *   deleted a couple of times, the last operation being deletion,
	 *   leaving an empty record with a high RSN, say 10.
	 *
	 * - Now a node N is turned off.
	 *
	 * - This leaves the local database copy of D on N with the empty
	 *   copy of R and RSN 10. On all other nodes, the recovery has deleted
	 *   the copy of record R.
	 *
	 * - Now the record is created again while node N is turned off.
	 *   This creates R with RSN = 1 on all nodes except for N.
	 *
	 * - Now node N is turned on again. The following recovery will chose
	 *   the older empty copy of R due to RSN 10 > RSN 1.
	 *
	 * ==> Hence the record is gone after the recovery.
	 *
	 * On databases like Samba's registry, this can damage the higher-level
	 * data structures built from the various tdb-level records.
	 */
	if (!params->persistent && data.dsize <= sizeof(struct ctdb_ltdb_header)) {
		return 0;
	}

	/* update the dmaster field to point to us */
	hdr = (struct ctdb_ltdb_header *)data.dptr;
	if (!params->persistent) {
		hdr->dmaster = params->ctdb->pnn;
		hdr->flags |= CTDB_REC_FLAG_MIGRATED_WITH_DATA;
	}

	/* add the record to the blob ready to send to the nodes */
	recdata = ctdb_marshall_record(params->recdata, 0, key, NULL, data);
	if (recdata == NULL) {
		params->failed = true;
		return -1;
	}
	if (params->len + recdata->length >= params->allocated_len) {
		params->allocated_len = recdata->length + params->len + params->ctdb->tunable.pulldb_preallocation_size;
		params->recdata = talloc_realloc_size(NULL, params->recdata, params->allocated_len);
	}
	if (params->recdata == NULL) {
		DEBUG(DEBUG_CRIT,(__location__ " Failed to expand recdata to %u\n",
			 recdata->length + params->len));
		params->failed = true;
		return -1;
	}
	params->recdata->count++;
	memcpy(params->len+(uint8_t *)params->recdata, recdata, recdata->length);
	params->len += recdata->length;
	talloc_free(recdata);

	return 0;
}

/*
  push the recdb database out to all nodes
 */
static int push_recdb_database(struct ctdb_context *ctdb, uint32_t dbid,
			       bool persistent,
			       struct tdb_wrap *recdb, struct ctdb_node_map_old *nodemap)
{
	struct recdb_data params;
	struct ctdb_marshall_buffer *recdata;
	TDB_DATA outdata;
	TALLOC_CTX *tmp_ctx;
	uint32_t *nodes;

	tmp_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY(ctdb, tmp_ctx);

	recdata = talloc_zero(recdb, struct ctdb_marshall_buffer);
	CTDB_NO_MEMORY(ctdb, recdata);

	recdata->db_id = dbid;

	params.ctdb = ctdb;
	params.recdata = recdata;
	params.len = offsetof(struct ctdb_marshall_buffer, data);
	params.allocated_len = params.len;
	params.failed = false;
	params.persistent = persistent;

	if (tdb_traverse_read(recdb->tdb, traverse_recdb, &params) == -1) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to traverse recdb database\n"));
		talloc_free(params.recdata);
		talloc_free(tmp_ctx);
		return -1;
	}

	if (params.failed) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to traverse recdb database\n"));
		talloc_free(params.recdata);
		talloc_free(tmp_ctx);
		return -1;		
	}

	recdata = params.recdata;

	outdata.dptr = (void *)recdata;
	outdata.dsize = params.len;

	nodes = list_of_active_nodes(ctdb, nodemap, tmp_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_PUSH_DB,
					nodes, 0,
					CONTROL_TIMEOUT(), false, outdata,
					NULL, NULL,
					NULL) != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to push recdb records to nodes for db 0x%x\n", dbid));
		talloc_free(recdata);
		talloc_free(tmp_ctx);
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - pushed remote database 0x%x of size %u\n", 
		  dbid, recdata->count));

	talloc_free(recdata);
	talloc_free(tmp_ctx);

	return 0;
}


/*
  go through a full recovery on one database 
 */
static int recover_database(struct ctdb_recoverd *rec, 
			    TALLOC_CTX *mem_ctx,
			    uint32_t dbid,
			    bool persistent,
			    uint32_t pnn, 
			    struct ctdb_node_map_old *nodemap,
			    uint32_t transaction_id)
{
	struct tdb_wrap *recdb;
	int ret;
	struct ctdb_context *ctdb = rec->ctdb;
	TDB_DATA data;
	struct ctdb_transdb w;
	uint32_t *nodes;

	recdb = create_recdb(ctdb, mem_ctx);
	if (recdb == NULL) {
		return -1;
	}

	/* pull all remote databases onto the recdb */
	ret = pull_remote_database(ctdb, rec, nodemap, recdb, dbid, persistent);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to pull remote database 0x%x\n", dbid));
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - pulled remote database 0x%x\n", dbid));

	/* wipe all the remote databases. This is safe as we are in a transaction */
	w.db_id = dbid;
	w.tid = transaction_id;

	data.dptr = (void *)&w;
	data.dsize = sizeof(w);

	nodes = list_of_active_nodes(ctdb, nodemap, recdb, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_WIPE_DATABASE,
					nodes, 0,
					CONTROL_TIMEOUT(), false, data,
					NULL, NULL,
					NULL) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to wipe database. Recovery failed.\n"));
		talloc_free(recdb);
		return -1;
	}
	
	/* push out the correct database. This sets the dmaster and skips 
	   the empty records */
	ret = push_recdb_database(ctdb, dbid, persistent, recdb, nodemap);
	if (ret != 0) {
		talloc_free(recdb);
		return -1;
	}

	/* all done with this database */
	talloc_free(recdb);

	return 0;
}

/* when we start a recovery, make sure all nodes use the same reclock file
   setting
*/
static int sync_recovery_lock_file_across_cluster(struct ctdb_recoverd *rec)
{
	struct ctdb_context *ctdb = rec->ctdb;
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	TDB_DATA data;
	uint32_t *nodes;

	if (ctdb->recovery_lock_file == NULL) {
		data.dptr  = NULL;
		data.dsize = 0;
	} else {
		data.dsize = strlen(ctdb->recovery_lock_file) + 1;
		data.dptr  = (uint8_t *)ctdb->recovery_lock_file;
	}

	nodes = list_of_active_nodes(ctdb, rec->nodemap, tmp_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_SET_RECLOCK_FILE,
					nodes, 0,
					CONTROL_TIMEOUT(),
					false, data,
					NULL, NULL,
					rec) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to sync reclock file settings\n"));
		talloc_free(tmp_ctx);
		return -1;
	}

	talloc_free(tmp_ctx);
	return 0;
}


/*
 * this callback is called for every node that failed to execute ctdb_takeover_run()
 * and set flag to re-run takeover run.
 */
static void takeover_fail_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	DEBUG(DEBUG_ERR, ("Node %u failed the takeover run\n", node_pnn));

	if (callback_data != NULL) {
		struct ctdb_recoverd *rec = talloc_get_type(callback_data, struct ctdb_recoverd);

		DEBUG(DEBUG_ERR, ("Setting node %u as recovery fail culprit\n", node_pnn));

		ctdb_set_culprit(rec, node_pnn);
	}
}


static void ban_misbehaving_nodes(struct ctdb_recoverd *rec, bool *self_ban)
{
	struct ctdb_context *ctdb = rec->ctdb;
	int i;
	struct ctdb_banning_state *ban_state;

	*self_ban = false;
	for (i=0; i<ctdb->num_nodes; i++) {
		if (ctdb->nodes[i]->ban_state == NULL) {
			continue;
		}
		ban_state = (struct ctdb_banning_state *)ctdb->nodes[i]->ban_state;
		if (ban_state->count < 2*ctdb->num_nodes) {
			continue;
		}

		DEBUG(DEBUG_NOTICE,("Node %u reached %u banning credits - banning it for %u seconds\n",
			ctdb->nodes[i]->pnn, ban_state->count,
			ctdb->tunable.recovery_ban_period));
		ctdb_ban_node(rec, ctdb->nodes[i]->pnn, ctdb->tunable.recovery_ban_period);
		ban_state->count = 0;

		/* Banning ourself? */
		if (ctdb->nodes[i]->pnn == rec->ctdb->pnn) {
			*self_ban = true;
		}
	}
}

static bool do_takeover_run(struct ctdb_recoverd *rec,
			    struct ctdb_node_map_old *nodemap,
			    bool banning_credits_on_fail)
{
	uint32_t *nodes = NULL;
	struct ctdb_disable_message dtr;
	TDB_DATA data;
	int i;
	uint32_t *rebalance_nodes = rec->force_rebalance_nodes;
	int ret;
	bool ok;

	DEBUG(DEBUG_NOTICE, ("Takeover run starting\n"));

	if (ctdb_op_is_in_progress(rec->takeover_run)) {
		DEBUG(DEBUG_ERR, (__location__
				  " takeover run already in progress \n"));
		ok = false;
		goto done;
	}

	if (!ctdb_op_begin(rec->takeover_run)) {
		ok = false;
		goto done;
	}

	/* Disable IP checks (takeover runs, really) on other nodes
	 * while doing this takeover run.  This will stop those other
	 * nodes from triggering takeover runs when think they should
	 * be hosting an IP but it isn't yet on an interface.  Don't
	 * wait for replies since a failure here might cause some
	 * noise in the logs but will not actually cause a problem.
	 */
	ZERO_STRUCT(dtr);
	dtr.srvid = 0; /* No reply */
	dtr.pnn = -1;

	data.dptr  = (uint8_t*)&dtr;
	data.dsize = sizeof(dtr);

	nodes = list_of_connected_nodes(rec->ctdb, nodemap, rec, false);

	/* Disable for 60 seconds.  This can be a tunable later if
	 * necessary.
	 */
	dtr.timeout = 60;
	for (i = 0; i < talloc_array_length(nodes); i++) {
		if (ctdb_client_send_message(rec->ctdb, nodes[i],
					     CTDB_SRVID_DISABLE_TAKEOVER_RUNS,
					     data) != 0) {
			DEBUG(DEBUG_INFO,("Failed to disable takeover runs\n"));
		}
	}

	ret = ctdb_takeover_run(rec->ctdb, nodemap,
				rec->force_rebalance_nodes,
				takeover_fail_callback,
				banning_credits_on_fail ? rec : NULL);

	/* Reenable takeover runs and IP checks on other nodes */
	dtr.timeout = 0;
	for (i = 0; i < talloc_array_length(nodes); i++) {
		if (ctdb_client_send_message(rec->ctdb, nodes[i],
					     CTDB_SRVID_DISABLE_TAKEOVER_RUNS,
					     data) != 0) {
			DEBUG(DEBUG_INFO,("Failed to re-enable takeover runs\n"));
		}
	}

	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("ctdb_takeover_run() failed\n"));
		ok = false;
		goto done;
	}

	ok = true;
	/* Takeover run was successful so clear force rebalance targets */
	if (rebalance_nodes == rec->force_rebalance_nodes) {
		TALLOC_FREE(rec->force_rebalance_nodes);
	} else {
		DEBUG(DEBUG_WARNING,
		      ("Rebalance target nodes changed during takeover run - not clearing\n"));
	}
done:
	rec->need_takeover_run = !ok;
	talloc_free(nodes);
	ctdb_op_end(rec->takeover_run);

	DEBUG(DEBUG_NOTICE, ("Takeover run %s\n", ok ? "completed successfully" : "unsuccessful"));
	return ok;
}

struct recovery_helper_state {
	int fd[2];
	pid_t pid;
	int result;
	bool done;
};

static void ctdb_recovery_handler(struct tevent_context *ev,
				  struct tevent_fd *fde,
				  uint16_t flags, void *private_data)
{
	struct recovery_helper_state *state = talloc_get_type_abort(
		private_data, struct recovery_helper_state);
	int ret;

	ret = sys_read(state->fd[0], &state->result, sizeof(state->result));
	if (ret != sizeof(state->result)) {
		state->result = EPIPE;
	}

	state->done = true;
}


static int db_recovery_parallel(struct ctdb_recoverd *rec, TALLOC_CTX *mem_ctx)
{
	static char prog[PATH_MAX+1] = "";
	const char **args;
	struct recovery_helper_state *state;
	struct tevent_fd *fde;
	int nargs, ret;

	if (!ctdb_set_helper("recovery_helper", prog, sizeof(prog),
			     "CTDB_RECOVERY_HELPER", CTDB_HELPER_BINDIR,
			     "ctdb_recovery_helper")) {
		ctdb_die(rec->ctdb, "Unable to set recovery helper\n");
	}

	state = talloc_zero(mem_ctx, struct recovery_helper_state);
	if (state == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " memory error\n"));
		return -1;
	}

	state->pid = -1;

	ret = pipe(state->fd);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,
		      ("Failed to create pipe for recovery helper\n"));
		goto fail;
	}

	set_close_on_exec(state->fd[0]);

	nargs = 4;
	args = talloc_array(state, const char *, nargs);
	if (args == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " memory error\n"));
		goto fail;
	}

	args[0] = talloc_asprintf(args, "%d", state->fd[1]);
	args[1] = rec->ctdb->daemon.name;
	args[2] = talloc_asprintf(args, "%u", new_generation());
	args[3] = NULL;

	if (args[0] == NULL || args[2] == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " memory error\n"));
		goto fail;
	}

	setenv("CTDB_DBDIR_STATE", rec->ctdb->db_directory_state, 1);

	if (!ctdb_vfork_with_logging(state, rec->ctdb, "recovery", prog, nargs,
				     args, NULL, NULL, &state->pid)) {
		DEBUG(DEBUG_ERR,
		      ("Failed to create child for recovery helper\n"));
		goto fail;
	}

	close(state->fd[1]);
	state->fd[1] = -1;

	state->done = false;

	fde = tevent_add_fd(rec->ctdb->ev, rec->ctdb, state->fd[0],
			    TEVENT_FD_READ, ctdb_recovery_handler, state);
	if (fde == NULL) {
		goto fail;
	}
	tevent_fd_set_auto_close(fde);

	while (!state->done) {
		tevent_loop_once(rec->ctdb->ev);
	}

	close(state->fd[0]);
	state->fd[0] = -1;

	if (state->result != 0) {
		goto fail;
	}

	ctdb_kill(rec->ctdb, state->pid, SIGKILL);
	talloc_free(state);
	return 0;

fail:
	if (state->fd[0] != -1) {
		close(state->fd[0]);
	}
	if (state->fd[1] != -1) {
		close(state->fd[1]);
	}
	if (state->pid != -1) {
		ctdb_kill(rec->ctdb, state->pid, SIGKILL);
	}
	talloc_free(state);
	return -1;
}

static int db_recovery_serial(struct ctdb_recoverd *rec, TALLOC_CTX *mem_ctx,
			      uint32_t pnn, struct ctdb_node_map_old *nodemap,
			      struct ctdb_vnn_map *vnnmap,
			      struct ctdb_dbid_map_old *dbmap)
{
	struct ctdb_context *ctdb = rec->ctdb;
	uint32_t generation;
	TDB_DATA data;
	uint32_t *nodes;
	int ret, i, j;

	/* set recovery mode to active on all nodes */
	ret = set_recovery_mode(ctdb, rec, nodemap, CTDB_RECOVERY_ACTIVE, true);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set recovery mode to active on cluster\n"));
		return -1;
	}

	/* execute the "startrecovery" event script on all nodes */
	ret = run_startrecovery_eventscript(rec, nodemap);
	if (ret!=0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to run the 'startrecovery' event on cluster\n"));
		return -1;
	}

	/* pick a new generation number */
	generation = new_generation();

	/* change the vnnmap on this node to use the new generation 
	   number but not on any other nodes.
	   this guarantees that if we abort the recovery prematurely
	   for some reason (a node stops responding?)
	   that we can just return immediately and we will reenter
	   recovery shortly again.
	   I.e. we deliberately leave the cluster with an inconsistent
	   generation id to allow us to abort recovery at any stage and
	   just restart it from scratch.
	 */
	vnnmap->generation = generation;
	ret = ctdb_ctrl_setvnnmap(ctdb, CONTROL_TIMEOUT(), pnn, mem_ctx, vnnmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set vnnmap for node %u\n", pnn));
		return -1;
	}

	/* Database generations are updated when the transaction is commited to
	 * the databases.  So make sure to use the final generation as the
	 * transaction id
	 */
	generation = new_generation();

	data.dptr = (void *)&generation;
	data.dsize = sizeof(uint32_t);

	nodes = list_of_active_nodes(ctdb, nodemap, mem_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_TRANSACTION_START,
					nodes, 0,
					CONTROL_TIMEOUT(), false, data,
					NULL,
					transaction_start_fail_callback,
					rec) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to start transactions. Recovery failed.\n"));
		if (ctdb_client_async_control(ctdb, CTDB_CONTROL_TRANSACTION_CANCEL,
					nodes, 0,
					CONTROL_TIMEOUT(), false, tdb_null,
					NULL,
					NULL,
					NULL) != 0) {
			DEBUG(DEBUG_ERR,("Failed to cancel recovery transaction\n"));
		}
		return -1;
	}

	DEBUG(DEBUG_NOTICE,(__location__ " started transactions on all nodes\n"));

	for (i=0;i<dbmap->num;i++) {
		ret = recover_database(rec, mem_ctx,
				       dbmap->dbs[i].db_id,
				       dbmap->dbs[i].flags & CTDB_DB_FLAGS_PERSISTENT,
				       pnn, nodemap, generation);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Failed to recover database 0x%x\n", dbmap->dbs[i].db_id));
			return -1;
		}
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - starting database commits\n"));

	/* commit all the changes */
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_TRANSACTION_COMMIT,
					nodes, 0,
					CONTROL_TIMEOUT(), false, data,
					NULL, NULL,
					NULL) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to commit recovery changes. Recovery failed.\n"));
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - committed databases\n"));

	/* build a new vnn map with all the currently active and
	   unbanned nodes */
	vnnmap = talloc(mem_ctx, struct ctdb_vnn_map);
	CTDB_NO_MEMORY(ctdb, vnnmap);
	vnnmap->generation = generation;
	vnnmap->size = 0;
	vnnmap->map = talloc_zero_array(vnnmap, uint32_t, vnnmap->size);
	CTDB_NO_MEMORY(ctdb, vnnmap->map);
	for (i=j=0;i<nodemap->num;i++) {
		if (nodemap->nodes[i].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		if (!ctdb_node_has_capabilities(rec->caps,
						ctdb->nodes[i]->pnn,
						CTDB_CAP_LMASTER)) {
			/* this node can not be an lmaster */
			DEBUG(DEBUG_DEBUG, ("Node %d cant be a LMASTER, skipping it\n", i));
			continue;
		}

		vnnmap->size++;
		vnnmap->map = talloc_realloc(vnnmap, vnnmap->map, uint32_t, vnnmap->size);
		CTDB_NO_MEMORY(ctdb, vnnmap->map);
		vnnmap->map[j++] = nodemap->nodes[i].pnn;

	}
	if (vnnmap->size == 0) {
		DEBUG(DEBUG_NOTICE, ("No suitable lmasters found. Adding local node (recmaster) anyway.\n"));
		vnnmap->size++;
		vnnmap->map = talloc_realloc(vnnmap, vnnmap->map, uint32_t, vnnmap->size);
		CTDB_NO_MEMORY(ctdb, vnnmap->map);
		vnnmap->map[0] = pnn;
	}

	/* update to the new vnnmap on all nodes */
	ret = update_vnnmap_on_all_nodes(ctdb, nodemap, pnn, vnnmap, mem_ctx);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to update vnnmap on all nodes\n"));
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - updated vnnmap\n"));

	/* disable recovery mode */
	ret = set_recovery_mode(ctdb, rec, nodemap, CTDB_RECOVERY_NORMAL, false);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set recovery mode to normal on cluster\n"));
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - disabled recovery mode\n"));

	/* execute the "recovered" event script on all nodes */
	ret = run_recovered_eventscript(rec, nodemap, "do_recovery");
	if (ret!=0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to run the 'recovered' event on cluster. Recovery process failed.\n"));
		return -1;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - finished the recovered event\n"));

	return 0;
}

/*
  we are the recmaster, and recovery is needed - start a recovery run
 */
static int do_recovery(struct ctdb_recoverd *rec,
		       TALLOC_CTX *mem_ctx, uint32_t pnn,
		       struct ctdb_node_map_old *nodemap, struct ctdb_vnn_map *vnnmap)
{
	struct ctdb_context *ctdb = rec->ctdb;
	int i, ret;
	struct ctdb_dbid_map_old *dbmap;
	struct timeval start_time;
	bool self_ban;
	bool par_recovery;

	DEBUG(DEBUG_NOTICE, (__location__ " Starting do_recovery\n"));

	/* Check if the current node is still the recmaster.  It's possible that
	 * re-election has changed the recmaster.
	 */
	if (pnn != rec->recmaster) {
		DEBUG(DEBUG_NOTICE,
		      ("Recovery master changed to %u, aborting recovery\n",
		       rec->recmaster));
		return -1;
	}

	/* if recovery fails, force it again */
	rec->need_recovery = true;

	if (!ctdb_op_begin(rec->recovery)) {
		return -1;
	}

	if (rec->election_timeout) {
		/* an election is in progress */
		DEBUG(DEBUG_ERR, ("do_recovery called while election in progress - try again later\n"));
		goto fail;
	}

	ban_misbehaving_nodes(rec, &self_ban);
	if (self_ban) {
		DEBUG(DEBUG_NOTICE, ("This node was banned, aborting recovery\n"));
		goto fail;
	}

        if (ctdb->recovery_lock_file != NULL) {
		if (ctdb_recovery_have_lock(ctdb)) {
			DEBUG(DEBUG_NOTICE, ("Already holding recovery lock\n"));
		} else {
			start_time = timeval_current();
			DEBUG(DEBUG_NOTICE, ("Attempting to take recovery lock (%s)\n",
					     ctdb->recovery_lock_file));
			if (!ctdb_recovery_lock(ctdb)) {
				if (ctdb->runstate == CTDB_RUNSTATE_FIRST_RECOVERY) {
					/* If ctdb is trying first recovery, it's
					 * possible that current node does not know
					 * yet who the recmaster is.
					 */
					DEBUG(DEBUG_ERR, ("Unable to get recovery lock"
							  " - retrying recovery\n"));
					goto fail;
				}

				DEBUG(DEBUG_ERR,("Unable to get recovery lock - aborting recovery "
						 "and ban ourself for %u seconds\n",
						 ctdb->tunable.recovery_ban_period));
				ctdb_ban_node(rec, pnn, ctdb->tunable.recovery_ban_period);
				goto fail;
			}
			ctdb_ctrl_report_recd_lock_latency(ctdb,
							   CONTROL_TIMEOUT(),
							   timeval_elapsed(&start_time));
			DEBUG(DEBUG_NOTICE,
			      ("Recovery lock taken successfully by recovery daemon\n"));
		}
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery initiated due to problem with node %u\n", rec->last_culprit_node));

	/* get a list of all databases */
	ret = ctdb_ctrl_getdbmap(ctdb, CONTROL_TIMEOUT(), pnn, mem_ctx, &dbmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to get dbids from node :%u\n", pnn));
		goto fail;
	}

	/* we do the db creation before we set the recovery mode, so the freeze happens
	   on all databases we will be dealing with. */

	/* verify that we have all the databases any other node has */
	ret = create_missing_local_databases(ctdb, nodemap, pnn, &dbmap, mem_ctx);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to create missing local databases\n"));
		goto fail;
	}

	/* verify that all other nodes have all our databases */
	ret = create_missing_remote_databases(ctdb, nodemap, pnn, dbmap, mem_ctx);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to create missing remote databases\n"));
		goto fail;
	}
	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - created remote databases\n"));

	/* update the database priority for all remote databases */
	ret = update_db_priority_on_remote_nodes(ctdb, nodemap, pnn, dbmap, mem_ctx);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set db priority on remote nodes\n"));
	}
	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - updated db priority for all databases\n"));


	/* update all other nodes to use the same setting for reclock files
	   as the local recovery master.
	*/
	sync_recovery_lock_file_across_cluster(rec);

	/* Retrieve capabilities from all connected nodes */
	ret = update_capabilities(rec, nodemap);
	if (ret!=0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to update node capabilities.\n"));
		return -1;
	}

	/*
	  update all nodes to have the same flags that we have
	 */
	for (i=0;i<nodemap->num;i++) {
		if (nodemap->nodes[i].flags & NODE_FLAGS_DISCONNECTED) {
			continue;
		}

		ret = update_flags_on_all_nodes(ctdb, nodemap, i, nodemap->nodes[i].flags);
		if (ret != 0) {
			if (nodemap->nodes[i].flags & NODE_FLAGS_INACTIVE) {
				DEBUG(DEBUG_WARNING, (__location__ "Unable to update flags on inactive node %d\n", i));
			} else {
				DEBUG(DEBUG_ERR, (__location__ " Unable to update flags on all nodes for node %d\n", i));
				return -1;
			}
		}
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery - updated flags\n"));

	/* Check if all participating nodes have parallel recovery capability */
	par_recovery = true;
	for (i=0; i<nodemap->num; i++) {
		if (nodemap->nodes[i].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		if (!(rec->caps[i].capabilities &
		      CTDB_CAP_PARALLEL_RECOVERY)) {
			par_recovery = false;
			break;
		}
	}

	if (par_recovery) {
		ret = db_recovery_parallel(rec, mem_ctx);
	} else {
		ret = db_recovery_serial(rec, mem_ctx, pnn, nodemap, vnnmap,
					 dbmap);
	}

	if (ret != 0) {
		goto fail;
	}

	do_takeover_run(rec, nodemap, false);

	/* send a message to all clients telling them that the cluster 
	   has been reconfigured */
	ret = ctdb_client_send_message(ctdb, CTDB_BROADCAST_CONNECTED,
				       CTDB_SRVID_RECONFIGURE, tdb_null);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to send reconfigure message\n"));
		goto fail;
	}

	DEBUG(DEBUG_NOTICE, (__location__ " Recovery complete\n"));

	rec->need_recovery = false;
	ctdb_op_end(rec->recovery);

	/* we managed to complete a full recovery, make sure to forgive
	   any past sins by the nodes that could now participate in the
	   recovery.
	*/
	DEBUG(DEBUG_ERR,("Resetting ban count to 0 for all nodes\n"));
	for (i=0;i<nodemap->num;i++) {
		struct ctdb_banning_state *ban_state;

		if (nodemap->nodes[i].flags & NODE_FLAGS_DISCONNECTED) {
			continue;
		}

		ban_state = (struct ctdb_banning_state *)ctdb->nodes[nodemap->nodes[i].pnn]->ban_state;
		if (ban_state == NULL) {
			continue;
		}

		ban_state->count = 0;
	}

	/* We just finished a recovery successfully.
	   We now wait for rerecovery_timeout before we allow
	   another recovery to take place.
	*/
	DEBUG(DEBUG_NOTICE, ("Just finished a recovery. New recoveries will now be supressed for the rerecovery timeout (%d seconds)\n", ctdb->tunable.rerecovery_timeout));
	ctdb_op_disable(rec->recovery, ctdb->ev,
			ctdb->tunable.rerecovery_timeout);
	return 0;

fail:
	ctdb_op_end(rec->recovery);
	return -1;
}


/*
  elections are won by first checking the number of connected nodes, then
  the priority time, then the pnn
 */
struct election_message {
	uint32_t num_connected;
	struct timeval priority_time;
	uint32_t pnn;
	uint32_t node_flags;
};

/*
  form this nodes election data
 */
static void ctdb_election_data(struct ctdb_recoverd *rec, struct election_message *em)
{
	int ret, i;
	struct ctdb_node_map_old *nodemap;
	struct ctdb_context *ctdb = rec->ctdb;

	ZERO_STRUCTP(em);

	em->pnn = rec->ctdb->pnn;
	em->priority_time = rec->priority_time;

	ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, rec, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " unable to get node map\n"));
		return;
	}

	rec->node_flags = nodemap->nodes[ctdb->pnn].flags;
	em->node_flags = rec->node_flags;

	for (i=0;i<nodemap->num;i++) {
		if (!(nodemap->nodes[i].flags & NODE_FLAGS_DISCONNECTED)) {
			em->num_connected++;
		}
	}

	/* we shouldnt try to win this election if we cant be a recmaster */
	if ((ctdb->capabilities & CTDB_CAP_RECMASTER) == 0) {
		em->num_connected = 0;
		em->priority_time = timeval_current();
	}

	talloc_free(nodemap);
}

/*
  see if the given election data wins
 */
static bool ctdb_election_win(struct ctdb_recoverd *rec, struct election_message *em)
{
	struct election_message myem;
	int cmp = 0;

	ctdb_election_data(rec, &myem);

	/* we cant win if we don't have the recmaster capability */
	if ((rec->ctdb->capabilities & CTDB_CAP_RECMASTER) == 0) {
		return false;
	}

	/* we cant win if we are banned */
	if (rec->node_flags & NODE_FLAGS_BANNED) {
		return false;
	}

	/* we cant win if we are stopped */
	if (rec->node_flags & NODE_FLAGS_STOPPED) {
		return false;
	}

	/* we will automatically win if the other node is banned */
	if (em->node_flags & NODE_FLAGS_BANNED) {
		return true;
	}

	/* we will automatically win if the other node is banned */
	if (em->node_flags & NODE_FLAGS_STOPPED) {
		return true;
	}

	/* then the longest running node */
	if (cmp == 0) {
		cmp = timeval_compare(&em->priority_time, &myem.priority_time);
	}

	if (cmp == 0) {
		cmp = (int)myem.pnn - (int)em->pnn;
	}

	return cmp > 0;
}

/*
  send out an election request
 */
static int send_election_request(struct ctdb_recoverd *rec, uint32_t pnn)
{
	int ret;
	TDB_DATA election_data;
	struct election_message emsg;
	uint64_t srvid;
	struct ctdb_context *ctdb = rec->ctdb;

	srvid = CTDB_SRVID_ELECTION;

	ctdb_election_data(rec, &emsg);

	election_data.dsize = sizeof(struct election_message);
	election_data.dptr  = (unsigned char *)&emsg;


	/* first we assume we will win the election and set 
	   recoverymaster to be ourself on the current node
	 */
	ret = ctdb_ctrl_setrecmaster(ctdb, CONTROL_TIMEOUT(),
				     CTDB_CURRENT_NODE, pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " failed to set recmaster\n"));
		return -1;
	}
	rec->recmaster = pnn;

	/* send an election message to all active nodes */
	DEBUG(DEBUG_INFO,(__location__ " Send election request to all active nodes\n"));
	return ctdb_client_send_message(ctdb, CTDB_BROADCAST_ALL, srvid, election_data);
}

/*
  we think we are winning the election - send a broadcast election request
 */
static void election_send_request(struct tevent_context *ev,
				  struct tevent_timer *te,
				  struct timeval t, void *p)
{
	struct ctdb_recoverd *rec = talloc_get_type(p, struct ctdb_recoverd);
	int ret;

	ret = send_election_request(rec, ctdb_get_pnn(rec->ctdb));
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to send election request!\n"));
	}

	TALLOC_FREE(rec->send_election_te);
}

/*
  handler for memory dumps
*/
static void mem_dump_handler(uint64_t srvid, TDB_DATA data, void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	TDB_DATA *dump;
	int ret;
	struct ctdb_srvid_message *rd;

	if (data.dsize != sizeof(struct ctdb_srvid_message)) {
		DEBUG(DEBUG_ERR, (__location__ " Wrong size of return address.\n"));
		talloc_free(tmp_ctx);
		return;
	}
	rd = (struct ctdb_srvid_message *)data.dptr;

	dump = talloc_zero(tmp_ctx, TDB_DATA);
	if (dump == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to allocate memory for memdump\n"));
		talloc_free(tmp_ctx);
		return;
	}
	ret = ctdb_dump_memory(ctdb, dump);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " ctdb_dump_memory() failed\n"));
		talloc_free(tmp_ctx);
		return;
	}

DEBUG(DEBUG_ERR, ("recovery master memory dump\n"));		

	ret = ctdb_client_send_message(ctdb, rd->pnn, rd->srvid, *dump);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to send rd memdump reply message\n"));
		talloc_free(tmp_ctx);
		return;
	}

	talloc_free(tmp_ctx);
}

/*
  handler for reload_nodes
*/
static void reload_nodes_handler(uint64_t srvid, TDB_DATA data,
				 void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);

	DEBUG(DEBUG_ERR, (__location__ " Reload nodes file from recovery daemon\n"));

	ctdb_load_nodes_file(rec->ctdb);
}


static void ctdb_rebalance_timeout(struct tevent_context *ev,
				   struct tevent_timer *te,
				   struct timeval t, void *p)
{
	struct ctdb_recoverd *rec = talloc_get_type(p, struct ctdb_recoverd);

	if (rec->force_rebalance_nodes == NULL) {
		DEBUG(DEBUG_ERR,
		      ("Rebalance timeout occurred - no nodes to rebalance\n"));
		return;
	}

	DEBUG(DEBUG_NOTICE,
	      ("Rebalance timeout occurred - trigger takeover run\n"));
	rec->need_takeover_run = true;
}


static void recd_node_rebalance_handler(uint64_t srvid, TDB_DATA data,
					void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	uint32_t pnn;
	uint32_t *t;
	int len;
	uint32_t deferred_rebalance;

	if (rec->recmaster != ctdb_get_pnn(ctdb)) {
		return;
	}

	if (data.dsize != sizeof(uint32_t)) {
		DEBUG(DEBUG_ERR,(__location__ " Incorrect size of node rebalance message. Was %zd but expected %zd bytes\n", data.dsize, sizeof(uint32_t)));
		return;
	}

	pnn = *(uint32_t *)&data.dptr[0];

	DEBUG(DEBUG_NOTICE,("Setting up rebalance of IPs to node %u\n", pnn));

	/* Copy any existing list of nodes.  There's probably some
	 * sort of realloc variant that will do this but we need to
	 * make sure that freeing the old array also cancels the timer
	 * event for the timeout... not sure if realloc will do that.
	 */
	len = (rec->force_rebalance_nodes != NULL) ?
		talloc_array_length(rec->force_rebalance_nodes) :
		0;

	/* This allows duplicates to be added but they don't cause
	 * harm.  A call to add a duplicate PNN arguably means that
	 * the timeout should be reset, so this is the simplest
	 * solution.
	 */
	t = talloc_zero_array(rec, uint32_t, len+1);
	CTDB_NO_MEMORY_VOID(ctdb, t);
	if (len > 0) {
		memcpy(t, rec->force_rebalance_nodes, sizeof(uint32_t) * len);
	}
	t[len] = pnn;

	talloc_free(rec->force_rebalance_nodes);

	rec->force_rebalance_nodes = t;

	/* If configured, setup a deferred takeover run to make sure
	 * that certain nodes get IPs rebalanced to them.  This will
	 * be cancelled if a successful takeover run happens before
	 * the timeout.  Assign tunable value to variable for
	 * readability.
	 */
	deferred_rebalance = ctdb->tunable.deferred_rebalance_on_node_add;
	if (deferred_rebalance != 0) {
		tevent_add_timer(ctdb->ev, rec->force_rebalance_nodes,
				 timeval_current_ofs(deferred_rebalance, 0),
				 ctdb_rebalance_timeout, rec);
	}
}



static void recd_update_ip_handler(uint64_t srvid, TDB_DATA data,
				   void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_public_ip *ip;

	if (rec->recmaster != rec->ctdb->pnn) {
		DEBUG(DEBUG_INFO,("Not recmaster, ignore update ip message\n"));
		return;
	}

	if (data.dsize != sizeof(struct ctdb_public_ip)) {
		DEBUG(DEBUG_ERR,(__location__ " Incorrect size of recd update ip message. Was %zd but expected %zd bytes\n", data.dsize, sizeof(struct ctdb_public_ip)));
		return;
	}

	ip = (struct ctdb_public_ip *)data.dptr;

	update_ip_assignment_tree(rec->ctdb, ip);
}

static void srvid_disable_and_reply(struct ctdb_context *ctdb,
				    TDB_DATA data,
				    struct ctdb_op_state *op_state)
{
	struct ctdb_disable_message *r;
	uint32_t timeout;
	TDB_DATA result;
	int32_t ret = 0;

	/* Validate input data */
	if (data.dsize != sizeof(struct ctdb_disable_message)) {
		DEBUG(DEBUG_ERR,(__location__ " Wrong size for data :%lu "
				 "expecting %lu\n", (long unsigned)data.dsize,
				 (long unsigned)sizeof(struct ctdb_srvid_message)));
		return;
	}
	if (data.dptr == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " No data received\n"));
		return;
	}

	r = (struct ctdb_disable_message *)data.dptr;
	timeout = r->timeout;

	ret = ctdb_op_disable(op_state, ctdb->ev, timeout);
	if (ret != 0) {
		goto done;
	}

	/* Returning our PNN tells the caller that we succeeded */
	ret = ctdb_get_pnn(ctdb);
done:
	result.dsize = sizeof(int32_t);
	result.dptr  = (uint8_t *)&ret;
	srvid_request_reply(ctdb, (struct ctdb_srvid_message *)r, result);
}

static void disable_takeover_runs_handler(uint64_t srvid, TDB_DATA data,
					  void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);

	srvid_disable_and_reply(rec->ctdb, data, rec->takeover_run);
}

/* Backward compatibility for this SRVID */
static void disable_ip_check_handler(uint64_t srvid, TDB_DATA data,
				     void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	uint32_t timeout;

	if (data.dsize != sizeof(uint32_t)) {
		DEBUG(DEBUG_ERR,(__location__ " Wrong size for data :%lu "
				 "expecting %lu\n", (long unsigned)data.dsize,
				 (long unsigned)sizeof(uint32_t)));
		return;
	}
	if (data.dptr == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " No data received\n"));
		return;
	}

	timeout = *((uint32_t *)data.dptr);

	ctdb_op_disable(rec->takeover_run, rec->ctdb->ev, timeout);
}

static void disable_recoveries_handler(uint64_t srvid, TDB_DATA data,
				       void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);

	srvid_disable_and_reply(rec->ctdb, data, rec->recovery);
}

/*
  handler for ip reallocate, just add it to the list of requests and 
  handle this later in the monitor_cluster loop so we do not recurse
  with other requests to takeover_run()
*/
static void ip_reallocate_handler(uint64_t srvid, TDB_DATA data,
				  void *private_data)
{
	struct ctdb_srvid_message *request;
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);

	if (data.dsize != sizeof(struct ctdb_srvid_message)) {
		DEBUG(DEBUG_ERR, (__location__ " Wrong size of return address.\n"));
		return;
	}

	request = (struct ctdb_srvid_message *)data.dptr;

	srvid_request_add(rec->ctdb, &rec->reallocate_requests, request);
}

static void process_ipreallocate_requests(struct ctdb_context *ctdb,
					  struct ctdb_recoverd *rec)
{
	TDB_DATA result;
	int32_t ret;
	struct srvid_requests *current;

	DEBUG(DEBUG_INFO, ("recovery master forced ip reallocation\n"));

	/* Only process requests that are currently pending.  More
	 * might come in while the takeover run is in progress and
	 * they will need to be processed later since they might
	 * be in response flag changes.
	 */
	current = rec->reallocate_requests;
	rec->reallocate_requests = NULL;

	if (do_takeover_run(rec, rec->nodemap, false)) {
		ret = ctdb_get_pnn(ctdb);
	} else {
		ret = -1;
	}

	result.dsize = sizeof(int32_t);
	result.dptr  = (uint8_t *)&ret;

	srvid_requests_reply(ctdb, &current, result);
}

/*
 * handler for assigning banning credits
 */
static void banning_handler(uint64_t srvid, TDB_DATA data, void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	uint32_t ban_pnn;

	/* Ignore if we are not recmaster */
	if (rec->ctdb->pnn != rec->recmaster) {
		return;
	}

	if (data.dsize != sizeof(uint32_t)) {
		DEBUG(DEBUG_ERR, (__location__ "invalid data size %zu\n",
				  data.dsize));
		return;
	}

	ban_pnn = *(uint32_t *)data.dptr;

	ctdb_set_culprit_count(rec, ban_pnn, rec->nodemap->num);
}

/*
  handler for recovery master elections
*/
static void election_handler(uint64_t srvid, TDB_DATA data, void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	int ret;
	struct election_message *em = (struct election_message *)data.dptr;

	/* Ignore election packets from ourself */
	if (ctdb->pnn == em->pnn) {
		return;
	}

	/* we got an election packet - update the timeout for the election */
	talloc_free(rec->election_timeout);
	rec->election_timeout = tevent_add_timer(
			ctdb->ev, ctdb,
			fast_start ?
				timeval_current_ofs(0, 500000) :
				timeval_current_ofs(ctdb->tunable.election_timeout, 0),
			ctdb_election_timeout, rec);

	/* someone called an election. check their election data
	   and if we disagree and we would rather be the elected node, 
	   send a new election message to all other nodes
	 */
	if (ctdb_election_win(rec, em)) {
		if (!rec->send_election_te) {
			rec->send_election_te = tevent_add_timer(
					ctdb->ev, rec,
					timeval_current_ofs(0, 500000),
					election_send_request, rec);
		}
		return;
	}

	/* we didn't win */
	TALLOC_FREE(rec->send_election_te);

	/* Release the recovery lock file */
	if (ctdb_recovery_have_lock(ctdb)) {
		ctdb_recovery_unlock(ctdb);
	}

	clear_ip_assignment_tree(ctdb);

	/* ok, let that guy become recmaster then */
	ret = ctdb_ctrl_setrecmaster(ctdb, CONTROL_TIMEOUT(),
				     CTDB_CURRENT_NODE, em->pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " failed to set recmaster"));
		return;
	}
	rec->recmaster = em->pnn;

	return;
}


/*
  force the start of the election process
 */
static void force_election(struct ctdb_recoverd *rec, uint32_t pnn, 
			   struct ctdb_node_map_old *nodemap)
{
	int ret;
	struct ctdb_context *ctdb = rec->ctdb;

	DEBUG(DEBUG_INFO,(__location__ " Force an election\n"));

	/* set all nodes to recovery mode to stop all internode traffic */
	ret = set_recovery_mode(ctdb, rec, nodemap, CTDB_RECOVERY_ACTIVE, false);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to set recovery mode to active on cluster\n"));
		return;
	}

	talloc_free(rec->election_timeout);
	rec->election_timeout = tevent_add_timer(
			ctdb->ev, ctdb,
			fast_start ?
				timeval_current_ofs(0, 500000) :
				timeval_current_ofs(ctdb->tunable.election_timeout, 0),
			ctdb_election_timeout, rec);

	ret = send_election_request(rec, pnn);
	if (ret!=0) {
		DEBUG(DEBUG_ERR, (__location__ " failed to initiate recmaster election"));
		return;
	}

	/* wait for a few seconds to collect all responses */
	ctdb_wait_election(rec);
}



/*
  handler for when a node changes its flags
*/
static void monitor_handler(uint64_t srvid, TDB_DATA data, void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	int ret;
	struct ctdb_node_flag_change *c = (struct ctdb_node_flag_change *)data.dptr;
	struct ctdb_node_map_old *nodemap=NULL;
	TALLOC_CTX *tmp_ctx;
	int i;
	int disabled_flag_changed;

	if (data.dsize != sizeof(*c)) {
		DEBUG(DEBUG_ERR,(__location__ "Invalid data in ctdb_node_flag_change\n"));
		return;
	}

	tmp_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY_VOID(ctdb, tmp_ctx);

	ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, tmp_ctx, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ "ctdb_ctrl_getnodemap failed in monitor_handler\n"));
		talloc_free(tmp_ctx);
		return;		
	}


	for (i=0;i<nodemap->num;i++) {
		if (nodemap->nodes[i].pnn == c->pnn) break;
	}

	if (i == nodemap->num) {
		DEBUG(DEBUG_CRIT,(__location__ "Flag change for non-existant node %u\n", c->pnn));
		talloc_free(tmp_ctx);
		return;
	}

	if (c->old_flags != c->new_flags) {
		DEBUG(DEBUG_NOTICE,("Node %u has changed flags - now 0x%x  was 0x%x\n", c->pnn, c->new_flags, c->old_flags));
	}

	disabled_flag_changed =  (nodemap->nodes[i].flags ^ c->new_flags) & NODE_FLAGS_DISABLED;

	nodemap->nodes[i].flags = c->new_flags;

	ret = ctdb_ctrl_getrecmode(ctdb, tmp_ctx, CONTROL_TIMEOUT(),
				   CTDB_CURRENT_NODE, &ctdb->recovery_mode);

	if (ret == 0 &&
	    rec->recmaster == ctdb->pnn &&
	    ctdb->recovery_mode == CTDB_RECOVERY_NORMAL) {
		/* Only do the takeover run if the perm disabled or unhealthy
		   flags changed since these will cause an ip failover but not
		   a recovery.
		   If the node became disconnected or banned this will also
		   lead to an ip address failover but that is handled 
		   during recovery
		*/
		if (disabled_flag_changed) {
			rec->need_takeover_run = true;
		}
	}

	talloc_free(tmp_ctx);
}

/*
  handler for when we need to push out flag changes ot all other nodes
*/
static void push_flags_handler(uint64_t srvid, TDB_DATA data,
			       void *private_data)
{
	struct ctdb_recoverd *rec = talloc_get_type(
		private_data, struct ctdb_recoverd);
	struct ctdb_context *ctdb = rec->ctdb;
	int ret;
	struct ctdb_node_flag_change *c = (struct ctdb_node_flag_change *)data.dptr;
	struct ctdb_node_map_old *nodemap=NULL;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	uint32_t *nodes;

	/* read the node flags from the recmaster */
	ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), rec->recmaster,
				   tmp_ctx, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to get nodemap from node %u\n", c->pnn));
		talloc_free(tmp_ctx);
		return;
	}
	if (c->pnn >= nodemap->num) {
		DEBUG(DEBUG_ERR,(__location__ " Nodemap from recmaster does not contain node %d\n", c->pnn));
		talloc_free(tmp_ctx);
		return;
	}

	/* send the flags update to all connected nodes */
	nodes = list_of_connected_nodes(ctdb, nodemap, tmp_ctx, true);

	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_MODIFY_FLAGS,
				      nodes, 0, CONTROL_TIMEOUT(),
				      false, data,
				      NULL, NULL,
				      NULL) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " ctdb_control to modify node flags failed\n"));

		talloc_free(tmp_ctx);
		return;
	}

	talloc_free(tmp_ctx);
}


struct verify_recmode_normal_data {
	uint32_t count;
	enum monitor_result status;
};

static void verify_recmode_normal_callback(struct ctdb_client_control_state *state)
{
	struct verify_recmode_normal_data *rmdata = talloc_get_type(state->async.private_data, struct verify_recmode_normal_data);


	/* one more node has responded with recmode data*/
	rmdata->count--;

	/* if we failed to get the recmode, then return an error and let
	   the main loop try again.
	*/
	if (state->state != CTDB_CONTROL_DONE) {
		if (rmdata->status == MONITOR_OK) {
			rmdata->status = MONITOR_FAILED;
		}
		return;
	}

	/* if we got a response, then the recmode will be stored in the
	   status field
	*/
	if (state->status != CTDB_RECOVERY_NORMAL) {
		DEBUG(DEBUG_NOTICE, ("Node:%u was in recovery mode. Start recovery process\n", state->c->hdr.destnode));
		rmdata->status = MONITOR_RECOVERY_NEEDED;
	}

	return;
}


/* verify that all nodes are in normal recovery mode */
static enum monitor_result verify_recmode(struct ctdb_context *ctdb, struct ctdb_node_map_old *nodemap)
{
	struct verify_recmode_normal_data *rmdata;
	TALLOC_CTX *mem_ctx = talloc_new(ctdb);
	struct ctdb_client_control_state *state;
	enum monitor_result status;
	int j;
	
	rmdata = talloc(mem_ctx, struct verify_recmode_normal_data);
	CTDB_NO_MEMORY_FATAL(ctdb, rmdata);
	rmdata->count  = 0;
	rmdata->status = MONITOR_OK;

	/* loop over all active nodes and send an async getrecmode call to 
	   them*/
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		state = ctdb_ctrl_getrecmode_send(ctdb, mem_ctx, 
					CONTROL_TIMEOUT(), 
					nodemap->nodes[j].pnn);
		if (state == NULL) {
			/* we failed to send the control, treat this as 
			   an error and try again next iteration
			*/			
			DEBUG(DEBUG_ERR,("Failed to call ctdb_ctrl_getrecmode_send during monitoring\n"));
			talloc_free(mem_ctx);
			return MONITOR_FAILED;
		}

		/* set up the callback functions */
		state->async.fn = verify_recmode_normal_callback;
		state->async.private_data = rmdata;

		/* one more control to wait for to complete */
		rmdata->count++;
	}


	/* now wait for up to the maximum number of seconds allowed
	   or until all nodes we expect a response from has replied
	*/
	while (rmdata->count > 0) {
		tevent_loop_once(ctdb->ev);
	}

	status = rmdata->status;
	talloc_free(mem_ctx);
	return status;
}


struct verify_recmaster_data {
	struct ctdb_recoverd *rec;
	uint32_t count;
	uint32_t pnn;
	enum monitor_result status;
};

static void verify_recmaster_callback(struct ctdb_client_control_state *state)
{
	struct verify_recmaster_data *rmdata = talloc_get_type(state->async.private_data, struct verify_recmaster_data);


	/* one more node has responded with recmaster data*/
	rmdata->count--;

	/* if we failed to get the recmaster, then return an error and let
	   the main loop try again.
	*/
	if (state->state != CTDB_CONTROL_DONE) {
		if (rmdata->status == MONITOR_OK) {
			rmdata->status = MONITOR_FAILED;
		}
		return;
	}

	/* if we got a response, then the recmaster will be stored in the
	   status field
	*/
	if (state->status != rmdata->pnn) {
		DEBUG(DEBUG_ERR,("Node %d thinks node %d is recmaster. Need a new recmaster election\n", state->c->hdr.destnode, state->status));
		ctdb_set_culprit(rmdata->rec, state->c->hdr.destnode);
		rmdata->status = MONITOR_ELECTION_NEEDED;
	}

	return;
}


/* verify that all nodes agree that we are the recmaster */
static enum monitor_result verify_recmaster(struct ctdb_recoverd *rec, struct ctdb_node_map_old *nodemap, uint32_t pnn)
{
	struct ctdb_context *ctdb = rec->ctdb;
	struct verify_recmaster_data *rmdata;
	TALLOC_CTX *mem_ctx = talloc_new(ctdb);
	struct ctdb_client_control_state *state;
	enum monitor_result status;
	int j;
	
	rmdata = talloc(mem_ctx, struct verify_recmaster_data);
	CTDB_NO_MEMORY_FATAL(ctdb, rmdata);
	rmdata->rec    = rec;
	rmdata->count  = 0;
	rmdata->pnn    = pnn;
	rmdata->status = MONITOR_OK;

	/* loop over all active nodes and send an async getrecmaster call to
	   them*/
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].pnn == rec->recmaster) {
			continue;
		}
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		state = ctdb_ctrl_getrecmaster_send(ctdb, mem_ctx, 
					CONTROL_TIMEOUT(),
					nodemap->nodes[j].pnn);
		if (state == NULL) {
			/* we failed to send the control, treat this as 
			   an error and try again next iteration
			*/			
			DEBUG(DEBUG_ERR,("Failed to call ctdb_ctrl_getrecmaster_send during monitoring\n"));
			talloc_free(mem_ctx);
			return MONITOR_FAILED;
		}

		/* set up the callback functions */
		state->async.fn = verify_recmaster_callback;
		state->async.private_data = rmdata;

		/* one more control to wait for to complete */
		rmdata->count++;
	}


	/* now wait for up to the maximum number of seconds allowed
	   or until all nodes we expect a response from has replied
	*/
	while (rmdata->count > 0) {
		tevent_loop_once(ctdb->ev);
	}

	status = rmdata->status;
	talloc_free(mem_ctx);
	return status;
}

static bool interfaces_have_changed(struct ctdb_context *ctdb,
				    struct ctdb_recoverd *rec)
{
	struct ctdb_iface_list_old *ifaces = NULL;
	TALLOC_CTX *mem_ctx;
	bool ret = false;

	mem_ctx = talloc_new(NULL);

	/* Read the interfaces from the local node */
	if (ctdb_ctrl_get_ifaces(ctdb, CONTROL_TIMEOUT(),
				 CTDB_CURRENT_NODE, mem_ctx, &ifaces) != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get interfaces from local node %u\n", ctdb->pnn));
		/* We could return an error.  However, this will be
		 * rare so we'll decide that the interfaces have
		 * actually changed, just in case.
		 */
		talloc_free(mem_ctx);
		return true;
	}

	if (!rec->ifaces) {
		/* We haven't been here before so things have changed */
		DEBUG(DEBUG_NOTICE, ("Initial interface fetched\n"));
		ret = true;
	} else if (rec->ifaces->num != ifaces->num) {
		/* Number of interfaces has changed */
		DEBUG(DEBUG_NOTICE, ("Interface count changed from %d to %d\n",
				     rec->ifaces->num, ifaces->num));
		ret = true;
	} else {
		/* See if interface names or link states have changed */
		int i;
		for (i = 0; i < rec->ifaces->num; i++) {
			struct ctdb_iface * iface = &rec->ifaces->ifaces[i];
			if (strcmp(iface->name, ifaces->ifaces[i].name) != 0) {
				DEBUG(DEBUG_NOTICE,
				      ("Interface in slot %d changed: %s => %s\n",
				       i, iface->name, ifaces->ifaces[i].name));
				ret = true;
				break;
			}
			if (iface->link_state != ifaces->ifaces[i].link_state) {
				DEBUG(DEBUG_NOTICE,
				      ("Interface %s changed state: %d => %d\n",
				       iface->name, iface->link_state,
				       ifaces->ifaces[i].link_state));
				ret = true;
				break;
			}
		}
	}

	talloc_free(rec->ifaces);
	rec->ifaces = talloc_steal(rec, ifaces);

	talloc_free(mem_ctx);
	return ret;
}

/* called to check that the local allocation of public ip addresses is ok.
*/
static int verify_local_ip_allocation(struct ctdb_context *ctdb, struct ctdb_recoverd *rec, uint32_t pnn, struct ctdb_node_map_old *nodemap)
{
	TALLOC_CTX *mem_ctx = talloc_new(NULL);
	int ret, j;
	bool need_takeover_run = false;

	if (interfaces_have_changed(ctdb, rec)) {
		DEBUG(DEBUG_NOTICE, ("The interfaces status has changed on "
				     "local node %u - force takeover run\n",
				     pnn));
		need_takeover_run = true;
	}

	/* verify that we have the ip addresses we should have
	   and we don't have ones we shouldnt have.
	   if we find an inconsistency we set recmode to
	   active on the local node and wait for the recmaster
	   to do a full blown recovery.
	   also if the pnn is -1 and we are healthy and can host the ip
	   we also request a ip reallocation.
	*/
	if (ctdb->tunable.disable_ip_failover == 0) {
		struct ctdb_public_ip_list_old *ips = NULL;

		/* read the *available* IPs from the local node */
		ret = ctdb_ctrl_get_public_ips_flags(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, mem_ctx, CTDB_PUBLIC_IP_FLAGS_ONLY_AVAILABLE, &ips);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, ("Unable to get available public IPs from local node %u\n", pnn));
			talloc_free(mem_ctx);
			return -1;
		}

		for (j=0; j<ips->num; j++) {
			if (ips->ips[j].pnn == -1 &&
			    nodemap->nodes[pnn].flags == 0) {
				DEBUG(DEBUG_CRIT,("Public IP '%s' is not assigned and we could serve it\n",
						  ctdb_addr_to_str(&ips->ips[j].addr)));
				need_takeover_run = true;
			}
		}

		talloc_free(ips);

		/* read the *known* IPs from the local node */
		ret = ctdb_ctrl_get_public_ips_flags(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, mem_ctx, 0, &ips);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, ("Unable to get known public IPs from local node %u\n", pnn));
			talloc_free(mem_ctx);
			return -1;
		}

		for (j=0; j<ips->num; j++) {
			if (ips->ips[j].pnn == pnn) {
				if (ctdb->do_checkpublicip && !ctdb_sys_have_ip(&ips->ips[j].addr)) {
					DEBUG(DEBUG_CRIT,("Public IP '%s' is assigned to us but not on an interface\n",
						ctdb_addr_to_str(&ips->ips[j].addr)));
					need_takeover_run = true;
				}
			} else {
				if (ctdb->do_checkpublicip &&
				    ctdb_sys_have_ip(&ips->ips[j].addr)) {

					DEBUG(DEBUG_CRIT,("We are still serving a public IP '%s' that we should not be serving. Removing it\n", 
						ctdb_addr_to_str(&ips->ips[j].addr)));

					if (ctdb_ctrl_release_ip(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, &ips->ips[j]) != 0) {
						DEBUG(DEBUG_ERR,("Failed to release local IP address\n"));
					}
				}
			}
		}
	}

	if (need_takeover_run) {
		struct ctdb_srvid_message rd;
		TDB_DATA data;

		DEBUG(DEBUG_CRIT,("Trigger takeoverrun\n"));

		ZERO_STRUCT(rd);
		rd.pnn = ctdb->pnn;
		rd.srvid = 0;
		data.dptr = (uint8_t *)&rd;
		data.dsize = sizeof(rd);

		ret = ctdb_client_send_message(ctdb, rec->recmaster, CTDB_SRVID_TAKEOVER_RUN, data);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to send ipreallocate to recmaster :%d\n", (int)rec->recmaster));
		}
	}
	talloc_free(mem_ctx);
	return 0;
}


static void async_getnodemap_callback(struct ctdb_context *ctdb, uint32_t node_pnn, int32_t res, TDB_DATA outdata, void *callback_data)
{
	struct ctdb_node_map_old **remote_nodemaps = callback_data;

	if (node_pnn >= ctdb->num_nodes) {
		DEBUG(DEBUG_ERR,(__location__ " pnn from invalid node\n"));
		return;
	}

	remote_nodemaps[node_pnn] = (struct ctdb_node_map_old *)talloc_steal(remote_nodemaps, outdata.dptr);

}

static int get_remote_nodemaps(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx,
	struct ctdb_node_map_old *nodemap,
	struct ctdb_node_map_old **remote_nodemaps)
{
	uint32_t *nodes;

	nodes = list_of_active_nodes(ctdb, nodemap, mem_ctx, true);
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_GET_NODEMAP,
					nodes, 0,
					CONTROL_TIMEOUT(), false, tdb_null,
					async_getnodemap_callback,
					NULL,
					remote_nodemaps) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to pull all remote nodemaps\n"));

		return -1;
	}

	return 0;
}

static int update_recovery_lock_file(struct ctdb_context *ctdb)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	const char *reclockfile;

	if (ctdb_ctrl_getreclock(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, tmp_ctx, &reclockfile) != 0) {
		DEBUG(DEBUG_ERR,("Failed to read reclock file from daemon\n"));
		talloc_free(tmp_ctx);
		return -1;	
	}

	if (reclockfile == NULL) {
		if (ctdb->recovery_lock_file != NULL) {
			DEBUG(DEBUG_NOTICE,("Recovery lock file disabled\n"));
			talloc_free(ctdb->recovery_lock_file);
			ctdb->recovery_lock_file = NULL;
			ctdb_recovery_unlock(ctdb);
		}
		talloc_free(tmp_ctx);
		return 0;
	}

	if (ctdb->recovery_lock_file == NULL) {
		DEBUG(DEBUG_NOTICE,
		      ("Recovery lock file enabled (%s)\n", reclockfile));
		ctdb->recovery_lock_file = talloc_strdup(ctdb, reclockfile);
		ctdb_recovery_unlock(ctdb);
		talloc_free(tmp_ctx);
		return 0;
	}


	if (!strcmp(reclockfile, ctdb->recovery_lock_file)) {
		talloc_free(tmp_ctx);
		return 0;
	}

	DEBUG(DEBUG_NOTICE,
	      ("Recovery lock file changed (now %s)\n", reclockfile));
	talloc_free(ctdb->recovery_lock_file);
	ctdb->recovery_lock_file = talloc_strdup(ctdb, reclockfile);
	ctdb_recovery_unlock(ctdb);

	talloc_free(tmp_ctx);
	return 0;
}

static enum monitor_result validate_recovery_master(struct ctdb_recoverd *rec,
						    TALLOC_CTX *mem_ctx)
{
	struct ctdb_context *ctdb = rec->ctdb;
	uint32_t pnn = ctdb_get_pnn(ctdb);
	struct ctdb_node_map_old *nodemap = rec->nodemap;
	struct ctdb_node_map_old *recmaster_nodemap = NULL;
	int ret;

	/* When recovery daemon is started, recmaster is set to
	 * "unknown" so it knows to start an election.
	 */
	if (rec->recmaster == CTDB_UNKNOWN_PNN) {
		DEBUG(DEBUG_NOTICE,
		      ("Initial recovery master set - forcing election\n"));
		return MONITOR_ELECTION_NEEDED;
	}

	/*
	 * If the current recmaster does not have CTDB_CAP_RECMASTER,
	 * but we have, then force an election and try to become the new
	 * recmaster.
	 */
	if (!ctdb_node_has_capabilities(rec->caps,
					rec->recmaster,
					CTDB_CAP_RECMASTER) &&
	    (rec->ctdb->capabilities & CTDB_CAP_RECMASTER) &&
	    !(nodemap->nodes[pnn].flags & NODE_FLAGS_INACTIVE)) {
		DEBUG(DEBUG_ERR,
		      (" Current recmaster node %u does not have CAP_RECMASTER,"
		       " but we (node %u) have - force an election\n",
		       rec->recmaster, pnn));
		return MONITOR_ELECTION_NEEDED;
	}

	/* Verify that the master node has not been deleted.  This
	 * should not happen because a node should always be shutdown
	 * before being deleted, causing a new master to be elected
	 * before now.  However, if something strange has happened
	 * then checking here will ensure we don't index beyond the
	 * end of the nodemap array. */
	if (rec->recmaster >= nodemap->num) {
		DEBUG(DEBUG_ERR,
		      ("Recmaster node %u has been deleted. Force election\n",
		       rec->recmaster));
		return MONITOR_ELECTION_NEEDED;
	}

	/* if recovery master is disconnected/deleted we must elect a new recmaster */
	if (nodemap->nodes[rec->recmaster].flags &
	    (NODE_FLAGS_DISCONNECTED|NODE_FLAGS_DELETED)) {
		DEBUG(DEBUG_NOTICE,
		      ("Recmaster node %u is disconnected/deleted. Force election\n",
		       rec->recmaster));
		return MONITOR_ELECTION_NEEDED;
	}

	/* get nodemap from the recovery master to check if it is inactive */
	ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), rec->recmaster,
				   mem_ctx, &recmaster_nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,
		      (__location__
		       " Unable to get nodemap from recovery master %u\n",
			  rec->recmaster));
		return MONITOR_FAILED;
	}


	if ((recmaster_nodemap->nodes[rec->recmaster].flags & NODE_FLAGS_INACTIVE) &&
	    (rec->node_flags & NODE_FLAGS_INACTIVE) == 0) {
		DEBUG(DEBUG_NOTICE,
		      ("Recmaster node %u is inactive. Force election\n",
		       rec->recmaster));
		/*
		 * update our nodemap to carry the recmaster's notion of
		 * its own flags, so that we don't keep freezing the
		 * inactive recmaster node...
		 */
		nodemap->nodes[rec->recmaster].flags =
			recmaster_nodemap->nodes[rec->recmaster].flags;
		return MONITOR_ELECTION_NEEDED;
	}

	return MONITOR_OK;
}

static void main_loop(struct ctdb_context *ctdb, struct ctdb_recoverd *rec,
		      TALLOC_CTX *mem_ctx)
{
	uint32_t pnn;
	struct ctdb_node_map_old *nodemap=NULL;
	struct ctdb_node_map_old **remote_nodemaps=NULL;
	struct ctdb_vnn_map *vnnmap=NULL;
	struct ctdb_vnn_map *remote_vnnmap=NULL;
	uint32_t num_lmasters;
	int32_t debug_level;
	int i, j, ret;
	bool self_ban;


	/* verify that the main daemon is still running */
	if (ctdb_kill(ctdb, ctdb->ctdbd_pid, 0) != 0) {
		DEBUG(DEBUG_CRIT,("CTDB daemon is no longer available. Shutting down recovery daemon\n"));
		exit(-1);
	}

	/* ping the local daemon to tell it we are alive */
	ctdb_ctrl_recd_ping(ctdb);

	if (rec->election_timeout) {
		/* an election is in progress */
		return;
	}

	/* read the debug level from the parent and update locally */
	ret = ctdb_ctrl_get_debuglevel(ctdb, CTDB_CURRENT_NODE, &debug_level);
	if (ret !=0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to read debuglevel from parent\n"));
		return;
	}
	DEBUGLEVEL = debug_level;

	/* get relevant tunables */
	ret = ctdb_ctrl_get_all_tunables(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, &ctdb->tunable);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to get tunables - retrying\n"));
		return;
	}

	/* get runstate */
	ret = ctdb_ctrl_get_runstate(ctdb, CONTROL_TIMEOUT(),
				     CTDB_CURRENT_NODE, &ctdb->runstate);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Failed to get runstate - retrying\n"));
		return;
	}

	/* get the current recovery lock file from the server */
	if (update_recovery_lock_file(ctdb) != 0) {
		DEBUG(DEBUG_ERR,("Failed to update the recovery lock file\n"));
		return;
	}

	pnn = ctdb_get_pnn(ctdb);

	/* get nodemap */
	TALLOC_FREE(rec->nodemap);
	ret = ctdb_ctrl_getnodemap(ctdb, CONTROL_TIMEOUT(), pnn, rec, &rec->nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to get nodemap from node %u\n", pnn));
		return;
	}
	nodemap = rec->nodemap;

	/* remember our own node flags */
	rec->node_flags = nodemap->nodes[pnn].flags;

	ban_misbehaving_nodes(rec, &self_ban);
	if (self_ban) {
		DEBUG(DEBUG_NOTICE, ("This node was banned, restart main_loop\n"));
		return;
	}

	/* if the local daemon is STOPPED or BANNED, we verify that the databases are
	   also frozen and that the recmode is set to active.
	*/
	if (rec->node_flags & (NODE_FLAGS_STOPPED | NODE_FLAGS_BANNED)) {
		/* If this node has become inactive then we want to
		 * reduce the chances of it taking over the recovery
		 * master role when it becomes active again.  This
		 * helps to stabilise the recovery master role so that
		 * it stays on the most stable node.
		 */
		rec->priority_time = timeval_current();

		ret = ctdb_ctrl_getrecmode(ctdb, mem_ctx, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, &ctdb->recovery_mode);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to read recmode from local node\n"));
		}
		if (ctdb->recovery_mode == CTDB_RECOVERY_NORMAL) {
			DEBUG(DEBUG_ERR,("Node is stopped or banned but recovery mode is not active. Activate recovery mode and lock databases\n"));

			ret = ctdb_ctrl_setrecmode(ctdb, CONTROL_TIMEOUT(), CTDB_CURRENT_NODE, CTDB_RECOVERY_ACTIVE);
			if (ret != 0) {
				DEBUG(DEBUG_ERR,(__location__ " Failed to activate recovery mode in STOPPED or BANNED state\n"));

				return;
			}
		}
		if (! rec->frozen_on_inactive) {
			ret = ctdb_ctrl_freeze(ctdb, CONTROL_TIMEOUT(),
					       CTDB_CURRENT_NODE);
			if (ret != 0) {
				DEBUG(DEBUG_ERR,
				      (__location__ " Failed to freeze node "
				       "in STOPPED or BANNED state\n"));
				return;
			}

			rec->frozen_on_inactive = true;
		}

		/* If this node is stopped or banned then it is not the recovery
		 * master, so don't do anything. This prevents stopped or banned
		 * node from starting election and sending unnecessary controls.
		 */
		return;
	}

	rec->frozen_on_inactive = false;

	/* If we are not the recmaster then do some housekeeping */
	if (rec->recmaster != pnn) {
		/* Ignore any IP reallocate requests - only recmaster
		 * processes them
		 */
		TALLOC_FREE(rec->reallocate_requests);
		/* Clear any nodes that should be force rebalanced in
		 * the next takeover run.  If the recovery master role
		 * has moved then we don't want to process these some
		 * time in the future.
		 */
		TALLOC_FREE(rec->force_rebalance_nodes);
	}

	/* Retrieve capabilities from all connected nodes */
	ret = update_capabilities(rec, nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to update node capabilities.\n"));
		return;
	}

	switch (validate_recovery_master(rec, mem_ctx)) {
	case MONITOR_RECOVERY_NEEDED:
		/* can not happen */
		return;
	case MONITOR_ELECTION_NEEDED:
		force_election(rec, pnn, nodemap);
		return;
	case MONITOR_OK:
		break;
	case MONITOR_FAILED:
		return;
	}

	/* verify that we have all ip addresses we should have and we dont
	 * have addresses we shouldnt have.
	 */ 
	if (ctdb->tunable.disable_ip_failover == 0 &&
	    !ctdb_op_is_disabled(rec->takeover_run)) {
		if (verify_local_ip_allocation(ctdb, rec, pnn, nodemap) != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Public IPs were inconsistent.\n"));
		}
	}


	/* if we are not the recmaster then we do not need to check
	   if recovery is needed
	 */
	if (pnn != rec->recmaster) {
		return;
	}


	/* ensure our local copies of flags are right */
	ret = update_local_flags(rec, nodemap);
	if (ret == MONITOR_ELECTION_NEEDED) {
		DEBUG(DEBUG_NOTICE,("update_local_flags() called for a re-election.\n"));
		force_election(rec, pnn, nodemap);
		return;
	}
	if (ret != MONITOR_OK) {
		DEBUG(DEBUG_ERR,("Unable to update local flags\n"));
		return;
	}

	if (ctdb->num_nodes != nodemap->num) {
		DEBUG(DEBUG_ERR, (__location__ " ctdb->num_nodes (%d) != nodemap->num (%d) reloading nodes file\n", ctdb->num_nodes, nodemap->num));
		ctdb_load_nodes_file(ctdb);
		return;
	}

	/* verify that all active nodes agree that we are the recmaster */
	switch (verify_recmaster(rec, nodemap, pnn)) {
	case MONITOR_RECOVERY_NEEDED:
		/* can not happen */
		return;
	case MONITOR_ELECTION_NEEDED:
		force_election(rec, pnn, nodemap);
		return;
	case MONITOR_OK:
		break;
	case MONITOR_FAILED:
		return;
	}


	/* get the vnnmap */
	ret = ctdb_ctrl_getvnnmap(ctdb, CONTROL_TIMEOUT(), pnn, mem_ctx, &vnnmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to get vnnmap from node %u\n", pnn));
		return;
	}

	if (rec->need_recovery) {
		/* a previous recovery didn't finish */
		do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
		return;
	}

	/* verify that all active nodes are in normal mode 
	   and not in recovery mode 
	*/
	switch (verify_recmode(ctdb, nodemap)) {
	case MONITOR_RECOVERY_NEEDED:
		do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
		return;
	case MONITOR_FAILED:
		return;
	case MONITOR_ELECTION_NEEDED:
		/* can not happen */
	case MONITOR_OK:
		break;
	}


        if (ctdb->recovery_lock_file != NULL) {
		/* We must already hold the recovery lock */
		if (!ctdb_recovery_have_lock(ctdb)) {
			DEBUG(DEBUG_ERR,("Failed recovery lock sanity check.  Force a recovery\n"));
			ctdb_set_culprit(rec, ctdb->pnn);
			do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
			return;
		}
	}


	/* if there are takeovers requested, perform it and notify the waiters */
	if (!ctdb_op_is_disabled(rec->takeover_run) &&
	    rec->reallocate_requests) {
		process_ipreallocate_requests(ctdb, rec);
	}

	/* If recoveries are disabled then there is no use doing any
	 * nodemap or flags checks.  Recoveries might be disabled due
	 * to "reloadnodes", so doing these checks might cause an
	 * unnecessary recovery.  */
	if (ctdb_op_is_disabled(rec->recovery)) {
		return;
	}

	/* get the nodemap for all active remote nodes
	 */
	remote_nodemaps = talloc_array(mem_ctx, struct ctdb_node_map_old *, nodemap->num);
	if (remote_nodemaps == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " failed to allocate remote nodemap array\n"));
		return;
	}
	for(i=0; i<nodemap->num; i++) {
		remote_nodemaps[i] = NULL;
	}
	if (get_remote_nodemaps(ctdb, mem_ctx, nodemap, remote_nodemaps) != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to read remote nodemaps\n"));
		return;
	} 

	/* verify that all other nodes have the same nodemap as we have
	*/
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		if (remote_nodemaps[j] == NULL) {
			DEBUG(DEBUG_ERR,(__location__ " Did not get a remote nodemap for node %d, restarting monitoring\n", j));
			ctdb_set_culprit(rec, j);

			return;
		}

 		/* if the nodes disagree on how many nodes there are
		   then this is a good reason to try recovery
		 */
		if (remote_nodemaps[j]->num != nodemap->num) {
			DEBUG(DEBUG_ERR, (__location__ " Remote node:%u has different node count. %u vs %u of the local node\n",
				  nodemap->nodes[j].pnn, remote_nodemaps[j]->num, nodemap->num));
			ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
			do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
			return;
		}

		/* if the nodes disagree on which nodes exist and are
		   active, then that is also a good reason to do recovery
		 */
		for (i=0;i<nodemap->num;i++) {
			if (remote_nodemaps[j]->nodes[i].pnn != nodemap->nodes[i].pnn) {
				DEBUG(DEBUG_ERR, (__location__ " Remote node:%u has different nodemap pnn for %d (%u vs %u).\n", 
					  nodemap->nodes[j].pnn, i, 
					  remote_nodemaps[j]->nodes[i].pnn, nodemap->nodes[i].pnn));
				ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
				do_recovery(rec, mem_ctx, pnn, nodemap, 
					    vnnmap);
				return;
			}
		}
	}

	/*
	 * Update node flags obtained from each active node. This ensure we have
	 * up-to-date information for all the nodes.
	 */
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		nodemap->nodes[j].flags = remote_nodemaps[j]->nodes[j].flags;
	}

	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}

		/* verify the flags are consistent
		*/
		for (i=0; i<nodemap->num; i++) {
			if (nodemap->nodes[i].flags & NODE_FLAGS_DISCONNECTED) {
				continue;
			}
			
			if (nodemap->nodes[i].flags != remote_nodemaps[j]->nodes[i].flags) {
				DEBUG(DEBUG_ERR, (__location__ " Remote node:%u has different flags for node %u. It has 0x%02x vs our 0x%02x\n", 
				  nodemap->nodes[j].pnn, 
				  nodemap->nodes[i].pnn, 
				  remote_nodemaps[j]->nodes[i].flags,
				  nodemap->nodes[i].flags));
				if (i == j) {
					DEBUG(DEBUG_ERR,("Use flags 0x%02x from remote node %d for cluster update of its own flags\n", remote_nodemaps[j]->nodes[i].flags, j));
					update_flags_on_all_nodes(ctdb, nodemap, nodemap->nodes[i].pnn, remote_nodemaps[j]->nodes[i].flags);
					ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
					do_recovery(rec, mem_ctx, pnn, nodemap, 
						    vnnmap);
					return;
				} else {
					DEBUG(DEBUG_ERR,("Use flags 0x%02x from local recmaster node for cluster update of node %d flags\n", nodemap->nodes[i].flags, i));
					update_flags_on_all_nodes(ctdb, nodemap, nodemap->nodes[i].pnn, nodemap->nodes[i].flags);
					ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
					do_recovery(rec, mem_ctx, pnn, nodemap, 
						    vnnmap);
					return;
				}
			}
		}
	}


	/* count how many active nodes there are */
	num_lmasters  = 0;
	for (i=0; i<nodemap->num; i++) {
		if (!(nodemap->nodes[i].flags & NODE_FLAGS_INACTIVE)) {
			if (ctdb_node_has_capabilities(rec->caps,
						       ctdb->nodes[i]->pnn,
						       CTDB_CAP_LMASTER)) {
				num_lmasters++;
			}
		}
	}


	/* There must be the same number of lmasters in the vnn map as
	 * there are active nodes with the lmaster capability...  or
	 * do a recovery.
	 */
	if (vnnmap->size != num_lmasters) {
		DEBUG(DEBUG_ERR, (__location__ " The vnnmap count is different from the number of active lmaster nodes: %u vs %u\n",
			  vnnmap->size, num_lmasters));
		ctdb_set_culprit(rec, ctdb->pnn);
		do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
		return;
	}

	/* verify that all active nodes in the nodemap also exist in 
	   the vnnmap.
	 */
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		if (nodemap->nodes[j].pnn == pnn) {
			continue;
		}

		for (i=0; i<vnnmap->size; i++) {
			if (vnnmap->map[i] == nodemap->nodes[j].pnn) {
				break;
			}
		}
		if (i == vnnmap->size) {
			DEBUG(DEBUG_ERR, (__location__ " Node %u is active in the nodemap but did not exist in the vnnmap\n", 
				  nodemap->nodes[j].pnn));
			ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
			do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
			return;
		}
	}

	
	/* verify that all other nodes have the same vnnmap
	   and are from the same generation
	 */
	for (j=0; j<nodemap->num; j++) {
		if (nodemap->nodes[j].flags & NODE_FLAGS_INACTIVE) {
			continue;
		}
		if (nodemap->nodes[j].pnn == pnn) {
			continue;
		}

		ret = ctdb_ctrl_getvnnmap(ctdb, CONTROL_TIMEOUT(), nodemap->nodes[j].pnn, 
					  mem_ctx, &remote_vnnmap);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, (__location__ " Unable to get vnnmap from remote node %u\n", 
				  nodemap->nodes[j].pnn));
			return;
		}

		/* verify the vnnmap generation is the same */
		if (vnnmap->generation != remote_vnnmap->generation) {
			DEBUG(DEBUG_ERR, (__location__ " Remote node %u has different generation of vnnmap. %u vs %u (ours)\n", 
				  nodemap->nodes[j].pnn, remote_vnnmap->generation, vnnmap->generation));
			ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
			do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
			return;
		}

		/* verify the vnnmap size is the same */
		if (vnnmap->size != remote_vnnmap->size) {
			DEBUG(DEBUG_ERR, (__location__ " Remote node %u has different size of vnnmap. %u vs %u (ours)\n", 
				  nodemap->nodes[j].pnn, remote_vnnmap->size, vnnmap->size));
			ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
			do_recovery(rec, mem_ctx, pnn, nodemap, vnnmap);
			return;
		}

		/* verify the vnnmap is the same */
		for (i=0;i<vnnmap->size;i++) {
			if (remote_vnnmap->map[i] != vnnmap->map[i]) {
				DEBUG(DEBUG_ERR, (__location__ " Remote node %u has different vnnmap.\n", 
					  nodemap->nodes[j].pnn));
				ctdb_set_culprit(rec, nodemap->nodes[j].pnn);
				do_recovery(rec, mem_ctx, pnn, nodemap, 
					    vnnmap);
				return;
			}
		}
	}

	/* we might need to change who has what IP assigned */
	if (rec->need_takeover_run) {
		/* If takeover run fails, then the offending nodes are
		 * assigned ban culprit counts. And we re-try takeover.
		 * If takeover run fails repeatedly, the node would get
		 * banned.
		 */
		do_takeover_run(rec, nodemap, true);
	}
}

/*
  the main monitoring loop
 */
static void monitor_cluster(struct ctdb_context *ctdb)
{
	struct ctdb_recoverd *rec;

	DEBUG(DEBUG_NOTICE,("monitor_cluster starting\n"));

	rec = talloc_zero(ctdb, struct ctdb_recoverd);
	CTDB_NO_MEMORY_FATAL(ctdb, rec);

	rec->ctdb = ctdb;
	rec->recmaster = CTDB_UNKNOWN_PNN;

	rec->takeover_run = ctdb_op_init(rec, "takeover runs");
	CTDB_NO_MEMORY_FATAL(ctdb, rec->takeover_run);

	rec->recovery = ctdb_op_init(rec, "recoveries");
	CTDB_NO_MEMORY_FATAL(ctdb, rec->recovery);

	rec->priority_time = timeval_current();
	rec->frozen_on_inactive = false;

	/* register a message port for sending memory dumps */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_MEM_DUMP, mem_dump_handler, rec);

	/* when a node is assigned banning credits */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_BANNING,
					banning_handler, rec);

	/* register a message port for recovery elections */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_ELECTION, election_handler, rec);

	/* when nodes are disabled/enabled */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_SET_NODE_FLAGS, monitor_handler, rec);

	/* when we are asked to puch out a flag change */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_PUSH_NODE_FLAGS, push_flags_handler, rec);

	/* register a message port for vacuum fetch */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_VACUUM_FETCH, vacuum_fetch_handler, rec);

	/* register a message port for reloadnodes  */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_RELOAD_NODES, reload_nodes_handler, rec);

	/* register a message port for performing a takeover run */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_TAKEOVER_RUN, ip_reallocate_handler, rec);

	/* register a message port for disabling the ip check for a short while */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_DISABLE_IP_CHECK, disable_ip_check_handler, rec);

	/* register a message port for updating the recovery daemons node assignment for an ip */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_RECD_UPDATE_IP, recd_update_ip_handler, rec);

	/* register a message port for forcing a rebalance of a node next
	   reallocation */
	ctdb_client_set_message_handler(ctdb, CTDB_SRVID_REBALANCE_NODE, recd_node_rebalance_handler, rec);

	/* Register a message port for disabling takeover runs */
	ctdb_client_set_message_handler(ctdb,
					CTDB_SRVID_DISABLE_TAKEOVER_RUNS,
					disable_takeover_runs_handler, rec);

	/* Register a message port for disabling recoveries */
	ctdb_client_set_message_handler(ctdb,
					CTDB_SRVID_DISABLE_RECOVERIES,
					disable_recoveries_handler, rec);

	/* register a message port for detaching database */
	ctdb_client_set_message_handler(ctdb,
					CTDB_SRVID_DETACH_DATABASE,
					detach_database_handler, rec);

	for (;;) {
		TALLOC_CTX *mem_ctx = talloc_new(ctdb);
		struct timeval start;
		double elapsed;

		if (!mem_ctx) {
			DEBUG(DEBUG_CRIT,(__location__
					  " Failed to create temp context\n"));
			exit(-1);
		}

		start = timeval_current();
		main_loop(ctdb, rec, mem_ctx);
		talloc_free(mem_ctx);

		/* we only check for recovery once every second */
		elapsed = timeval_elapsed(&start);
		if (elapsed < ctdb->tunable.recover_interval) {
			ctdb_wait_timeout(ctdb, ctdb->tunable.recover_interval
					  - elapsed);
		}
	}
}

/*
  event handler for when the main ctdbd dies
 */
static void ctdb_recoverd_parent(struct tevent_context *ev,
				 struct tevent_fd *fde,
				 uint16_t flags, void *private_data)
{
	DEBUG(DEBUG_ALERT,("recovery daemon parent died - exiting\n"));
	_exit(1);
}

/*
  called regularly to verify that the recovery daemon is still running
 */
static void ctdb_check_recd(struct tevent_context *ev,
			    struct tevent_timer *te,
			    struct timeval yt, void *p)
{
	struct ctdb_context *ctdb = talloc_get_type(p, struct ctdb_context);

	if (ctdb_kill(ctdb, ctdb->recoverd_pid, 0) != 0) {
		DEBUG(DEBUG_ERR,("Recovery daemon (pid:%d) is no longer running. Trying to restart recovery daemon.\n", (int)ctdb->recoverd_pid));

		tevent_add_timer(ctdb->ev, ctdb, timeval_zero(),
				 ctdb_restart_recd, ctdb);

		return;
	}

	tevent_add_timer(ctdb->ev, ctdb->recd_ctx,
			 timeval_current_ofs(30, 0),
			 ctdb_check_recd, ctdb);
}

static void recd_sig_child_handler(struct tevent_context *ev,
				   struct tevent_signal *se, int signum,
				   int count, void *dont_care,
				   void *private_data)
{
//	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	int status;
	pid_t pid = -1;

	while (pid != 0) {
		pid = waitpid(-1, &status, WNOHANG);
		if (pid == -1) {
			if (errno != ECHILD) {
				DEBUG(DEBUG_ERR, (__location__ " waitpid() returned error. errno:%s(%d)\n", strerror(errno),errno));
			}
			return;
		}
		if (pid > 0) {
			DEBUG(DEBUG_DEBUG, ("RECD SIGCHLD from %d\n", (int)pid));
		}
	}
}

/*
  startup the recovery daemon as a child of the main ctdb daemon
 */
int ctdb_start_recoverd(struct ctdb_context *ctdb)
{
	int fd[2];
	struct tevent_signal *se;
	struct tevent_fd *fde;

	if (pipe(fd) != 0) {
		return -1;
	}

	ctdb->recoverd_pid = ctdb_fork(ctdb);
	if (ctdb->recoverd_pid == -1) {
		return -1;
	}

	if (ctdb->recoverd_pid != 0) {
		talloc_free(ctdb->recd_ctx);
		ctdb->recd_ctx = talloc_new(ctdb);
		CTDB_NO_MEMORY(ctdb, ctdb->recd_ctx);

		close(fd[0]);
		tevent_add_timer(ctdb->ev, ctdb->recd_ctx,
				 timeval_current_ofs(30, 0),
				 ctdb_check_recd, ctdb);
		return 0;
	}

	close(fd[1]);

	srandom(getpid() ^ time(NULL));

	prctl_set_comment("ctdb_recovered");
	if (switch_from_server_to_client(ctdb, "recoverd") != 0) {
		DEBUG(DEBUG_CRIT, (__location__ "ERROR: failed to switch recovery daemon into client mode. shutting down.\n"));
		exit(1);
	}

	DEBUG(DEBUG_DEBUG, (__location__ " Created PIPE FD:%d to recovery daemon\n", fd[0]));

	fde = tevent_add_fd(ctdb->ev, ctdb, fd[0], TEVENT_FD_READ,
			    ctdb_recoverd_parent, &fd[0]);
	tevent_fd_set_auto_close(fde);

	/* set up a handler to pick up sigchld */
	se = tevent_add_signal(ctdb->ev, ctdb, SIGCHLD, 0,
			       recd_sig_child_handler, ctdb);
	if (se == NULL) {
		DEBUG(DEBUG_CRIT,("Failed to set up signal handler for SIGCHLD in recovery daemon\n"));
		exit(1);
	}

	monitor_cluster(ctdb);

	DEBUG(DEBUG_ALERT,("ERROR: ctdb_recoverd finished!?\n"));
	return -1;
}

/*
  shutdown the recovery daemon
 */
void ctdb_stop_recoverd(struct ctdb_context *ctdb)
{
	if (ctdb->recoverd_pid == 0) {
		return;
	}

	DEBUG(DEBUG_NOTICE,("Shutting down recovery daemon\n"));
	ctdb_kill(ctdb, ctdb->recoverd_pid, SIGTERM);

	TALLOC_FREE(ctdb->recd_ctx);
	TALLOC_FREE(ctdb->recd_ping_count);
}

static void ctdb_restart_recd(struct tevent_context *ev,
			      struct tevent_timer *te,
			      struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);

	DEBUG(DEBUG_ERR,("Restarting recovery daemon\n"));
	ctdb_stop_recoverd(ctdb);
	ctdb_start_recoverd(ctdb);
}
