/*
 * Event sink management
 *
 * Copyright (C) 2000-2019 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/ist.h>
#include <common/mini-clist.h>
#include <common/time.h>
#include <proto/cli.h>
#include <proto/log.h>
#include <proto/ring.h>
#include <proto/sink.h>
#include <proto/stream_interface.h>

struct list sink_list = LIST_HEAD_INIT(sink_list);

struct sink *cfg_sink;

struct sink *sink_find(const char *name)
{
	struct sink *sink;

	list_for_each_entry(sink, &sink_list, sink_list)
		if (strcmp(sink->name, name) == 0)
			return sink;
	return NULL;
}

/* creates a new sink and adds it to the list, it's still generic and not fully
 * initialized. Returns NULL on allocation failure. If another one already
 * exists with the same name, it will be returned. The caller can detect it as
 * a newly created one has type SINK_TYPE_NEW.
 */
static struct sink *__sink_new(const char *name, const char *desc, enum sink_fmt fmt)
{
	struct sink *sink;

	sink = sink_find(name);
	if (sink)
		goto end;

	sink = malloc(sizeof(*sink));
	if (!sink)
		goto end;

	sink->name = strdup(name);
	sink->desc = strdup(desc);
	sink->fmt  = fmt;
	sink->type = SINK_TYPE_NEW;
	sink->maxlen = BUFSIZE;
	/* address will be filled by the caller if needed */
	sink->ctx.fd = -1;
	sink->ctx.dropped = 0;
	HA_RWLOCK_INIT(&sink->ctx.lock);
	LIST_ADDQ(&sink_list, &sink->sink_list);
 end:
	return sink;
}

/* creates a sink called <name> of type FD associated to fd <fd>, format <fmt>,
 * and description <desc>. Returns NULL on allocation failure or conflict.
 * Perfect duplicates are merged (same type, fd, and name).
 */
struct sink *sink_new_fd(const char *name, const char *desc, enum sink_fmt fmt, int fd)
{
	struct sink *sink;

	sink = __sink_new(name, desc, fmt);
	if (!sink || (sink->type == SINK_TYPE_FD && sink->ctx.fd == fd))
		goto end;

	if (sink->type != SINK_TYPE_NEW) {
		sink = NULL;
		goto end;
	}

	sink->type = SINK_TYPE_FD;
	sink->ctx.fd = fd;
 end:
	return sink;
}

/* creates a sink called <name> of type BUF of size <size>, format <fmt>,
 * and description <desc>. Returns NULL on allocation failure or conflict.
 * Perfect duplicates are merged (same type and name). If sizes differ, the
 * largest one is kept.
 */
struct sink *sink_new_buf(const char *name, const char *desc, enum sink_fmt fmt, size_t size)
{
	struct sink *sink;

	sink = __sink_new(name, desc, fmt);
	if (!sink)
		goto fail;

	if (sink->type == SINK_TYPE_BUFFER) {
		/* such a buffer already exists, we may have to resize it */
		if (!ring_resize(sink->ctx.ring, size))
			goto fail;
		goto end;
	}

	if (sink->type != SINK_TYPE_NEW) {
		/* already exists of another type */
		goto fail;
	}

	sink->ctx.ring = ring_new(size);
	if (!sink->ctx.ring) {
		LIST_DEL(&sink->sink_list);
		free(sink->name);
		free(sink->desc);
		free(sink);
		goto fail;
	}

	sink->type = SINK_TYPE_BUFFER;
 end:
	return sink;
 fail:
	return NULL;
}

/* tries to send <nmsg> message parts (up to 8, ignored above) from message
 * array <msg> to sink <sink>. Formating according to the sink's preference is
 * done here. Lost messages are NOT accounted for. It is preferable to call
 * sink_write() instead which will also try to emit the number of dropped
 * messages when there are any. It returns >0 if it could write anything,
 * <=0 otherwise.
 */
ssize_t __sink_write(struct sink *sink, const struct ist msg[], size_t nmsg,
	             int level, int facility, struct ist *tag,
		     struct ist *pid, struct ist *sd)
{
	int log_format;
	char short_hdr[4];
	struct ist pfx[6];
	size_t npfx = 0;
	char *hdr_ptr;
	int fac_level;

	if (sink->fmt == SINK_FMT_RAW)
		goto send;

	if (sink->fmt == SINK_FMT_SHORT || sink->fmt == SINK_FMT_TIMED) {
		short_hdr[0] = '<';
		short_hdr[1] = '0' + level;
		short_hdr[2] = '>';

		pfx[npfx].ptr = short_hdr;
		pfx[npfx].len = 3;
		npfx++;
		if (sink->fmt == SINK_FMT_SHORT)
			goto send;
        }


	if (sink->fmt == SINK_FMT_ISO || sink->fmt == SINK_FMT_TIMED) {
		pfx[npfx].ptr = timeofday_as_iso_us(1);
		pfx[npfx].len = 27;
		npfx++;
		goto send;
        }
	else if (sink->fmt == SINK_FMT_RFC5424) {
		pfx[npfx].ptr = logheader_rfc5424;
                pfx[npfx].len = update_log_hdr_rfc5424(date.tv_sec) - pfx[npfx].ptr;
		log_format = LOG_FORMAT_RFC5424;
	}
	else {
		pfx[npfx].ptr = logheader;
                pfx[npfx].len = update_log_hdr(date.tv_sec) - pfx[npfx].ptr;
		log_format = LOG_FORMAT_RFC3164;
		sd = NULL;
	}

	fac_level = (facility << 3) + level;
	hdr_ptr = pfx[npfx].ptr + 3; /* last digit of the log level */
        do {
		*hdr_ptr = '0' + fac_level % 10;
		fac_level /= 10;
                hdr_ptr--;
	} while (fac_level && hdr_ptr > pfx[npfx].ptr);
	*hdr_ptr = '<';
	pfx[npfx].len -= hdr_ptr - pfx[npfx].ptr;
	pfx[npfx].ptr = hdr_ptr;
	npfx++;

	if (tag && tag->len) {
		pfx[npfx].ptr = tag->ptr;
		pfx[npfx].len = tag->len;
		npfx++;
	}
	pfx[npfx].ptr = get_format_pid_sep1(log_format, &pfx[npfx].len);
	if (pfx[npfx].len)
		npfx++;

	if (pid && pid->len) {
		pfx[npfx].ptr = pid->ptr;
		pfx[npfx].len = pid->len;
		npfx++;
	}

	pfx[npfx].ptr = get_format_pid_sep2(log_format, &pfx[npfx].len);
	if (pfx[npfx].len)
		npfx++;

	if (sd && sd->len) {
		pfx[npfx].ptr = sd->ptr;
		pfx[npfx].len = sd->len;
		npfx++;
	}

send:
	if (sink->type == SINK_TYPE_FD) {
		return fd_write_frag_line(sink->ctx.fd, sink->maxlen, pfx, npfx, msg, nmsg, 1);
	}
	else if (sink->type == SINK_TYPE_BUFFER) {
		return ring_write(sink->ctx.ring, sink->maxlen, pfx, npfx, msg, nmsg);
	}
	return 0;
}

/* Tries to emit a message indicating the number of dropped events. In case of
 * success, the amount of drops is reduced by as much. It's supposed to be
 * called under an exclusive lock on the sink to avoid multiple produces doing
 * the same. On success, >0 is returned, otherwise <=0 on failure.
 */
int sink_announce_dropped(struct sink *sink, int facility, struct ist *pid)
{
	unsigned int dropped;
	struct buffer msg;
	struct ist msgvec[1];
	char logbuf[64];
	struct ist sd;
	struct ist tag;

	while (unlikely((dropped = sink->ctx.dropped) > 0)) {
		chunk_init(&msg, logbuf, sizeof(logbuf));
		chunk_printf(&msg, "%u event%s dropped", dropped, dropped > 1 ? "s" : "");
		msgvec[0] = ist2(msg.area, msg.data);

		sd.ptr = default_rfc5424_sd_log_format;
		sd.len = 2;
		tag.ptr = global.log_tag.area;
		tag.len = global.log_tag.data;
		if (__sink_write(sink, msgvec, 1, LOG_NOTICE, facility, &tag, pid, &sd) <= 0)
			return 0;
		/* success! */
		HA_ATOMIC_SUB(&sink->ctx.dropped, dropped);
	}
	return 1;
}

/* parse the "show events" command, returns 1 if a message is returned, otherwise zero */
static int cli_parse_show_events(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct sink *sink;
	int arg;

	args++; // make args[1] the 1st arg

	if (!*args[1]) {
		/* no arg => report the list of supported sink */
		chunk_printf(&trash, "Supported events sinks are listed below. Add -w(wait), -n(new). Any key to stop\n");
		list_for_each_entry(sink, &sink_list, sink_list) {
			chunk_appendf(&trash, "    %-10s : type=%s, %u dropped, %s\n",
				      sink->name,
				      sink->type == SINK_TYPE_NEW ? "init" :
				      sink->type == SINK_TYPE_FD ? "fd" :
				      sink->type == SINK_TYPE_BUFFER ? "buffer" : "?",
				      sink->ctx.dropped, sink->desc);
		}

		trash.area[trash.data] = 0;
		return cli_msg(appctx, LOG_WARNING, trash.area);
	}

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	sink = sink_find(args[1]);
	if (!sink)
		return cli_err(appctx, "No such event sink");

	if (sink->type != SINK_TYPE_BUFFER)
		return cli_msg(appctx, LOG_NOTICE, "Nothing to report for this sink");

	for (arg = 2; *args[arg]; arg++) {
		if (strcmp(args[arg], "-w") == 0)
			appctx->ctx.cli.i0 |= 1; // wait mode
		else if (strcmp(args[arg], "-n") == 0)
			appctx->ctx.cli.i0 |= 2; // seek to new
		else if (strcmp(args[arg], "-nw") == 0 || strcmp(args[arg], "-wn") == 0)
			appctx->ctx.cli.i0 |= 3; // seek to new + wait
		else
			return cli_err(appctx, "unknown option");
	}
	return ring_attach_cli(sink->ctx.ring, appctx);
}

/*
 * Parse "ring" section and create corresponding sink buffer.
 *
 * The function returns 0 in success case, otherwise, it returns error
 * flags.
 */
int cfg_parse_ring(const char *file, int linenum, char **args, int kwm)
{
	int err_code = 0;
	const char *inv;
	size_t size = BUFSIZE;

	if (strcmp(args[0], "ring") == 0) { /* new peers section */
		if (!*args[1]) {
			ha_alert("parsing [%s:%d] : missing ring name.\n", file, linenum);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		inv = invalid_char(args[1]);
		if (inv) {
			ha_alert("parsing [%s:%d] : invalid ring name '%s' (character '%c' is not permitted).\n", file, linenum, args[1], *inv);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		if (sink_find(args[1])) {
			ha_alert("parsing [%s:%d] : sink named '%s' already exists.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		cfg_sink = sink_new_buf(args[1], args[1] , SINK_FMT_RAW, size);
		if (!cfg_sink || cfg_sink->type != SINK_TYPE_BUFFER) {
			ha_alert("parsing [%s:%d] : unable to create a new sink buffer for ring '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
	}
	else if (strcmp(args[0], "size") == 0) {
		size = atol(args[1]);
		if (!size) {
			ha_alert("parsing [%s:%d] : invalid size '%s' for new sink buffer.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		if (!cfg_sink || (cfg_sink->type != SINK_TYPE_BUFFER)
		              || !ring_resize(cfg_sink->ctx.ring, size)) {
			ha_alert("parsing [%s:%d] : fail to set sink buffer size '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
	}
	else if (strcmp(args[0],"format") == 0) {
		if (!cfg_sink) {
			ha_alert("parsing [%s:%d] : unable to set format '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		if (strcmp(args[1], "raw") == 0) {
			cfg_sink->fmt = SINK_FMT_RAW;
		}
		else if (strcmp(args[1], "short") == 0) {
			cfg_sink->fmt = SINK_FMT_SHORT;
		}
		else if (strcmp(args[1], "iso") == 0) {
			cfg_sink->fmt = SINK_FMT_ISO;
		}
		else if (strcmp(args[1], "timed") == 0) {
			cfg_sink->fmt = SINK_FMT_TIMED;
		}
		else if (strcmp(args[1], "rfc3164") == 0) {
			cfg_sink->fmt = SINK_FMT_RFC3164;
		}
		else if (strcmp(args[1], "rfc5424") == 0) {
			cfg_sink->fmt = SINK_FMT_RFC5424;
		}
		else {
			ha_alert("parsing [%s:%d] : unknown format '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
	}
	else if (strcmp(args[0],"maxlen") == 0) {
		if (!cfg_sink) {
			ha_alert("parsing [%s:%d] : unable to set event max length '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		cfg_sink->maxlen = atol(args[1]);
		if (!cfg_sink->maxlen) {
			ha_alert("parsing [%s:%d] : invalid size '%s' for new sink buffer.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
	}
	else if (strcmp(args[0],"description") == 0) {
		if (!cfg_sink) {
			ha_alert("parsing [%s:%d] : unable to set description '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		if (!*args[1]) {
			ha_alert("parsing [%s:%d] : missing ring description text.\n", file, linenum);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		free(cfg_sink->desc);

		cfg_sink->desc = strdup(args[1]);
		if (!cfg_sink->desc) {
			ha_alert("parsing [%s:%d] : fail to set description '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
	}
	else {
		ha_alert("parsing [%s:%d] : unknown statement '%s'.\n", file, linenum, args[0]);
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

err:
	return err_code;
}

/*
 * Post parsing "ring" section.
 *
 * The function returns 0 in success case, otherwise, it returns error
 * flags.
 */
int cfg_post_parse_ring()
{
	int err_code = 0;

	if (cfg_sink && (cfg_sink->type == SINK_TYPE_BUFFER)) {
		if (cfg_sink->maxlen > b_size(&cfg_sink->ctx.ring->buf)) {
			ha_warning("ring '%s' event max length '%u' exceeds size, forced to size '%lu'.\n",
			           cfg_sink->name, cfg_sink->maxlen, b_size(&cfg_sink->ctx.ring->buf));
			cfg_sink->maxlen = b_size(&cfg_sink->ctx.ring->buf);
			err_code |= ERR_ALERT;
		}
	}

	cfg_sink = NULL;

	return err_code;
}

/* resolve sink names at end of config. Returns 0 on success otherwise error
 * flags.
*/
int post_sink_resolve()
{
	int err_code = 0;
	struct logsrv *logsrv, *logb;
	struct sink *sink;
	struct proxy *px;

	list_for_each_entry_safe(logsrv, logb, &global.logsrvs, list) {
		if (logsrv->type == LOG_TARGET_BUFFER) {
			sink = sink_find(logsrv->ring_name);
			if (!sink || sink->type != SINK_TYPE_BUFFER) {
				ha_alert("global log server uses unkown ring named '%s'.\n", logsrv->ring_name);
				err_code |= ERR_ALERT | ERR_FATAL;
			}
			logsrv->sink = sink;
		}
	}

	for (px = proxies_list; px; px = px->next) {
		list_for_each_entry_safe(logsrv, logb, &px->logsrvs, list) {
			if (logsrv->type == LOG_TARGET_BUFFER) {
				sink = sink_find(logsrv->ring_name);
				if (!sink || sink->type != SINK_TYPE_BUFFER) {
					ha_alert("proxy '%s' log server uses unkown ring named '%s'.\n", px->id, logsrv->ring_name);
					err_code |= ERR_ALERT | ERR_FATAL;
				}
				logsrv->sink = sink;
			}
		}
	}
	return err_code;
}


static void sink_init()
{
	sink_new_fd("stdout", "standard output (fd#1)", SINK_FMT_RAW, 1);
	sink_new_fd("stderr", "standard output (fd#2)", SINK_FMT_RAW, 2);
	sink_new_buf("buf0",  "in-memory ring buffer", SINK_FMT_TIMED, 1048576);
}

static void sink_deinit()
{
	struct sink *sink, *sb;

	list_for_each_entry_safe(sink, sb, &sink_list, sink_list) {
		if (sink->type == SINK_TYPE_BUFFER)
			ring_free(sink->ctx.ring);
		LIST_DEL(&sink->sink_list);
		free(sink->name);
		free(sink->desc);
		free(sink);
	}
}

INITCALL0(STG_REGISTER, sink_init);
REGISTER_POST_DEINIT(sink_deinit);

static struct cli_kw_list cli_kws = {{ },{
	{ { "show", "events", NULL }, "show events [<sink>] : show event sink state", cli_parse_show_events, NULL, NULL },
	{{},}
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

/* config parsers for this section */
REGISTER_CONFIG_SECTION("ring", cfg_parse_ring, cfg_post_parse_ring);
REGISTER_POST_CHECK(post_sink_resolve);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
