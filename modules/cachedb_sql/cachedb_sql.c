/*
 * Copyright (C) 2013 Steve Frécinaux
 *    Be IP s.a. http://www.beip.be
 * Copyright (C) 2013 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * history:
 * ---------
 *  2013-01-xx  created (Steve Frécinaux)
 *  2013-01-xx  improved implementation of cachedb (vlad-paiu)
 *  2014-05-xx  full rework of the connection management (vlad-paiu)
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "../../cachedb/cachedb.h"
#include "../../db/db.h"
#include "../../timer.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../lib/osips_malloc.h"

typedef struct {
	struct cachedb_id *id;
	unsigned int ref;
	struct cachedb_pool_con_t *next;

	db_con_t* cdb_db_handle;
	db_func_t cdb_dbf;
    str table;
} cachedbsql_con;

#define MAX_RAW_QUERY_SIZE	512
static str cache_mod_name = str_init("sql");
struct cachedb_url *sql_script_urls = NULL;
static char query_buf[MAX_RAW_QUERY_SIZE];
static str query_str;

static int mod_init(void);
static int child_init(int rank);
static void destroy(void);

#define KEY_COL "keyname"
#define KEY_COL_LEN (sizeof(KEY_COL) - 1)
#define VALUE_COL "value"
#define VALUE_COL_LEN (sizeof(VALUE_COL) - 1)
#define COUNTER_VALUE_COL "counter"
#define COUNTER_VALUE_COL_LEN (sizeof(COUNTER_VALUE_COL) - 1)
#define EXPIRES_COL "expires"
#define EXPIRES_COL_LEN (sizeof(EXPIRES_COL) - 1)
#define MAX_NUM_COL 16

#define CACHEDB_SQL_TABLE_VERSION	2

static str db_table = str_init("cachedb");
static str key_column = {KEY_COL, KEY_COL_LEN};
static str value_column = {VALUE_COL, VALUE_COL_LEN};
static str counter_column = {COUNTER_VALUE_COL, COUNTER_VALUE_COL_LEN};
static str expires_column = {EXPIRES_COL, EXPIRES_COL_LEN};
static int cache_clean_period = 60;

int set_connection(unsigned int type, void *val)
{
	return cachedb_store_url(&sql_script_urls,(char *)val);
}

static param_export_t params[] = {
	{"cachedb_url",         STR_PARAM|USE_FUNC_PARAM, (void *)&set_connection },
	{"db_table",            STR_PARAM, &db_table.s         },
	{"key_column",          STR_PARAM, &key_column.s       },
	{"value_column",        STR_PARAM, &value_column.s     },
	{"counter_column",      STR_PARAM, &counter_column.s   },
	{"expires_column",      STR_PARAM, &expires_column.s   },
	{"cache_clean_period",  INT_PARAM, &cache_clean_period },
	{0, 0, 0}
};

/** module exports */
struct module_exports exports = {
	"cachedb_sql",               /* module name */
	MOD_TYPE_CACHEDB,/* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,            /* dlopen flags */
	0,				            /* load function */
	NULL,            /* OpenSIPS module dependencies */
	0,                          /* exported functions */
	0,                          /* exported async functions */
	params,                     /* exported parameters */
	0,                          /* exported statistics */
	0,                          /* exported MI functions */
	0,                          /* exported pseudo-variables */
	0,							/* exported transformations */
	0,                          /* extra processes */
	0,                          /* module pre-initialization function */
	mod_init,                   /* module initialization function */
	(response_function) 0,      /* response handling function */
	(destroy_function) destroy, /* destroy function */
	child_init,                 /* per-child init function */
	0                           /* reload confirm function */
};

#define CACHEDBSQL_DB_DELIMITER '-'
cachedbsql_con* dbcache_new_connection(struct cachedb_id* id)
{
	cachedbsql_con *con;
	str db_url;
	char *p,*end;
	int group_name_len,scheme_len;

	if(id == NULL) {
		LM_ERR("null db_id\n");
		return 0;
	}

	if((id->flags & (CACHEDB_ID_NO_URL | CACHEDB_ID_MULTIPLE_HOSTS)) != 0) {
		LM_ERR("bogus url for local cachedb\n");
		return 0;
	}

	if (id->group_name == NULL) {
		LM_ERR("No sql back-end info provided \n");
		return 0;
	}

	group_name_len = strlen(id->group_name);
	scheme_len = strlen(id->scheme);
	db_url.s = id->initial_url + scheme_len + 1;
	db_url.len = strlen(id->initial_url) - scheme_len - 1;

	for (p=id->group_name,end=p+group_name_len;p<end;p++) {
		if (*p == CACHEDBSQL_DB_DELIMITER) {
			db_url.s += (p-id->group_name) + 1;
			db_url.len -= (p-id->group_name) + 1;
			break;
		}
	}
    
    con = pkg_malloc(sizeof(cachedbsql_con));
	if(con == NULL) {
		LM_ERR("no more pkg\n");
		return 0;
	}

	memset(con,0,sizeof(cachedbsql_con));
	con->id = id;
	con->ref = 1;
    
    for (p = db_url.s + db_url.len - 1; p > db_url.s; p--) {
        if (*p == '/')
            break;
        // allow to specify table name by syntax "mysql://user:pass@domain:3306/database.table"
		if (*p == '.') {
            con->table.s = p + 1;
            con->table.len = db_url.len - (p - db_url.s) - 1;
			db_url.len = p - db_url.s;
			break;
		}
	}
    
    if (!con->table.s) {
        con->table = db_table;
    }

	if (db_bind_mod(&db_url, &con->cdb_dbf) < 0){
		LM_ERR("unable to bind to a database driver\n");
		pkg_free(con);
		return 0;
	}

	con->cdb_db_handle = con->cdb_dbf.init(&db_url);
	if (con->cdb_db_handle == 0) {
		LM_ERR("Failed to connect to the DB \n");
		pkg_free(con);
		return 0;
	}

	if(db_check_table_version(&con->cdb_dbf, con->cdb_db_handle,
	&db_table, CACHEDB_SQL_TABLE_VERSION) < 0) {
		LM_ERR("error during table version check.\n");
		con->cdb_dbf.close(con->cdb_db_handle);
		pkg_free(con);
		return 0;
	}

	return con;
}

static cachedb_con* dbcache_init(str *url)
{
	return cachedb_do_init(url, (void *)dbcache_new_connection);
}

void dbcache_free_connection(cachedb_pool_con *con)
{
	cachedbsql_con *c;	
	if (!con)
		return;

	c = (cachedbsql_con*)con;
	c->cdb_dbf.close(c->cdb_db_handle);
	pkg_free(c);
}

static void dbcache_destroy(cachedb_con *con)
{
	cachedb_do_close(con, dbcache_free_connection);
}

#define CACHEDBSQL_CON(c)  (((cachedbsql_con*)((c)->data))->cdb_db_handle)
#define CACHEDBSQL_FUNC(c) (((cachedbsql_con*)((c)->data))->cdb_dbf)
#define CACHEDBSQL_TABLE(c) (((cachedbsql_con*)((c)->data))->table)

static int dbcache_set(cachedb_con *con, str* attr, str* value, int expires)
{
	db_key_t keys[3];
	db_val_t vals[3];

	keys[0] = &key_column;
	keys[1] = &value_column;
	keys[2] = &expires_column;

	vals[0].type = DB_STR;
	vals[0].nul = 0;
	vals[0].val.str_val.s = attr->s;
	vals[0].val.str_val.len = attr->len;

	vals[1].type = DB_STR;
	vals[1].nul = 0;
	vals[1].val.str_val.s = value->s;
	vals[1].val.str_val.len = value->len;

	vals[2].type = DB_INT;
	vals[2].nul = 0;
	if (expires > 0)
		vals[2].val.int_val = (int)time(NULL) + expires;
	else
		vals[2].val.int_val = 0;

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &db_table) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}

	if (CACHEDBSQL_FUNC(con).insert_update(CACHEDBSQL_CON(con), keys, vals, 3) < 0) {
		LM_ERR("inserting cache entry in db failed\n");
		return -1;
	}

	return 1;
}

static int dbcache_get(cachedb_con *con, str* attr, str* res)
{
	db_key_t key;
	db_val_t val;
	db_key_t col;
	db_res_t* db_res = NULL;

	key = &key_column;

	val.type = DB_STR;
	val.nul = 0;
	val.val.str_val.s = attr->s;
	val.val.str_val.len = attr->len;

	col = &value_column;

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &db_table) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}

	if(CACHEDBSQL_FUNC(con).query(CACHEDBSQL_CON(con), &key, NULL, &val, 
	&col, 1, 1, NULL, &db_res) < 0) {
		LM_ERR("failed to query database\n");
		return -1;
	}

	if (db_res == NULL || RES_ROW_N(db_res) <= 0 || RES_ROWS(db_res)[0].values[0].nul != 0) {
		LM_DBG("no value found for keyI\n");
		if (db_res != NULL && CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con),db_res) < 0)
			LM_DBG("failed to free result of query\n");
		return -2;
	}

	switch(RES_ROWS(db_res)[0].values[0].type) {
		case DB_STRING:
			res->len = strlen((char*)RES_ROWS(db_res)[0].values[0].val.string_val);
			res->s = pkg_malloc(res->len + 1);

			if (res->s == NULL) {
				LM_ERR("no more pkg\n");
				goto out_err;
			}

			memcpy(res->s, (char*)RES_ROWS(db_res)[0].values[0].val.string_val, res->len);
			break;
		case DB_STR:
			res->len = RES_ROWS(db_res)[0].values[0].val.str_val.len;
			res->s = pkg_malloc(res->len + 1);

			if (res->s == NULL) {
				LM_ERR("no more pkg\n");
				goto out_err;
			}

			memcpy(res->s, (char*)RES_ROWS(db_res)[0].values[0].val.str_val.s, res->len);
			break;
		case DB_BLOB:
			res->len = RES_ROWS(db_res)[0].values[0].val.blob_val.len;
			res->s = pkg_malloc(res->len + 1);
			if (res->s == NULL) {
				LM_ERR("no more pkg\n");
				goto out_err;
			}
			memcpy(res->s, (char*)RES_ROWS(db_res)[0].values[0].val.blob_val.s, res->len);
			break;
		default:
			LM_ERR("unknown type of DB user column\n");
			goto out_err;
	}

	if (CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
		LM_DBG("failed to free result of query\n");

	return 1;

out_err:
	if (CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
		LM_DBG("failed to free result of query\n");

	return -1;
}

static int _dbcache_remove(cachedb_con *con, str* attr, const str *key)
{
	db_val_t val;

	val.type = DB_STR;
	val.nul = 0;
	val.val.str_val.s = attr->s;
	val.val.str_val.len = attr->len;

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &CACHEDBSQL_TABLE(con)) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}

	if (CACHEDBSQL_FUNC(con).delete(CACHEDBSQL_CON(con), (const db_key_t*)&key, 0, &val, 1) < 0) {
		LM_ERR("deleting from database failed\n");
		return -1;
	}

	return 0;
}

static int dbcache_remove(cachedb_con *con, str* attr) {
    return _dbcache_remove(con, attr, &key_column);
}

static int dbcache_add(cachedb_con *con, str *attr, int val, int expires, int *new_val)
{
	int i;
	db_res_t* res = NULL;

	if (expires > 0)
		expires += (int)time(NULL);
	else
		expires = 0;

	i = snprintf(query_buf, sizeof(query_buf),
				 "insert into %.*s (%.*s, %.*s, %.*s) values ('%.*s', %d, %d)"
				 "on duplicate key update %.*s=%.*s %c %d, %.*s=%d",
				 db_table.len, db_table.s,
				 key_column.len, key_column.s,
				 counter_column.len, counter_column.s,
				 expires_column.len, expires_column.s,
				 attr->len, attr->s,
				 val, expires,
				 counter_column.len, counter_column.s,
				 counter_column.len, counter_column.s,
				 val > 0 ? '+' : '-',
				 val > 0 ? val : -val,
				 expires_column.len, expires_column.s,
				 expires);

	if(i >= sizeof(query_buf)) {
		LM_ERR("DB query too long\n");
		return -1;
	}
	query_str.s = query_buf;
	query_str.len = i;

	if(CACHEDBSQL_FUNC(con).raw_query(CACHEDBSQL_CON(con), &query_str, &res) < 0) {
		LM_ERR("raw_query failed\n");
		return -1;
	}

	if(res != NULL)
		CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), res);

	/* Beware of the race conditions! */
	if(new_val) {
		str val;
		if (dbcache_get(con, attr, &val) < 0) {
			LM_ERR("could not get the new value\n");
			return -1;
		}
		*new_val = atoi(val.s);
		pkg_free(val.s);
	}

	return 0;
}

static int dbcache_sub(cachedb_con *con, str *attr, int val, int expires, int *new_val)
{
	return dbcache_add(con, attr, -val, expires, new_val);
}

static int dbcache_fetch_counter(cachedb_con *con,str *attr,int *ret_val)
{
	db_key_t key;
	db_val_t val;
	db_key_t col;
	db_res_t* db_res = NULL;

	key = &key_column;

	val.type = DB_STR;
	val.nul = 0;
	val.val.str_val.s = attr->s;
	val.val.str_val.len = attr->len;

	col = &counter_column;

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &db_table) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}

	if(CACHEDBSQL_FUNC(con).query(CACHEDBSQL_CON(con), &key, NULL, &val, &col, 1, 1, NULL, &db_res) < 0) {
		LM_ERR("failed to query database\n");
		return -1;
	}

	if (db_res == NULL || RES_ROW_N(db_res) <= 0 || RES_ROWS(db_res)[0].values[0].nul != 0) {
		LM_DBG("no value found for keyI\n");
		if (db_res != NULL && CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
			LM_DBG("failed to free result of query\n");
		return -2;
	}

	switch(RES_ROWS(db_res)[0].values[0].type) {
		case DB_INT:
			if (ret_val)
				*ret_val = RES_ROWS(db_res)[0].values[0].val.int_val;
			if (CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
				LM_ERR("failed to freeing result of query\n");
			break;
		default:
			LM_ERR("unknown type of DB user column\n");
			if (db_res != NULL && CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
				LM_ERR("failed to freeing result of query\n");
				return -1;
	}

	return 1;
}

static void dbcache_clean(unsigned int ticks, void* param)
{
	cachedb_pool_con **lst;
	cachedbsql_con *c;
	int size=0,i;

	db_key_t keys[2];
	db_op_t ops[2];
	db_val_t vals[2];

	keys[0] = &expires_column;
	keys[1] = &expires_column;

	ops[0] = OP_NEQ;
	ops[1] = OP_LT;

	vals[0].type = DB_INT;
	vals[0].nul = 0;
	vals[0].val.int_val = 0;

	vals[1].type = DB_INT;
	vals[1].nul = 0;
	vals[1].val.int_val = (int)time(NULL);

	lst = filter_pool_by_scheme(&cache_mod_name,&size);
	for (i=0;i<size;i++) {
		c = (cachedbsql_con*)(lst[i]);	
			
		if (c->cdb_dbf.use_table(c->cdb_db_handle, &db_table) < 0) {
			LM_ERR("sql use_table failed\n");
			return;
		}

		if (c->cdb_dbf.delete(c->cdb_db_handle, keys, ops, vals, 2) < 0) {
			LM_ERR("deleting from database failed\n");
			return;
		}
	}

	if (lst)
		pkg_free(lst);
}

static int append_dbcache_val_to_dict(const db_val_t *value, cdb_dict_t *cdb_dict,
									cdb_key_t *cdb_key)
{
	cdb_pair_t *pair;

	pair = cdb_mk_pair(cdb_key, NULL);
	if (!pair) {
		LM_ERR("oom\n");
		return -1;
	}

	if (VAL_NULL(value)) {
		pair->val.type = CDB_NULL;
		cdb_dict_add(pair, cdb_dict);
		return 0;
	}

	switch (VAL_TYPE(value)) {
	case DB_STR:
        pair->val.type = CDB_STR;
        if (pkg_str_dup(&pair->val.val.st, &VAL_STR(value)) < 0)
            goto error;
		break;
    case DB_STRING:
        pair->val.type = CDB_STR;
        pair->val.val.st.len = strlen(VAL_STRING(value));
        pair->val.val.st.s = pkg_malloc(pair->val.val.st.len);

        if (pair->val.val.st.s == NULL) {
            LM_ERR("no more pkg\n");
            goto error;
        }
        memcpy(pair->val.val.st.s, VAL_STRING(value), pair->val.val.st.len);
        
		break;
	case DB_INT:
		pair->val.type = CDB_INT32;
		pair->val.val.i32 = VAL_INT(value);
		break;
	default:
		LM_ERR("Unsupported SQL data type: %d\n", VAL_TYPE(value));
		return -1;
	}

	cdb_dict_add(pair, cdb_dict);

	return 0;

error:
	pkg_free(pair);
	return -1;
}

static int dbcache_result_to_cdb_res(const db_res_t *res, cdb_res_t *cdb_res)
{
	db_row_t* row;
	db_val_t *val;
	cdb_row_t *cdb_row = NULL;
	cdb_key_t cdb_key;
	int col_idx;
    int i;
    int nrows = RES_ROW_N(res);
    int ncols = RES_COL_N(res);

	cdb_res_init(cdb_res);

    for(i = 0; i < nrows; i++) {
		cdb_row = pkg_malloc(sizeof *cdb_row);
		if (!cdb_row) {
			LM_ERR("oom\n");
			goto error;
		}

		INIT_LIST_HEAD(&cdb_row->dict);
        
        row = RES_ROWS(res) + i;

		for (col_idx = 0; col_idx < ncols; col_idx++) {
            cdb_key.name = **(RES_NAMES(res) + col_idx);
			if (col_idx == 0)
				cdb_key.is_pk = 1;
			else
				cdb_key.is_pk = 0;

			val = ROW_VALUES(row) + col_idx;
			
            if (append_dbcache_val_to_dict(val, &cdb_row->dict, &cdb_key) < 0) {
				LM_ERR("Failed to add column to cdb result\n");
				cdb_free_entries(&cdb_row->dict, osips_pkg_free);
				goto error;
			}
		}

		cdb_res->count++;
		list_add_tail(&cdb_row->list, &cdb_res->rows);
	}

	return 0;

error:
	if (cdb_row)
		pkg_free(cdb_row);
	return -1;
}

/*
 * query function
 */
static int dbcache_col_query(cachedb_con *con, const cdb_filter_t *filter,
						cdb_res_t *cdb_res)
{
    db_key_t key[MAX_NUM_COL];
    db_op_t op[MAX_NUM_COL];
	db_val_t val[MAX_NUM_COL];
    int nkey = 0;
	db_res_t* db_res = NULL;
    const cdb_filter_t *f;

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &CACHEDBSQL_TABLE(con)) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}
    
	for (f = filter; f; f = f->next) {
        if (nkey >= MAX_NUM_COL) {
            LM_BUG("Maximum number of filter supported is %d\n", MAX_NUM_COL);
        }
        
        key[nkey] = &f->key.name;
        
        switch(f->op) {
        case CDB_OP_EQ:
			op[nkey] = OP_EQ;
			break;
		case CDB_OP_LT:
			op[nkey] = OP_LT;
			break;
		case CDB_OP_LTE:
			op[nkey] = OP_LEQ;
			break;
		case CDB_OP_GT:
			op[nkey] = OP_GT;
			break;
		case CDB_OP_GTE:
			op[nkey] = OP_GEQ;
			break;
		default:
			LM_BUG("unsupported operator: %d\n", f->op);
			return -1;
        }
        
        if (f->val.is_str) {
			val[nkey].type          = DB_STR;
            val[nkey].nul           = 0;
            val[nkey].val.str_val   = f->val.s;
		} else {
			val[nkey].type          = DB_INT;
            val[nkey].nul           = 0;
            val[nkey].val.int_val   = f->val.i;
		}
        
        nkey++;
    }

	if (CACHEDBSQL_FUNC(con).query(CACHEDBSQL_CON(con), key, op, val, NULL, nkey, 0, NULL, &db_res) < 0) {
		LM_ERR("failed to query database\n");
		return -1;
	}

	if (db_res == NULL) {
		LM_DBG("failed to query database\n");
		return -2;
	}
    
    if (dbcache_result_to_cdb_res(db_res, cdb_res) < 0) {
        if (CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0)
            LM_DBG("failed to free result of query\n");
        return -1;
    }
    
    if (CACHEDBSQL_FUNC(con).free_result(CACHEDBSQL_CON(con), db_res) < 0) {
		LM_DBG("failed to free result of query\n");
        return -1;
    }
    
    return 0;
}

static int dbcache_truncate(cachedb_con *con)
{
    char buf[128];
    int buf_len;
    str sql;
    
    if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &CACHEDBSQL_TABLE(con)) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}

	buf_len = snprintf(buf, 128, "TRUNCATE %.*s", db_table.len, db_table.s);
    
    sql.s = buf;
    sql.len = buf_len;

	if (buf_len < 0) {
		LM_ERR("Failed to build query string for cachedb 'truncate'\n");
		return -1;
	}

	if (CACHEDBSQL_FUNC(con).raw_query(CACHEDBSQL_CON(con), &sql, NULL) < 0) {
		LM_ERR("failed to query database\n");
		return -1;
	}
    
    return 0;
}

static int dbcache_col_update(cachedb_con *con, const cdb_filter_t *row_filter,
						const cdb_dict_t *pairs)
{
    db_key_t key[MAX_NUM_COL], set_key[MAX_NUM_COL];
    db_op_t op[MAX_NUM_COL];
	db_val_t val[MAX_NUM_COL], set_val[MAX_NUM_COL];
    int n_filter = 0, n_set = 0, n_equal = 0;
    const cdb_filter_t *f;
    int max_ttl = 0;
    struct list_head *_;
    cdb_pair_t *pair;
    char is_complex_filter = 0;

	if (!con) {
		LM_ERR("null parameter\n");
		return -1;
	}
    
    if (!row_filter) {
		LM_ERR("Updating all the rows at once is not supported\n");
		return -1;
	}

	if (CACHEDBSQL_FUNC(con).use_table(CACHEDBSQL_CON(con), &CACHEDBSQL_TABLE(con)) < 0) {
		LM_ERR("sql use_table failed\n");
		return -1;
	}
    
    list_for_each (_, pairs) {
        pair = list_entry(_, cdb_pair_t, list);
        if (!pair->key.name.s)
            continue;

        if (pair->ttl > max_ttl)
            max_ttl = pair->ttl;
        
        if (n_set >= MAX_NUM_COL) {
            LM_BUG("Maximum number of update column supported is %d\n", MAX_NUM_COL);
        }

        if (pair->subkey.s) {
            LM_BUG("Cachedb SQL does not support subkey\n");
        } else {
            set_key[n_set] = &pair->key.name;
            switch (pair->val.type) {
            case CDB_NULL:
                set_val[n_set].nul = 1;
                break;
            case CDB_INT32:
                set_val[n_set].type         = DB_INT;
                set_val[n_set].nul          = 0;
                set_val[n_set].val.int_val  = pair->val.val.i32;
                break;
            case CDB_INT64:
                set_val[n_set].type             = DB_INT;
                set_val[n_set].nul              = 0;
                set_val[n_set].val.bigint_val   = pair->val.val.i64;
                break;
            case CDB_STR:
                set_val[n_set].type         = DB_STR;
                set_val[n_set].nul          = 0;
                set_val[n_set].val.str_val  = pair->val.val.st;
                break;
            default:
                LM_ERR("unsupported type %d for key %.*s\n", pair->val.type,
                       pair->key.name.len, pair->key.name.s);
                return -1;
            }
            
            n_set++;
        }
    }
    
    n_equal = n_set;
    
	for (f = row_filter; f; f = f->next) {
        if (n_filter >= MAX_NUM_COL) {
            LM_BUG("Maximum number of filter supported is %d\n", MAX_NUM_COL);
        }
        
        key[n_filter] = &f->key.name;
        
        switch(f->op) {
        case CDB_OP_EQ:
			op[n_filter] = OP_EQ;
            
            if (n_equal >= MAX_NUM_COL) {
                LM_BUG("Maximum number of update & filter column supported is %d\n", MAX_NUM_COL);
            }
            
            set_key[n_equal] = &f->key.name;
            if (f->val.is_str) {
                set_val[n_equal].type           = DB_STR;
                set_val[n_equal].nul            = 0;
                set_val[n_equal].val.str_val    = f->val.s;
            } else {
                set_val[n_equal].type           = DB_INT;
                set_val[n_equal].nul            = 0;
                set_val[n_equal].val.int_val    = f->val.i;
            }
            
            n_equal++;
            
			break;
		case CDB_OP_LT:
			op[n_filter] = OP_LT;
			break;
		case CDB_OP_LTE:
			op[n_filter] = OP_LEQ;
			break;
		case CDB_OP_GT:
			op[n_filter] = OP_GT;
			break;
		case CDB_OP_GTE:
			op[n_filter] = OP_GEQ;
			break;
		default:
			LM_BUG("unsupported operator: %d\n", f->op);
			return -1;
        }
        
        if (f->val.is_str) {
			val[n_filter].type          = DB_STR;
            val[n_filter].nul           = 0;
            val[n_filter].val.str_val   = f->val.s;
		} else {
			val[n_filter].type          = DB_INT;
            val[n_filter].nul           = 0;
            val[n_filter].val.int_val   = f->val.i;
		}
        
        if (f->op != CDB_OP_EQ) {
            is_complex_filter = 1;
        }
        
        n_filter++;
    }
    
    if (CACHEDBSQL_FUNC(con).insert_update(CACHEDBSQL_CON(con), set_key, set_val, n_equal) < 0) {
		LM_ERR("failed to query database\n");
		return -1;
	}

    // if there is complex op (different than = op) in filter, we have to run an extra update, otherwise, just a INSERT UPDATE above is enough
    if (is_complex_filter) {
        if (CACHEDBSQL_FUNC(con).update(CACHEDBSQL_CON(con), key, op, val, set_key, set_val, n_filter, n_set) < 0) {
            LM_ERR("failed to query database\n");
            return -1;
        }
    }
    
    return 0;
}

/**
 * init module function
 */
static int mod_init(void)
{
	cachedb_engine cde;

	LM_INFO("initializing module cachedb_sql...\n");
	memset(&cde, 0, sizeof cde);

	db_table.len = strlen(db_table.s);
	key_column.len = strlen(key_column.s);
	value_column.len = strlen(value_column.s);
	counter_column.len = strlen(counter_column.s);
	expires_column.len = strlen(expires_column.s);

	/* register the cache system */
	cde.name = cache_mod_name;

	cde.cdb_func.init = dbcache_init;
	cde.cdb_func.destroy = dbcache_destroy;
	cde.cdb_func.get = dbcache_get;
	cde.cdb_func.set = dbcache_set;
	cde.cdb_func.remove = dbcache_remove;
    cde.cdb_func._remove = _dbcache_remove;
	cde.cdb_func.add = dbcache_add;
	cde.cdb_func.sub = dbcache_sub;
	cde.cdb_func.get_counter = dbcache_fetch_counter;
    cde.cdb_func.query = dbcache_col_query;
	cde.cdb_func.update = dbcache_col_update;
	cde.cdb_func.truncate = dbcache_truncate;
	cde.cdb_func.capability = 0;

	if(cache_clean_period <= 0) {
			LM_ERR("wrong parameter cache_clean_period - need a positive value\n");
			return -1;
	}

	if(register_cachedb(&cde) < 0) {
			LM_ERR("failed to register to core memory store interface\n");
			return -1;
	}

	/* register timer to delete the expired entries */
	register_timer("cachedb_sql",dbcache_clean, 0, cache_clean_period,
		TIMER_FLAG_DELAY_ON_DELAY);

	return 0;
}

/**
 * Initialize children
 */
static int child_init(int rank)
{
	struct cachedb_url *it;
	cachedb_con *con;

	for (it = sql_script_urls;it;it=it->next) {
		LM_DBG("iterating through conns - [%.*s]\n",it->url.len,it->url.s);
		con = dbcache_init(&it->url);
		if (con == NULL) {
			LM_ERR("failed to open connection\n");
			return -1;
		}
		if (cachedb_put_connection(&cache_mod_name,con) < 0) {
			LM_ERR("failed to insert connection\n");
			return -1;
		}
	}

	cachedb_free_url(sql_script_urls);

	return 0;
}

/*
 * destroy function
 */
static void destroy(void)
{
	cachedb_end_connections(&cache_mod_name);
}