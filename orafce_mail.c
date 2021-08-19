#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"

#include <curl/curl.h>
#include <mb/pg_wchar.h>
#include <signal.h>
#include <string.h>
#include <utils/guc.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(orafce_mail_send);
PG_FUNCTION_INFO_V1(orafce_mail_send_attach_raw);
PG_FUNCTION_INFO_V1(orafce_mail_send_attach_varchar2);
PG_FUNCTION_INFO_V1(orafce_mail_dbms_mail_send);

void _PG_init(void);
void _PG_fini(void);

typedef struct
{
	char	    *data;
	size_t		size;
	size_t		used;
} DynBuffer;

typedef struct
{
	char	   *ptr;
	size_t		size;
	size_t		processed;
} StringReader;

/*
 * Invisible super user settings
 */
char	   *orafce_smtp_url = NULL;
char	   *orafce_smtp_userpwd = NULL;

/*
* Interrupt support is dependent on CURLOPT_XFERINFOFUNCTION which is
* only available from 7.32.0 and up
*/
#if LIBCURL_VERSION_NUM >= 0x072700 /* 7.39.0 */

pqsigfunc pgsql_interrupt_handler = NULL;
int interrupt_requested = 0;

/*
* To support request interruption, we have libcurl run the progress meter
* callback frequently, and here we watch to see if PgSQL has flipped our
* global 'http_interrupt_requested' flag. If it has been flipped,
* the non-zero return value will cue libcurl to abort the transfer,
* leading to a CURLE_ABORTED_BY_CALLBACK return on the curl_easy_perform()
*/
static int
progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
#ifdef WIN32

	if (UNBLOCKED_SIGNAL_QUEUE())
	{
		pgwin32_dispatch_queued_signals();
	}

#endif

	(void) clientp;
	(void) dltotal;
	(void) dlnow;
	(void) ultotal;
	(void) ulnow;

	/* elog(DEBUG3, "http_interrupt_requested = %d", http_interrupt_requested); */
	return interrupt_requested;
}

/*
* We register this callback with the PgSQL signal handler to
* capture SIGINT and set our local interupt flag so that
* libcurl will eventually notice that a cancel is requested
*/
static void
http_interrupt_handler(int sig)
{
	/* Handle the signal here */
	interrupt_requested = sig;
	pgsql_interrupt_handler(sig);

	return;
}

#endif /* 7.39.0 */


/*
 * Input argument checks
 */
static Datum
not_null_arg(FunctionCallInfo fcinfo, int argno, const char *fcname, const char *argname)
{
	if (PG_ARGISNULL(argno))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("NULL is not allowed"),
				 errhint("The value of argument \"%s\" of function \"%s\" is NULL.",
						  argname,
						  fcname)));

	return PG_GETARG_DATUM(argno);
}

static char *
null_or_empty_arg(FunctionCallInfo fcinfo, int argno)
{
	text	   *txt;

	if (PG_ARGISNULL(argno))
		return NULL;

	txt = DatumGetTextPP(PG_GETARG_DATUM(argno));

	return VARSIZE_ANY_EXHDR(txt) > 0 ? text_to_cstring(txt) : NULL;
}

static char *
not_empty_darg(Datum d, const char *fcname, const char *argname)
{
	text	   *txt = DatumGetTextPP(d);

	if (VARSIZE_ANY_EXHDR(txt) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty string is not allowed"),
				 errhint("The value of argument \"%s\" of function \"%s\" is empty string.",
						  argname,
						  fcname)));

	return text_to_cstring(txt);
}

static char *
not_null_not_empty_arg(FunctionCallInfo fcinfo, int argno, const char *fcname, const char *argname)
{
	return
		not_empty_darg(
			not_null_arg(fcinfo,
						 argno,
						 fcname,
						 argname),
			fcname,
			argname);
}

/*
 * Parse string as comma delimited list.
 *
 * Attention, this function modifies input string.
 */
static struct curl_slist *
add_fields(struct curl_slist *sl, char *str)
{
	char	   *tok;

	if (!str)
		return sl;

	tok = strtok(str, ",");
	while (tok)
	{
		sl = curl_slist_append(sl, tok);
		tok = strtok(NULL, ",");
	}

	return sl;
}

static void
dynbuf_add_bytes(DynBuffer *buf, const char *str, size_t bytes)
{
	if (str && *str && bytes > 0)
	{
		if (bytes + 1 > (buf->size - buf->used))
		{
			size_t		newsize = (((buf->used + bytes + 1024) / 1024) + 1) * 1024;

			if (buf->data)
				buf->data = repalloc(buf->data, newsize);
			else
				buf->data = palloc(newsize);

			buf->size = newsize;
		}

		memcpy(buf->data + buf->used, str, bytes);
		buf->used += bytes;
		buf->data[buf->used] = '\0';
	}
}

static void
dynbuf_add_string(DynBuffer *buf, const char *str)
{
	if (str && *str)
		dynbuf_add_bytes(buf, str, strlen(str));
}

static void
dynbuf_add_fields(DynBuffer *buf, const char *fieldname, char *strlist)
{
	char	   *tok;

	if (strlist)
	{
		tok = strtok(strlist, ",");
		while ((tok))
		{
			dynbuf_add_string(buf, fieldname);
			dynbuf_add_string(buf, tok);
			dynbuf_add_string(buf, "\r\n");

			tok = strtok(NULL, ",");
		}
	}
}

static void
dynbuf_add_field(DynBuffer *buf, const char *fieldname, char *str)
{
	if (str)
	{
		dynbuf_add_string(buf, fieldname);
		dynbuf_add_string(buf, str);
		dynbuf_add_string(buf, "\r\n");
	}
}

static void
dynbuf_add_lines(DynBuffer *buf, const char *str)
{
	while (str && *str)
	{
		const char	   *ptr = str;

		while (*ptr && *ptr != '\r' && *ptr != '\n')
			ptr += 1;

		dynbuf_add_bytes(buf, str, ptr - str);
		dynbuf_add_bytes(buf, "\r\n", 2);

		if (*ptr == '\0')
			break;

		if (ptr[0] == '\r' && ptr[1] == '\n')
			str = ptr + 2;

		if (*ptr == '\r' || *ptr == '\n')
			str = ptr + 1;
	}
}


static size_t
read_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	StringReader *sreader = (StringReader *) userdata;
	size_t not_processed_yet = sreader->size - sreader->processed;
	size_t read_bytes = size * nmemb;

	if ((size == 0) || (nmemb == 0) || ((size*nmemb) < 1))
		return 0;

	if (read_bytes > not_processed_yet)
		read_bytes = not_processed_yet;

	if (read_bytes > 0)
	{
		memcpy(ptr, sreader->ptr, read_bytes);
		sreader->processed += read_bytes;
	}

	return read_bytes;
}

static void
OOM_CHECK(CURLcode res)
{
	if (res == (CURLcode) CURLM_OUT_OF_MEMORY)
		elog(ERROR, "out of memory");
}

/*
 *
 * PROCEDURE utl_mail.send(
 * 		sender varchar2,
 * 		recipients varchar2,
 * 		cc varchar2 DEFAULT NULL,
 * 		bcc varchar2 DEFAULT NULL,
 * 		subject varchar2 DEFAULT NULL,
 * 		message varchar2
 * 		mime_type varchar2 DEFAULT 'text/plain; charset=us-ascii',
 * 		priority integer DEFAULT NULL)
 *
 */
Datum
orafce_mail_send(PG_FUNCTION_ARGS)
{
	CURL	   *curl;

	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *mime_type;
	volatile int priority = 0;
	volatile bool priority_is_null = false;
	char		mime_type_buffer[1024];

	sender = not_null_not_empty_arg(fcinfo, 0, "utl_mail.send", "sender");
	recipients = not_null_not_empty_arg(fcinfo, 1, "utl_mail.send", "recipients");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	message = not_null_not_empty_arg(fcinfo, 5, "utl_mail.send", "message");
	mime_type = null_or_empty_arg(fcinfo, 6);

	if (!PG_ARGISNULL(7))
		priority = PG_GETARG_INT32(7);
	else
		priority_is_null = true;

	if (!orafce_smtp_url)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("orafce.smtp_url is not specified"),
				 errdetail("The address (url) of smtp service is not known.")));

	if (!mime_type)
	{
		snprintf(mime_type_buffer, sizeof(mime_type_buffer), "text/plain; charset=\"%s\"",
				get_encoding_name_for_icu(pg_get_client_encoding()));
		mime_type = mime_type_buffer;
	}

	curl = curl_easy_init();
	if (curl)
	{
		CURLcode	res;
		struct curl_slist *recip = NULL;
		DynBuffer dbuf;
		StringReader sreader;

		PG_TRY();
		{
			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_URL, orafce_smtp_url));

			if (orafce_smtp_userpwd)
				OOM_CHECK(curl_easy_setopt(curl, CURLOPT_USERPWD, orafce_smtp_userpwd));

			if (strncmp(orafce_smtp_url, "smtps://", 8) == 0)
				(void) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT_ALLLOWFAILS, 1L);

			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sender));

			recip = add_fields(recip, recipients);

			if (cc)
				recip = add_fields(recip, pstrdup(cc));

			if (bcc)
				recip = add_fields(recip, pstrdup(bcc));

			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recip);

			dbuf.data = NULL;
			dbuf.size = 0;
			dbuf.used = 0;

			dynbuf_add_fields(&dbuf, "To: ", recipients);
			dynbuf_add_fields(&dbuf, "From: ", sender);
			dynbuf_add_fields(&dbuf, "Cc: ", cc);
			dynbuf_add_fields(&dbuf, "Bcc: ", bcc);

			dynbuf_add_field(&dbuf, "Content-type: ", mime_type);

			if (!priority_is_null)
			{
				char	buffer[10];

				snprintf(buffer, 10, "%d", priority);
				dynbuf_add_field(&dbuf, "X-Priority: ", buffer);
			}

			dynbuf_add_field(&dbuf, "Subject: ", subject);
			dynbuf_add_string(&dbuf, "\r\n");
			dynbuf_add_lines(&dbuf, message);

			sreader.ptr = dbuf.data;
			sreader.size = dbuf.used;
			sreader.processed = 0;

			(void) curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
			(void) curl_easy_setopt(curl, CURLOPT_READDATA, &sreader);
			(void) curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

#if LIBCURL_VERSION_NUM >= 0x072700 /* 7.39.0 */

			/* Connect the progress callback for interrupt support */
			(void) curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
			(void) curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

#endif

			res = curl_easy_perform(curl);

			if (res != CURLE_OK)
				ereport(ERROR,
						errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
						errmsg("cannot send mail"),
						errdetail("curl_easy_perform() failed: %s", curl_easy_strerror(res)));

			curl_slist_free_all(recip);
			curl_easy_cleanup(curl);
		}
		PG_CATCH();
		{
			curl_slist_free_all(recip);
			curl_easy_cleanup(curl);

			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		elog(ERROR, "cannot to start libcurl");

	return (Datum) 0;
}

/*
 * PROCEDURE utl_mail.send_attach_raw(
 * 		sender varchar2,
 * 		recipients varchar2,
 * 		cc varchar2 DEFAULT NULL,
 * 		bcc varchar2 DEFAULT NULL,
 * 		subject varchar2 DEFAULT NULL,
 * 		message varchar2
 * 		mime_type varchar2 DEFAULT 'text/plain; charset=us-ascii',
 * 		priority integer DEFAULT NULL
 * 		attachment bytea,
 * 		att_inline boolean DEFAULT true,
 * 		att_mime_type varchar2 DEFAULT 'application/octet',
 * 		att_filename varchar2 DEFAULT NULL)
 *
 */
Datum
orafce_mail_send_attach_raw(PG_FUNCTION_ARGS)
{
	CURL	   *curl;

	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *mime_type;
	volatile int priority = 0;
	volatile bool priority_is_null = false;
	char		mime_type_buffer[1024];

	sender = not_null_not_empty_arg(fcinfo, 0, "utl_mail.send", "sender");
	recipients = not_null_not_empty_arg(fcinfo, 1, "utl_mail.send", "recipients");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	message = not_null_not_empty_arg(fcinfo, 5, "utl_mail.send", "message");
	mime_type = null_or_empty_arg(fcinfo, 6);

	if (!PG_ARGISNULL(7))
		priority = PG_GETARG_INT32(7);
	else
		priority_is_null = true;

	if (!orafce_smtp_url)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("orafce.smtp_url is not specified"),
				 errdetail("The address (url) of smtp service is not known.")));

	if (!mime_type)
	{
		snprintf(mime_type_buffer, sizeof(mime_type_buffer), "text/plain; charset=\"%s\"",
				get_encoding_name_for_icu(pg_get_client_encoding()));
		mime_type = mime_type_buffer;
	}

	curl = curl_easy_init();
	if (curl)
	{
		CURLcode	res;
		struct curl_slist *recip = NULL;
		DynBuffer dbuf;
		StringReader sreader;

		PG_TRY();
		{
			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_URL, orafce_smtp_url));

			if (orafce_smtp_userpwd)
				OOM_CHECK(curl_easy_setopt(curl, CURLOPT_USERPWD, orafce_smtp_userpwd));

			if (strncmp(orafce_smtp_url, "smtps://", 8) == 0)
				(void) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT_ALLLOWFAILS, 1L);

			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sender));

			recip = add_fields(recip, recipients);

			if (cc)
				recip = add_fields(recip, pstrdup(cc));

			if (bcc)
				recip = add_fields(recip, pstrdup(bcc));

			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recip);

			dbuf.data = NULL;
			dbuf.size = 0;
			dbuf.used = 0;

			dynbuf_add_fields(&dbuf, "To: ", recipients);
			dynbuf_add_fields(&dbuf, "From: ", sender);
			dynbuf_add_fields(&dbuf, "Cc: ", cc);
			dynbuf_add_fields(&dbuf, "Bcc: ", bcc);

			dynbuf_add_field(&dbuf, "Content-type: ", mime_type);

			if (!priority_is_null)
			{
				char	buffer[10];

				snprintf(buffer, 10, "%d", priority);
				dynbuf_add_field(&dbuf, "X-Priority: ", buffer);
			}

			dynbuf_add_field(&dbuf, "Subject: ", subject);
			dynbuf_add_string(&dbuf, "\r\n");
			dynbuf_add_lines(&dbuf, message);

			sreader.ptr = dbuf.data;
			sreader.size = dbuf.used;
			sreader.processed = 0;

			(void) curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
			(void) curl_easy_setopt(curl, CURLOPT_READDATA, &sreader);
			(void) curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

#if LIBCURL_VERSION_NUM >= 0x072700 /* 7.39.0 */

			/* Connect the progress callback for interrupt support */
			(void) curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
			(void) curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

#endif

			res = curl_easy_perform(curl);

			if (res != CURLE_OK)
				ereport(ERROR,
						errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
						errmsg("cannot send mail"),
						errdetail("curl_easy_perform() failed: %s", curl_easy_strerror(res)));

			curl_slist_free_all(recip);
			curl_easy_cleanup(curl);
		}
		PG_CATCH();
		{
			curl_slist_free_all(recip);
			curl_easy_cleanup(curl);

			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		elog(ERROR, "cannot to start libcurl");

	return (Datum) 0;


}

/*
 * PROCEDURE utl_mail.send_attach_varchar2(
 * 		sender varchar2,
 * 		recipients varchar2,
 * 		cc varchar2 DEFAULT NULL,
 * 		bcc varchar2 DEFAULT NULL,
 * 		subject varchar2 DEFAULT NULL,
 * 		message varchar2
 * 		mime_type varchar2 DEFAULT 'text/plain; charset=us-ascii',
 * 		priority integer DEFAULT NULL
 * 		attachment varchar2,
 * 		att_inline boolean DEFAULT true,
 * 		att_mime_type varchar2 DEFAULT 'text/plain;charset=us-ascii',
 * 		att_filename varchar2 DEFAULT NULL)
 *
 */
Datum
orafce_mail_send_attach_varchar2(PG_FUNCTION_ARGS)
{
	char	   *sender = text_to_cstring(PG_GETARG_TEXT_P(0));

	(void) sender;

	return (Datum) 0;
}

/*
 * PROCEDURE dbms_mail.send(
 * 		from_str varchar2,
 * 		to_str varchar2,
 * 		cc varchar2,
 * 		bcc varchar2,
 * 		subject varchar2,
 * 		reply_to varchar2,
 * 		body varchar2)
 *
 */
Datum
orafce_mail_dbms_mail_send(PG_FUNCTION_ARGS)
{
	char	   *sender = text_to_cstring(PG_GETARG_TEXT_P(0));

	(void) sender;

	return (Datum) 0;
}

void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomStringVariable("orafce_mail.smtp_server_url",
									"smtp server url.",
									NULL,
									&orafce_smtp_url,
									NULL,
									PGC_USERSET,
									0,
									NULL,
									NULL, NULL);

	DefineCustomStringVariable("orafce_mail.smtp_server_userpwd",
									"smtp server username and password in format username:password",
									NULL,
									&orafce_smtp_userpwd,
									NULL,
									PGC_USERSET,
									0,
									NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("orafce_mail");

	curl_global_init(CURL_GLOBAL_ALL);

#if LIBCURL_VERSION_NUM >= 0x072700 /* 7.39.0 */

	/* Register our interrupt handler (http_handle_interrupt) */
	/* and store the existing one so we can call it when we're */
	/* through with our work */

	pgsql_interrupt_handler = pqsignal(SIGINT, http_interrupt_handler);
	interrupt_requested = 0;

#endif

}

void
_PG_fini(void)
{

#if LIBCURL_VERSION_NUM >= 0x072700
	/* Re-register the original signal handler */
	pqsignal(SIGINT, pgsql_interrupt_handler);
#endif

	curl_global_cleanup();
}
