/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
 *
 * Matt Jordan <mjordan@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Stasis Messages and Data Types for Channel Objects
 *
 * \author \verbatim Matt Jordan <mjordan@digium.com> \endverbatim
 *
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/astobj2.h"
#include "asterisk/json.h"
#include "asterisk/pbx.h"
#include "asterisk/bridge.h"
#include "asterisk/translate.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_cache_pattern.h"
#include "asterisk/stasis_channels.h"

/*** DOCUMENTATION
	<managerEvent language="en_US" name="VarSet">
		<managerEventInstance class="EVENT_FLAG_DIALPLAN">
			<synopsis>Raised when a variable is set to a particular value.</synopsis>
			<syntax>
				<channel_snapshot/>
				<parameter name="Variable">
					<para>The variable being set.</para>
				</parameter>
				<parameter name="Value">
					<para>The new value of the variable.</para>
				</parameter>
			</syntax>
		</managerEventInstance>
	</managerEvent>
	<managerEvent language="en_US" name="AgentLogin">
		<managerEventInstance class="EVENT_FLAG_AGENT">
			<synopsis>Raised when an Agent has logged in.</synopsis>
			<syntax>
				<channel_snapshot/>
				<parameter name="Agent">
					<para>Agent ID of the agent.</para>
				</parameter>
			</syntax>
			<see-also>
				<ref type="application">AgentLogin</ref>
				<ref type="managerEvent">AgentLogoff</ref>
			</see-also>
		</managerEventInstance>
	</managerEvent>
	<managerEvent language="en_US" name="AgentLogoff">
		<managerEventInstance class="EVENT_FLAG_AGENT">
			<synopsis>Raised when an Agent has logged off.</synopsis>
			<syntax>
				<xi:include xpointer="xpointer(/docs/managerEvent[@name='AgentLogin']/managerEventInstance/syntax/parameter)" />
				<parameter name="Logintime">
					<para>The number of seconds the agent was logged in.</para>
				</parameter>
			</syntax>
			<see-also>
				<ref type="managerEvent">AgentLogin</ref>
			</see-also>
		</managerEventInstance>
	</managerEvent>
***/

#define NUM_MULTI_CHANNEL_BLOB_BUCKETS 7

static struct stasis_cp_all *channel_cache_all;
static struct stasis_cache *channel_cache_by_name;
static struct stasis_caching_topic *channel_by_name_topic;

struct stasis_cp_all *ast_channel_cache_all(void)
{
	return channel_cache_all;
}

struct stasis_cache *ast_channel_cache(void)
{
	return stasis_cp_all_cache(channel_cache_all);
}

struct stasis_topic *ast_channel_topic_all(void)
{
	return stasis_cp_all_topic(channel_cache_all);
}

struct stasis_topic *ast_channel_topic_all_cached(void)
{
	return stasis_cp_all_topic_cached(channel_cache_all);
}

struct stasis_cache *ast_channel_cache_by_name(void)
{
	return channel_cache_by_name;
}

static const char *channel_snapshot_get_id(struct stasis_message *message)
{
	struct ast_channel_snapshot *snapshot;
	if (ast_channel_snapshot_type() != stasis_message_type(message)) {
		return NULL;
	}
	snapshot = stasis_message_data(message);
	return snapshot->uniqueid;
}

static const char *channel_snapshot_get_name(struct stasis_message *message)
{
	struct ast_channel_snapshot *snapshot;
	if (ast_channel_snapshot_type() != stasis_message_type(message)) {
		return NULL;
	}
	snapshot = stasis_message_data(message);
	return snapshot->name;
}

/*!
 * \internal
 * \brief Hash function for \ref ast_channel_snapshot objects
 */
static int channel_snapshot_hash_cb(const void *obj, const int flags)
{
	const struct ast_channel_snapshot *snapshot = obj;
	const char *name = (flags & OBJ_KEY) ? obj : snapshot->name;
	return ast_str_case_hash(name);
}

/*!
 * \internal
 * \brief Comparison function for \ref ast_channel_snapshot objects
 */
static int channel_snapshot_cmp_cb(void *obj, void *arg, int flags)
{
	struct ast_channel_snapshot *left = obj;
	struct ast_channel_snapshot *right = arg;
	const char *match = (flags & OBJ_KEY) ? arg : right->name;
	return strcasecmp(left->name, match) ? 0 : (CMP_MATCH | CMP_STOP);
}

static void channel_snapshot_dtor(void *obj)
{
	struct ast_channel_snapshot *snapshot = obj;

	ast_string_field_free_memory(snapshot);
	ao2_cleanup(snapshot->manager_vars);
	ao2_cleanup(snapshot->channel_vars);
}

struct ast_channel_snapshot *ast_channel_snapshot_create(struct ast_channel *chan)
{
	struct ast_channel_snapshot *snapshot;
	struct ast_bridge *bridge;
	char nativeformats[256];
	struct ast_str *write_transpath = ast_str_alloca(256);
	struct ast_str *read_transpath = ast_str_alloca(256);
	struct ast_party_id effective_connected_id;
	struct ast_callid *callid;

	/* no snapshots for dummy channels */
	if (!ast_channel_tech(chan)) {
		return NULL;
	}

	snapshot = ao2_alloc(sizeof(*snapshot), channel_snapshot_dtor);
	if (!snapshot || ast_string_field_init(snapshot, 1024)) {
		ao2_cleanup(snapshot);
		return NULL;
	}

	ast_string_field_set(snapshot, name, ast_channel_name(chan));
	ast_string_field_set(snapshot, type, ast_channel_tech(chan)->type);
	ast_string_field_set(snapshot, accountcode, ast_channel_accountcode(chan));
	ast_string_field_set(snapshot, peeraccount, ast_channel_peeraccount(chan));
	ast_string_field_set(snapshot, userfield, ast_channel_userfield(chan));
	ast_string_field_set(snapshot, uniqueid, ast_channel_uniqueid(chan));
	ast_string_field_set(snapshot, linkedid, ast_channel_linkedid(chan));
	ast_string_field_set(snapshot, hangupsource, ast_channel_hangupsource(chan));
	if (ast_channel_appl(chan)) {
		ast_string_field_set(snapshot, appl, ast_channel_appl(chan));
	}
	if (ast_channel_data(chan)) {
		ast_string_field_set(snapshot, data, ast_channel_data(chan));
	}
	ast_string_field_set(snapshot, context, ast_channel_context(chan));
	ast_string_field_set(snapshot, exten, ast_channel_exten(chan));

	ast_string_field_set(snapshot, caller_name,
		S_COR(ast_channel_caller(chan)->id.name.valid, ast_channel_caller(chan)->id.name.str, ""));
	ast_string_field_set(snapshot, caller_number,
		S_COR(ast_channel_caller(chan)->id.number.valid, ast_channel_caller(chan)->id.number.str, ""));
	ast_string_field_set(snapshot, caller_dnid, S_OR(ast_channel_dialed(chan)->number.str, ""));
	ast_string_field_set(snapshot, caller_subaddr,
		S_COR(ast_channel_caller(chan)->id.subaddress.valid, ast_channel_caller(chan)->id.subaddress.str, ""));
	ast_string_field_set(snapshot, dialed_subaddr,
		S_COR(ast_channel_dialed(chan)->subaddress.valid, ast_channel_dialed(chan)->subaddress.str, ""));
	ast_string_field_set(snapshot, caller_ani,
		S_COR(ast_channel_caller(chan)->ani.number.valid, ast_channel_caller(chan)->ani.number.str, ""));
	ast_string_field_set(snapshot, caller_rdnis,
		S_COR(ast_channel_redirecting(chan)->from.number.valid, ast_channel_redirecting(chan)->from.number.str, ""));
	ast_string_field_set(snapshot, caller_dnid,
		S_OR(ast_channel_dialed(chan)->number.str, ""));

	ast_string_field_set(snapshot, connected_name,
		S_COR(ast_channel_connected(chan)->id.name.valid, ast_channel_connected(chan)->id.name.str, ""));
	ast_string_field_set(snapshot, connected_number,
		S_COR(ast_channel_connected(chan)->id.number.valid, ast_channel_connected(chan)->id.number.str, ""));
	ast_string_field_set(snapshot, language, ast_channel_language(chan));

	if ((bridge = ast_channel_get_bridge(chan))) {
		ast_string_field_set(snapshot, bridgeid, bridge->uniqueid);
		ao2_cleanup(bridge);
	}

	ast_string_field_set(snapshot, nativeformats, ast_getformatname_multiple(nativeformats, sizeof(nativeformats),
		ast_channel_nativeformats(chan)));
	ast_string_field_set(snapshot, readformat, ast_getformatname(ast_channel_readformat(chan)));
	ast_string_field_set(snapshot, writeformat, ast_getformatname(ast_channel_writeformat(chan)));
	ast_string_field_set(snapshot, writetrans, ast_translate_path_to_str(ast_channel_writetrans(chan), &write_transpath));
	ast_string_field_set(snapshot, readtrans, ast_translate_path_to_str(ast_channel_readtrans(chan), &read_transpath));

	effective_connected_id = ast_channel_connected_effective_id(chan);
	ast_string_field_set(snapshot, effective_name,
		S_COR(effective_connected_id.name.valid, effective_connected_id.name.str, ""));
	ast_string_field_set(snapshot, effective_number,
		S_COR(effective_connected_id.number.valid, effective_connected_id.number.str, ""));

	if ((callid = ast_channel_callid(chan))) {
		ast_callid_strnprint(snapshot->callid, sizeof(snapshot->callid), callid);
		ast_callid_unref(callid);
	}

	snapshot->creationtime = ast_channel_creationtime(chan);
	snapshot->hanguptime = *(ast_channel_whentohangup(chan));
	snapshot->state = ast_channel_state(chan);
	snapshot->priority = ast_channel_priority(chan);
	snapshot->amaflags = ast_channel_amaflags(chan);
	snapshot->hangupcause = ast_channel_hangupcause(chan);
	ast_copy_flags(&snapshot->flags, ast_channel_flags(chan), 0xFFFFFFFF);
	snapshot->caller_pres = ast_party_id_presentation(&ast_channel_caller(chan)->id);
	snapshot->callgroup = ast_channel_callgroup(chan);
	snapshot->pickupgroup = ast_channel_pickupgroup(chan);
	ast_set_flag(&snapshot->softhangup_flags, ast_channel_softhangup_internal_flag(chan));

	snapshot->manager_vars = ast_channel_get_manager_vars(chan);
	snapshot->channel_vars = ast_channel_get_vars(chan);
	snapshot->tech_properties = ast_channel_tech(chan)->properties;

	return snapshot;
}

static void publish_message_for_channel_topics(struct stasis_message *message, struct ast_channel *chan)
{
	if (chan) {
		stasis_publish(ast_channel_topic(chan), message);
	} else {
		stasis_publish(ast_channel_topic_all(), message);
	}
}

static void channel_blob_dtor(void *obj)
{
	struct ast_channel_blob *event = obj;
	ao2_cleanup(event->snapshot);
	ast_json_unref(event->blob);
}

/*! \brief Dummy callback for receiving events */
static void dummy_event_cb(void *data, struct stasis_subscription *sub, struct stasis_message *message)
{
}

void ast_channel_publish_dial_forward(struct ast_channel *caller, struct ast_channel *peer,
	struct ast_channel *forwarded, const char *dialstring, const char *dialstatus,
	const char *forward)
{
	RAII_VAR(struct ast_multi_channel_blob *, payload, NULL, ao2_cleanup);
	RAII_VAR(struct stasis_message *, msg, NULL, ao2_cleanup);
	RAII_VAR(struct ast_json *, blob, NULL, ast_json_unref);
	RAII_VAR(struct ast_channel_snapshot *, caller_snapshot, NULL, ao2_cleanup);
	RAII_VAR(struct ast_channel_snapshot *, peer_snapshot, NULL, ao2_cleanup);
	RAII_VAR(struct ast_channel_snapshot *, forwarded_snapshot, NULL, ao2_cleanup);

	ast_assert(peer != NULL);
	blob = ast_json_pack("{s: s, s: s, s: s}",
			     "dialstatus", S_OR(dialstatus, ""),
			     "forward", S_OR(forward, ""),
			     "dialstring", S_OR(dialstring, ""));
	if (!blob) {
		return;
	}
	payload = ast_multi_channel_blob_create(blob);
	if (!payload) {
		return;
	}

	if (caller) {
		ast_channel_lock(caller);
		caller_snapshot = ast_channel_snapshot_create(caller);
		ast_channel_unlock(caller);
		if (!caller_snapshot) {
			return;
		}
		ast_multi_channel_blob_add_channel(payload, "caller", caller_snapshot);
	}

	ast_channel_lock(peer);
	peer_snapshot = ast_channel_snapshot_create(peer);
	ast_channel_unlock(peer);
	if (!peer_snapshot) {
		return;
	}
	ast_multi_channel_blob_add_channel(payload, "peer", peer_snapshot);

	if (forwarded) {
		ast_channel_lock(forwarded);
		forwarded_snapshot = ast_channel_snapshot_create(forwarded);
		ast_channel_unlock(forwarded);
		if (!forwarded_snapshot) {
			return;
		}
		ast_multi_channel_blob_add_channel(payload, "forwarded", forwarded_snapshot);
	}

	msg = stasis_message_create(ast_channel_dial_type(), payload);
	if (!msg) {
		return;
	}

	if (forwarded) {
		struct stasis_subscription *subscription = stasis_subscribe(ast_channel_topic(peer), dummy_event_cb, NULL);

		stasis_publish(ast_channel_topic(peer), msg);
		stasis_unsubscribe_and_join(subscription);
	} else {
		publish_message_for_channel_topics(msg, caller);
	}
}

void ast_channel_publish_dial(struct ast_channel *caller, struct ast_channel *peer,
	const char *dialstring, const char *dialstatus)
{
	ast_channel_publish_dial_forward(caller, peer, NULL, dialstring, dialstatus, NULL);
}

static struct stasis_message *create_channel_blob_message(struct ast_channel_snapshot *snapshot,
		struct stasis_message_type *type,
		struct ast_json *blob)
{
	struct stasis_message *msg;
	struct ast_channel_blob *obj;

	obj = ao2_alloc(sizeof(*obj), channel_blob_dtor);
	if (!obj) {
		return NULL;
	}

	if (snapshot) {
		obj->snapshot = snapshot;
		ao2_ref(obj->snapshot, +1);
	}
	if (!blob) {
		blob = ast_json_null();
	}
	obj->blob = ast_json_ref(blob);

	msg = stasis_message_create(type, obj);
	ao2_cleanup(obj);
	return msg;
}

struct stasis_message *ast_channel_blob_create_from_cache(const char *channel_id,
					       struct stasis_message_type *type,
					       struct ast_json *blob)
{
	RAII_VAR(struct ast_channel_snapshot *, snapshot,
			ast_channel_snapshot_get_latest(channel_id),
			ao2_cleanup);

	return create_channel_blob_message(snapshot, type, blob);
}

struct stasis_message *ast_channel_blob_create(struct ast_channel *chan,
	struct stasis_message_type *type, struct ast_json *blob)
{
	RAII_VAR(struct ast_channel_snapshot *, snapshot, NULL, ao2_cleanup);

	if (chan) {
		snapshot = ast_channel_snapshot_create(chan);
	}

	return create_channel_blob_message(snapshot, type, blob);
}

/*! \brief A channel snapshot wrapper object used in \ref ast_multi_channel_blob objects */
struct channel_role_snapshot {
	struct ast_channel_snapshot *snapshot;	/*!< A channel snapshot */
	char role[0];							/*!< The role assigned to the channel */
};

/*! \brief A multi channel blob data structure for multi_channel_blob stasis messages */
struct ast_multi_channel_blob {
	struct ao2_container *channel_snapshots;	/*!< A container holding the snapshots */
	struct ast_json *blob;						/*< A blob of JSON data */
};

/*!
 * \internal
 * \brief Standard comparison function for \ref channel_role_snapshot objects
 */
static int channel_role_single_cmp_cb(void *obj, void *arg, int flags)
{
	struct channel_role_snapshot *left = obj;
	struct channel_role_snapshot *right = arg;
	const char *match = (flags & OBJ_KEY) ? arg : right->role;
	return strcasecmp(left->role, match) ? 0 : (CMP_MATCH | CMP_STOP);
}

/*!
 * \internal
 * \brief Multi comparison function for \ref channel_role_snapshot objects
 */
static int channel_role_multi_cmp_cb(void *obj, void *arg, int flags)
{
	struct channel_role_snapshot *left = obj;
	struct channel_role_snapshot *right = arg;
	const char *match = (flags & OBJ_KEY) ? arg : right->role;
	return strcasecmp(left->role, match) ? 0 : (CMP_MATCH);
}

/*!
 * \internal
 * \brief Hash function for \ref channel_role_snapshot objects
 */
static int channel_role_hash_cb(const void *obj, const int flags)
{
	const struct channel_role_snapshot *snapshot = obj;
	const char *name = (flags & OBJ_KEY) ? obj : snapshot->role;
	return ast_str_case_hash(name);
}

/*!
 * \internal
 * \brief Destructor for \ref ast_multi_channel_blob objects
 */
static void multi_channel_blob_dtor(void *obj)
{
	struct ast_multi_channel_blob *multi_blob = obj;

	ao2_cleanup(multi_blob->channel_snapshots);
	ast_json_unref(multi_blob->blob);
}

struct ast_multi_channel_blob *ast_multi_channel_blob_create(struct ast_json *blob)
{
	RAII_VAR(struct ast_multi_channel_blob *, obj,
			ao2_alloc(sizeof(*obj), multi_channel_blob_dtor),
			ao2_cleanup);

	ast_assert(blob != NULL);

	if (!obj) {
		return NULL;
	}

	obj->channel_snapshots = ao2_container_alloc(NUM_MULTI_CHANNEL_BLOB_BUCKETS,
			channel_role_hash_cb, channel_role_single_cmp_cb);
	if (!obj->channel_snapshots) {
		return NULL;
	}

	obj->blob = ast_json_ref(blob);

	ao2_ref(obj, +1);
	return obj;
}

struct ast_channel_snapshot *ast_channel_snapshot_get_latest(const char *uniqueid)
{
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);
	struct ast_channel_snapshot *snapshot;

	ast_assert(!ast_strlen_zero(uniqueid));

	message = stasis_cache_get(ast_channel_cache(),
			ast_channel_snapshot_type(),
			uniqueid);
	if (!message) {
		return NULL;
	}

	snapshot = stasis_message_data(message);
	if (!snapshot) {
		return NULL;
	}
	ao2_ref(snapshot, +1);
	return snapshot;
}

struct ast_channel_snapshot *ast_channel_snapshot_get_latest_by_name(const char *name)
{
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);
	struct ast_channel_snapshot *snapshot;

	ast_assert(!ast_strlen_zero(name));

	message = stasis_cache_get(ast_channel_cache_by_name(),
			ast_channel_snapshot_type(),
			name);
	if (!message) {
		return NULL;
	}

	snapshot = stasis_message_data(message);
	if (!snapshot) {
		return NULL;
	}
	ao2_ref(snapshot, +1);
	return snapshot;
}

static void channel_role_snapshot_dtor(void *obj)
{
	struct channel_role_snapshot *role_snapshot = obj;
	ao2_cleanup(role_snapshot->snapshot);
}

void ast_multi_channel_blob_add_channel(struct ast_multi_channel_blob *obj, const char *role, struct ast_channel_snapshot *snapshot)
{
	RAII_VAR(struct channel_role_snapshot *, role_snapshot, NULL, ao2_cleanup);
	int role_len = strlen(role) + 1;

	if (!obj || ast_strlen_zero(role) || !snapshot) {
		return;
	}

	role_snapshot = ao2_alloc(sizeof(*role_snapshot) + role_len, channel_role_snapshot_dtor);
	if (!role_snapshot) {
		return;
	}
	ast_copy_string(role_snapshot->role, role, role_len);
	role_snapshot->snapshot = snapshot;
	ao2_ref(role_snapshot->snapshot, +1);
	ao2_link(obj->channel_snapshots, role_snapshot);
}

struct ast_channel_snapshot *ast_multi_channel_blob_get_channel(struct ast_multi_channel_blob *obj, const char *role)
{
	struct channel_role_snapshot *role_snapshot;

	if (!obj || ast_strlen_zero(role)) {
		return NULL;
	}
	role_snapshot = ao2_find(obj->channel_snapshots, role, OBJ_KEY);
	/* Note that this function does not increase the ref count on snapshot */
	if (!role_snapshot) {
		return NULL;
	}
	ao2_ref(role_snapshot, -1);
	return role_snapshot->snapshot;
}

struct ao2_container *ast_multi_channel_blob_get_channels(struct ast_multi_channel_blob *obj, const char *role)
{
	RAII_VAR(struct ao2_container *, ret_container,
		ao2_container_alloc(NUM_MULTI_CHANNEL_BLOB_BUCKETS, channel_snapshot_hash_cb, channel_snapshot_cmp_cb),
		ao2_cleanup);
	struct ao2_iterator *it_role_snapshots;
	struct channel_role_snapshot *role_snapshot;
	char *arg;

	if (!obj || ast_strlen_zero(role) || !ret_container) {
		return NULL;
	}
	arg = ast_strdupa(role);

	it_role_snapshots = ao2_callback(obj->channel_snapshots, OBJ_MULTIPLE | OBJ_KEY, channel_role_multi_cmp_cb, arg);
	if (!it_role_snapshots) {
		return NULL;
	}

	while ((role_snapshot = ao2_iterator_next(it_role_snapshots))) {
		ao2_link(ret_container, role_snapshot->snapshot);
		ao2_ref(role_snapshot, -1);
	}
	ao2_iterator_destroy(it_role_snapshots);

	ao2_ref(ret_container, +1);
	return ret_container;
}

struct ast_json *ast_multi_channel_blob_get_json(struct ast_multi_channel_blob *obj)
{
	if (!obj) {
		return NULL;
	}
	return obj->blob;
}

void ast_channel_stage_snapshot(struct ast_channel *chan)
{
	ast_set_flag(ast_channel_flags(chan), AST_FLAG_SNAPSHOT_STAGE);
}

void ast_channel_stage_snapshot_done(struct ast_channel *chan)
{
	ast_clear_flag(ast_channel_flags(chan), AST_FLAG_SNAPSHOT_STAGE);
	ast_channel_publish_snapshot(chan);
}

void ast_channel_publish_snapshot(struct ast_channel *chan)
{
	RAII_VAR(struct ast_channel_snapshot *, snapshot, NULL, ao2_cleanup);
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);

	if (ast_test_flag(ast_channel_flags(chan), AST_FLAG_SNAPSHOT_STAGE)) {
		return;
	}

	snapshot = ast_channel_snapshot_create(chan);
	if (!snapshot) {
		return;
	}

	message = stasis_message_create(ast_channel_snapshot_type(), snapshot);
	if (!message) {
		return;
	}

	ast_assert(ast_channel_topic(chan) != NULL);
	stasis_publish(ast_channel_topic(chan), message);
}

void ast_channel_publish_blob(struct ast_channel *chan, struct stasis_message_type *type, struct ast_json *blob)
{
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);

	if (!blob) {
		blob = ast_json_null();
	}

	message = ast_channel_blob_create(chan, type, blob);
	if (message) {
		stasis_publish(ast_channel_topic(chan), message);
	}
}

void ast_channel_publish_varset(struct ast_channel *chan, const char *name, const char *value)
{
	RAII_VAR(struct ast_json *, blob, NULL, ast_json_unref);

	ast_assert(name != NULL);
	ast_assert(value != NULL);

	blob = ast_json_pack("{s: s, s: s}",
			     "variable", name,
			     "value", value);
	if (!blob) {
		ast_log(LOG_ERROR, "Error creating message\n");
		return;
	}

	ast_channel_publish_blob(chan, ast_channel_varset_type(), blob);
}

static struct ast_manager_event_blob *varset_to_ami(struct stasis_message *msg)
{
	RAII_VAR(struct ast_str *, channel_event_string, NULL, ast_free);
	struct ast_channel_blob *obj = stasis_message_data(msg);
	const char *variable =
		ast_json_string_get(ast_json_object_get(obj->blob, "variable"));
	const char *value =
		ast_json_string_get(ast_json_object_get(obj->blob, "value"));

	if (obj->snapshot) {
		channel_event_string =
			ast_manager_build_channel_state_string(obj->snapshot);
	} else {
		channel_event_string = ast_str_create(35);
		ast_str_set(&channel_event_string, 0,
			    "Channel: none\r\n"
			    "Uniqueid: none\r\n");
	}

	if (!channel_event_string) {
		return NULL;
	}

	return ast_manager_event_blob_create(EVENT_FLAG_DIALPLAN, "VarSet",
		"%s"
		"Variable: %s\r\n"
		"Value: %s\r\n",
		ast_str_buffer(channel_event_string), variable, value);
}

static struct ast_manager_event_blob *agent_login_to_ami(struct stasis_message *msg)
{
	RAII_VAR(struct ast_str *, channel_string, NULL, ast_free);
	struct ast_channel_blob *obj = stasis_message_data(msg);
	const char *agent = ast_json_string_get(ast_json_object_get(obj->blob, "agent"));

	channel_string = ast_manager_build_channel_state_string(obj->snapshot);
	if (!channel_string) {
		return NULL;
	}

	return ast_manager_event_blob_create(EVENT_FLAG_AGENT, "AgentLogin",
		"%s"
		"Agent: %s\r\n",
		ast_str_buffer(channel_string), agent);
}

static struct ast_manager_event_blob *agent_logoff_to_ami(struct stasis_message *msg)
{
	RAII_VAR(struct ast_str *, channel_string, NULL, ast_free);
	struct ast_channel_blob *obj = stasis_message_data(msg);
	const char *agent = ast_json_string_get(ast_json_object_get(obj->blob, "agent"));
	long logintime = ast_json_integer_get(ast_json_object_get(obj->blob, "logintime"));

	channel_string = ast_manager_build_channel_state_string(obj->snapshot);
	if (!channel_string) {
		return NULL;
	}

	return ast_manager_event_blob_create(EVENT_FLAG_AGENT, "AgentLogoff",
		"%s"
		"Agent: %s\r\n"
		"Logintime: %ld\r\n",
		ast_str_buffer(channel_string), agent, logintime);
}

void ast_publish_channel_state(struct ast_channel *chan)
{
	RAII_VAR(struct ast_channel_snapshot *, snapshot, NULL, ao2_cleanup);
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);

	ast_assert(chan != NULL);
	if (!chan) {
		return;
	}

	snapshot = ast_channel_snapshot_create(chan);
	if (!snapshot) {
		return;
	}

	message = stasis_message_create(ast_channel_snapshot_type(), snapshot);
	if (!message) {
		return;
	}

	ast_assert(ast_channel_topic(chan) != NULL);
	stasis_publish(ast_channel_topic(chan), message);
}

struct ast_json *ast_channel_snapshot_to_json(
	const struct ast_channel_snapshot *snapshot,
	const struct stasis_message_sanitizer *sanitize)
{
	RAII_VAR(struct ast_json *, json_chan, NULL, ast_json_unref);

	if (snapshot == NULL
		|| (sanitize && sanitize->channel_snapshot
		&& sanitize->channel_snapshot(snapshot))) {
		return NULL;
	}

	json_chan = ast_json_pack(
		/* Broken up into groups of three for readability */
		"{ s: s, s: s, s: s,"
		"  s: o, s: o, s: s,"
		"  s: o, s: o }",
		/* First line */
		"id", snapshot->uniqueid,
		"name", snapshot->name,
		"state", ast_state2str(snapshot->state),
		/* Second line */
		"caller", ast_json_name_number(
			snapshot->caller_name, snapshot->caller_number),
		"connected", ast_json_name_number(
			snapshot->connected_name, snapshot->connected_number),
		"accountcode", snapshot->accountcode,
		/* Third line */
		"dialplan", ast_json_dialplan_cep(
			snapshot->context, snapshot->exten, snapshot->priority),
		"creationtime", ast_json_timeval(snapshot->creationtime, NULL));

	return ast_json_ref(json_chan);
}

int ast_channel_snapshot_cep_equal(
	const struct ast_channel_snapshot *old_snapshot,
	const struct ast_channel_snapshot *new_snapshot)
{
	ast_assert(old_snapshot != NULL);
	ast_assert(new_snapshot != NULL);

	/* We actually get some snapshots with CEP set, but before the
	 * application is set. Since empty application is invalid, we treat
	 * setting the application from nothing as a CEP change.
	 */
	if (ast_strlen_zero(old_snapshot->appl) &&
	    !ast_strlen_zero(new_snapshot->appl)) {
		return 0;
	}

	return old_snapshot->priority == new_snapshot->priority &&
		strcmp(old_snapshot->context, new_snapshot->context) == 0 &&
		strcmp(old_snapshot->exten, new_snapshot->exten) == 0;
}

int ast_channel_snapshot_caller_id_equal(
	const struct ast_channel_snapshot *old_snapshot,
	const struct ast_channel_snapshot *new_snapshot)
{
	ast_assert(old_snapshot != NULL);
	ast_assert(new_snapshot != NULL);
	return strcmp(old_snapshot->caller_number, new_snapshot->caller_number) == 0 &&
		strcmp(old_snapshot->caller_name, new_snapshot->caller_name) == 0;
}

static struct ast_json *channel_blob_to_json(
	struct stasis_message *message,
	const char *type,
	const struct stasis_message_sanitizer *sanitize)
{
	RAII_VAR(struct ast_json *, out, NULL, ast_json_unref);
	struct ast_channel_blob *channel_blob = stasis_message_data(message);
	struct ast_json *blob = channel_blob->blob;
	struct ast_channel_snapshot *snapshot = channel_blob->snapshot;
	const struct timeval *tv = stasis_message_timestamp(message);
	int res = 0;

	if (blob == NULL || ast_json_is_null(blob)) {
		out = ast_json_object_create();
	} else {
		/* blobs are immutable, so shallow copies are fine */
		out = ast_json_copy(blob);
	}

	if (!out) {
		return NULL;
	}

	res |= ast_json_object_set(out, "type", ast_json_string_create(type));
	res |= ast_json_object_set(out, "timestamp",
		ast_json_timeval(*tv, NULL));

	/* For global channel messages, the snapshot is optional */
	if (snapshot) {
		struct ast_json *json_channel = ast_channel_snapshot_to_json(snapshot, sanitize);

		if (!json_channel) {
			return NULL;
		}

		res |= ast_json_object_set(out, "channel", json_channel);
	}

	if (res != 0) {
		return NULL;
	}

	return ast_json_ref(out);
}

static struct ast_json *dtmf_end_to_json(
	struct stasis_message *message,
	const struct stasis_message_sanitizer *sanitize)
{
	struct ast_channel_blob *channel_blob = stasis_message_data(message);
	struct ast_json *blob = channel_blob->blob;
	struct ast_channel_snapshot *snapshot = channel_blob->snapshot;
	const char *direction =
		ast_json_string_get(ast_json_object_get(blob, "direction"));
	const struct timeval *tv = stasis_message_timestamp(message);
	struct ast_json *json_channel = ast_channel_snapshot_to_json(snapshot, sanitize);

	/* Only present received DTMF end events as JSON */
	if (strcasecmp("Received", direction) != 0) {
		return NULL;
	}

	if (!json_channel) {
		return NULL;
	}

	return ast_json_pack("{s: s, s: o, s: O, s: O, s: o}",
		"type", "ChannelDtmfReceived",
		"timestamp", ast_json_timeval(*tv, NULL),
		"digit", ast_json_object_get(blob, "digit"),
		"duration_ms", ast_json_object_get(blob, "duration_ms"),
		"channel", json_channel);
}

static struct ast_json *user_event_to_json(
	struct stasis_message *message,
	const struct stasis_message_sanitizer *sanitize)
{
	struct ast_channel_blob *channel_blob = stasis_message_data(message);
	struct ast_json *blob = channel_blob->blob;
	struct ast_channel_snapshot *snapshot = channel_blob->snapshot;
	const struct timeval *tv = stasis_message_timestamp(message);
	struct ast_json *json_channel = ast_channel_snapshot_to_json(snapshot, sanitize);

	if (!json_channel) {
		return NULL;
	}

	return ast_json_pack("{s: s, s: o, s: O, s: O, s: o}",
		"type", "ChannelUserevent",
		"timestamp", ast_json_timeval(*tv, NULL),
		"eventname", ast_json_object_get(blob, "eventname"),
		"userevent", blob,
		"channel", json_channel);
}

static struct ast_json *varset_to_json(
	struct stasis_message *message,
	const struct stasis_message_sanitizer *sanitize)
{
	return channel_blob_to_json(message, "ChannelVarset", sanitize);
}

static struct ast_json *hangup_request_to_json(
	struct stasis_message *message,
	const struct stasis_message_sanitizer *sanitize)
{
	return channel_blob_to_json(message, "ChannelHangupRequest", sanitize);
}

static struct ast_json *dial_to_json(
	struct stasis_message *message,
	const struct stasis_message_sanitizer *sanitize)
{
	struct ast_multi_channel_blob *payload = stasis_message_data(message);
	struct ast_json *blob = ast_multi_channel_blob_get_json(payload);
	struct ast_json *caller_json = ast_channel_snapshot_to_json(ast_multi_channel_blob_get_channel(payload, "caller"), sanitize);
	struct ast_json *peer_json = ast_channel_snapshot_to_json(ast_multi_channel_blob_get_channel(payload, "peer"), sanitize);
	struct ast_json *forwarded_json = ast_channel_snapshot_to_json(ast_multi_channel_blob_get_channel(payload, "forwarded"), sanitize);
	struct ast_json *json;
	const struct timeval *tv = stasis_message_timestamp(message);
	int res = 0;

	json = ast_json_pack("{s: s, s: o, s: O, s: O, s: O}",
		"type", "Dial",
		"timestamp", ast_json_timeval(*tv, NULL),
		"dialstatus", ast_json_object_get(blob, "dialstatus"),
		"forward", ast_json_object_get(blob, "forward"),
		"dialstring", ast_json_object_get(blob, "dialstring"));
	if (!json) {
		ast_json_unref(caller_json);
		ast_json_unref(peer_json);
		ast_json_unref(forwarded_json);
		return NULL;
	}

	if (caller_json) {
		res |= ast_json_object_set(json, "caller", caller_json);
	}
	if (peer_json) {
		res |= ast_json_object_set(json, "peer", peer_json);
	}
	if (forwarded_json) {
		res |= ast_json_object_set(json, "forwarded", forwarded_json);
	}

	if (res) {
		ast_json_unref(json);
		return NULL;
	}

	return json;
}

/*!
 * @{ \brief Define channel message types.
 */
STASIS_MESSAGE_TYPE_DEFN(ast_channel_snapshot_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_dial_type,
	.to_json = dial_to_json,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_varset_type,
	.to_ami = varset_to_ami,
	.to_json = varset_to_json,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_user_event_type,
	.to_json = user_event_to_json,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_hangup_request_type,
	.to_json = hangup_request_to_json,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_dtmf_begin_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_dtmf_end_type,
	.to_json = dtmf_end_to_json,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_hold_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_unhold_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_chanspy_start_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_chanspy_stop_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_fax_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_hangup_handler_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_moh_start_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_moh_stop_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_monitor_start_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_monitor_stop_type);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_agent_login_type,
	.to_ami = agent_login_to_ami,
	);
STASIS_MESSAGE_TYPE_DEFN(ast_channel_agent_logoff_type,
	.to_ami = agent_logoff_to_ami,
	);

/*! @} */

static void stasis_channels_cleanup(void)
{
	stasis_caching_unsubscribe_and_join(channel_by_name_topic);
	channel_by_name_topic = NULL;
	ao2_cleanup(channel_cache_by_name);
	channel_cache_by_name = NULL;
	ao2_cleanup(channel_cache_all);
	channel_cache_all = NULL;

	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_snapshot_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_dial_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_varset_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_user_event_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_hangup_request_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_dtmf_begin_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_dtmf_end_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_hold_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_unhold_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_chanspy_start_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_chanspy_stop_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_fax_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_hangup_handler_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_moh_start_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_moh_stop_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_monitor_start_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_monitor_stop_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_agent_login_type);
	STASIS_MESSAGE_TYPE_CLEANUP(ast_channel_agent_logoff_type);
}

int ast_stasis_channels_init(void)
{
	int res = 0;

	ast_register_cleanup(stasis_channels_cleanup);

	channel_cache_all = stasis_cp_all_create("ast_channel_topic_all",
		channel_snapshot_get_id);
	if (!channel_cache_all) {
		return -1;
	}
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_agent_login_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_agent_logoff_type);

	channel_cache_by_name = stasis_cache_create(channel_snapshot_get_name);
	if (!channel_cache_by_name) {
		return -1;
	}

	/* This should be initialized before the caching topic */
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_snapshot_type);

	channel_by_name_topic = stasis_caching_topic_create(
		stasis_cp_all_topic(channel_cache_all),
		channel_cache_by_name);
	if (!channel_by_name_topic) {
		return -1;
	}

	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_dial_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_varset_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_user_event_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_hangup_request_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_dtmf_begin_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_dtmf_end_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_hold_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_unhold_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_chanspy_start_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_chanspy_stop_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_fax_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_hangup_handler_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_moh_start_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_moh_stop_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_monitor_start_type);
	res |= STASIS_MESSAGE_TYPE_INIT(ast_channel_monitor_stop_type);

	return res;
}
