/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Tim Waugh <twaugh@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "pd-common.h"
#include "pd-daemon.h"
#include "pd-engine.h"
#include "pd-job-impl.h"
#include "pd-printer-impl.h"

/**
 * SECTION:pdjob
 * @title: PdJobImpl
 * @short_description: Implementation of #PdJobImpl
 *
 * This type provides an implementation of the #PdJobImpl
 * interface on .
 */

typedef struct _PdJobImplClass	PdJobImplClass;

struct _PdJobProcess
{
	GPid		 pid;
	guint		 process_watch_source;

	guint		 io_source[4];
	GIOChannel	*channel[4];

	gint		 backchannel_fds[2];

	/* Data ready to send to this process */
	gchar		 buffer[1024];
	gsize		 buflen;
	gsize		 bufsent;
};

/**
 * PdJobImpl:
 *
 * The #PdJobImpl structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _PdJobImpl
{
	PdJobSkeleton	 parent_instance;
	PdDaemon	*daemon;
	gchar		*name;
	GHashTable	*attributes;
	GHashTable	*state_reasons;

	gint		 document_fd;
	gchar		*document_filename;

	struct _PdJobProcess spoolfile;
	struct _PdJobProcess arranger;
	struct _PdJobProcess backend;
};

struct _PdJobImplClass
{
	PdJobSkeletonClass parent_class;
};

enum
{
	PROP_0,
	PROP_DAEMON,
	PROP_NAME,
	PROP_ATTRIBUTES,
	PROP_STATE_REASONS,
};

static void pd_job_iface_init (PdJobIface *iface);
static void pd_job_impl_add_state_reason (PdJobImpl *job,
					  const gchar *reason);
static void pd_job_impl_remove_state_reason (PdJobImpl *job,
					     const gchar *reason);
static void pd_job_impl_job_state_notify (PdJobImpl *job);
static gboolean pd_job_impl_message_io_cb (GIOChannel *channel,
					   GIOCondition condition,
					   gpointer data);
static gboolean pd_job_impl_data_io_cb (GIOChannel *channel,
					GIOCondition condition,
					gpointer data);

G_DEFINE_TYPE_WITH_CODE (PdJobImpl, pd_job_impl, PD_TYPE_JOB_SKELETON,
			 G_IMPLEMENT_INTERFACE (PD_TYPE_JOB, pd_job_iface_init));

/* ------------------------------------------------------------------ */

static void
pd_job_impl_finalize (GObject *object)
{
	PdJobImpl *job = PD_JOB_IMPL (object);
	guint job_id = pd_job_get_id (PD_JOB (job));
	gint i;

	g_debug ("[Job %u] Finalize", job_id);
	/* note: we don't hold a reference to job->daemon */
	g_free (job->name);
	if (job->document_fd != -1)
		close (job->document_fd);
	if (job->document_filename) {
		g_unlink (job->document_filename);
		g_free (job->document_filename);
	}
	g_hash_table_unref (job->attributes);
	g_hash_table_unref (job->state_reasons);

	/* Stop reading spool file */
	if (job->spoolfile.io_source[STDOUT_FILENO] != -1)
		g_source_remove (job->spoolfile.io_source[STDOUT_FILENO]);
	if (job->spoolfile.channel[STDOUT_FILENO])
		g_io_channel_unref (job->spoolfile.channel[STDOUT_FILENO]);

	/* Shut down arranger */
	if (job->arranger.pid != -1) {
		g_debug ("[Job %u] Sending signal 9 to arranger PID %d",
			 job_id, job->arranger.pid);
		kill (job->arranger.pid, 9);
		g_spawn_close_pid (job->arranger.pid);
	}

	if (job->arranger.process_watch_source != 0)
		g_source_remove (job->arranger.process_watch_source);
	for (i = 0; i <= 3; i++)
		if (job->arranger.io_source[i] != -1)
			g_source_remove (job->arranger.io_source[i]);
	for (i = 0; i <= 3; i++)
		if (job->arranger.channel[i])
			g_io_channel_unref (job->arranger.channel[i]);

	/* Shut down backend */
	if (job->backend.pid != -1) {
		g_debug ("[Job %u] Sending signal 9 to backend PID %d",
			 job_id, job->backend.pid);
		kill (job->backend.pid, 9);
		g_spawn_close_pid (job->backend.pid);
	}

	if (job->backend.process_watch_source != 0)
		g_source_remove (job->backend.process_watch_source);
	for (i = 0; i <= 3; i++)
		if (job->backend.io_source[i] != -1)
			g_source_remove (job->backend.io_source[i]);
	for (i = 0; i <= 3; i++)
		if (job->backend.channel[i])
			g_io_channel_unref (job->backend.channel[i]);

	g_signal_handlers_disconnect_by_func (job,
					      pd_job_impl_job_state_notify,
					      job);
	G_OBJECT_CLASS (pd_job_impl_parent_class)->finalize (object);
}

static void
pd_job_impl_get_property (GObject *object,
			  guint prop_id,
			  GValue *value,
			  GParamSpec *pspec)
{
	PdJobImpl *job = PD_JOB_IMPL (object);
	GVariantBuilder builder;
	GHashTableIter iter;
	gchar *dkey;
	GVariant *dvalue;

	switch (prop_id) {
	case PROP_DAEMON:
		g_value_set_object (value, job->daemon);
		break;
	case PROP_NAME:
		g_value_set_string (value, job->name);
		break;
	case PROP_ATTRIBUTES:
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
		g_hash_table_iter_init (&iter, job->attributes);
		while (g_hash_table_iter_next (&iter,
					       (gpointer *) &dkey,
					       (gpointer *) &dvalue))
			g_variant_builder_add (&builder, "{sv}",
					       g_strdup (dkey), dvalue);

		g_value_set_variant (value, g_variant_builder_end (&builder));
		break;
	case PROP_STATE_REASONS:
		g_value_set_boxed (value,
				   g_hash_table_get_keys (job->state_reasons));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
pd_job_impl_set_property (GObject *object,
			  guint prop_id,
			  const GValue *value,
			  GParamSpec *pspec)
{
	PdJobImpl *job = PD_JOB_IMPL (object);
	GVariantIter iter;
	gchar *dkey;
	GVariant *dvalue;
	const gchar **state_reasons;
	const gchar **state_reason;

	switch (prop_id) {
	case PROP_DAEMON:
		g_assert (job->daemon == NULL);
		/* we don't take a reference to the daemon */
		job->daemon = g_value_get_object (value);
		break;
	case PROP_NAME:
		g_free (job->name);
		job->name = g_value_dup_string (value);
		break;
	case PROP_ATTRIBUTES:
		g_hash_table_remove_all (job->attributes);
		g_variant_iter_init (&iter, g_value_get_variant (value));
		while (g_variant_iter_next (&iter, "{sv}", &dkey, &dvalue))
			g_hash_table_insert (job->attributes, dkey, dvalue);
		break;
	case PROP_STATE_REASONS:
		state_reasons = g_value_get_boxed (value);
		g_hash_table_remove_all (job->state_reasons);
		for (state_reason = state_reasons;
		     *state_reason;
		     state_reason++) {
			gchar *r = g_strdup (*state_reason);
			g_hash_table_insert (job->state_reasons, r, r);
		}
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
pd_job_impl_init (PdJobImpl *job)
{
	gint i;

	g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (job),
					     G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

	job->document_fd = -1;

	job->backend.pid = -1;
	for (i = 0; i <= 3; i++)
		job->backend.io_source[i] = -1;

	job->arranger.pid = -1;
	for (i = 0; i <= 3; i++)
		job->arranger.io_source[i] = -1;

	job->attributes = g_hash_table_new_full (g_str_hash,
						 g_str_equal,
						 g_free,
						 (GDestroyNotify) g_variant_unref);

	job->state_reasons = g_hash_table_new_full (g_str_hash,
						    g_str_equal,
						    g_free,
						    NULL);
	gchar *incoming = g_strdup ("job-incoming");
	g_hash_table_insert (job->state_reasons, incoming, incoming);
	pd_job_set_state (PD_JOB (job), PD_JOB_STATE_PENDING_HELD);
}

static void
pd_job_impl_constructed (GObject *object)
{
	PdJobImpl *job = PD_JOB_IMPL (object);

	/* Watch our own state so we can start processing jobs when
	   entering state PD_JOB_STATE_PROCESSING. */
	g_signal_connect (job,
			  "notify::state",
			  G_CALLBACK (pd_job_impl_job_state_notify),
			  job);
}

static void
pd_job_impl_class_init (PdJobImplClass *klass)
{
	GObjectClass *gobject_class;

	gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = pd_job_impl_finalize;
	gobject_class->constructed = pd_job_impl_constructed;
	gobject_class->set_property = pd_job_impl_set_property;
	gobject_class->get_property = pd_job_impl_get_property;

	/**
	 * PdPrinterImpl:daemon:
	 *
	 * The #PdDaemon the printer is for.
	 */
	g_object_class_install_property (gobject_class,
					 PROP_DAEMON,
					 g_param_spec_object ("daemon",
							      "Daemon",
							      "The daemon the engine is for",
							      PD_TYPE_DAEMON,
							      G_PARAM_READABLE |
							      G_PARAM_WRITABLE |
							      G_PARAM_CONSTRUCT_ONLY |
							      G_PARAM_STATIC_STRINGS));

	/**
	 * PdJobImpl:name:
	 *
	 * The name for the job.
	 */
	g_object_class_install_property (gobject_class,
					 PROP_NAME,
					 g_param_spec_string ("name",
							      "Name",
							      "The name for the job",
							      NULL,
							      G_PARAM_READWRITE));

	/**
	 * PdJobImpl:attributes:
	 *
	 * The name for the job.
	 */
	g_object_class_install_property (gobject_class,
					 PROP_ATTRIBUTES,
					 g_param_spec_variant ("attributes",
							       "Attributes",
							       "The job attributes",
							       G_VARIANT_TYPE ("a{sv}"),
							       NULL,
							       G_PARAM_READWRITE));

	/**
	 * PdJobImpl:state-reasons:
	 *
	 * The job's state reasons.
	 */
	g_object_class_install_property (gobject_class,
					 PROP_STATE_REASONS,
					 g_param_spec_boxed ("state-reasons",
							     "State reasons",
							     "The job's state reasons",
							     G_TYPE_STRV,
							     G_PARAM_READWRITE));
}

/**
 * pd_job_impl_get_daemon:
 * @job: A #PdJobImpl.
 *
 * Gets the daemon used by @job.
 *
 * Returns: A #PdDaemon. Do not free, the object is owned by @job.
 */
PdDaemon *
pd_job_impl_get_daemon (PdJobImpl *job)
{
	g_return_val_if_fail (PD_IS_JOB_IMPL (job), NULL);
	return job->daemon;
}

static void
pd_job_impl_log_state_reason (PdJobImpl *job,
			      const gchar *reason,
			      gchar add_or_remove)
{
	GList *keys = g_hash_table_get_keys (job->state_reasons);
	GList *r;
	GString *reasons = NULL;

	for (r = g_list_first (keys); r; r = g_list_next (r)) {
		if (reasons == NULL) {
			reasons = g_string_new ("[");
			g_string_append (reasons, r->data);
		} else
			g_string_append_printf (reasons,
						",%s",
						(gchar *) r->data);
	}

	if (reasons)
		g_string_append_c (reasons, ']');

	g_debug ("[Job %u] State reasons %c= %s, now %s",
		 pd_job_get_id (PD_JOB (job)),
		 add_or_remove,
		 reason,
		 reasons ? reasons->str : "[none]");

	g_list_free (keys);
	if (reasons)
		g_string_free (reasons, TRUE);
}

static void
pd_job_impl_add_state_reason (PdJobImpl *job,
			      const gchar *reason)
{
	gchar *r = g_strdup (reason);
	g_hash_table_insert (job->state_reasons, r, r);
	pd_job_impl_log_state_reason (job, reason, '+');
}

static void
pd_job_impl_remove_state_reason (PdJobImpl *job,
				 const gchar *reason)
{
	if (g_hash_table_lookup (job->state_reasons, reason)) {
		g_hash_table_remove (job->state_reasons, reason);
		pd_job_impl_log_state_reason (job, reason, '-');
	}
}

static void
pd_job_impl_arranger_watch_cb (GPid pid,
			       gint status,
			       gpointer user_data)
{
	PdJobImpl *job = PD_JOB_IMPL (user_data);
	guint job_id = pd_job_get_id (PD_JOB (job));
	g_spawn_close_pid (pid);
	job->arranger.pid = -1;
	g_debug ("[Job %u] Arranger finished with status %d",
		 job_id, WEXITSTATUS (status));

	/* Close file descriptors */
	if (job->arranger.channel[STDIN_FILENO]) {
		g_io_channel_unref (job->arranger.channel[STDIN_FILENO]);
		job->arranger.channel[STDIN_FILENO] = NULL;
	}
}

static void
pd_job_impl_backend_watch_cb (GPid pid,
			      gint status,
			      gpointer user_data)
{
	PdJobImpl *job = PD_JOB_IMPL (user_data);
	guint job_id = pd_job_get_id (PD_JOB (job));
	g_spawn_close_pid (pid);
	job->backend.pid = -1;
	g_debug ("[Job %u] Backend finished with status %d",
		 job_id, WEXITSTATUS (status));

	if (WEXITSTATUS (status) == 0) {
		if (g_hash_table_lookup (job->state_reasons,
					 "processing-to-stop-point")) {
			g_debug ("[Job %u] -> Set job state to canceled",
				 job_id);
			pd_job_set_state (PD_JOB (job),
					  PD_JOB_STATE_CANCELED);
		} else {
			g_debug ("[Job %u] -> Set job state to completed",
				 job_id);
			pd_job_set_state (PD_JOB (job),
					  PD_JOB_STATE_COMPLETED);
		}
	} else {
		g_debug ("[Job %u] -> Set job state to aborted", job_id);
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_ABORTED);
	}

	/* Close file descriptors */
	if (job->backend.channel[STDIN_FILENO]) {
		g_io_channel_unref (job->backend.channel[STDIN_FILENO]);
		job->backend.channel[STDIN_FILENO] = NULL;
	}
}

static void
pd_job_impl_parse_stderr (PdJobImpl *job,
			  const gchar *line)
{
	if (!strncmp (line, "STATE:", 6)) {
		const gchar *token = line + 6;
		const gchar *end;
		gchar *reason;
		gchar add_or_remove;

		/* Skip whitespace */
		token += strspn (token, " \t");
		add_or_remove = token[0];
		if (add_or_remove == '+' ||
		    add_or_remove == '-') {
			token++;
			while (token[0] != '\0') {
				end = token + strcspn (token, ", \t\n");
				reason = g_strndup (token, end - token);

				/* Process this reason */
				if (add_or_remove == '+')
					pd_job_impl_add_state_reason (job,
								      reason);
				else
					pd_job_impl_remove_state_reason (job,
									 reason);

				g_free (reason);

				/* Next reason */
				token = end + strspn (end, ", \t\n");
			}
		}
	}
}

static gboolean
pd_job_impl_data_io_cb (GIOChannel *channel,
			GIOCondition condition,
			gpointer data)
{
	PdJobImpl *job = PD_JOB_IMPL (data);
	guint job_id = pd_job_get_id (PD_JOB (job));
	gboolean keep_source = TRUE;
	GError *error = NULL;
	GIOStatus status;
	gsize got, wrote;
	struct _PdJobProcess *thisjp;
	struct _PdJobProcess *nextjp;
	GIOChannel *nextchannel;
	gint thisfd;
	gint nextfd;
	const gchar *what;
	struct _PdJobProcess discard;

	if (condition & (G_IO_IN | G_IO_HUP)) {
		nextfd = STDIN_FILENO;
		if (channel == job->spoolfile.channel[STDOUT_FILENO]) {
			what = "spool file";
			thisfd = STDOUT_FILENO;
			thisjp = &job->spoolfile;
			nextjp = &job->arranger;
		} else if (channel == job->arranger.channel[STDOUT_FILENO]) {
			what = "arranger";
			thisfd = STDOUT_FILENO;
			thisjp = &job->arranger;
			nextjp = &job->backend;
		} else if (channel == job->arranger.channel[3]) {
			what = "arranger (back-channel)";
			thisfd = 3;
			thisjp = &job->arranger;
			nextjp = &discard;
			memset (&discard, 0, sizeof (discard));
		} else {
			g_assert (channel == job->backend.channel[3]);
			thisfd = 3;
			what = "backend (back-channel)";
			thisjp = &job->backend;
			nextjp = &discard;
			memset (&discard, 0, sizeof (discard));
		}

		/* Read some data */
		status = g_io_channel_read_chars (channel,
						  nextjp->buffer,
						  sizeof (nextjp->buffer),
						  &got,
						  &error);
		switch (status) {
		case G_IO_STATUS_NORMAL:
			nextjp->buflen = got;
			nextjp->bufsent = 0;
			g_debug ("[Job %u] Read %zu bytes from %s",
				 job_id,
				 got,
				 what);

			nextchannel = nextjp->channel[nextfd];
			if (nextchannel)
				nextjp->io_source[nextfd] =
					g_io_add_watch (nextchannel,
							G_IO_OUT,
							pd_job_impl_data_io_cb,
							job);

			keep_source = FALSE;
			break;

		case G_IO_STATUS_EOF:
			g_debug ("[Job %u] Output from %s closed",
				 job_id,
				 what);
			g_io_channel_unref (channel);

			keep_source = FALSE;
			break;

		case G_IO_STATUS_ERROR:
			g_warning ("[Job %u] read() from %s failed: %s",
				   job_id,
				   what,
				   error->message);
			g_error_free (error);
			break;

		case G_IO_STATUS_AGAIN:
			/* End of data for now */
			break;
		}

		if (!keep_source) {
			thisjp->io_source[thisfd] = -1;

			if (status == G_IO_STATUS_EOF) {
				thisjp->channel[thisfd] = NULL;

				nextchannel = nextjp->channel[nextfd];
				if (nextchannel)
					nextjp->io_source[nextfd] =
						g_io_add_watch (nextchannel,
								G_IO_OUT,
								pd_job_impl_data_io_cb,
								job);

			}
		}
	} else {
		g_assert (condition == G_IO_OUT);
		thisfd = STDIN_FILENO;
		nextfd = STDOUT_FILENO;
		if (channel == job->arranger.channel[STDIN_FILENO]) {
			what = "arranger";
			thisjp = &job->arranger;
			nextjp = &job->spoolfile;
		} else {
			g_assert (channel == job->backend.channel[STDIN_FILENO]);
			what = "backend";
			thisjp = &job->backend;
			nextjp = &job->arranger;
		}

		if (thisjp->buflen - thisjp->bufsent == 0) {
			/* End of input */
			g_io_channel_shutdown (channel,
					       TRUE,
					       NULL);
			keep_source = FALSE;
			thisjp->io_source[thisfd] = -1;
			g_debug ("[Job %u] Closing input to %s",
				 job_id, what);
			thisjp->channel[thisfd] = NULL;
			g_io_channel_unref (channel);
			goto out;
		}

		status = g_io_channel_write_chars (channel,
						   thisjp->buffer +
						   thisjp->bufsent,
						   thisjp->buflen -
						   thisjp->bufsent,
						   &wrote,
						   &error);
		switch (status) {
		case G_IO_STATUS_ERROR:
			g_warning ("[Job %u] Error writing to %s: %s",
				   job_id, what, error->message);
			g_error_free (error);
			goto out;
		case G_IO_STATUS_EOF:
			g_debug ("[Job %u] EOF on write?", job_id);
			break;
		case G_IO_STATUS_AGAIN:
			g_debug ("[Job %u] Resource temporarily unavailable",
				 job_id);
			break;
		case G_IO_STATUS_NORMAL:
			g_debug ("[Job %u] Wrote %zu bytes to %s",
				 job_id, wrote, what);
			break;
		}

		thisjp->bufsent += wrote;

		if (thisjp->buflen - thisjp->bufsent == 0) {
			/* Need to read more data now */
			keep_source = FALSE;
			thisjp->io_source[thisfd] = -1;

			nextchannel = nextjp->channel[nextfd];
			nextjp->io_source[nextfd] = g_io_add_watch (nextchannel,
								    G_IO_IN |
								    G_IO_HUP,
								    pd_job_impl_data_io_cb,
								    job);
		}

	}

 out:
	return keep_source;
}

static gboolean
pd_job_impl_message_io_cb (GIOChannel *channel,
			   GIOCondition condition,
			   gpointer data)
{
	PdJobImpl *job = PD_JOB_IMPL (data);
	guint job_id = pd_job_get_id (PD_JOB (job));
	gboolean keep_source = TRUE;
	GError *error = NULL;
	GIOStatus status;
	gchar *line = NULL;
	gsize got;

	g_assert (condition == G_IO_IN || condition == G_IO_HUP);

	while ((status = g_io_channel_read_line (channel,
						 &line,
						 &got,
						 NULL,
						 &error)) ==
	       G_IO_STATUS_NORMAL) {
		if (channel == job->arranger.channel[STDERR_FILENO]) {
			/* Read messages from arranger */
			g_debug ("[Job %u] arranger(stderr): %s",
				 job_id,
				 g_strchomp (line));

			pd_job_impl_parse_stderr (job, line);
		} else if (channel == job->backend.channel[STDERR_FILENO]) {
			/* Read messages from backend */
			g_debug ("[Job %u] backend(stderr): %s",
				 job_id,
				 g_strchomp (line));

			pd_job_impl_parse_stderr (job, line);
		} else {
			g_assert (channel == job->backend.channel[STDOUT_FILENO]);
			g_debug ("[Job %u] backend(stdout): %s",
				 job_id,
				 g_strchomp (line));
		}
	}

	switch (status) {
	case G_IO_STATUS_ERROR:
		g_warning ("[Job %u] Error reading from channel: %s",
			   job_id, error->message);
		g_error_free (error);
		goto out;
	case G_IO_STATUS_EOF:
		g_io_channel_unref (channel);
		if (channel == job->arranger.channel[STDOUT_FILENO]) {
			g_debug ("[Job %u] Arranger stdout closed",
				 job_id);
			job->arranger.channel[STDOUT_FILENO] = NULL;
			job->arranger.io_source[STDOUT_FILENO] = -1;
		} else if (channel == job->backend.channel[STDOUT_FILENO]) {
			g_debug ("[Job %u] Backend stdout closed",
				 job_id);
			job->backend.channel[STDOUT_FILENO] = NULL;
			job->backend.io_source[STDOUT_FILENO] = -1;
		} else if (channel == job->arranger.channel[STDERR_FILENO]) {
			g_debug ("[Job %u] Arranger stderr closed",
				 job_id);
			job->arranger.channel[STDERR_FILENO] = NULL;
			job->arranger.io_source[STDERR_FILENO] = -1;
		} else {
			g_assert (channel == job->backend.channel[STDERR_FILENO]);
			g_debug ("[Job %u] Backend stderr closed",
				 job_id);
			job->backend.channel[STDERR_FILENO] = NULL;
			job->backend.io_source[STDERR_FILENO] = -1;
		}

		keep_source = FALSE;
		break;
	case G_IO_STATUS_AGAIN:
		/* End of messages for now */
		break;
	case G_IO_STATUS_NORMAL:
		g_assert_not_reached ();
		break;
	}

 out:
	if (line)
		g_free (line);
	return keep_source;
}

static void
pd_job_impl_process_setup (gpointer user_data)
{
	struct _PdJobProcess *jp = (struct _PdJobProcess *) user_data;

	/* Set our writable back-channel to a known fd */
	if (jp->backchannel_fds[1] != 3) {
		dup2 (jp->backchannel_fds[1], 3);
		close (jp->backchannel_fds[1]);
	}

	/* Close the parent's end of the pipe */
	close (jp->backchannel_fds[0]);
}

static gboolean
pd_job_impl_run_process (PdJobImpl *job,
			 const gchar *cmd,
			 struct _PdJobProcess *jp,
			 GError **error)
{
	gboolean ret = FALSE;
	guint job_id = pd_job_get_id (PD_JOB (job));
	gchar *username;
	GVariant *variant;
	const gchar *uri;
	char **argv = NULL;
	char **envp = NULL;
	gint process_fd[3];
	gchar **s;
	gint i;

	uri = pd_job_get_device_uri (PD_JOB (job));
	variant = g_hash_table_lookup (job->attributes,
				       "job-originating-user-name");
	if (variant) {
		username = g_variant_dup_string (variant, NULL);
		/* no need to free variant: we don't own a reference */
	} else
		username = g_strdup ("unknown");

	argv = g_malloc0 (sizeof (char *) * 8);
	argv[0] = g_strdup (cmd);
	/* URI */
	argv[1] = g_strdup (uri);
	/* Job ID */
	argv[2] = g_strdup_printf ("%u", job_id);
	/* User name */
	argv[3] = username;
	/* Job title */
	argv[4] = g_strdup_printf ("job %u", job_id);
	/* Copies */
	argv[5] = g_strdup ("1");
	/* Options */
	argv[6] = g_strdup ("");
	argv[7] = NULL;

	envp = g_malloc0 (sizeof (char *) * 2);
	envp[0] = g_strdup_printf ("DEVICE_URI=%s", uri);
	envp[1] = NULL;

	g_debug ("[Job %u] Executing %s", job_id, argv[0]);
	for (s = envp; *s; s++)
		g_debug ("[Job %u]  Env: %s", job_id, *s);
	for (s = argv + 1; *s; s++)
		g_debug ("[Job %u]  Arg: %s", job_id, *s);

	/* Set up back-channel pipe */
	if (pipe (jp->backchannel_fds) == -1) {
		g_set_error (error,
			     PD_ERROR,
			     PD_ERROR_FAILED,
			     "Failed to create back-channel pipe: %s",
			     g_strerror (errno));
		goto out;
	}

	jp->channel[3] = g_io_channel_unix_new (jp->backchannel_fds[0]);
	g_io_channel_set_flags (jp->channel[3],
				G_IO_FLAG_NONBLOCK,
				NULL);
	g_io_channel_set_close_on_unref (jp->channel[3],
					 TRUE);
	g_io_channel_set_encoding (jp->channel[3], NULL, NULL);

	ret = g_spawn_async_with_pipes ("/" /* wd */,
					argv,
					envp,
					G_SPAWN_DO_NOT_REAP_CHILD |
					G_SPAWN_FILE_AND_ARGV_ZERO,
					pd_job_impl_process_setup,
					jp,
					&jp->pid,
					&process_fd[STDIN_FILENO],
					&process_fd[STDOUT_FILENO],
					&process_fd[STDERR_FILENO],
					error);
	if (!ret)
		goto out;

	/* Close the backend's end of the pipe now they've started */
	close (jp->backchannel_fds[1]);
	jp->backchannel_fds[1] = -1;

	/* Set up IO channels */
	for (i = 0; i < 3; i++) {
		GIOChannel *channel = g_io_channel_unix_new (process_fd[i]);
		g_io_channel_set_flags (channel,
					G_IO_FLAG_NONBLOCK,
					NULL);
		g_io_channel_set_close_on_unref (channel,
						 TRUE);
		if (i == STDIN_FILENO)
			g_io_channel_set_encoding (channel,
						   NULL,
						   NULL);

		jp->channel[i] = channel;
	}

 out:
	g_strfreev (argv);
	g_strfreev (envp);
	return ret;
}

/**
 * pd_job_impl_start_processing:
 * @job: A #PdJobImpl
 *
 * The job state is processing and the printer is ready so
 * start processing the job.
 */
static void
pd_job_impl_start_processing (PdJobImpl *job)
{
	GError *error = NULL;
	guint job_id = pd_job_get_id (PD_JOB (job));
	const gchar *printer_path;
	const gchar *uri;
	PdPrinter *printer = NULL;
	char *scheme = NULL;
	gchar *cmd = NULL;
	GIOChannel *channel;

	if (job->backend.pid != -1) {
		g_warning ("[Job %u] Already processing!", job_id);
		goto out;
	}

	g_debug ("[Job %u] Starting to process job", job_id);

	/* No filtering yet (to be done): instead just run it through
	   the backend. */

	/* Get the device URI to use from the Printer */
	printer_path = pd_job_get_printer (PD_JOB (job));
	printer = pd_engine_get_printer_by_path (pd_daemon_get_engine (job->daemon),
						 printer_path);
	if (!printer) {
		g_warning ("[Job %u] Incorrect printer path %s",
			   job_id, printer_path);
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_ABORTED);
		goto out;
	}

	uri = pd_printer_impl_get_uri (PD_PRINTER_IMPL (printer));
	g_debug ("[Job %u] Using device URI %s", job_id, uri);
	pd_job_set_device_uri (PD_JOB (job), uri);
	scheme = g_uri_parse_scheme (uri);

	/* Run backend */
	cmd = g_strdup_printf ("/usr/lib/cups/backend/%s", scheme);
	if (!pd_job_impl_run_process (job, cmd, &job->backend, &error)) {
		g_error ("[Job %u] Running backend: %s",
			 job_id, error->message);
		g_error_free (error);

		/* Update job state */
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_ABORTED);
		goto out;
	}

	job->backend.process_watch_source = 
		g_child_watch_add (job->backend.pid,
				   pd_job_impl_backend_watch_cb,
				   job);

	job->backend.io_source[STDIN_FILENO] = -1;

	channel = job->backend.channel[STDOUT_FILENO];
	job->backend.io_source[STDOUT_FILENO] =
		g_io_add_watch (channel,
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_message_io_cb,
				job);

	channel = job->backend.channel[STDERR_FILENO];
	job->backend.io_source[STDERR_FILENO] =
		g_io_add_watch (channel,
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_message_io_cb,
				job);

	job->backend.io_source[3] =
		g_io_add_watch (job->backend.channel[3],
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_data_io_cb,
				job);

	/* Run arranger */
	g_free (cmd);
	cmd = g_strdup_printf ("/usr/lib/cups/filter/pstops");
	if (!pd_job_impl_run_process (job, cmd, &job->arranger, &error)) {
		g_error ("[Job %u] Running arranger: %s",
			 job_id, error->message);
		g_error_free (error);
		goto out;
	}

	job->arranger.process_watch_source =
		g_child_watch_add (job->arranger.pid,
				   pd_job_impl_arranger_watch_cb,
				   job);

	job->arranger.io_source[STDIN_FILENO] = -1;

	channel = job->arranger.channel[STDOUT_FILENO];
	job->arranger.io_source[STDOUT_FILENO] =
		g_io_add_watch (channel,
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_data_io_cb,
				job);

	channel = job->arranger.channel[STDERR_FILENO];
	job->arranger.io_source[STDERR_FILENO] =
		g_io_add_watch (channel,
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_message_io_cb,
				job);

	job->arranger.io_source[3] =
		g_io_add_watch (job->arranger.channel[3],
				G_IO_IN |
				G_IO_HUP,
				pd_job_impl_data_io_cb,
				job);

	/* Open document */
	channel = g_io_channel_new_file (job->document_filename,
					 "r",
					 &error);
	if (channel == NULL) {
		g_error ("[Job %u] Failed to open spool file %s: %s",
			 job_id, job->document_filename, error->message);
		g_error_free (error);

		/* Update job state */
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_ABORTED);
		goto out;
	}

	g_io_channel_set_encoding (channel,
				   NULL,
				   NULL);
	job->spoolfile.channel[STDOUT_FILENO] = channel;

	/* Start sending document */
	job->arranger.buflen = 0;
	job->arranger.bufsent = 0;
	job->spoolfile.io_source[STDOUT_FILENO] =
		g_io_add_watch (job->spoolfile.channel[STDOUT_FILENO],
				G_IO_IN,
				pd_job_impl_data_io_cb,
				job);
 out:
	if (printer)
		g_object_unref (printer);
	g_free (scheme);
	g_free (cmd);
}

static void
pd_job_impl_job_state_notify (PdJobImpl *job)
{
	/* This function watches changes to the job state and
	   starts/stop things accordingly. */

	switch (pd_job_get_state (PD_JOB (job))) {
	case PD_JOB_STATE_PROCESSING:
		/* Job has moved to processing state. */

		if (job->backend.pid == -1)
			/* Not running it yet, so do that. */
			pd_job_impl_start_processing (job);

		break;

	case PD_JOB_STATE_CANCELED:
	case PD_JOB_STATE_ABORTED:
	case PD_JOB_STATE_COMPLETED:
		/* Job is now terminated. */
		pd_job_impl_remove_state_reason (job, "job-incoming");
		pd_job_impl_remove_state_reason (job,
						 "processing-to-stop-point");

		g_signal_handlers_disconnect_by_func (job,
						      pd_job_impl_job_state_notify,
						      job);
	default:
		;
	}
}

void
pd_job_impl_set_attribute (PdJobImpl *job,
			   const gchar *name,
			   GVariant *value)
{
	g_hash_table_insert (job->attributes,
			     g_strdup (name),
			     g_variant_ref_sink (value));
}

/* ------------------------------------------------------------------ */

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_job_impl_add_document (PdJob *_job,
			  GDBusMethodInvocation *invocation,
			  GVariant *options,
			  GVariant *file_descriptor)
{
	PdJobImpl *job = PD_JOB_IMPL (_job);
	guint job_id = pd_job_get_id (PD_JOB (job));
	GDBusMessage *message;
	GUnixFDList *fd_list;
	gint32 fd_handle;
	GError *error = NULL;
	GVariant *attr_user;
	gchar *requesting_user = NULL;
	const gchar *originating_user = NULL;

	/* Check if the user is authorized to add a document */
	if (!pd_daemon_check_authorization_sync (job->daemon,
						 "org.freedesktop.printerd.job-add",
						 options,
						 N_("Authentication is required to add a job"),
						 invocation))
		goto out;

	/* Check if this user owns the job */
	attr_user = g_hash_table_lookup (job->attributes,
					 "job-originating-user-name");
	if (attr_user)
		originating_user = g_variant_get_string (attr_user, NULL);
	requesting_user = pd_get_unix_user (invocation);
	if (g_strcmp0 (originating_user, requesting_user)) {
		g_debug ("[Job %u] AddDocument: denied "
			 "[originating user: %s; requesting user: %s]",
			 job_id, originating_user, requesting_user);
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("Not job owner"));
		goto out;
	}

	if (job->document_fd != -1 ||
	    job->document_filename != NULL) {
		g_debug ("[Job %u] Tried to add second document", job_id);
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("No more documents allowed"));
		goto out;
	}

	g_debug ("[Job %u] Adding document", job_id);
	message = g_dbus_method_invocation_get_message (invocation);
	fd_list = g_dbus_message_get_unix_fd_list (message);
	if (fd_list != NULL && g_unix_fd_list_get_length (fd_list) == 1) {
		fd_handle = g_variant_get_handle (file_descriptor);
		job->document_fd = g_unix_fd_list_get (fd_list,
						       fd_handle,
						       &error);
		if (job->document_fd < 0) {
			g_debug ("[Job %u] failed to get file descriptor: %s",
				 job_id, error->message);
			g_dbus_method_invocation_return_gerror (invocation,
								error);
			g_error_free (error);
			goto out;
		}

		g_debug ("[Job %u] Got file descriptor: %d",
			 job_id, job->document_fd);
	}

	g_dbus_method_invocation_return_value (invocation, NULL);

 out:
	g_free (requesting_user);
	return TRUE; /* handled the method invocation */
}

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_job_impl_start (PdJob *_job,
		   GDBusMethodInvocation *invocation,
		   GVariant *options)
{
	PdJobImpl *job = PD_JOB_IMPL (_job);
	guint job_id = pd_job_get_id (PD_JOB (job));
	gchar *name_used = NULL;
	GError *error = NULL;
	gint infd = -1;
	gint spoolfd = -1;
	char buffer[1024];
	ssize_t got, wrote;
	GVariant *attr_user;
	gchar *requesting_user = NULL;
	const gchar *originating_user = NULL;

	/* Check if the user is authorized to start a job */
	if (!pd_daemon_check_authorization_sync (job->daemon,
						 "org.freedesktop.printerd.job-add",
						 options,
						 N_("Authentication is required to add a job"),
						 invocation))
		goto out;

	/* Check if this user owns the job */
	attr_user = g_hash_table_lookup (job->attributes,
					 "job-originating-user-name");
	if (attr_user)
		originating_user = g_variant_get_string (attr_user, NULL);
	requesting_user = pd_get_unix_user (invocation);
	if (g_strcmp0 (originating_user, requesting_user)) {
		g_debug ("[Job %u] AddDocument: denied "
			 "[originating user: %s; requesting user: %s]",
			 job_id, originating_user, requesting_user);
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("Not job owner"));
		goto out;
	}

	if (job->document_fd == -1) {
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       "No document");
		goto out;
	}

	g_assert (job->document_filename == NULL);
	spoolfd = g_file_open_tmp ("printerd-spool-XXXXXX",
				   &name_used,
				   &error);

	if (spoolfd < 0) {
		g_debug ("[Job %u] Error making temporary file: %s",
			 job_id, error->message);
		g_dbus_method_invocation_return_gerror (invocation,
							error);
		g_error_free (error);
		goto out;
	}

	job->document_filename = g_strdup (name_used);

	g_debug ("[Job %u] Starting job", job_id);

	g_debug ("[Job %u] Spooling", job_id);
	g_debug ("[Job %u]   Created temporary file %s", job_id, name_used);
	infd = job->document_fd;
	job->document_fd = -1;

	for (;;) {
		char *ptr;
		got = read (infd, buffer, sizeof (buffer));
		if (got == 0)
			/* end of file */
			break;
		else if (got < 0) {
			/* error */
			g_dbus_method_invocation_return_error (invocation,
							       PD_ERROR,
							       PD_ERROR_FAILED,
							       "Error reading file");
			goto out;
		}

		ptr = buffer;
		while (got > 0) {
			wrote = write (spoolfd, ptr, got);
			if (wrote == -1) {
				if (errno == EINTR)
					continue;
				else {
					g_dbus_method_invocation_return_error (invocation,
									       PD_ERROR,
									       PD_ERROR_FAILED,
									       "Error writing file");
					goto out;
				}
			}

			ptr += wrote;
			got -= wrote;
		}
	}

	/* Job is no longer incoming so remove that state reason if
	   present */
	pd_job_impl_remove_state_reason (job, "job-incoming");

	/* Move the job state to pending: this is now a candidate to
	   start processing */
	g_debug ("[Job %u]  Set job state to pending", job_id);
	pd_job_set_state (PD_JOB (job), PD_JOB_STATE_PENDING);

	/* Return success */
	g_dbus_method_invocation_return_value (invocation,
					       g_variant_new ("()"));

 out:
	g_free (requesting_user);
	if (infd != -1)
		close (infd);
	if (spoolfd != -1)
		close (spoolfd);
	g_free (name_used);
	return TRUE; /* handled the method invocation */
}

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_job_impl_cancel (PdJob *_job,
		    GDBusMethodInvocation *invocation,
		    GVariant *options)
{
	PdJobImpl *job = PD_JOB_IMPL (_job);
	guint job_id = pd_job_get_id (PD_JOB (job));
	GVariant *attr_user;
	gchar *requesting_user = NULL;
	const gchar *originating_user = NULL;

	/* Check if the user is authorized to cancel this job */
	if (!pd_daemon_check_authorization_sync (job->daemon,
						 "org.freedesktop.printerd.job-cancel",
						 options,
						 N_("Authentication is required to cancel a job"),
						 invocation))
		goto out;

	/* Check if this user owns the job */
	attr_user = g_hash_table_lookup (job->attributes,
					 "job-originating-user-name");
	if (attr_user)
		originating_user = g_variant_get_string (attr_user, NULL);
	requesting_user = pd_get_unix_user (invocation);
	if (g_strcmp0 (originating_user, requesting_user)) {
		g_debug ("[Job %u] AddDocument: denied "
			 "[originating user: %s; requesting user: %s]",
			 job_id, originating_user, requesting_user);
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("Not job owner"));
		goto out;
	}

        /* RFC 2911, 3.3.3: only these jobs in these states can be
	   canceled */
	switch (pd_job_get_state (_job)) {
	case PD_JOB_STATE_PENDING:
	case PD_JOB_STATE_PENDING_HELD:
		/* These can be canceled right away. */

		/* Change job state. */
		g_debug ("[Job %u] Canceled", job_id);
		pd_job_impl_add_state_reason (job, "canceled-by-user");
		pd_job_set_state (_job, PD_JOB_STATE_CANCELED);

		/* Return success */
		g_dbus_method_invocation_return_value (invocation,
						       g_variant_new ("()"));
		break;

	case PD_JOB_STATE_PROCESSING:
	case PD_JOB_STATE_PROCESSING_STOPPED:
		/* These need some time to tell the printer to
		   e.g. eject the current page */

		if (g_hash_table_lookup (job->state_reasons,
					 "processing-to-stop-point")) {
			g_dbus_method_invocation_return_error (invocation,
							       PD_ERROR,
							       PD_ERROR_FAILED,
							       N_("Already canceled"));
			goto out;
		}

		pd_job_impl_add_state_reason (job, "processing-to-stop-point");

		if (job->backend.channel[STDIN_FILENO] != NULL) {
			/* Stop sending data to the backend. */
			g_debug ("[Job %u] Stop sending data to the backend",
				 job_id);
			g_source_remove (job->backend.io_source[STDIN_FILENO]);
			g_io_channel_shutdown (job->backend.channel[STDIN_FILENO],
					       FALSE,
					       NULL);
			g_io_channel_unref (job->backend.channel[STDIN_FILENO]);
			job->backend.channel[STDIN_FILENO] = NULL;
		}

		/* Simple implementation for now: just kill the processes */
		if (job->arranger.pid != -1) {
			g_debug ("[Job %u] Sending signal 9 to PID %d",
				 job_id,
				 job->arranger.pid);
			kill (job->arranger.pid, 9);
			g_spawn_close_pid (job->arranger.pid);
		}

		if (job->backend.pid != -1) {
			g_debug ("[Job %u] Sending signal 9 to PID %d",
				 job_id,
				 job->backend.pid);
			kill (job->backend.pid, 9);
			g_spawn_close_pid (job->backend.pid);
		}

		g_dbus_method_invocation_return_value (invocation,
						       g_variant_new ("()"));
		break;

	default: /* only completed job states remaining */
		/* Send error */
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("Cannot cancel completed job"));
		break;
		;
	}

 out:
	g_free (requesting_user);
	return TRUE; /* handled the method invocation */
}


static void
pd_job_iface_init (PdJobIface *iface)
{
	iface->handle_add_document = pd_job_impl_add_document;
	iface->handle_start = pd_job_impl_start;
	iface->handle_cancel = pd_job_impl_cancel;
}
