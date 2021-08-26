
#include <curl/curl.h>
#include <signal.h>
#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(orafce_mail_send);
PG_FUNCTION_INFO_V1(orafce_mail_send_attach_raw);
PG_FUNCTION_INFO_V1(orafce_mail_send_attach_varchar2);
PG_FUNCTION_INFO_V1(orafce_mail_dbms_mail_send);

void _PG_init(void);
void _PG_fini(void);

Oid		ORAFCE_MAIL_ROLE_USE = InvalidOid;
Oid		ORAFCE_MAIL_ROLE_CONFIG_URL = InvalidOid;
Oid		ORAFCE_MAIL_ROLE_CONFIG_USERPWD = InvalidOid;

typedef struct
{
	char	   *header_data;
	size_t		header_size;
	size_t		header_position;

	char	   *data;
	size_t		size;
	size_t		position;

	bool		unix2dos_nl;

} BinaryReader;

typedef struct
{
	char	   *data;
	size_t		size;
	size_t		used;
} DynamicBuffer;

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

static bool
check_priv_of_role(Oid *oidptr, char *rolname)
{
	if (!OidIsValid(*oidptr))
		*oidptr = get_role_oid(rolname, false);

	return has_privs_of_role(GetUserId(), *oidptr);
}


static void
add_line(DynamicBuffer *dbuf, char *str)
{
	size_t		len = strlen(str);
	char	   *write_ptr;

	if (dbuf->used + len + 3 > dbuf->size)
	{
		size_t	new_size = ((dbuf->used + len + 1024) / 1024 + 1) * 1024;

		if (dbuf->size == 0)
			dbuf->data = palloc(new_size);
		else
			dbuf->data = repalloc(dbuf->data, new_size);

		dbuf->size = new_size;
	}

	write_ptr = dbuf->data + dbuf->used;

	memcpy(write_ptr, str, len);
	memcpy(write_ptr + len, "\r\n\0", 3);

	dbuf->used += len + 2;
}

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
 * CURL list is linked list of duplicated strings. This routine
 * does formatting of one row of header and add this row to list.
 *
 */
static struct curl_slist *
add_header_item(struct curl_slist *sl, DynamicBuffer *dbuf, const char *fieldname, const char *arg)
{
	char	   *buf, *ptr;

	if (!arg)
		return sl;

	buf = malloc(strlen(fieldname) + strlen(arg) + 2);
	if (!buf)
		elog(ERROR, "out of memory");

	ptr = buf;
	while (*fieldname)
		*ptr++ = *fieldname++;

	while (*arg)
		*ptr++ = *arg++;

	*ptr = '\0';

	if (dbuf)
	{
		add_line(dbuf, buf);
	}
	else
	{
		sl = curl_slist_append(sl, buf);
		if (!sl)
			elog(ERROR, "out of memory");
	}

	free(buf);

	return sl;
}

static struct curl_slist *
add_header_priority_item(struct curl_slist *sl, DynamicBuffer *dbuf, int priority, bool isnull)
{
	char	buffer[100];

	if (isnull)
		return sl;

	snprintf(buffer, sizeof(buffer), "%d", priority);

	return add_header_item(sl, dbuf, "X-Priority: ", buffer);
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
		if (!sl)
			elog(ERROR, "out of memory");

		tok = strtok(NULL, ",");
	}

	return sl;
}

static size_t
read_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	BinaryReader *reader = (BinaryReader *) userdata;
	size_t not_processed_yet;
	size_t write_buffer_size = size * nmemb;

	if ((size == 0) ||
		(nmemb == 0) ||
		((size * nmemb) < 1))
		return 0;

	/*
	 * When we have to send header's lines first
	 */
	not_processed_yet = reader->header_size - reader->header_position;
	if (reader->header_size > reader->header_position)
	{
		char	   *read_buffer = reader->header_data + reader->header_position;

		if (write_buffer_size > not_processed_yet)
			write_buffer_size = not_processed_yet;

		memcpy(ptr, read_buffer, write_buffer_size);
		reader->header_position += write_buffer_size;

		return write_buffer_size;
	}

	/*
	 * Header was processed, now print empty line as separator
	 */
	if (reader->header_size > 0)
	{
		memcpy(ptr, "\r\n", 2);
		reader->header_size = 0;
		reader->header_position = 0;

		return 2;
	}

	/*
	 * Now, send data.
	 */
	not_processed_yet = reader->size - reader->position;
	if (not_processed_yet > 0)
	{
		char	   *write_buffer = ptr;
		char	   *read_buffer = reader->data + reader->position;
		char	   *rptr = read_buffer;

		if (!reader->unix2dos_nl)
		{
			if (write_buffer_size > not_processed_yet)
				write_buffer_size = not_processed_yet;

			memcpy(write_buffer, read_buffer, write_buffer_size);
			reader->position += write_buffer_size;

			return write_buffer_size;
		}

		while (not_processed_yet > 0 && write_buffer_size > 0)
		{
			if ((not_processed_yet > 1) &&
				(rptr[0] == '\r' && rptr[1] == '\n'))
			{
				if (write_buffer_size >= 2)
				{
					*ptr++ = *rptr++;
					*ptr++ = *rptr++;
					write_buffer_size -= 2;
					not_processed_yet -= 2;
				}
				else
					break;
			}

			if (rptr[0] == '\n')
			{
				if (write_buffer_size >= 2)
				{
					*ptr++ = '\r';
					*ptr++ = *rptr++;
					write_buffer_size -= 2;
					not_processed_yet -= 1;
				}
				else
					break;
			}
			else
			{
				*ptr++ = *rptr++;
				write_buffer_size -= 1;
				not_processed_yet -= 1;
			}
		}

		reader->position += rptr - read_buffer;

		return ptr - write_buffer;
	}

	return 0;
}

static int
seek_callback(void *arg, curl_off_t offset, int origin)
{
	BinaryReader *p = (BinaryReader *) arg;

	switch(origin)
	{
		case SEEK_END:
			offset += p->size;
			break;

		case SEEK_CUR:
			offset += p->position;
			break;
	}

	if(offset < 0)
		return CURL_SEEKFUNC_FAIL;

	p->position = offset;

	return CURL_SEEKFUNC_OK;
}

static void
OOM_CHECK(CURLcode res)
{
	if (res == (CURLcode) CURLM_OUT_OF_MEMORY)
		elog(ERROR, "out of memory");
}

static void
CHECK_OK(CURLcode res)
{
	if (res != CURLE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("curl_easy_setopt fails"),
				 errdetail("%s", curl_easy_strerror(res))));
}

static void
orafce_send_mail(char *sender,
				 char *recipients,
				 char *cc,
				 char *bcc,
				 char *subject,
				 char *replyto,
				 int priority,
				 bool priority_is_null,
				 char *message,
				 char *mime_type,
				 char *attachment_data,
				 size_t attachment_size,
				 char *att_mime_type,
				 char *att_filename,
				 bool att_is_text)
{
	CURL	   *curl;
	char		charbuffer[1024];

	if (!check_priv_of_role(&ORAFCE_MAIL_ROLE_USE, "orafce_mail"))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be a member of the role \"orafce_mail\"")));

	if (!orafce_smtp_url)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("orafce.smtp_url is not specified"),
				 errdetail("The address (url) of smtp service is not known.")));

	curl = curl_easy_init();
	if (curl)
	{
		CURLcode	res;
		struct curl_slist *recip = NULL;
		struct curl_slist *headers = NULL;
		curl_mime *mime = NULL;
		curl_mimepart *part;
		BinaryReader reader;
		BinaryReader message_reader;
		DynamicBuffer dbuf, *_dbuf;

		memset(&message_reader, 0, sizeof(BinaryReader));
		memset(&reader, 0, sizeof(BinaryReader));

		PG_TRY();
		{
			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_URL, orafce_smtp_url));

			if (orafce_smtp_userpwd)
				OOM_CHECK(curl_easy_setopt(curl, CURLOPT_USERPWD, orafce_smtp_userpwd));

			if (strncmp(orafce_smtp_url, "smtps://", 8) == 0)
				(void) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

#if LIBCURL_VERSION_NUM >= 0x074500 /* 7.69.0 */
			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT_ALLLOWFAILS, 1L);
#endif

			OOM_CHECK(curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sender));

			recip = add_fields(recip, recipients);

			(void) curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recip);

			/*
			 * curl http header is working only when MIME is used. Without
			 * MIME I have to collect headers in dynamic string.
			 */
			if (!attachment_data)
			{
				dbuf.data = NULL;
				dbuf.size = 0;
				dbuf.used = 0;
				_dbuf = &dbuf;
			}
			else
				_dbuf = NULL;

			headers = add_header_item(headers, _dbuf, "From: ", sender);
			headers = add_header_item(headers, _dbuf, "To: " , recipients);
			headers = add_header_item(headers, _dbuf, "Cc: ", cc);
			headers = add_header_item(headers, _dbuf, "Bcc: ", bcc);
			headers = add_header_item(headers, _dbuf, "Reply-To: ", replyto);
			headers = add_header_priority_item(headers, _dbuf, priority, priority_is_null);
			headers = add_header_item(headers, _dbuf, "Subject: ", subject);

			/*
			 * When there are an attachment, then we make multipart mime
			 */
			if (attachment_data)
			{
				mime = curl_mime_init(curl);
				if (!mime)
					elog(ERROR, "out of memory");

				if (message)
				{
					part = curl_mime_addpart(mime);
					if (!part)
						elog(ERROR, "out of memory");

					if (!mime_type)
					{
						snprintf(charbuffer,
								 sizeof(charbuffer),
								 "text/plain; charset=\"%s\"",
								 get_encoding_name_for_icu(pg_get_client_encoding()));

						CHECK_OK(curl_mime_type(part, charbuffer));
					}
					else
						CHECK_OK(curl_mime_type(part, mime_type));

					message_reader.data = message;
					message_reader.size = strlen(message);
					message_reader.position = 0;

					if (!mime_type || strncmp(mime_type, "text/plain;", 11) == 0)
						message_reader.unix2dos_nl = true;
					else
						message_reader.unix2dos_nl = false;

					(void) curl_mime_data_cb(part,
											  -1,
											  read_callback,
											  NULL,
											  NULL,
											  &message_reader);

					CHECK_OK(curl_mime_encoder(part, "8bit"));
				}

				part = curl_mime_addpart(mime);
				if (!part)
					elog(ERROR, "out of memory");

				if (att_mime_type)
					CHECK_OK(curl_mime_type(part, att_mime_type));
				else
				{
					if (att_is_text)
					{
						snprintf(charbuffer,
								 sizeof(charbuffer),
								 "text/plain; charset=\"%s\"",
								 get_encoding_name_for_icu(pg_get_client_encoding()));

						CHECK_OK(curl_mime_type(part, charbuffer));
					}
					else
						CHECK_OK(curl_mime_type(part, "application/octet"));
				}

				(void) curl_mime_encoder(part, "base64");

				if (att_filename)
				{
					CHECK_OK(curl_mime_filename(part, att_filename));
					CHECK_OK(curl_mime_name(part, att_filename));
				}

				reader.data = attachment_data;
				reader.size = attachment_size;
				reader.position = 0;

				if (att_is_text &&
					(!att_mime_type || strncmp(att_mime_type, "text/plain;", 11) == 0))
					reader.unix2dos_nl = true;
				else
					reader.unix2dos_nl = false;

				(void) curl_mime_data_cb(part,
								  reader.unix2dos_nl ? (size_t) -1 : reader.size,
								  read_callback,
								  reader.unix2dos_nl ? NULL : seek_callback,
								  NULL,
								  &reader);

				(void) curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
			}
			else
			{
				if (!mime_type)
				{
					snprintf(charbuffer,
							 sizeof(charbuffer),
							 "text/plain; charset=\"%s\"",
							 get_encoding_name_for_icu(pg_get_client_encoding()));

					headers = add_header_item(headers, _dbuf, "Content-Type: ", charbuffer);
				}
				else
					headers = add_header_item(headers, _dbuf, "Content-Type: ", mime_type);

				headers = add_header_item(headers, _dbuf, "Content-Transfer-Encoding: ", "8bit");

				message_reader.data = message;
				message_reader.size = strlen(message);
				message_reader.position = 0;

				if (!mime_type || strncmp(mime_type, "text/plain;", 11) == 0)
					message_reader.unix2dos_nl = true;
				else
					message_reader.unix2dos_nl = false;

				CHECK_OK(curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback));
				CHECK_OK(curl_easy_setopt(curl, CURLOPT_READDATA, &message_reader));
				CHECK_OK(curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L));
			}

			if (headers)
				CHECK_OK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers));
			else
			{
				if (!_dbuf)
					elog(ERROR, "dynamic buffer is NULL");

				message_reader.header_data = _dbuf->data;
				message_reader.header_size = _dbuf->used;
				message_reader.header_position = 0;
			}

#if LIBCURL_VERSION_NUM >= 0x072700 /* 7.39.0 */

			/* Connect the progress callback for interrupt support */
			(void) curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
			(void) curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

#endif

			res = curl_easy_perform(curl);

			if (res != CURLE_OK)
				ereport(ERROR,
						(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
						 errmsg("cannot send mail"),
						 errdetail("curl_easy_perform() failed: %s", curl_easy_strerror(res))));

			if (_dbuf)
				pfree(_dbuf->data);

			curl_slist_free_all(recip);
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			curl_mime_free(mime);
		}
		PG_CATCH();
		{
			if (_dbuf)
				pfree(_dbuf->data);

			curl_slist_free_all(recip);
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
			curl_mime_free(mime);

			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		elog(ERROR, "cannot to start libcurl");
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
	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *mime_type;
	volatile int priority = 0;
	volatile bool priority_is_null = false;
	char	   *replyto;

	sender = not_null_not_empty_arg(fcinfo, 0, "utl_mail.send_attach_raw", "sender");
	recipients = not_null_not_empty_arg(fcinfo, 1, "utl_mail.send_attach_raw", "recipients");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	message = null_or_empty_arg(fcinfo, 5);
	mime_type = null_or_empty_arg(fcinfo, 6);

	if (!PG_ARGISNULL(7))
		priority = PG_GETARG_INT32(7);
	else
		priority_is_null = true;

	replyto = null_or_empty_arg(fcinfo, 8);

	orafce_send_mail(sender,
					 recipients,
					 cc,
					 bcc,
					 subject,
					 replyto,
					 priority,
					 priority_is_null,
					 message,
					 mime_type,
					 NULL,
					 0,
					 NULL,
					 NULL,
					 false);

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
 * 		mime_type varchar2 DEFAULT NULL,
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
	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *mime_type;
	volatile int priority = 0;
	char	   *att_mime_type;
	char	   *att_filename;
	volatile bool priority_is_null = false;
	bytea	   *vlena;
	char	   *attachment_data;
	size_t		attachment_size;
	char	   *replyto;

	sender = not_null_not_empty_arg(fcinfo, 0, "utl_mail.send_attach_raw", "sender");
	recipients = not_null_not_empty_arg(fcinfo, 1, "utl_mail.send_attach_raw", "recipients");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	message = null_or_empty_arg(fcinfo, 5);
	mime_type = null_or_empty_arg(fcinfo, 6);

	if (!PG_ARGISNULL(7))
		priority = PG_GETARG_INT32(7);
	else
		priority_is_null = true;

	vlena = DatumGetByteaPP(not_null_arg(fcinfo, 8, "utl_mail.send_attach_raw", "attachment"));
	attachment_data = VARDATA_ANY(vlena);
	attachment_size = (size_t) VARSIZE_ANY_EXHDR(vlena);

	att_mime_type = null_or_empty_arg(fcinfo, 10);
	att_filename = null_or_empty_arg(fcinfo, 11);

	replyto = null_or_empty_arg(fcinfo, 12);

	orafce_send_mail(sender,
					 recipients,
					 cc,
					 bcc,
					 subject,
					 replyto,
					 priority,
					 priority_is_null,
					 message,
					 mime_type,
					 attachment_data,
					 attachment_size,
					 att_mime_type,
					 att_filename,
					 false);

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
	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *mime_type;
	volatile int priority = 0;
	char	   *att_mime_type;
	char	   *att_filename;
	volatile bool priority_is_null = false;
	bytea	   *vlena;
	char	   *attachment_data;
	size_t		attachment_size;
	char	   *replyto;

	sender = not_null_not_empty_arg(fcinfo, 0, "utl_mail.send_attach_varchar2", "sender");
	recipients = not_null_not_empty_arg(fcinfo, 1, "utl_mail.send_attach_varchar2", "recipients");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	message = null_or_empty_arg(fcinfo, 5);
	mime_type = null_or_empty_arg(fcinfo, 6);

	if (!PG_ARGISNULL(7))
		priority = PG_GETARG_INT32(7);
	else
		priority_is_null = true;

	vlena = DatumGetByteaPP(not_null_arg(fcinfo, 8, "utl_mail.send_attach_varchar2", "attachment"));
	attachment_data = VARDATA_ANY(vlena);
	attachment_size = (size_t) VARSIZE_ANY_EXHDR(vlena);

	att_mime_type = null_or_empty_arg(fcinfo, 10);
	att_filename = null_or_empty_arg(fcinfo, 11);

	replyto = null_or_empty_arg(fcinfo, 12);

	orafce_send_mail(sender,
					 recipients,
					 cc,
					 bcc,
					 subject,
					 replyto,
					 priority,
					 priority_is_null,
					 message,
					 mime_type,
					 attachment_data,
					 attachment_size,
					 att_mime_type,
					 att_filename,
					 true);

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
	char	   *sender;
	char	   *recipients;
	char	   *cc;
	char	   *bcc;
	char	   *subject;
	char	   *message;
	char	   *replyto;

	sender = not_null_not_empty_arg(fcinfo, 0, "dbms_mail.send", "from_str");
	recipients = not_null_not_empty_arg(fcinfo, 1, "dbms_mail.send", "to_str");
	cc = null_or_empty_arg(fcinfo, 2);
	bcc = null_or_empty_arg(fcinfo, 3);
	subject = null_or_empty_arg(fcinfo, 4);
	replyto = null_or_empty_arg(fcinfo, 5);
	message = null_or_empty_arg(fcinfo, 6);

	orafce_send_mail(sender,
					 recipients,
					 cc,
					 bcc,
					 subject,
					 replyto,
					 0,
					 true,
					 message,
					 NULL,
					 NULL,
					 0,
					 NULL,
					 NULL,
					 false);

	return (Datum) 0;
}

static bool
smtp_server_url_acl_check(char **newval, void **extra, GucSource source)
{
	(void) newval;
	(void) extra;
	(void) source;

	if (!check_priv_of_role(&ORAFCE_MAIL_ROLE_CONFIG_URL,
							"orafce_mail_config_url"))
	{
		GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
		GUC_check_errmsg("must be a member of the role \"orafce_mail_config_url\"");

		return false;
	}

	return true;
}

static bool
smtp_server_userpwd_acl_check(char **newval, void **extra, GucSource source)
{
	(void) newval;
	(void) extra;
	(void) source;

	if (!check_priv_of_role(&ORAFCE_MAIL_ROLE_CONFIG_USERPWD,
							"orafce_mail_config_userpwd"))
	{
		GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
		GUC_check_errmsg("must be a member of the role \"orafce_mail_config_userpwd\"");

		return false;
	}

	return true;
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
									smtp_server_url_acl_check,
									NULL, NULL);

	DefineCustomStringVariable("orafce_mail.smtp_server_userpwd",
									"smtp server username and password in format username:password",
									NULL,
									&orafce_smtp_userpwd,
									NULL,
									PGC_USERSET,
									0,
									smtp_server_userpwd_acl_check,
									NULL, NULL);

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
