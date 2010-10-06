/*
 * Copyright (c) 2009, Rambler media
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Rambler media ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "buffer.h"
#include "main.h"

#define G_DISPATCHER_ERROR dispatcher_error_quark()

static void                     dispatcher_cb (gint fd, short what, void *arg);

static inline                   GQuark
dispatcher_error_quark (void)
{
	return g_quark_from_static_string ("g-dispatcher-error-quark");
}

static gboolean
sendfile_callback (rspamd_io_dispatcher_t *d)
{

	GError                         *err;

#ifdef HAVE_SENDFILE
# if defined(FREEBSD) || defined(DARWIN)
	off_t                           off = 0;
	#if defined(FREEBSD)
	/* FreeBSD version */
	if (sendfile (d->sendfile_fd, d->fd, d->offset, 0, NULL, &off, 0) != 0) {
	#elif defined(DARWIN)
	/* Darwin version */
	if (sendfile (d->sendfile_fd, d->fd, d->offset, &off, NULL, 0) != 0) {
	#endif
		if (errno != EAGAIN) {
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, errno, "%s", strerror (errno));
				d->err_callback (err, d->user_data);
				return FALSE;
			}
		}
		else {
			debug_ip (d->peer_addr, "partially write data, retry");
			/* Wait for other event */
			d->offset += off;
			event_del (d->ev);
			event_set (d->ev, d->fd, EV_WRITE, dispatcher_cb, (void *)d);
			event_add (d->ev, d->tv);
		}
	}
	else {
		if (d->write_callback) {
			if (!d->write_callback (d->user_data)) {
				debug_ip (d->peer_addr, "callback set wanna_die flag, terminating");
				return FALSE;
			}
		}
		event_del (d->ev);
		event_set (d->ev, d->fd, EV_READ | EV_PERSIST, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
		d->in_sendfile = FALSE;
	}
# else
	ssize_t                         r;
	/* Linux version */
	r = sendfile (d->fd, d->sendfile_fd, &d->offset, d->file_size);
	if (r == -1) {
		if (errno != EAGAIN) {
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, errno, "%s", strerror (errno));
				d->err_callback (err, d->user_data);
				return FALSE;
			}
		}
		else {
			debug_ip (d->peer_addr, "partially write data, retry");
			/* Wait for other event */
			event_del (d->ev);
			event_set (d->ev, d->fd, EV_WRITE, dispatcher_cb, (void *)d);
			event_add (d->ev, d->tv);
		}
	}
	else if (r + d->offset < d->file_size) {
		debug_ip (d->peer_addr, "partially write data, retry");
		/* Wait for other event */
		event_del (d->ev);
		event_set (d->ev, d->fd, EV_WRITE, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
	}
	else {
		if (d->write_callback) {
			if (!d->write_callback (d->user_data)) {
				debug_ip (d->peer_addr, "callback set wanna_die flag, terminating");
				return FALSE;
			}
		}
		event_del (d->ev);
		event_set (d->ev, d->fd, EV_READ | EV_PERSIST, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
		d->in_sendfile = FALSE;
	}
# endif
#else
	ssize_t                         r;
	r = write (d->fd, d->map, d->file_size - d->offset);
	if (r == -1) {
		if (errno != EAGAIN) {
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, errno, "%s", strerror (errno));
				d->err_callback (err, d->user_data);
				return FALSE;
			}
		}
		else {
			debug_ip (d->peer_addr, "partially write data, retry");
			/* Wait for other event */
			event_del (d->ev);
			event_set (d->ev, d->fd, EV_WRITE, dispatcher_cb, (void *)d);
			event_add (d->ev, d->tv);
		}
	}
	else if (r + d->offset < d->file_size) {
		d->offset += r;
		debug_ip (d->peer_addr, "partially write data, retry");
		/* Wait for other event */
		event_del (d->ev);
		event_set (d->ev, d->fd, EV_WRITE, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
	}
	else {
		if (d->write_callback) {
			if (!d->write_callback (d->user_data)) {
				debug_ip (d->peer_addr, "callback set wanna_die flag, terminating");
				return FALSE;
			}
		}
		event_del (d->ev);
		event_set (d->ev, d->fd, EV_READ | EV_PERSIST, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
		d->in_sendfile = FALSE;
	}
#endif
	return TRUE;
}

#define BUFREMAIN(x) (x)->data->size - ((x)->pos - (x)->data->begin)

static                          gboolean
write_buffers (gint fd, rspamd_io_dispatcher_t * d, gboolean is_delayed)
{
	GList                          *cur;
	GError                         *err;
	rspamd_buffer_t                *buf;
	ssize_t                         r;

	/* Fix order */
	if (d->out_buffers) {
		d->out_buffers = g_list_reverse (d->out_buffers);
	}
	cur = g_list_first (d->out_buffers);
	while (cur) {
		buf = (rspamd_buffer_t *) cur->data;
		if (BUFREMAIN (buf) == 0) {
			/* Skip empty buffers */
			cur = g_list_next (cur);
			continue;
		}
		r = write (fd, buf->pos, BUFREMAIN (buf));
		if (r == -1 && errno != EAGAIN) {
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, errno, "%s", strerror (errno));
				d->err_callback (err, d->user_data);
				return FALSE;
			}
		}
		else if (r > 0) {
			buf->pos += r;
			if (BUFREMAIN (buf) != 0) {
				/* Continue with this buffer */
				debug_ip (d->peer_addr, "wrote %z bytes of %z", r, buf->data->len);
				continue;
			}
		}
		else if (r == 0) {
			/* Got EOF while we wait for data */
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, EOF, "got EOF");
				d->err_callback (err, d->user_data);
				return FALSE;
			}
		}
		else if (r == -1 && errno == EAGAIN) {
			debug_ip (d->peer_addr, "partially write data, retry");
			/* Wait for other event */
			event_del (d->ev);
			event_set (d->ev, fd, EV_WRITE, dispatcher_cb, (void *)d);
			event_add (d->ev, d->tv);
			return TRUE;
		}
		cur = g_list_next (cur);
	}

	if (cur == NULL) {
		/* Disable write event for this time */
		g_list_free (d->out_buffers);
		d->out_buffers = NULL;

		debug_ip (d->peer_addr, "all buffers were written successfully");

		if (is_delayed && d->write_callback) {
			if (!d->write_callback (d->user_data)) {
				debug_ip (d->peer_addr, "callback set wanna_die flag, terminating");
				return FALSE;
			}
		}

		event_del (d->ev);
		event_set (d->ev, fd, EV_READ | EV_PERSIST, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
	}
	else {
		/* Plan other write event */
		event_del (d->ev);
		event_set (d->ev, fd, EV_WRITE, dispatcher_cb, (void *)d);
		event_add (d->ev, d->tv);
	}

	return TRUE;
}

static void
read_buffers (gint fd, rspamd_io_dispatcher_t * d, gboolean skip_read)
{
	ssize_t                         r;
	GError                         *err;
	f_str_t                         res;
	gchar                           *c, *b;
	gchar                           *end;
	size_t                          len;
	enum io_policy                  saved_policy;

	if (d->wanna_die) {
		rspamd_remove_dispatcher (d);
		return;
	}

	if (d->in_buf == NULL) {
		d->in_buf = memory_pool_alloc (d->pool, sizeof (rspamd_buffer_t));
		if (d->policy == BUFFER_LINE || d->policy == BUFFER_ANY) {
			d->in_buf->data = fstralloc (d->pool, BUFSIZ);
		}
		else {
			d->in_buf->data = fstralloc (d->pool, d->nchars + 1);
		}
		d->in_buf->pos = d->in_buf->data->begin;
	}

	end = d->in_buf->pos;
	len = d->in_buf->data->len;

	if (BUFREMAIN (d->in_buf) == 0) {
		/* Buffer is full, try to call callback with overflow error */
		if (d->err_callback) {
			err = g_error_new (G_DISPATCHER_ERROR, E2BIG, "buffer overflow");
			d->err_callback (err, d->user_data);
			return;
		}
	}
	else if (!skip_read) {
		/* Try to read the whole buffer */
		r = read (fd, end, BUFREMAIN (d->in_buf));
		if (r == -1 && errno != EAGAIN) {
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, errno, "%s", strerror (errno));
				d->err_callback (err, d->user_data);
				return;
			}
		}
		else if (r == 0) {
			/* Got EOF while we wait for data */
			if (d->err_callback) {
				err = g_error_new (G_DISPATCHER_ERROR, EOF, "got EOF");
				d->err_callback (err, d->user_data);
				return;
			}
		}
		else if (r == -1 && errno == EAGAIN) {
			debug_ip (d->peer_addr, "partially read data, retry");
			return;
		}
		else {
			/* Set current position in buffer */
			d->in_buf->pos += r;
			d->in_buf->data->len += r;
		}
		debug_ip (d->peer_addr, "read %z characters, policy is %s, watermark is: %z", r, 
				d->policy == BUFFER_LINE ? "LINE" : "CHARACTER", d->nchars);
	}

	saved_policy = d->policy;
	c = d->in_buf->data->begin;
	end = d->in_buf->pos;
	len = d->in_buf->data->len;
	b = c;
	r = 0;
	
	switch (d->policy) {
	case BUFFER_LINE:
		/** Variables:
		* b - begin of line
		* r - current position in buffer
		* *len - length of remaining buffer
		* c - pointer to current position (buffer->begin + r)
		* res - result string 
		*/
		while (r < len) {
			if (*c == '\n') {
				res.begin = b;
				res.len = c - b;
				/* Strip EOL */
				if (d->strip_eol) {
					if (r != 0 && *(c - 1) == '\r') {
						res.len--;
					}
				}
				else {
					/* Include EOL in reply */
					res.len ++;
				}
				/* Call callback for a line */
				if (d->read_callback) {
					if (!d->read_callback (&res, d->user_data)) {
						return;
					}
					if (d->policy != saved_policy) {
						/* Drain buffer as policy is changed */
						/* Note that d->in_buffer is other pointer now, so we need to reinit all pointers */
						/* First detect how much symbols do we have */
						if (end == c) {
							/* In fact we read the whole buffer and change input policy, so just set current pos to begin of buffer */
							d->in_buf->pos = d->in_buf->data->begin;
							d->in_buf->data->len = 0;
						}
						else {
							/* Otherwise we need to move buffer */
							/* Reinit pointers */
							len = d->in_buf->data->len - r - 1;
							end = d->in_buf->data->begin + r + 1;
							memmove (d->in_buf->data->begin, end, len);
							d->in_buf->data->len = len;
							d->in_buf->pos = d->in_buf->data->begin + len;
							/* Process remaining buffer */
							read_buffers (fd, d, TRUE);
						}
						return;
					}
				}
				/* Set new begin of line */
				b = c + 1;
			}
			r++;
			c++;
		}
		/* Now drain remaining characters in buffer */
		memmove (d->in_buf->data->begin, b, c - b);
		d->in_buf->data->len = c - b;
		d->in_buf->pos = d->in_buf->data->begin + (c - b);
		break;
	case BUFFER_CHARACTER:
		r = d->nchars;
		if (len >= r) {
			res.begin = b;
			res.len = r;
			c = b + r;
			if (d->read_callback) {
				if (!d->read_callback (&res, d->user_data)) {
					return;
				}
				/* Move remaining string to begin of buffer (draining) */
				if (len > r) {
					len -= r;
					memmove (d->in_buf->data->begin, c, len);
					d->in_buf->data->len = len;
					d->in_buf->pos = d->in_buf->data->begin + len;
					b = d->in_buf->data->begin;
					c = b;
				}
				else {
					d->in_buf->data->len = 0;
					d->in_buf->pos = d->in_buf->data->begin;
				}
				if (d->policy != saved_policy && len != r) {
					debug_ip (d->peer_addr, "policy changed during callback, restart buffer's processing");
					read_buffers (fd, d, TRUE);
					return;
				}
			}
		}
		break;
	case BUFFER_ANY:
		res.begin = d->in_buf->data->begin;
		res.len = len;
		if (d->read_callback) {
			if (!d->read_callback (&res, d->user_data)) {
				return;
			}
			if (d->policy != saved_policy) {
				debug_ip (d->peer_addr, "policy changed during callback, restart buffer's processing");
				read_buffers (fd, d, TRUE);
				return;
			}
		}
		d->in_buf->pos = d->in_buf->data->begin;
		d->in_buf->data->len = 0;
		break;
	}
}

#undef BUFREMAIN

static void
dispatcher_cb (gint fd, short what, void *arg)
{
	rspamd_io_dispatcher_t         *d = (rspamd_io_dispatcher_t *) arg;
	GError                         *err;

	debug_ip (d->peer_addr, "in dispatcher callback, what: %d, fd: %d", (gint)what, fd);

	switch (what) {
	case EV_TIMEOUT:
		if (d->err_callback) {
			err = g_error_new (G_DISPATCHER_ERROR, ETIMEDOUT, "IO timeout");
			d->err_callback (err, d->user_data);
		}
		break;
	case EV_WRITE:
		/* No data to write, disable further EV_WRITE to this fd */
		if (d->in_sendfile) {
			sendfile_callback (d);
		}
		else {
			if (d->out_buffers == NULL) {
				event_del (d->ev);
				event_set (d->ev, fd, EV_READ | EV_PERSIST, dispatcher_cb, (void *)d);
				event_add (d->ev, d->tv);
			}
			else {
				/* Delayed write */
				write_buffers (fd, d, TRUE);
			}
		}
		break;
	case EV_READ:
		read_buffers (fd, d, FALSE);
		break;
	}
}


rspamd_io_dispatcher_t         *
rspamd_create_dispatcher (gint fd, enum io_policy policy,
	dispatcher_read_callback_t read_cb, dispatcher_write_callback_t write_cb, dispatcher_err_callback_t err_cb, struct timeval *tv, void *user_data)
{
	rspamd_io_dispatcher_t         *new;

	if (fd == -1) {
		return NULL;
	}

	new = g_malloc (sizeof (rspamd_io_dispatcher_t));
	bzero (new, sizeof (rspamd_io_dispatcher_t));

	new->pool = memory_pool_new (memory_pool_get_size ());
	if (tv != NULL) {
		new->tv = memory_pool_alloc (new->pool, sizeof (struct timeval));
		memcpy (new->tv, tv, sizeof (struct timeval));
	}
	else {
		new->tv = NULL;
	}
	new->nchars = 0;
	new->in_sendfile = FALSE;
	new->policy = policy;
	new->read_callback = read_cb;
	new->write_callback = write_cb;
	new->err_callback = err_cb;
	new->user_data = user_data;
	new->strip_eol = TRUE;

	new->ev = memory_pool_alloc0 (new->pool, sizeof (struct event));
	new->fd = fd;

	event_set (new->ev, fd, EV_WRITE, dispatcher_cb, (void *)new);
	event_add (new->ev, new->tv);

	return new;
}

void
rspamd_remove_dispatcher (rspamd_io_dispatcher_t * dispatcher)
{
	if (dispatcher != NULL) {
		event_del (dispatcher->ev);
		memory_pool_delete (dispatcher->pool);
		if (dispatcher->out_buffers) {
			g_list_free (dispatcher->out_buffers);
		}
		g_free (dispatcher);
	}
}

void
rspamd_set_dispatcher_policy (rspamd_io_dispatcher_t * d, enum io_policy policy, size_t nchars)
{
	f_str_t                        *tmp;
	gint                            t;

	if (d->policy != policy) {
		d->policy = policy;
		d->nchars = nchars ? nchars : BUFSIZ;
		/* Resize input buffer if needed */
		if (policy == BUFFER_CHARACTER && nchars != 0) {
			if (d->in_buf && d->in_buf->data->size < nchars) {
				tmp = fstralloc (d->pool, d->nchars + 1);
				memcpy (tmp->begin, d->in_buf->data->begin, d->in_buf->data->len);
				t = d->in_buf->pos - d->in_buf->data->begin;
				tmp->len = d->in_buf->data->len;
				d->in_buf->data = tmp;
				d->in_buf->pos = d->in_buf->data->begin + t;
			}
		}
		else if (policy == BUFFER_LINE || policy == BUFFER_ANY) {
			if (d->in_buf && d->nchars < BUFSIZ) {
				tmp = fstralloc (d->pool, BUFSIZ);
				memcpy (tmp->begin, d->in_buf->data->begin, d->in_buf->data->len);
				t = d->in_buf->pos - d->in_buf->data->begin;
				tmp->len = d->in_buf->data->len;
				d->in_buf->data = tmp;
				d->in_buf->pos = d->in_buf->data->begin + t;
			}
			d->strip_eol = TRUE;
		}
	}

	debug_ip (d->peer_addr, "new input length watermark is %uz", d->nchars);
}

gboolean
rspamd_dispatcher_write (rspamd_io_dispatcher_t * d, void *data, size_t len, gboolean delayed, gboolean allocated)
{
	rspamd_buffer_t                *newbuf;

	newbuf = memory_pool_alloc (d->pool, sizeof (rspamd_buffer_t));
	if (len == 0) {
		/* Assume NULL terminated */
		len = strlen ((gchar *)data);
	}

	if (!allocated) {
		newbuf->data = fstralloc (d->pool, len);

		/* We need to copy data to temporary internal buffer to avoid using of stack variables */
		memcpy (newbuf->data->begin, data, len);
	}
	else {
		newbuf->data = memory_pool_alloc (d->pool, sizeof (f_str_t));
		newbuf->data->begin = data;
		newbuf->data->size = len;
	}

	newbuf->pos = newbuf->data->begin;
	newbuf->data->len = len;

	d->out_buffers = g_list_prepend (d->out_buffers, newbuf);

	if (!delayed) {
		debug_ip (d->peer_addr, "plan write event");
		return write_buffers (d->fd, d, FALSE);
	}
	return TRUE;
}


gboolean 
rspamd_dispatcher_sendfile (rspamd_io_dispatcher_t *d, gint fd, size_t len)
{
	if (lseek (fd, 0, SEEK_SET) == -1) {
		msg_warn ("lseek failed: %s", strerror (errno));
		return FALSE;
	}

	d->offset = 0;
	d->in_sendfile = TRUE;
	d->sendfile_fd = fd;
	d->file_size = len;

#ifndef HAVE_SENDFILE
	#ifdef HAVE_MMAP_NOCORE
	if ((d->map = mmap (NULL, len, PROT_READ, MAP_SHARED | MAP_NOCORE, fd, 0)) == MAP_FAILED) {
	#else
	if ((d->map = mmap (NULL, len, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
	#endif
		msg_warn ("mmap failed: %s", strerror (errno));
		return FALSE;
	}
#endif

	return sendfile_callback (d);
}

void
rspamd_dispatcher_pause (rspamd_io_dispatcher_t * d)
{
	event_del (d->ev);
}

void
rspamd_dispatcher_restore (rspamd_io_dispatcher_t * d)
{
	event_add (d->ev, d->tv);
}

/* 
 * vi:ts=4 
 */
