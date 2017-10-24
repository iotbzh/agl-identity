/*
 * Copyright 2017 IoT.bzh
 *
 * author: Loïc Collignon <loic.collignon@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <json-c/json.h>
#include <db.h>

#define AFB_BINDING_VERSION 2
#include <afb/afb-binding.h>

#include "utils.h"

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif

#define DBFILE		"/ll-database-binding.db"

#if !defined(TO_STRING_FLAGS)
# if !defined(JSON_C_TO_STRING_NOSLASHESCAPE)
#  define JSON_C_TO_STRING_NOSLASHESCAPE (1<<4)
# endif
# define TO_STRING_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)
#endif
#define USERNAME	"agl"
#define APPNAME		"firefox"

// ----- Globals -----
static DB*		database;
static char	database_file[MAX_PATH];

// ----- Binding's declarations -----
static int ll_database_binding_init();
static void verb_insert(struct afb_req req);
static void verb_update(struct afb_req req);
static void verb_delete(struct afb_req req);
static void verb_read(struct afb_req req);

// ----- Binding's implementations -----

/**
 * @brief Initialize the binding.
 * @return Exit code, zero if success.
 */
static int ll_database_binding_init()
{
	struct passwd pwd;
	struct passwd* result;
	char buf[MAX_PATH];
	size_t bufsize;
	int ret;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize == -1 || bufsize > MAX_PATH) bufsize = MAX_PATH;

	ret = getpwuid_r(getuid(), &pwd, buf, bufsize, &result);
	if (result == NULL)
	{
		if (ret == 0) AFB_ERROR("User not found");
		else AFB_ERROR("getpwuid_r failed with %d code", ret);
		return -1;
	}

	memset(database_file, 0, MAX_PATH);
	strcat(database_file, result->pw_dir);
	strcat(database_file, DBFILE);

	AFB_INFO("The database file is '%s'", database_file);

	if ((ret = db_create(&database, NULL, 0)) != 0)
	{
		AFB_ERROR("Failed to create database: %s.", db_strerror(ret));
		return -1;
	}

	if ((ret = database->open(database, NULL, database_file, NULL, DB_BTREE, DB_CREATE, 0664)) != 0)
	{
		AFB_ERROR("Failed to open the '%s' database: %s.", database_file, db_strerror(ret));
		database->close(database, 0);
		return -1;
	}

	return 0;
}

/**
 * Returns the database key for the 'req'
 */
static int get_key(struct afb_req req, DBT *key)
{
	char *appid, *data;
	const char *jkey;

	size_t ljkey, lappid, size;

	struct json_object* args;
	struct json_object* item;

	/* get the key */
	args = afb_req_json(req);
	if (!json_object_object_get_ex(args, "key", &item))
	{
		afb_req_fail(req, "no-key", NULL);
		return -1;
	}
	if (!item
	 || !(jkey = json_object_get_string(item))
	 || !(ljkey = strlen(jkey)))
	{
		afb_req_fail(req, "bad-key", NULL);
		return -1;
	}

	/* get the appid */
	appid = afb_req_get_application_id(req);
	if (!appid)
	{
		afb_req_fail(req, "bad-context", NULL);
		return -1;
	}

	/* make the db-key */
	lappid = strlen(appid);
	size = lappid + ljkey + 2;
	data = realloc(appid, size);
	if (!data)
	{
		free(appid);
		afb_req_fail(req, "out-of-memory", NULL);
		return -1;
	}
	data[lappid] = ':';
	memcpy(&data[lappid + 1], jkey, ljkey + 1);

	/* return the key */
	key->data = data;
	key->size = (uint32_t)size;
	return 0;
}

static void put(struct afb_req req, int replace)
{
	int ret;
	DBT key;
	DBT data;

	const char* value;

	struct json_object* args;
	struct json_object* item;

	args = afb_req_json(req);

	if (!json_object_object_get_ex(args, "value", &item))
	{
		afb_req_fail(req, "no-value", NULL);
		return;
	}

	value = json_object_to_json_string_ext(item, TO_STRING_FLAGS);
	if (!value)
	{
		afb_req_fail(req, "out-of-memory", NULL);
		return;
	}

	if (get_key(req, &key))
		return;

	AFB_INFO("put: key=%s, value=%s", (char*)key.data, value);

	data.data = (void*)value;
	data.size = (uint32_t)strlen(value);

	ret = database->put(database, NULL, &key, &data, replace ? 0 : DB_NOOVERWRITE);
	if (ret == 0)
		afb_req_success(req, NULL, NULL);
	else
	{
		AFB_ERROR("can't %s key %s with %s", replace ? "replace" : "insert", (char*)key.data, (char*)data.data);
		afb_req_fail_f(req, "failed", "%s", db_strerror(ret));
	}
	free(key.data);
}

static void verb_insert(struct afb_req req)
{
	put(req, 0);
}

static void verb_update(struct afb_req req)
{
	put(req, 1);
}

static void verb_delete(struct afb_req req)
{
	DBT key;
	int ret;

	if (get_key(req, &key))
		return;

	AFB_INFO("delete: key=%s", (char*)key.data);

	if ((ret = database->del(database, NULL, &key, 0)) == 0)
		afb_req_success_f(req, NULL, "db success: delete %s.", (char *)key.data);
	else
		afb_req_fail_f(req, "Failed to delete datas.", "db fail: delete %s - %s", (char*)key.data, db_strerror(ret));

	free(key.data);
}

static void verb_read(struct afb_req req)
{
	DBT key;
	DBT data;
	int ret;

	char value[4096];

	struct json_object* result;
	struct json_object* val;


	if (get_key(req, &key))
		return;

	AFB_INFO("read: key=%s", (char*)key.data);

	data.data = value;
	data.ulen = 4096;
	data.flags = DB_DBT_USERMEM;

	if ((ret = database->get(database, NULL, &key, &data, 0)) == 0)
	{
		result = json_object_new_object();
		val = json_tokener_parse((char*)data.data);
		json_object_object_add(result, "value", val ? val : json_object_new_string((char*)data.data));

		afb_req_success_f(req, result, "db success: read %s=%s.", (char*)key.data, (char*)data.data);
	}
	else
		afb_req_fail_f(req, "Failed to read datas.", "db fail: read %s - %s", (char*)key.data, db_strerror(ret));

	free(key.data);
}

// ----- Binding's configuration -----
/*
static const struct afb_auth ll_database_binding_auths[] = {
};
*/

static const afb_verb_v2 ll_database_binding_verbs[]= {
		REGISTER_VERB(insert,	NULL, NULL, AFB_SESSION_NONE_V2),
		REGISTER_VERB(update,	NULL, NULL, AFB_SESSION_NONE_V2),
		REGISTER_VERB(delete,	NULL, NULL, AFB_SESSION_NONE_V2),
		REGISTER_VERB(read,		NULL, NULL, AFB_SESSION_NONE_V2),
        { .verb = NULL}
};

const struct afb_binding_v2 afbBindingV2 = {
                .api = "ll-database",
                .specification = NULL,
                .verbs = ll_database_binding_verbs,
                .preinit = NULL,
                .init = ll_database_binding_init,
                .onevent = NULL,
                .noconcurrency = 0
};
