/* passwd.c - password extended operation routines */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2003 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/krb.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/unistd.h>

#include "slap.h"

#include <lber_pvt.h>
#include <lutil.h>

int passwd_extop(
	Operation *op,
	SlapReply *rs )
{
	struct berval id = {0, NULL}, old = {0, NULL}, new = {0, NULL},
		dn, ndn, hash, vals[2];
	int freenew = 0;
	Modifications ml, **modtail;
	Operation op2;
	slap_callback cb = { slap_null_cb, NULL };

	assert( ber_bvcmp( &slap_EXOP_MODIFY_PASSWD, &op->ore_reqoid ) == 0 );

	if( op->o_dn.bv_len == 0 ) {
		rs->sr_text = "only authenticated users may change passwords";
		return LDAP_STRONG_AUTH_REQUIRED;
	}

	rs->sr_err = slap_passwd_parse( op->oq_extended.rs_reqdata, &id,
		&old, &new, &rs->sr_text );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		return rs->sr_err;
	}

	if ( id.bv_len ) {
		dn = id;
		/* ndn is in tmpmem, so we don't need to free it */
		rs->sr_err = dnNormalize( 0, NULL, NULL, &dn, &ndn, op->o_tmpmemctx );
		if ( rs->sr_err != LDAP_SUCCESS ) {
			rs->sr_text = "Invalid DN";
			return rs->sr_err;
		}
		op->o_bd = select_backend( &ndn, 0, 0 );
	} else {
		dn = op->o_dn;
		ndn = op->o_ndn;
		ldap_pvt_thread_mutex_lock( &op->o_conn->c_mutex );
		op->o_bd = op->o_conn->c_authz_backend;
		ldap_pvt_thread_mutex_unlock( &op->o_conn->c_mutex );
	}

	if ( ndn.bv_len == 0 ) {
		rs->sr_text = "no password is associated with the Root DSE";
		return LDAP_UNWILLING_TO_PERFORM;
	}

	if( op->o_bd && !op->o_bd->be_modify ) {
		rs->sr_text = "operation not supported for current user";
		return LDAP_UNWILLING_TO_PERFORM;
	}

	if (backend_check_restrictions( op, rs,
			(struct berval *)&slap_EXOP_MODIFY_PASSWD ) != LDAP_SUCCESS) {
		return rs->sr_err;
	}

	if( op->o_bd == NULL ) {
#ifdef HAVE_CYRUS_SASL
		return slap_sasl_setpass( op, rs );
#else
		rs->sr_text = "no authz backend";
		return LDAP_OTHER;
#endif
	}

#ifndef SLAPD_MULTIMASTER
	/* This does not apply to multi-master case */
	if( op->o_bd->be_update_ndn.bv_len ) {
		/* we SHOULD return a referral in this case */
		BerVarray defref = NULL;
		if ( !LDAP_STAILQ_EMPTY( &op->o_bd->be_syncinfo )) {
			syncinfo_t *si;
			LDAP_STAILQ_FOREACH( si, &op->o_bd->be_syncinfo, si_next ) {
				struct berval tmpbv;
				ber_dupbv( &tmpbv, &si->si_provideruri_bv[0] );
				ber_bvarray_add( &defref, &tmpbv );
			}
		} else {
			defref = referral_rewrite( op->o_bd->be_update_refs,
				NULL, NULL, LDAP_SCOPE_DEFAULT );
		}
		rs->sr_ref = defref;
		return LDAP_REFERRAL;
	}
#endif /* !SLAPD_MULTIMASTER */
	if ( new.bv_len == 0 ) {
		slap_passwd_generate( &new );
		freenew = 1;
	}
	if ( new.bv_len == 0 ) {
		rs->sr_text = "password generation failed";
		return LDAP_OTHER;
	}
	slap_passwd_hash( &new, &hash, &rs->sr_text );
	if ( freenew ) {
		free( new.bv_val );
	}
	if ( hash.bv_len == 0 ) {
		if ( !rs->sr_text ) {
			rs->sr_text = "password hash failed";
		}
		return LDAP_OTHER;
	}
	vals[0] = hash;
	vals[1].bv_val = NULL;
	ml.sml_desc = slap_schema.si_ad_userPassword;
	ml.sml_values = vals;
	ml.sml_nvalues = NULL;
	ml.sml_op = LDAP_MOD_REPLACE;
	ml.sml_next = NULL;

	op2 = *op;
	op2.o_tag = LDAP_REQ_MODIFY;
	op2.o_callback = &cb;
	op2.o_req_dn = dn;
	op2.o_req_ndn = ndn;
	op2.orm_modlist = &ml;

	modtail = &ml.sml_next;
	rs->sr_err = slap_mods_opattrs( &op2, &ml, modtail, &rs->sr_text,
		NULL, 0 );
	
	if ( rs->sr_err == LDAP_SUCCESS ) {
		rs->sr_err = op2.o_bd->be_modify( &op2, rs );
		if ( rs->sr_err == LDAP_SUCCESS ) {
			replog( &op2 );
		}
	}
	slap_mods_free( ml.sml_next );
	free( hash.bv_val );

	return rs->sr_err;
}

int slap_passwd_parse( struct berval *reqdata,
	struct berval *id,
	struct berval *oldpass,
	struct berval *newpass,
	const char **text )
{
	int rc = LDAP_SUCCESS;
	ber_tag_t tag;
	ber_len_t len;
	BerElementBuffer berbuf;
	BerElement *ber = (BerElement *)&berbuf;

	if( reqdata == NULL ) {
		return LDAP_SUCCESS;
	}

	if( reqdata->bv_len == 0 ) {
		*text = "empty request data field";
		return LDAP_PROTOCOL_ERROR;
	}

	/* ber_init2 uses reqdata directly, doesn't allocate new buffers */
	ber_init2( ber, reqdata, 0 );

	tag = ber_scanf( ber, "{" /*}*/ );

	if( tag != LBER_ERROR ) {
		tag = ber_peek_tag( ber, &len );
	}

	if( tag == LDAP_TAG_EXOP_MODIFY_PASSWD_ID ) {
		if( id == NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse: ID not allowed.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: ID not allowed.\n",
				0, 0, 0 );
#endif

			*text = "user must change own password";
			rc = LDAP_UNWILLING_TO_PERFORM;
			goto done;
		}

		tag = ber_scanf( ber, "m", id );

		if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse:  ID parse failed.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: ID parse failed.\n",
				0, 0, 0 );
#endif

			goto decoding_error;
		}

		tag = ber_peek_tag( ber, &len);
	}

	if( tag == LDAP_TAG_EXOP_MODIFY_PASSWD_OLD ) {
		if( oldpass == NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse: OLD not allowed.\n" , 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: OLD not allowed.\n",
				0, 0, 0 );
#endif

			*text = "use bind to verify old password";
			rc = LDAP_UNWILLING_TO_PERFORM;
			goto done;
		}

		tag = ber_scanf( ber, "m", oldpass );

		if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse:  ID parse failed.\n" , 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: ID parse failed.\n",
				0, 0, 0 );
#endif

			goto decoding_error;
		}

		tag = ber_peek_tag( ber, &len );
	}

	if( tag == LDAP_TAG_EXOP_MODIFY_PASSWD_NEW ) {
		if( newpass == NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse:  NEW not allowed.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: NEW not allowed.\n",
				0, 0, 0 );
#endif

			*text = "user specified passwords disallowed";
			rc = LDAP_UNWILLING_TO_PERFORM;
			goto done;
		}

		tag = ber_scanf( ber, "m", newpass );

		if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "slap_passwd_parse:  OLD parse failed.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_TRACE, "slap_passwd_parse: OLD parse failed.\n",
				0, 0, 0 );
#endif

			goto decoding_error;
		}

		tag = ber_peek_tag( ber, &len );
	}

	if( len != 0 ) {
decoding_error:
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"slap_passwd_parse: decoding error, len=%ld\n", (long)len, 0, 0 );
#else
		Debug( LDAP_DEBUG_TRACE,
			"slap_passwd_parse: decoding error, len=%ld\n",
			(long) len, 0, 0 );
#endif

		*text = "data decoding error";
		rc = LDAP_PROTOCOL_ERROR;
	}

done:
	return rc;
}

struct berval * slap_passwd_return(
	struct berval		*cred )
{
	int rc;
	struct berval *bv = NULL;
	BerElementBuffer berbuf;
	/* opaque structure, size unknown but smaller than berbuf */
	BerElement *ber = (BerElement *)&berbuf;

	assert( cred != NULL );

#ifdef NEW_LOGGING
	LDAP_LOG( OPERATION, ENTRY, 
		"slap_passwd_return: %ld\n",(long)cred->bv_len, 0, 0 );
#else
	Debug( LDAP_DEBUG_TRACE, "slap_passwd_return: %ld\n",
		(long) cred->bv_len, 0, 0 );
#endif
	
	ber_init_w_nullc( ber, LBER_USE_DER );

	rc = ber_printf( ber, "{tON}",
		LDAP_TAG_EXOP_MODIFY_PASSWD_GEN, cred );

	if( rc >= 0 ) {
		(void) ber_flatten( ber, &bv );
	}

	ber_free_buf( ber );

	return bv;
}

int
slap_passwd_check(
	Connection *conn,
	Attribute *a,
	struct berval *cred,
	const char **text )
{
	int result = 1;
	struct berval *bv;

#if defined( SLAPD_CRYPT ) || defined( SLAPD_SPASSWD )
	ldap_pvt_thread_mutex_lock( &passwd_mutex );
#ifdef SLAPD_SPASSWD
	lutil_passwd_sasl_conn = conn->c_sasl_authctx;
#endif
#endif

	for ( bv = a->a_vals; bv->bv_val != NULL; bv++ ) {
		if( !lutil_passwd( bv, cred, NULL, text ) ) {
			result = 0;
			break;
		}
	}

#if defined( SLAPD_CRYPT ) || defined( SLAPD_SPASSWD )
#ifdef SLAPD_SPASSWD
	lutil_passwd_sasl_conn = NULL;
#endif
	ldap_pvt_thread_mutex_unlock( &passwd_mutex );
#endif

	return result;
}

void
slap_passwd_generate( struct berval *pass )
{
	struct berval *tmp;
#ifdef NEW_LOGGING
	LDAP_LOG( OPERATION, ENTRY, "slap_passwd_generate: begin\n", 0, 0, 0 );
#else
	Debug( LDAP_DEBUG_TRACE, "slap_passwd_generate\n", 0, 0, 0 );
#endif
	/*
	 * generate passwords of only 8 characters as some getpass(3)
	 * implementations truncate at 8 characters.
	 */
	tmp = lutil_passwd_generate( 8 );
	if (tmp) {
		*pass = *tmp;
		free(tmp);
	} else {
		pass->bv_val = NULL;
		pass->bv_len = 0;
	}
}

void
slap_passwd_hash(
	struct berval * cred,
	struct berval * new,
	const char **text )
{
	struct berval *tmp;
#ifdef LUTIL_SHA1_BYTES
	char* hash = default_passwd_hash ?  default_passwd_hash : "{SSHA}";
#else
	char* hash = default_passwd_hash ?  default_passwd_hash : "{SMD5}";
#endif
	

#if defined( SLAPD_CRYPT ) || defined( SLAPD_SPASSWD )
	ldap_pvt_thread_mutex_lock( &passwd_mutex );
#endif

	tmp = lutil_passwd_hash( cred , hash, text );
	
#if defined( SLAPD_CRYPT ) || defined( SLAPD_SPASSWD )
	ldap_pvt_thread_mutex_unlock( &passwd_mutex );
#endif

	if( tmp == NULL ) {
		new->bv_len = 0;
		new->bv_val = NULL;
	}

	*new = *tmp;
	free( tmp );
	return;
}
