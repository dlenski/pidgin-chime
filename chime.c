#define PURPLE_PLUGINS

#include <prpl.h>
#include <version.h>
#include <accountopt.h>
#include <status.h>

#include <glib/gi18n.h>
#include <glib/gstrfuncs.h>
#include <glib/gquark.h>

#include <json-glib/json-glib.h>

#include <libsoup/soup.h>

#include "chime.h"

G_DEFINE_QUARK(PidginChimeError, pidgin_chime_error);

#define CONNECT_STEPS 3

SoupURI *soup_uri_new_printf(const gchar *base, const gchar *format, ...)
{
	SoupURI *uri;
	va_list args;
	gchar *constructed;
	gchar *append;

	va_start(args, format);
	append = g_strdup_vprintf(format, args);
	va_end(args);
	constructed = g_strdup_printf("%s%s%s", base,
				      g_str_has_suffix(base, "/") ? "" : "/",
				      append[0] == '/' ? append + 1 : append);
	uri = soup_uri_new(constructed);
	g_free(constructed);
	g_free(append);
	return uri;
}
/* Helper function to get a string from a JSON child node */
static gboolean parse_string(JsonNode *parent, const gchar *name, const gchar **res)
{
	JsonObject *obj;
	JsonNode *node;
	const gchar *str;

	if (!parent)
		return FALSE;

	obj = json_node_get_object(parent);
	if (!obj)
		return FALSE;

	node = json_object_get_member(obj, name);
	if (!node)
		return FALSE;

	str = json_node_get_string(node);
	if (!str)
		return FALSE;

	*res = str;
	printf("Got %s = %s\n", name, str);
	return TRUE;
}

static JsonNode *process_soup_response(SoupMessage *msg, GError **error)
{
	const gchar *content_type;
	JsonParser *parser;
	JsonNode *node;

	if (msg->status_code != 200 && msg->status_code != 201) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_REQUEST_FAILED,
			    _("Request failed(%d): %s"),
			    msg->status_code, msg->reason_phrase);
		return NULL;
	}

	content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (!content_type || strcmp(content_type, "application/json")) {
		g_set_error(error, CHIME_ERROR,
			    CHIME_ERROR_BAD_RESPONSE,
			    _("Server sent wrong content-type '%s'"),
			    content_type);
		return NULL;
	}
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length,
					error)) {
		g_object_unref(parser);
		return NULL;
	}

	node = json_node_ref(json_parser_get_root(parser));
	g_object_unref(parser);
	return node;
}

static void renew_cb(SoupSession *soup_sess, SoupMessage *msg,
		     gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	GError *error = NULL;
	GList *l;
	const gchar *sess_tok;
	JsonNode *tok_node;
	gchar *cookie_hdr;

	tok_node = process_soup_response(msg, &error);
	if (!tok_node) {
		gchar *reason = g_strdup_printf(_("Token renewal: %s"),
						error->message);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, reason);
		g_free(reason);
		return;
	}

	if (!parse_string(tok_node, "SessionToken", &sess_tok)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to renew session token"));
		return;
	}
	purple_account_set_string(cxn->prpl_conn->account, "token", sess_tok);
	g_free(cxn->session_token);
	cxn->session_token = g_strdup(sess_tok);

	cookie_hdr = g_strdup_printf("_aws_wt_session=%s", cxn->session_token);

	while ( (l = g_list_first(cxn->msg_queue)) ) {
		struct chime_msg_queue *cmsg = l->data;

		soup_message_headers_replace(cmsg->msg->request_headers, "Cookie", cookie_hdr);
		soup_session_queue_message(cxn->soup_sess, cmsg->msg, cmsg->cb, cxn);
		cxn->msg_queue = g_list_remove(cxn->msg_queue, cmsg);
	}

	g_free(cookie_hdr);
}

void chime_queue_http_request(struct chime_connection *cxn, JsonNode *node,
			      SoupURI *uri, SoupSessionCallback callback)
{
	SoupMessage *msg = soup_message_new_from_uri(node?"POST":"GET", uri);

	if (cxn->session_token) {
		gchar *cookie = g_strdup_printf("_aws_wt_session=%s", cxn->session_token);
		soup_message_headers_append(msg->request_headers, "Cookie", cookie);
		g_free(cookie);
	}
	if (node) {
		gchar *body;
		gsize body_size;
		JsonGenerator *gen = json_generator_new();
		json_generator_set_root(gen, node);
		body = json_generator_to_data(gen, &body_size);
		soup_message_set_request(msg, "application/json",
					 SOUP_MEMORY_TAKE,
					 body, body_size);
		g_object_unref(gen);
		json_node_unref(node);
	}
	soup_session_queue_message(cxn->soup_sess, msg, callback, cxn);
}


static void chime_renew_token(struct chime_connection *cxn)
{
	SoupURI *uri;
	JsonBuilder *builder;
	JsonNode *node;

	builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "Token");
	builder = json_builder_add_string_value(builder, cxn->session_token);
	builder = json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	g_object_unref(builder);

	uri = soup_uri_new_printf(cxn->profile_url, "/tokens");
	soup_uri_set_query_from_fields(uri, "Token", cxn->session_token, NULL);
	chime_queue_http_request(cxn, node, uri, renew_cb);
}


static void resubmit_msg_for_auth(struct chime_connection *cxn, SoupMessage *msg, SoupSessionCallback cb)
{
	struct chime_msg_queue *cmsg = g_new0(struct chime_msg_queue, 1);

	cmsg->msg = g_object_ref(msg);
	cmsg->cb = cb;

	if (cxn->msg_queue) {
		/* Already renewing; just add to the list */
		cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
		return;
	}

	cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
	chime_renew_token(cxn);
}

static gboolean chime_purple_plugin_load(PurplePlugin *plugin)
{
	printf("Chime plugin load\n");
	purple_notify_message(plugin, PURPLE_NOTIFY_MSG_INFO, "Foo",
			      "Chime plugin starting...", NULL, NULL, NULL);
	return TRUE;
}

static gboolean chime_purple_plugin_unload(PurplePlugin *plugin)
{
	return TRUE;
}

void chime_purple_plugin_destroy(PurplePlugin *plugin)
{
}
const char *chime_purple_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
        return "chime";
}

static JsonNode *chime_device_register_req(PurpleAccount *account)
{
	JsonBuilder *jb;
	JsonNode *jn;

	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Device");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Platform");
	jb = json_builder_add_string_value(jb, "android");
	jb = json_builder_set_member_name(jb, "DeviceToken");
	jb = json_builder_add_string_value(jb, "not-a-real-device-not-even-android");
	jb = json_builder_set_member_name(jb, "UaChannelToken");
	jb = json_builder_add_string_value(jb, "blah42");
	jb = json_builder_set_member_name(jb, "Capabilities");
	jb = json_builder_add_int_value(jb, 1234);
	jb = json_builder_end_object(jb);
	jb = json_builder_end_object(jb);
	jn = json_builder_get_root(jb);
	g_object_unref(jb);

	return jn;
}


static void on_websocket_closed(SoupWebsocketConnection *ws,
				gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;

	printf("websocket closed: %d %s!\n", soup_websocket_connection_get_close_code(ws),
	       soup_websocket_connection_get_close_data(ws));
}

static void ws2_cb(GObject *obj, GAsyncResult *res, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	GError *error = NULL;


	cxn->ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(obj),
							      res, &error);
	if (!cxn->ws_conn) {
		gchar *reason = g_strdup_printf(_("Websocket connection error %s"),
						error->message);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       reason);
		g_free(reason);
		return;
	}
	printf("Got ws conn %p\n", cxn->ws_conn);
	g_signal_connect(G_OBJECT(cxn->ws_conn), "closed",
			 G_CALLBACK(on_websocket_closed), cxn);
	purple_connection_set_state(cxn->prpl_conn, PURPLE_CONNECTED);
}

static void ws_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	GError *error = NULL;
	gchar **ws_opts = NULL;
	gchar **protos = NULL;
	gchar *url;
	SoupURI *uri;

	if (msg->status_code == 401) {
		resubmit_msg_for_auth(cxn, msg, ws_cb);
		return;
	}
	if (msg->status_code != 200) {
		gchar *reason = g_strdup_printf(_("Websocket connection error (%d): %s"),
						msg->status_code, msg->reason_phrase);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       reason);
		g_free(reason);
		return;
	}
	if (msg->response_body->data)
		ws_opts = g_strsplit(msg->response_body->data, ":", 4);

	if (!ws_opts || !ws_opts[1] || !ws_opts[2] || !ws_opts[3] ||
	    strncmp(ws_opts[3], "websocket,", 10)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Unexpected response in WebSocket setup"));
		return;
	}

	url = g_strdup_printf("%s/1/websocket/%s", cxn->websocket_url, ws_opts[1]);
	uri = soup_uri_new(url);
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);
	g_free(url);

	/* New message */
	msg = soup_message_new_from_uri("GET", uri);
	purple_connection_update_progress(cxn->prpl_conn, _("Establishing WebSocket connection..."),
					  4, CONNECT_STEPS);
	protos = g_strsplit(ws_opts[3], ",", 0);
	soup_session_websocket_connect_async(cxn->soup_sess, msg, NULL, protos, NULL, ws2_cb, cxn);
	g_strfreev(protos);
	g_strfreev(ws_opts);
	/* There's more to it than this but this will do for now... */

}


static gboolean parse_regnode(struct chime_connection *cxn, JsonNode *regnode)
{
	JsonObject *obj = json_node_get_object(cxn->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *str, *sess_tok;

	if (!sess_node)
		return FALSE;
	if (!parse_string(sess_node, "SessionToken", &sess_tok))
		return FALSE;
	purple_account_set_string(cxn->prpl_conn->account, "token", sess_tok);
	cxn->session_token = g_strdup(sess_tok);

	obj = json_node_get_object(sess_node);

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "id", &cxn->session_id) ||
	    !parse_string(node, "profile_channel", &cxn->profile_channel))
		return FALSE;

	node = json_object_get_member(obj, "Device");
	if (!parse_string(node, "DeviceId", &cxn->device_id) ||
	    !parse_string(node, "Channel", &cxn->device_channel))
		return FALSE;

	node = json_object_get_member(obj, "ServiceConfig");
	if (!node)
		return FALSE;
	obj = json_node_get_object(node);

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &cxn->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Push");
	if (!parse_string(node, "ReachabilityUrl", &cxn->reachability_url) ||
	    !parse_string(node, "WebsocketUrl", &cxn->websocket_url))
		return FALSE;

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "RestUrl", &cxn->profile_url))
		return FALSE;

	node = json_object_get_member(obj, "Contacts");
	if (!parse_string(node, "RestUrl", &cxn->contacts_url))
		return FALSE;

	node = json_object_get_member(obj, "Messaging");
	if (!parse_string(node, "RestUrl", &cxn->messaging_url))
		return FALSE;

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &cxn->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Conference");
	if (!parse_string(node, "RestUrl", &cxn->conference_url))
		return FALSE;

	return TRUE;
}

static void register_cb(SoupSession *soup_sess, SoupMessage *msg,
			gpointer _cxn)
{
	struct chime_connection *cxn = _cxn;
	GError *error = NULL;

	cxn->reg_node = process_soup_response(msg, &error);
	if (!cxn->reg_node) {
		gchar *reason = g_strdup_printf(_("Device registration failed: %s"),
						error->message);
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, reason);
		g_free(reason);
		return;
	}

	if (!parse_regnode(cxn, cxn->reg_node)) {
		purple_connection_error_reason(cxn->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to process registration response"));
		return;
	}

	SoupURI *uri = soup_uri_new_printf(cxn->websocket_url, "/1");
	soup_uri_set_query_from_fields(uri, "session_uuid", cxn->session_id, NULL);

	purple_connection_update_progress(cxn->prpl_conn, _("Obtaining WebSocket params..."),
					  2, CONNECT_STEPS);
	chime_queue_http_request(cxn, NULL, uri, ws_cb);
}

#define SIGNIN_DEFAULT "https://signin.id.ue1.app.chime.aws/"
void chime_purple_login(PurpleAccount *account)
{
	PurpleConnection *conn = purple_account_get_connection(account);
	struct chime_connection *cxn;
	const gchar *token = purple_account_get_string(account, "token",
						       NULL);
	JsonNode *node;
	SoupURI *uri;

	if (!token) {
		purple_connection_error(cxn->prpl_conn,
					_("No authentication token"));
		return;
	}

	cxn = g_new0(struct chime_connection, 1);
	purple_connection_set_protocol_data(conn, cxn);
	cxn->prpl_conn = conn;
	cxn->soup_sess = soup_session_new();
	printf("conn %p cxn %p\n", conn, cxn);
	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(cxn->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(cxn->soup_sess, "ssl-strict", FALSE, NULL);
	}
	node = chime_device_register_req(account);
	uri = soup_uri_new_printf(purple_account_get_string(account, "server", SIGNIN_DEFAULT),
				  "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", token, NULL);

	purple_connection_update_progress(conn, _("Connecting..."), 1, CONNECT_STEPS);
	chime_queue_http_request(cxn, node, uri, register_cb);
}

void chime_purple_close(PurpleConnection *conn)
{
	struct chime_connection *cxn = purple_connection_get_protocol_data(conn);

	if (cxn) {
		if (cxn->soup_sess) {
			soup_session_abort(cxn->soup_sess);
			g_object_unref(cxn->soup_sess);
			cxn->soup_sess = NULL;
		}
		if (cxn->reg_node) {
			json_node_unref(cxn->reg_node);
			cxn->reg_node = NULL;
		}
		if (cxn->ws_conn) {
			g_object_unref(cxn->ws_conn);
			cxn->ws_conn = NULL;
		}
		if (cxn->msg_queue) {
			g_list_free_full(cxn->msg_queue, g_object_unref);
			cxn->msg_queue = NULL;
		}
		purple_connection_set_protocol_data(cxn->prpl_conn, NULL);
		g_free(cxn);
	}
	printf("Chime close\n");
}

GList *chime_purple_chat_info(PurpleConnection *conn)
{
	printf("chat_info\n");
	return NULL;
}

GList *chime_purple_status_types(PurpleAccount *account)
{
	PurpleStatusType *type;
	GList *types = NULL;

	type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, NULL,
				      _("available"), TRUE);
	types = g_list_append(types, type);

	return types;
}

gchar *chime_purple_status_text(PurpleBuddy *buddy)
{
	return g_strdup("fish");
}

static int chime_purple_send_im(PurpleConnection *gc,
				const char *who,
				const char *what,
				PurpleMessageFlags flags)
{

	printf("send %s to %s\n", what, who);
	return 1;
}

static PurplePluginProtocolInfo chime_prpl_info = {
	.options = OPT_PROTO_NO_PASSWORD,
	.list_icon = chime_purple_list_icon,
	.login = chime_purple_login,
	.close = chime_purple_close,
	.status_text = chime_purple_status_text,
	.status_types = chime_purple_status_types,
	.send_im = chime_purple_send_im,
	.chat_info = chime_purple_chat_info,
};

static void chime_purple_show_about_plugin(PurplePluginAction *action)
{
	purple_notify_formatted(action->context,
				NULL, _("Foo"), NULL, _("Hello"),
				NULL, NULL);
}

static GList *chime_purple_plugin_actions(PurplePlugin *plugin,
					  gpointer context)
{
	PurplePluginAction *act;
	GList *acts = NULL;
	printf("chime acts\n");
	act = purple_plugin_action_new(_("About Chime plugin..."),
				       chime_purple_show_about_plugin);
	acts = g_list_append(acts, act);

	return acts;
}

static PurplePluginInfo chime_plugin_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_PROTOCOL,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = "prpl-chime",
	.name = "Amazon Chime",
	.version = PACKAGE_VERSION,
	.summary = "Amazon Chime Protocol Plugin",
	.description = "A plugin for Chime",
	.author = "David Woodhouse <dwmw2@infradead.org>",
	.load = chime_purple_plugin_load,
	.unload = chime_purple_plugin_unload,
	.destroy = chime_purple_plugin_destroy,
	.extra_info = &chime_prpl_info,
	.actions = chime_purple_plugin_actions,
};

static void chime_purple_init_plugin(PurplePlugin *plugin)
{
	PurpleAccountOption *opt;
	GList *opts = NULL;

	opt = purple_account_option_string_new(_("Signin URL"),
					       "server", NULL);
	opts = g_list_append(opts, opt);

	opt = purple_account_option_string_new(_("Token"), "token", NULL);
	opts = g_list_append(opts, opt);

	chime_prpl_info.protocol_options = opts;
}

PURPLE_INIT_PLUGIN(chime, chime_purple_init_plugin, chime_plugin_info);
