/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Comma Separated Value CDR records.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License.
 *
 * Includes code and algorithms from the Zapata library.
 *
 */

#include <sys/types.h>
#include <asterisk/channel.h>
#include <asterisk/cdr.h>
#include <asterisk/module.h>
#include <asterisk/logger.h>
#include "../asterisk.h"
#include "../astconf.h"

#define CSV_LOG_DIR "/cdr-csv"
#define CSV_MASTER  "/Master.csv"

#define DATE_FORMAT "%Y-%m-%d %T"

/* #define CSV_LOGUNIQUEID 1 */

#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

/* The values are as follows:


  "accountcode", 	// accountcode is the account name of detail records, Master.csv contains all records
  			// Detail records are configured on a channel basis, IAX and SIP are determined by user
			// Zap is determined by channel in zaptel.conf
  "source",
  "destination",
  "destination context", 
  "callerid",
  "channel",
  "destination channel",	(if applicable)
  "last application",	// Last application run on the channel
  "last app argument",	// argument to the last channel
  "start time", 
  "answer time", 
  "end time", 
  duration,   		// Duration is the whole length that the entire call lasted. ie. call rx'd to hangup 
  			// "end time" minus "start time"
  billable seconds, 	// the duration that a call was up after other end answered which will be <= to duration 
  			// "end time" minus "answer time"
  "disposition",    	// ANSWERED, NO ANSWER, BUSY
  "amaflags",       	// DOCUMENTATION, BILL, IGNORE etc, specified on a per channel basis like accountcode.
  "uniqueid",           // unique call identifier
*/

static char *desc = "Comma Separated Values CDR Backend";

static char *name = "csv";

static FILE *mf = NULL;

static int append_string(char *buf, char *s, int len)
{
	int pos = strlen(buf);
	int spos = 0;
	int error = 0;
	if (pos >= len - 4)
		return -1;
	buf[pos++] = '\"';
	error = -1;
	while(pos < len - 3) {
		if (!s[spos]) {
			error = 0;
			break;
		}
		if (s[spos] == '\"')
			buf[pos++] = '\"';
		buf[pos++] = s[spos];
		spos++;
	}
	buf[pos++] = '\"';
	buf[pos++] = ',';
	buf[pos++] = '\0';
	return error;
}

static int append_int(char *buf, int s, int len)
{
	char tmp[32];
	int pos = strlen(buf);
	snprintf(tmp, sizeof(tmp), "%d", s);
	if (pos + strlen(tmp) > len - 3)
		return -1;
	strncat(buf, tmp, len);
	pos = strlen(buf);
	buf[pos++] = ',';
	buf[pos++] = '\0';
	return 0;
}

static int append_date(char *buf, struct timeval tv, int len)
{
	char tmp[80];
	struct tm tm;
	time_t t;
	t = tv.tv_sec;
	if (strlen(buf) > len - 3)
		return -1;
	if (!tv.tv_sec && !tv.tv_usec) {
		strncat(buf, ",", len);
		return 0;
	}
	localtime_r(&t,&tm);
	strftime(tmp, sizeof(tmp), DATE_FORMAT, &tm);
	return append_string(buf, tmp, len);
}

static int build_csv_record(char *buf, int len, struct ast_cdr *cdr)
{
	buf[0] = '\0';
	/* Account code */
	append_string(buf, cdr->accountcode, len);
	/* Source */
	append_string(buf, cdr->src, len);
	/* Destination */
	append_string(buf, cdr->dst, len);
	/* Destination context */
	append_string(buf, cdr->dcontext, len);
	/* Caller*ID */
	append_string(buf, cdr->clid, len);
	/* Channel */
	append_string(buf, cdr->channel, len);
	/* Destination Channel */
	append_string(buf, cdr->dstchannel, len);
	/* Last Application */
	append_string(buf, cdr->lastapp, len);
	/* Last Data */
	append_string(buf, cdr->lastdata, len);
	/* Start Time */
	append_date(buf, cdr->start, len);
	/* Answer Time */
	append_date(buf, cdr->answer, len);
	/* End Time */
	append_date(buf, cdr->end, len);
	/* Duration */
	append_int(buf, cdr->duration, len);
	/* Billable seconds */
	append_int(buf, cdr->billsec, len);
	/* Disposition */
	append_string(buf, ast_cdr_disp2str(cdr->disposition), len);
	/* AMA Flags */
	append_string(buf, ast_cdr_flags2str(cdr->amaflags), len);

#ifdef CSV_LOGUNIQUEID
	/* Unique ID */
	append_string(buf, cdr->uniqueid, len);
#endif
	/* If we hit the end of our buffer, log an error */
	if (strlen(buf) < len - 5) {
		/* Trim off trailing comma */
		buf[strlen(buf) - 1] = '\0';
		strncat(buf, "\n", len);
		return 0;
	}
	return -1;
}

static int writefile(char *s, char *acc)
{
	char tmp[256];
	FILE *f;
	if (strchr(acc, '/') || (acc[0] == '.')) {
		ast_log(LOG_WARNING, "Account code '%s' insecure for writing file\n", acc);
		return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s/%s/%s.csv", (char *)ast_config_AST_LOG_DIR,CSV_LOG_DIR, acc);
	f = fopen(tmp, "a");
	if (!f)
		return -1;
	fputs(s, f);
	fclose(f);
	return 0;
}


static int csv_log(struct ast_cdr *cdr)
{
	/* Make sure we have a big enough buf */
	char buf[1024];
	char csvmaster[AST_CONFIG_MAX_PATH];
	snprintf((char *)csvmaster,sizeof(csvmaster)-1,"%s/%s/%s",(char *)ast_config_AST_LOG_DIR,CSV_LOG_DIR,CSV_MASTER);
#if 0
	printf("[CDR] %s ('%s' -> '%s') Dur: %ds Bill: %ds Disp: %s Flags: %s Account: [%s]\n", cdr->channel, cdr->src, cdr->dst, cdr->duration, cdr->billsec, ast_cdr_disp2str(cdr->disposition), ast_cdr_flags2str(cdr->amaflags), cdr->accountcode);
#endif
	if (build_csv_record(buf, sizeof(buf), cdr)) {
		ast_log(LOG_WARNING, "Unable to create CSV record in %d bytes.  CDR not recorded!\n", sizeof(buf));
	} else {
		/* because of the absolutely unconditional need for the
		   highest reliability possible in writing billing records,
		   we open write and close the log file each time */
		mf = fopen(csvmaster, "a");
		if (!mf) {
			ast_log(LOG_ERROR, "Unable to re-open master file %s\n", csvmaster);
		}
		if (mf) {
			fputs(buf, mf);
			fflush(mf); /* be particularly anal here */
			fclose(mf);
			mf = NULL;
		}
		if (strlen(cdr->accountcode)) {
			if (writefile(buf, cdr->accountcode))
				ast_log(LOG_WARNING, "Unable to write CSV record to account file '%s'\n", cdr->accountcode);
		}
	}
	return 0;
}

char *description(void)
{
	return desc;
}

int unload_module(void)
{
	if (mf)
		fclose(mf);
	ast_cdr_unregister(name);
	return 0;
}

int load_module(void)
{
	int res;

	res = ast_cdr_register(name, desc, csv_log);
	if (res) {
		ast_log(LOG_ERROR, "Unable to register CSV CDR handling\n");
		if (mf)
			fclose(mf);
	}
	return res;
}

int reload(void)
{
	return 0;
}

int usecount(void)
{
	return 0;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
