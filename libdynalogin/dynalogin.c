/*
 * dynalogin.c
 *
 *      Implementation of the public API functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>

#include <apr_dso.h>
#include <apr_file_io.h>
#include <apr_hash.h>
#include <apr_pools.h>
#include <apr_strings.h>

#include <oath.h>

#include "dynalogin.h"
#include "dynalogin-datastore.h"
#include "hotpdigest.h"

#define DIR_SEPARATOR '/'

#define HOTP_WINDOW 20
#define HOTP_DIGITS 6

/* In seconds */
#define TOTP_STEP_SIZE_X 30
/* Offset from UNIX time 0, in seconds */
#define TOTP_OFFSET_T0 0
#define TOTP_DIGITS 6
/* the TOTP_WINDOW is multiples of TOTP_STEP_SIZE_X */
#define TOTP_WINDOW 2

#define CFG_LINEBUF_LEN 512

#define DYNALOGIN_PARAM_DSNAME "dynalogin.datasource"

#define GET_STRING_PARAM(r, m, s) \
	if((r = (char *)apr_hash_get(m, s, APR_HASH_KEY_STRING)) == NULL) \
		{ syslog(LOG_ERR, "missing parameter %s", s); return DYNALOGIN_ERROR; }

#define ERRMSG(s) syslog(LOG_ERR, s)

const char *scheme_names[] = { "HOTP", "TOTP", NULL };

struct oath_callback_pvt_t
{
	apr_pool_t *pool;
	const char *code;
        const char *password;
};

/*
 * Initialise a HOTP authentication stack.
 *
 */
dynalogin_result_t dynalogin_init(dynalogin_session_t **session,
		apr_pool_t *pool, apr_hash_t *config)
{
	apr_status_t ret;
	dynalogin_session_t *h;
	char *ds_module_name;
	char *ds_module_filename;
	char *ds_sym_name;

	*session = NULL;

	if(oath_init() != OATH_OK)
	{
		ERRMSG("libhotp init failed");
		return DYNALOGIN_ERROR;
	}

	if(pool == NULL)
	{
		return DYNALOGIN_ERROR;
	}

	/* pool is not required, APR will create a root pool if it
	 * is NULL.
	 * Maybe we don't need a config and can run on defaults? */
	if(config == NULL)
	{
		ERRMSG("no config");
		return DYNALOGIN_ERROR;
	}

	if((h = (dynalogin_session_t *)apr_pcalloc(pool, sizeof(dynalogin_session_t)))
			== NULL)
		return DYNALOGIN_ERROR;
	h->pool = pool;

	syslog(LOG_DEBUG, "looking in %s for modules", PKGLIBDIR);

	GET_STRING_PARAM(ds_module_name, config, DYNALOGIN_PARAM_DSNAME)

	ds_module_filename = apr_psprintf(h->pool, "%s%c%s.so",
			PKGLIBDIR, DIR_SEPARATOR, ds_module_name);
	ds_sym_name = apr_psprintf(h->pool, "%s_module", ds_module_name);
	if(ds_module_filename==NULL || ds_sym_name==NULL)
		return DYNALOGIN_ERROR;

	if(apr_dso_load(&h->dso_handle,
			ds_module_filename, h->pool)!=APR_SUCCESS)
		return DYNALOGIN_ERROR;

	if(apr_dso_sym((apr_dso_handle_sym_t *)&(h->datasource), h->dso_handle, ds_sym_name)
			!=APR_SUCCESS)
	{
		apr_dso_unload(h->dso_handle);
		return DYNALOGIN_ERROR;
	}

	if(h->datasource->init != NULL)
		if(h->datasource->init(h->pool, config) != DYNALOGIN_SUCCESS)
		{
			apr_dso_unload(h->dso_handle);
			return DYNALOGIN_ERROR;
		}

	*session = h;

	return DYNALOGIN_SUCCESS;
}

void dynalogin_done(dynalogin_session_t *h)
{
	if(h==NULL)
		return;
	if(h->datasource->done != NULL)
		h->datasource->done();
	apr_dso_unload(h->dso_handle);
	oath_done();
}

int oath_callback(void *handle, const char *test_otp) {
	const char *password = "";
	char *test_str;

	struct oath_callback_pvt_t *pvt =
		(struct oath_callback_pvt_t *)handle;
	if(pvt->password != NULL)
		password = pvt->password;	

	if((test_str = apr_pstrcat(pvt->pool,
             password, test_otp, NULL)) == NULL)
	{
		return -1;
	}

	return strcmp(pvt->code, test_str);
}

dynalogin_result_t dynalogin_authenticate
	(dynalogin_session_t *h, const dynalogin_userid_t userid,
			dynalogin_scheme_t scheme, const dynalogin_code_t code)
{
	int rc;
	dynalogin_user_data_t *ud;
	dynalogin_result_t res;
	struct oath_callback_pvt_t pvt;
	time_t now;
	dynalogin_counter_t _now, next_counter;
	int fail_inc = 1;

	if(h == NULL || userid == NULL || code == NULL)
		return DYNALOGIN_ERROR;

	h->datasource->user_fetch(&ud, userid, h->pool);
	if(ud == NULL)
	{
		syslog(LOG_ERR, "userid not found: %s", userid);
		return DYNALOGIN_DENY;
	}

	if(ud->scheme != scheme)
	{
		syslog(LOG_ERR, "scheme mismatch for user %s (expected %s)",
				userid, get_scheme_name(ud->scheme));
		return DYNALOGIN_DENY;
	}

	if(ud->locked != 0)
	{
		syslog(LOG_ERR, "account locked: %s", userid);
		return DYNALOGIN_DENY;
	}

	pvt.code = code;
	pvt.password = ud->password;

	if(apr_pool_create(&(pvt.pool), h->pool) != APR_SUCCESS)
	{
		return DYNALOGIN_ERROR;
	}

	switch(scheme)
	{
	case HOTP:
		rc = oath_hotp_validate_callback (
				ud->secret,
				strlen(ud->secret),
				ud->counter,
				HOTP_WINDOW, HOTP_DIGITS,
				oath_callback,
				&pvt);
		next_counter = ud->counter + (rc + 1);
		break;
	case TOTP:
		time(&now);
		_now = (now - TOTP_OFFSET_T0) / TOTP_STEP_SIZE_X;
		if(_now >= ud->counter)
			rc = oath_totp_validate_callback (
					ud->secret,
					strlen(ud->secret),
					now,
					TOTP_STEP_SIZE_X,
					TOTP_OFFSET_T0,
					TOTP_DIGITS,
					TOTP_WINDOW,
					oath_callback,
					&pvt);
		else
		{
			fail_inc = 0;
			rc = OATH_REPLAYED_OTP;
		}
		next_counter = _now + (rc + 1);
		break;
	default:
		syslog(LOG_ERR, "unsupported scheme");
		fail_inc = 0;
		rc = -1;
	}
	apr_pool_destroy(pvt.pool);
	if(rc < 0)
	{
		ud->failure_count += fail_inc;
		res = DYNALOGIN_DENY;
	}
	else
	{
		ud->counter = next_counter;
		ud->failure_count = 0;
		time(&ud->last_success);
		ud->last_code = code;
		res = DYNALOGIN_SUCCESS;
	}
	time(&ud->last_attempt);
	h->datasource->user_update(ud, h->pool);
	return res;
}

dynalogin_result_t dynalogin_authenticate_digest
	(dynalogin_session_t *h, const dynalogin_userid_t userid,
			dynalogin_scheme_t scheme,
			const char *response, const char *realm,
			const char *digest_suffix)
{
	int rc;
	dynalogin_user_data_t *ud;
	dynalogin_result_t res;
	struct oath_digest_callback_pvt_t pvt;
	time_t now;
	dynalogin_counter_t _now, next_counter;
	int fail_inc = 1;

	if(h == NULL || userid == NULL || response == NULL ||
		realm == NULL || digest_suffix == NULL)
		return DYNALOGIN_ERROR;

	h->datasource->user_fetch(&ud, userid, h->pool);
	if(ud == NULL)
	{
		syslog(LOG_ERR, "userid not found: %s", userid);
		return DYNALOGIN_DENY;
	}

	if(ud->scheme != scheme)
	{
		syslog(LOG_ERR, "scheme mismatch for user %s (expected %s)",
				userid, get_scheme_name(ud->scheme));
		return DYNALOGIN_DENY;
	}

	if(ud->locked != 0)
	{
		syslog(LOG_ERR, "account locked: %s", userid);
		return DYNALOGIN_DENY;
	}

	pvt.response = response;
	pvt.username = userid;
	pvt.realm = realm;
	pvt.digest_suffix = digest_suffix;
	pvt.password = ud->password;

	if(apr_pool_create(&(pvt.pool), h->pool) != APR_SUCCESS)
	{
		return DYNALOGIN_ERROR;
	}

	switch(scheme)
	{
	case HOTP:
		rc = oath_hotp_validate_callback (
				ud->secret,
				strlen(ud->secret),
				ud->counter,
				HOTP_WINDOW, HOTP_DIGITS,
				oath_digest_callback,
				&pvt);
		next_counter = ud->counter + (rc + 1);
		break;
	case TOTP:
		time(&now);
		_now = (now - TOTP_OFFSET_T0) / TOTP_STEP_SIZE_X;
		if(_now >= ud->counter)
			rc = oath_totp_validate_callback (
					ud->secret,
					strlen(ud->secret),
					now,
					TOTP_STEP_SIZE_X,
					TOTP_OFFSET_T0,
					TOTP_DIGITS,
					TOTP_WINDOW,
					oath_digest_callback,
					&pvt);
		else
		{
			fail_inc = 0;
			rc = OATH_REPLAYED_OTP;
		}
		next_counter = _now + (rc + 1);
		break;
	default:
		syslog(LOG_ERR, "unsupported scheme");
		fail_inc = 0;
		rc = -1;
	}
	apr_pool_destroy(pvt.pool);
	if(rc < 0)
	{
		ud->failure_count += fail_inc;
		res = DYNALOGIN_DENY;
	}
	else
	{
		ud->counter = next_counter;
		ud->failure_count = 0;
		time(&ud->last_success);
		ud->last_code = "000000";
		res = DYNALOGIN_SUCCESS;
	}
	time(&ud->last_attempt);
	h->datasource->user_update(ud, h->pool);
	return res;
}


dynalogin_result_t dynalogin_read_config_from_file(apr_hash_t **config,
		const char *filename, apr_pool_t *pool)
{
	apr_hash_t *_config = NULL;
	apr_status_t res;
	apr_file_t *f;
	char buf[CFG_LINEBUF_LEN + 1];
	size_t len;
	int i;
	char *key, *val;

	*config = NULL;

	if((_config=apr_hash_make(pool)) == NULL)
		return DYNALOGIN_ERROR;

	if(res=apr_file_open(&f, filename, APR_READ | APR_SHARELOCK, 0, pool)
			!= APR_SUCCESS)
		return DYNALOGIN_ERROR;

	while(apr_file_gets(buf, CFG_LINEBUF_LEN, f) == APR_SUCCESS)
	{
		len = strlen(buf);
		if(len > 0 && buf[len-1]=='\n')
			buf[--len] = 0;

		/* Lines starting with ; or # are comments, ignore them */
		if(buf[0] == ';' || buf[0] == '#')
			continue;

		for(i = 0; i < len && buf[i] != '='; i++);

		if(buf[i] == '=' && i > 0)
		{
			buf[i] = 0;
			if((val = apr_pstrdup(pool, &buf[i+1])) == NULL)
			{
				apr_file_close(f);
				return DYNALOGIN_ERROR;
			}
			if((key = apr_pstrdup(pool, buf))==NULL)
			{
				apr_file_close(f);
				return DYNALOGIN_ERROR;
			}
			apr_hash_set(_config, key, APR_HASH_KEY_STRING, val);
		}
	}

	apr_file_close(f);

	*config = _config;

	return DYNALOGIN_SUCCESS;
}


dynalogin_scheme_t get_scheme_by_name(const char *scheme_name)
{
	int i;
	for(i = 0; scheme_names[i] != NULL; i++)
		if(strcmp(scheme_names[i], scheme_name) == 0)
			return i;
	return -1;
}

const char *get_scheme_name(dynalogin_scheme_t scheme)
{
	int scheme_count;
	for(scheme_count = 0; scheme_names[scheme_count] != NULL; scheme_count++)
		if(scheme == scheme_count)
			return scheme_names[scheme];
	return NULL;
}
