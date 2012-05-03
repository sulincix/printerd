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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib/gstdio.h>

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
	GPid		 backend_pid;
	guint		 backend_watch_source;
	guint		 backend_io_source;
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

G_DEFINE_TYPE_WITH_CODE (PdJobImpl, pd_job_impl, PD_TYPE_JOB_SKELETON,
			 G_IMPLEMENT_INTERFACE (PD_TYPE_JOB, pd_job_iface_init));

/* ------------------------------------------------------------------ */

static void
pd_job_impl_finalize (GObject *object)
{
	PdJobImpl *job = PD_JOB_IMPL (object);
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
	if (job->backend_pid != -1)
		g_spawn_close_pid (job->backend_pid);
	if (job->backend_watch_source != 0)
		g_source_remove (job->backend_watch_source);
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
	g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (job),
					     G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

	job->document_fd = -1;
	job->backend_pid = -1;
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

	pd_job_set_state (PD_JOB (job),
			  PD_JOB_STATE_PENDING_HELD);
}

static void
pd_job_impl_class_init (PdJobImplClass *klass)
{
	GObjectClass *gobject_class;

	gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = pd_job_impl_finalize;
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
pd_job_impl_backend_watch_cb (GPid pid,
			      gint status,
			      gpointer user_data)
{
	PdJobImpl *job = PD_JOB_IMPL (user_data);
	g_spawn_close_pid (pid);
	job->backend_pid = -1;
	g_debug ("Backend finished with status %d", WEXITSTATUS (status));

	if (WEXITSTATUS (status) == 0) {
		g_debug ("-> Set job state to completed");
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_COMPLETED);
	} else {
		g_debug ("-> Set job state to aborted");
		pd_job_set_state (PD_JOB (job),
				  PD_JOB_STATE_ABORTED);
	}
}

static gboolean
pd_job_impl_backend_io_cb (GIOChannel *channel,
			   GIOCondition condition,
			   gpointer which)
{
	GError *error = NULL;
	GIOStatus status;
	gchar buffer[1024];
	gsize got;

	if (which == stdout) {
		g_debug ("Backend stdout output:");
	} else {
		/* Shouldn't get here! */
		g_assert_not_reached ();
	}

	g_assert (condition == G_IO_IN);

	status = g_io_channel_read_chars (channel,
					  buffer,
					  sizeof (buffer) - 1,
					  &got,
					  &error);
	switch (status) {
	case G_IO_STATUS_ERROR:
		g_warning ("Error reading from channel: %s", error->message);
		g_error_free (error);
		goto out;
	case G_IO_STATUS_EOF:
		g_debug ("Backend output finished");
		break;
	case G_IO_STATUS_AGAIN:
		g_debug ("Resource temporarily unavailable (weird?)");
		break;
	case G_IO_STATUS_NORMAL:
		break;
	}

	buffer[sizeof (buffer) - 1] = '\0';
	g_debug ("%s", g_strchomp (buffer));
 out:
	return TRUE; /* don't remove this source */
}

/**
 * pd_job_impl_start_processing:
 * @job: A #PdJobImpl
 *
 * The job is available for processing and the printer is ready so
 * start processing the job.
 */
void
pd_job_impl_start_processing (PdJobImpl *job)
{
	GError *error = NULL;
	const gchar *printer_path;
	const gchar *uri;
	gchar *username;
	GVariant *variant;
	PdPrinter *printer = NULL;
	char *scheme = NULL;
	char **argv = NULL;
	char **envp = NULL;
	gchar **s;
	gint stdin_fd, stdout_fd, stderr_fd;
	GIOChannel *stdout_channel = NULL;

	g_debug ("Starting to process job %u", pd_job_get_id (PD_JOB (job)));

	/* No filtering yet (to be done): instead just run it through
	   the backend. */

	/* Get the device URI to use from the Printer */
	printer_path = pd_job_get_printer (PD_JOB (job));
	printer = pd_engine_get_printer_by_path (pd_daemon_get_engine (job->daemon),
						 printer_path);
	if (!printer) {
		g_warning ("Incorrect printer path %s", printer_path);
		goto out;
	}

	uri = pd_printer_impl_get_uri (PD_PRINTER_IMPL (printer));
	g_debug ("  Using device URI %s", uri);
	pd_job_set_device_uri (PD_JOB (job), uri);
	scheme = g_uri_parse_scheme (uri);

	variant = g_hash_table_lookup (job->attributes,
				       "job-originating-user-name");
	if (variant) {
		username = g_variant_dup_string (variant, NULL);
		/* no need to free variant: we don't own a reference */
	} else
		username = g_strdup ("unknown");

	argv = g_malloc0 (sizeof (char *) * 8);
	argv[0] = g_strdup_printf ("/usr/lib/cups/backend/%s", scheme);
	/* URI */
	argv[1] = g_strdup (uri);
	/* Job ID */
	argv[2] = g_strdup_printf ("%u", pd_job_get_id (PD_JOB (job)));
	/* User name */
	argv[3] = username;
	/* Job title */
	argv[4] = g_strdup_printf ("job %u", pd_job_get_id (PD_JOB (job)));
	/* Copies */
	argv[5] = g_strdup ("1");
	/* Options */
	argv[6] = g_strdup ("");
	argv[7] = NULL;

	envp = g_malloc0 (sizeof (char *) * 2);
	envp[0] = g_strdup_printf ("DEVICE_URI=%s", uri);
	envp[1] = NULL;

	g_debug ("  Executing %s", argv[0]);
	for (s = envp; *s; s++)
		g_debug ("    Env: %s", *s);
	for (s = argv + 1; *s; s++)
		g_debug ("    Arg: %s", *s);
	if (!g_spawn_async_with_pipes ("/" /* wd */,
				       argv,
				       envp,
				       G_SPAWN_DO_NOT_REAP_CHILD |
				       G_SPAWN_FILE_AND_ARGV_ZERO,
				       NULL /* child setup */,
				       NULL /* user data */,
				       &job->backend_pid,
				       &stdin_fd,
				       &stdout_fd,
				       &stderr_fd,
				       &error)) {
		g_warning ("Failed to start backend: %s\n", error->message);
		g_error_free (error);
		goto out;
	}

	job->backend_watch_source = g_child_watch_add (job->backend_pid,
						       pd_job_impl_backend_watch_cb,
						       job);
	stdout_channel = g_io_channel_unix_new (stdout_fd);
	job->backend_io_source = g_io_add_watch (stdout_channel,
						 G_IO_IN,
						 pd_job_impl_backend_io_cb,
						 stdout);
 out:
	if (stdout_channel)
		g_io_channel_unref (stdout_channel);
	g_strfreev (argv);
	g_strfreev (envp);
	if (printer)
		g_object_unref (printer);
	g_free (scheme);
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
	GDBusMessage *message;
	GUnixFDList *fd_list;
	gint32 fd_handle;
	GError *error = NULL;

	/* Check if the user is authorized to create a printer */
	//if (!pd_daemon_util_check_authorization_sync ())
	//	goto out;

	if (job->document_fd != -1 ||
	    job->document_filename != NULL) {
		g_debug ("Tried to add second document");
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       "No more documents allowed");
		goto out;
	}

	g_debug ("Adding document");
	message = g_dbus_method_invocation_get_message (invocation);
	fd_list = g_dbus_message_get_unix_fd_list (message);
	if (fd_list != NULL && g_unix_fd_list_get_length (fd_list) == 1) {
		fd_handle = g_variant_get_handle (file_descriptor);
		job->document_fd = g_unix_fd_list_get (fd_list,
						       fd_handle,
						       &error);
		if (job->document_fd < 0) {
			g_debug ("  failed to get file descriptor: %s",
				 error->message);
			g_dbus_method_invocation_return_gerror (invocation,
								error);
			g_error_free (error);
			goto out;
		}

		g_debug ("  Got file descriptor: %d", job->document_fd);
	}

	g_dbus_method_invocation_return_value (invocation, NULL);

 out:
	return TRUE; /* handled the method invocation */
}

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_job_impl_start (PdJob *_job,
		   GDBusMethodInvocation *invocation)
{
	PdJobImpl *job = PD_JOB_IMPL (_job);
	gchar *name_used = NULL;
	GError *error = NULL;
	gint infd = -1;
	gint spoolfd = -1;
	char buffer[1024];
	ssize_t got, wrote;

	/* Check if the user is authorized to create a printer */
	//if (!pd_daemon_util_check_authorization_sync ())
	//	goto out;

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
		g_debug ("Error making temporary file: %s", error->message);
		g_dbus_method_invocation_return_gerror (invocation,
							error);
		g_error_free (error);
		goto out;
	}

	g_debug ("Starting job");

	g_debug ("  Spooling");
	g_debug ("    Created temporary file %s", name_used);
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

	/* Move the job state to pending */
	g_debug ("  Set job state to pending");
	pd_job_set_state (PD_JOB (job),
			  PD_JOB_STATE_PENDING);

	/* Job is no longer incoming so remove that state reason if
	   present */
	g_hash_table_remove (job->state_reasons, "job-incoming");

	/* Start processing it if possible */
	pd_engine_start_jobs (pd_daemon_get_engine (job->daemon));

	/* Return success */
	g_dbus_method_invocation_return_value (invocation,
					       g_variant_new ("()"));

 out:
	if (infd != -1)
		close (infd);
	if (spoolfd != -1)
		close (spoolfd);
	g_free (name_used);
	return TRUE; /* handled the method invocation */
}

static void
pd_job_iface_init (PdJobIface *iface)
{
	iface->handle_add_document = pd_job_impl_add_document;
	iface->handle_start = pd_job_impl_start;
}
