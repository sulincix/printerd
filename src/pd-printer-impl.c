/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2014 Tim Waugh <twaugh@redhat.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA	02110-1301	USA
 *
 */

#include "config.h"

#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <cups/ppd.h>

#include "pd-common.h"
#include "pd-printer-impl.h"
#include "pd-daemon.h"
#include "pd-engine.h"
#include "pd-job-impl.h"
#include "pd-log.h"

/**
 * SECTION:pdprinter
 * @title: PdPrinterImpl
 * @short_description: Implementation of #PdPrinterImpl
 *
 * This type provides an implementation of the #PdPrinterImpl
 * interface on .
 */

typedef struct _PdPrinterImplClass	PdPrinterImplClass;

/**
 * PdPrinterImpl:
 *
 * The #PdPrinterImpl structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _PdPrinterImpl
{
	PdPrinterSkeleton	 parent_instance;
	PdDaemon		*daemon;
	GPtrArray		*jobs;
	gboolean		 job_outgoing;

	gchar			*id;
	gchar			*final_content_type;
	gchar			*final_filter;

	GMutex			 lock;
};

struct _PdPrinterImplClass
{
	PdPrinterSkeletonClass parent_class;
};

enum
{
	PROP_0,
	PROP_DAEMON,
	PROP_JOB_OUTGOING,
};

static void pd_printer_iface_init (PdPrinterIface *iface);
static void pd_printer_impl_job_state_notify (PdJob *job);
static void pd_printer_impl_job_add_state_reason (PdJobImpl *job,
						  const gchar *reason,
						  PdPrinterImpl *printer);
static void pd_printer_impl_job_remove_state_reason (PdJobImpl *job,
						     const gchar *reason,
						     PdPrinterImpl *printer);

G_DEFINE_TYPE_WITH_CODE (PdPrinterImpl, pd_printer_impl, PD_TYPE_PRINTER_SKELETON,
			 G_IMPLEMENT_INTERFACE (PD_TYPE_PRINTER, pd_printer_iface_init));

/* ------------------------------------------------------------------ */

static void
pd_printer_impl_remove_job (gpointer data,
			    gpointer user_data)
{
	PdJob *job = data;
	PdDaemon *daemon;
	PdEngine *engine;
	gchar *job_path = NULL;

	daemon = pd_job_impl_get_daemon (PD_JOB_IMPL (job));
	engine = pd_daemon_get_engine (daemon);

	g_signal_handlers_disconnect_by_func (job,
					      pd_printer_impl_job_state_notify,
					      job);

	g_signal_handlers_disconnect_by_func (job,
					      pd_printer_impl_job_add_state_reason,
					      PD_PRINTER_IMPL (user_data));

	g_signal_handlers_disconnect_by_func (job,
					      pd_printer_impl_job_remove_state_reason,
					      PD_PRINTER_IMPL (user_data));

	job_path = g_strdup_printf ("/org/freedesktop/printerd/job/%u",
				    pd_job_get_id (job));
	pd_engine_remove_job (engine, job_path);
	g_free (job_path);
}

static void
pd_printer_impl_finalize (GObject *object)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (object);

	printer_debug (PD_PRINTER (printer), "Finalize");

	/* note: we don't hold a reference to printer->daemon */
	g_ptr_array_foreach (printer->jobs,
			     pd_printer_impl_remove_job,
			     printer);
	g_ptr_array_free (printer->jobs, TRUE);
	g_free (printer->id);
	if (printer->final_content_type)
		g_free (printer->final_content_type);

	if (printer->final_filter)
		g_free (printer->final_filter);

	g_mutex_clear (&printer->lock);

	G_OBJECT_CLASS (pd_printer_impl_parent_class)->finalize (object);
}

static void
pd_printer_impl_get_property (GObject *object,
			      guint prop_id,
			      GValue *value,
			      GParamSpec *pspec)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (object);

	switch (prop_id) {
	case PROP_DAEMON:
		g_value_set_object (value, pd_printer_impl_get_daemon (printer));
		break;
	case PROP_JOB_OUTGOING:
		g_value_set_boolean (value, printer->job_outgoing);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
pd_printer_impl_set_property (GObject *object,
			      guint prop_id,
			      const GValue *value,
			      GParamSpec *pspec)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (object);

	switch (prop_id) {
	case PROP_DAEMON:
		g_assert (printer->daemon == NULL);
		/* we don't take a reference to the daemon */
		printer->daemon = g_value_get_object (value);
		break;
	case PROP_JOB_OUTGOING:
		printer->job_outgoing = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
pd_printer_impl_init (PdPrinterImpl *printer)
{
	GVariantBuilder builder;
	GVariantBuilder val_builder;

	g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (printer),
					     G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

	g_mutex_init (&printer->lock);

	/* Defaults */

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

	/* set initial job template attributes */
	g_variant_builder_add (&builder, "{sv}",
			       "media",
			       g_variant_new ("s", "iso-a4"));

	/* set initial printer description attributes */
	g_variant_builder_add (&builder, "{sv}",
			       "document-format",
			       g_variant_new ("s", "application/octet-stream"));

	pd_printer_set_defaults (PD_PRINTER (printer),
				 g_variant_builder_end (&builder));

	/* Supported values */

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

	/* set initial job template supported attributes */
	g_variant_builder_init (&val_builder, G_VARIANT_TYPE ("as"));
	g_variant_builder_add (&val_builder, "s", "iso-a4");
	g_variant_builder_add (&val_builder, "s", "na-letter");
	g_variant_builder_add (&builder, "{sv}",
			       "media",
			       g_variant_builder_end (&val_builder));

	g_variant_builder_init (&val_builder, G_VARIANT_TYPE ("as"));
	g_variant_builder_add (&val_builder, "s", "application/pdf");
	g_variant_builder_add (&builder, "{sv}",
			       "document-format",
			       g_variant_builder_end (&val_builder));

	pd_printer_set_supported (PD_PRINTER (printer),
				  g_variant_builder_end (&builder));

	/* Array of jobs */
	printer->jobs = g_ptr_array_new_full (0,
					      (GDestroyNotify) g_object_unref);

	/* Set initial state */
	pd_printer_set_state (PD_PRINTER (printer), PD_PRINTER_STATE_IDLE);
}

static void
pd_printer_impl_class_init (PdPrinterImplClass *klass)
{
	GObjectClass *gobject_class;

	gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = pd_printer_impl_finalize;
	gobject_class->set_property = pd_printer_impl_set_property;
	gobject_class->get_property = pd_printer_impl_get_property;

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
	 * PdPrinterImpl:job-outgoing:
	 *
	 * Whether any job is outgoing.
	 */
	g_object_class_install_property (gobject_class,
					 PROP_JOB_OUTGOING,
					 g_param_spec_boolean ("job-outgoing",
							       "Job outgoing",
							       "Whether any job is outgoing",
							       FALSE,
							       G_PARAM_READWRITE));
}

const gchar *
pd_printer_impl_get_id (PdPrinterImpl *printer)
{
	/* shortcut */
	if (printer->id != NULL)
		goto out;

	printer->id = pd_printer_dup_name (PD_PRINTER (printer));

	/* ensure valid */
	g_strcanon (printer->id,
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "abcdefghijklmnopqrstuvwxyz"
		    "1234567890_",
		    '_');

 out:
	return printer->id;
}

void
pd_printer_impl_set_id (PdPrinterImpl *printer,
			const gchar *id)
{
	g_mutex_lock (&printer->lock);

	if (printer->id)
		g_free (printer->id);

	printer->id = g_strdup (id);

	g_mutex_unlock (&printer->lock);
}

/**
 * pd_printer_impl_get_daemon:
 * @printer: A #PdPrinterImpl.
 *
 * Gets the daemon used by @printer.
 *
 * Returns: A #PdDaemon. Do not free, the object is owned by @printer.
 */
PdDaemon *
pd_printer_impl_get_daemon (PdPrinterImpl *printer)
{
	g_return_val_if_fail (PD_IS_PRINTER_IMPL (printer), NULL);
	return printer->daemon;
}

gboolean
pd_printer_impl_set_driver (PdPrinterImpl *printer,
			    const gchar *driver)
{
	ppd_file_t *ppd;
	ppd_attr_t *cupsFilter;
	gchar *best_format = NULL;
	gchar *best_format_filter = NULL;
	int best_cost;

	if (driver == NULL)
		return FALSE;

	if ((ppd = ppdOpenFile (driver)) == NULL) {
		printer_debug (PD_PRINTER (printer), "Unable to open PPD");
		return FALSE;
	}

	cupsFilter = ppdFindAttr (ppd, "cupsFilter", NULL);
	while (cupsFilter) {
		int cost;
		gchar **tokens = g_strsplit_set (cupsFilter->value,
						 " \t",
						 3);
		if (!tokens[0] || !tokens[1] || !tokens[2])
			goto next;

		cost = atoi (tokens[1]);
		printer_debug (PD_PRINTER (printer), "Filter: %s (cost %d)",
			       tokens[0], cost);
		if (!best_format) {
			best_format = g_strdup (tokens[0]);
			best_format_filter = g_strdup (tokens[2]);
			best_cost = cost;
		} else if (cost < best_cost) {
			if (best_format)
				g_free (best_format);

			if (best_format_filter)
				g_free (best_format_filter);

			best_format = g_strdup (tokens[0]);
			best_format_filter = g_strdup (tokens[2]);
			best_cost = cost;
		}
	next:
		g_strfreev (tokens);
		cupsFilter = ppdFindNextAttr (ppd, "cupsFilter", NULL);
	}

	if (!best_format)
		best_format = g_strdup ("application/vnd.cups-pdf");

	g_mutex_lock (&printer->lock);
	if (printer->final_content_type)
		g_free (printer->final_content_type);

	if (printer->final_filter)
		g_free (printer->final_filter);

	printer_debug (PD_PRINTER (printer),
		       "Set final content type to %s (input to %s)",
		       best_format,
		       best_format_filter);
	printer->final_content_type = best_format;
	printer->final_filter = best_format_filter;

	g_mutex_unlock (&printer->lock);
	ppdClose (ppd);
	pd_printer_set_driver (PD_PRINTER (printer), driver);
	return TRUE;
}

static GVariant *
update_attributes (GVariant *attributes, GVariant *updates)
{
	GVariantBuilder builder;
	GVariantIter iter;
	gchar *dkey;
	GVariant *dvalue;

	/* Add any values from attributes that are not in updates */
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_iter_init (&iter, attributes);
	while (g_variant_iter_loop (&iter, "{sv}", &dkey, &dvalue))
		if (!g_variant_lookup_value (updates, dkey, NULL))
			g_variant_builder_add (&builder, "{sv}",
					       dkey, dvalue);

	/* Now add in the updates */
	g_variant_iter_init (&iter, updates);
	while (g_variant_iter_loop (&iter, "{sv}", &dkey, &dvalue))
		g_variant_builder_add (&builder, "{sv}", dkey, dvalue);

	return g_variant_builder_end (&builder);
}

void
pd_printer_impl_do_update_defaults (PdPrinterImpl *printer,
				    GVariant *defaults)
{
	GVariantIter iter;
	GVariant *current_defaults;
	gchar *key;
	GVariant *value;

	printer_debug (PD_PRINTER (printer), "Updating defaults");

	/* add/overwrite default values, keeping other existing values */
	g_variant_iter_init (&iter, defaults);
	while (g_variant_iter_loop (&iter, "{sv}", &key, &value)) {
		gchar *val = g_variant_print (value, TRUE);
		printer_debug (PD_PRINTER (printer),
			       "Defaults: set %s=%s",
			       key, val);
		g_free (val);
	}

	g_mutex_lock (&printer->lock);
	g_object_freeze_notify (G_OBJECT (printer));
	current_defaults = pd_printer_get_defaults (PD_PRINTER (printer));
	value = update_attributes (current_defaults,
				   defaults);
	pd_printer_set_defaults (PD_PRINTER (printer), value);
	g_mutex_unlock (&printer->lock);
	g_object_thaw_notify (G_OBJECT (printer));
}

static void
pd_printer_impl_complete_update_defaults (PdPrinter *_printer,
					  GDBusMethodInvocation *invocation,
					  GVariant *arg_defaults)
{
	pd_printer_impl_do_update_defaults (PD_PRINTER_IMPL (_printer),
					    arg_defaults);
	g_dbus_method_invocation_return_value (invocation, NULL);
}

static gboolean
pd_printer_impl_update_defaults (PdPrinter *_printer,
				 GDBusMethodInvocation *invocation,
				 GVariant *arg_defaults)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (_printer);

	/* Check if the user is authorized to do this */
	if (!pd_daemon_check_authorization_sync (printer->daemon,
						 NULL,
						 N_("Authentication is required to modify a printer"),
						 invocation,
						 "org.freedesktop.printerd.all-edit",
						 "org.freedesktop.printerd.printer-modify",
						 NULL))
		goto out;

	pd_printer_impl_complete_update_defaults (_printer,
						  invocation,
						  arg_defaults);
out:
	return TRUE; /* handled the method invocation */
}

void
pd_printer_impl_add_state_reason (PdPrinterImpl *printer,
				  const gchar *reason)
{
	const gchar *const *reasons;
	gchar **strv;

	printer_debug (PD_PRINTER (printer), "state-reasons += %s", reason);

	g_mutex_lock (&printer->lock);
	g_object_freeze_notify (G_OBJECT (printer));
	reasons = pd_printer_get_state_reasons (PD_PRINTER (printer));
	strv = add_or_remove_state_reason (reasons, '+', reason);
	pd_printer_set_state_reasons (PD_PRINTER (printer),
				      (const gchar *const *) strv);
	g_mutex_unlock (&printer->lock);
	g_object_thaw_notify (G_OBJECT (printer));
	g_strfreev (strv);
}

void
pd_printer_impl_remove_state_reason (PdPrinterImpl *printer,
				     const gchar *reason)
{
	const gchar *const *reasons;
	gchar **strv;

	printer_debug (PD_PRINTER (printer), "state-reasons -= %s", reason);

	g_mutex_lock (&printer->lock);
	g_object_freeze_notify (G_OBJECT (printer));
	reasons = pd_printer_get_state_reasons (PD_PRINTER (printer));
	strv = add_or_remove_state_reason (reasons, '-', reason);
	pd_printer_set_state_reasons (PD_PRINTER (printer),
				      (const gchar *const *) strv);
	g_mutex_unlock (&printer->lock);
	g_object_thaw_notify (G_OBJECT (printer));
	g_strfreev (strv);
}

/**
 * pd_printer_impl_get_uri:
 * @printer: A #PdPrinterImpl.
 *
 * Get an appropriate device URI to start a job on.
 *
 * Return: A device URI. Do not free.
 */
const gchar *
pd_printer_impl_get_uri (PdPrinterImpl *printer)
{
	const gchar *const *device_uris;

	/* Simple implementation: always use the first URI in the list */
	device_uris = pd_printer_get_device_uris (PD_PRINTER (printer));
	return device_uris[0];
}

/**
 * pd_printer_impl_get_next_job:
 * @printer: A #PdPrinterImpl.
 *
 * Get the next job which should be processed, or NULL if there is no
 * suitable job.
 *
 * Returns: A #PdJob or NULL.  Free with g_object_unref.
 */
PdJob *
pd_printer_impl_get_next_job (PdPrinterImpl *printer)
{
	PdJob *best = NULL;
	PdJob *job;
	guint index;

	g_return_val_if_fail (PD_IS_PRINTER_IMPL (printer), NULL);

	g_mutex_lock (&printer->lock);
	index = 0;
	for (index = 0; index < printer->jobs->len; index++) {
		job = g_ptr_array_index (printer->jobs, index);
		if (job == NULL)
			break;

		if (pd_job_get_state (job) == PD_JOB_STATE_PENDING) {
			best = job;
			break;
		}
	}

	if (best)
		g_object_ref (best);

	g_mutex_unlock (&printer->lock);
	return best;
}

/**
 * pd_printer_impl_dup_final_content_type:
 * @printer: A #PdPrinterImpl.
 * @content_type: (out): Content type.
 * @filter: (out): Final filter for processing content type.
 *
 * Get the final content type, i.e. the content type required by the
 * PPD. The PPD may specify a further filter for processing data
 * before it is sent to the backend.
 */
gboolean
pd_printer_impl_dup_final_content_type (PdPrinterImpl *printer,
					gchar **content_type,
					gchar **filter,
					GError **error)
{
	gchar *final_content_type;
	gchar *final_filter;

	g_mutex_lock (&printer->lock);
	if (printer->final_content_type)
		final_content_type = g_strdup (printer->final_content_type);
	if (printer->final_filter)
		final_filter = g_strdup (printer->final_filter);
	g_mutex_unlock (&printer->lock);

	if (!final_content_type || !final_filter)
		goto fail;

	*content_type = final_content_type;
	*filter = final_filter;

	return TRUE;

fail:
	if (final_content_type)
		g_free (final_content_type);

	if (final_filter)
		g_free (final_filter);

	*error = g_error_new (PD_ERROR,
			      PD_ERROR_FAILED,
			      "Memory allocation failure");
	return FALSE;
}

/* ------------------------------------------------------------------ */

static void
pd_printer_impl_complete_set_device_uris (PdPrinter *_printer,
					  GDBusMethodInvocation *invocation,
					  const gchar *const *device_uris)
{
	pd_printer_set_device_uris (_printer, device_uris);
	g_dbus_method_invocation_return_value (invocation, NULL);
}

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_printer_impl_set_device_uris (PdPrinter *_printer,
				 GDBusMethodInvocation *invocation,
				 const gchar *const *device_uris)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (_printer);

	/* Check if the user is authorized to set device URIs */
	if (!pd_daemon_check_authorization_sync (printer->daemon,
						 NULL,
						 N_("Authentication is required to modify a printer"),
						 invocation,
						 "org.freedesktop.printerd.all-edit",
						 "org.freedesktop.printerd.printer-modify",
						 NULL))
		goto out;

	pd_printer_impl_complete_set_device_uris (_printer,
						  invocation,
						  device_uris);

out:
	return TRUE; /* handled the method invocation */
}

static gboolean
attribute_value_is_supported (PdPrinterImpl *printer,
			      const gchar *key,
			      GVariant *value)
{
	gboolean ret = FALSE;
	GVariant *supported_values;
	GVariantIter iter_supported;
	GVariant *supported;
	gchar *supported_val;
	const gchar *provided_val;
	gboolean found;

	/* Is this an attribute for which there are restrictions? */
	supported = pd_printer_get_supported (PD_PRINTER (printer));
	supported_values = g_variant_lookup_value (supported,
						   key,
						   G_VARIANT_TYPE ("s"));
	if (!supported_values) {
		ret = TRUE;
		goto out;
	}

	/* Is the supplied value among those supported? */
	provided_val = g_variant_get_string (value, NULL);
	g_variant_iter_init (&iter_supported, supported_values);
	found = FALSE;
	while (g_variant_iter_loop (&iter_supported, "s", &supported_val)) {
		if (!g_strcmp0 (provided_val, supported_val)) {
			/* Yes, found it. */
			found = TRUE;
			break;
		}
	}

	if (!found) {
		printer_debug (PD_PRINTER (printer),
			       "Unsupported value for %s", key);
		goto out;
	}

	/* Passed all checks */
	ret = TRUE;
 out:
	return ret;
}

/**
 * pd_printer_impl_job_state_notify
 * @job: A #PdJob.
 *
 * Watch job state changes in order to update printer state.
 */
static void
pd_printer_impl_job_state_notify (PdJob *job)
{
	const gchar *printer_path;
	PdDaemon *daemon;
	PdObject *obj = NULL;
	PdPrinter *printer = NULL;

	printer_path = pd_job_get_printer (job);
	daemon = pd_job_impl_get_daemon (PD_JOB_IMPL (job));
	obj = pd_daemon_find_object (daemon, printer_path);
	if (!obj)
		goto out;

	printer = pd_object_get_printer (obj);

	g_mutex_lock (&PD_PRINTER_IMPL (printer)->lock);
	g_object_freeze_notify (G_OBJECT (printer));
	switch (pd_job_get_state (job)) {
	case PD_JOB_STATE_CANCELED:
	case PD_JOB_STATE_ABORTED:
	case PD_JOB_STATE_COMPLETED:
		/* Only one job can be processing at a time currently */
		if (pd_printer_get_state (printer) == PD_PRINTER_STATE_PROCESSING)
			pd_printer_set_state (printer,
					      PD_PRINTER_STATE_IDLE);
		break;
	}

	g_mutex_unlock (&PD_PRINTER_IMPL (printer)->lock);
	g_object_thaw_notify (G_OBJECT (printer));
 out:
	if (obj)
		g_object_unref (obj);
	if (printer)
		g_object_unref (printer);
}

/**
 * pd_printer_impl_job_add_state_reason
 * @job: A #PdJob.
 *
 * Add a state reason to printer-state-reasons
 */
static void
pd_printer_impl_job_add_state_reason (PdJobImpl *job,
				      const gchar *reason,
				      PdPrinterImpl *printer)
{
	g_return_if_fail (PD_IS_JOB_IMPL (job));
	g_return_if_fail (PD_IS_PRINTER_IMPL (printer));
	pd_printer_impl_add_state_reason (printer, reason);
}

/**
 * pd_printer_impl_job_remove_state_reason
 * @job: A #PdJob.
 *
 * Remove a state reason from printer-state-reasons
 */
static void
pd_printer_impl_job_remove_state_reason (PdJobImpl *job,
					 const gchar *reason,
					 PdPrinterImpl *printer)
{
	g_return_if_fail (PD_IS_JOB_IMPL (job));
	g_return_if_fail (PD_IS_PRINTER_IMPL (printer));
	pd_printer_impl_remove_state_reason (printer, reason);
}

static void
pd_printer_impl_complete_create_job (PdPrinter *_printer,
				     GDBusMethodInvocation *invocation,
				     GVariant *options,
				     const gchar *name,
				     GVariant *attributes)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (_printer);
	PdJob *job;
	gchar *object_path = NULL;
	gchar *printer_path = NULL;
	GVariant *defaults;
	GVariantBuilder unsupported;
	GVariantIter iter;
	gchar *dkey;
	GVariant *dvalue;
	GVariant *job_attributes;
	gchar *user = NULL;

	printer_debug (PD_PRINTER (printer), "Creating job");

	g_mutex_lock (&printer->lock);
	g_object_freeze_notify (G_OBJECT (printer));

	/* set attributes from job template attributes */
	defaults = pd_printer_get_defaults (PD_PRINTER (printer));

	/* Check for unsupported attributes */
	g_variant_builder_init (&unsupported, G_VARIANT_TYPE ("a{sv}"));
	g_variant_iter_init (&iter, attributes);
	while (g_variant_iter_loop (&iter, "{sv}", &dkey, &dvalue))
		/* Is there a list of supported values? */
		if (!attribute_value_is_supported (printer, dkey, dvalue)) {
			gchar *val = g_variant_print (dvalue, TRUE);
			printer_debug (PD_PRINTER (printer),
				       "Unsupported attribute %s=%s",
				       dkey, val);
			g_free (val);
			g_variant_builder_add (&unsupported, "{sv}",
					       dkey,
					       dvalue);
		}

	/* Tell the engine to create the job */
	printer_path = g_strdup_printf ("/org/freedesktop/printerd/printer/%s",
					printer->id);
	job_attributes = update_attributes (defaults,
					    attributes);
	job = pd_engine_add_job (pd_daemon_get_engine (printer->daemon),
				 printer_path,
				 name,
				 job_attributes);

	/* Store the job in our array */
	g_ptr_array_add (printer->jobs, (gpointer) job);

	/* Watch state changes */
	g_signal_connect (job,
			  "notify::state",
			  G_CALLBACK (pd_printer_impl_job_state_notify),
			  job);

	/* Watch for printer-state-reasons updates */
	g_signal_connect (job,
			  "add-printer-state-reason",
			  G_CALLBACK (pd_printer_impl_job_add_state_reason),
			  printer);
	g_signal_connect (job,
			  "remove-printer-state-reason",
			  G_CALLBACK (pd_printer_impl_job_remove_state_reason),
			  printer);

	/* Set job-originating-user-name */
	user = pd_get_unix_user (invocation);
	printer_debug (PD_PRINTER (printer), "Originating user is %s", user);
	pd_job_impl_set_attribute (PD_JOB_IMPL (job),
				   "job-originating-user-name",
				   g_variant_new_string (user));

	object_path = g_strdup_printf ("/org/freedesktop/printerd/job/%u",
				       pd_job_get_id (job));
	printer_debug (PD_PRINTER (printer), "Job path is %s", object_path);
	g_dbus_method_invocation_return_value (invocation,
					       g_variant_new ("(o@a{sv})",
							      object_path,
							      g_variant_builder_end (&unsupported)));

	g_mutex_unlock (&printer->lock);
	g_object_thaw_notify (G_OBJECT (printer));
	g_free (user);
	g_free (object_path);
	g_free (printer_path);
}

/* runs in thread dedicated to handling @invocation */
static gboolean
pd_printer_impl_create_job (PdPrinter *_printer,
			    GDBusMethodInvocation *invocation,
			    GVariant *options,
			    const gchar *name,
			    GVariant *attributes)
{
	PdPrinterImpl *printer = PD_PRINTER_IMPL (_printer);

	/* Check if the user is authorized to create a job */
	if (!pd_daemon_check_authorization_sync (printer->daemon,
						 options,
						 N_("Authentication is required to add a job"),
						 invocation,
						 "org.freedesktop.printerd.job-add",
						 NULL))
		goto out;

	pd_printer_impl_complete_create_job (_printer,
					     invocation,
					     options,
					     name,
					     attributes);

 out:
	return TRUE; /* handled the method invocation */
}

static void
pd_printer_impl_handle_complete_update_driver (PdPrinter *_printer,
					       GDBusMethodInvocation *invocation,
					       GVariant *arg_options)
{
	gchar *driver = NULL;
	g_variant_lookup (arg_options, "driver-name", "&s", &driver);
	if (!driver) {
		/* Should never happen */
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_FAILED,
						       N_("Internal error"));
		return;
	}

	pd_printer_impl_set_driver (PD_PRINTER_IMPL (_printer), driver);
	g_dbus_method_invocation_return_value (invocation, NULL);
}

static gboolean
pd_printer_impl_handle_update_driver (PdPrinter *_printer,
				      GDBusMethodInvocation *invocation,
				      GVariant *arg_options)
{
	gchar *driver;
	if (!g_variant_lookup (arg_options,
			       "driver-name",
			       "&s",
			       &driver)) {
		g_dbus_method_invocation_return_error (invocation,
						       PD_ERROR,
						       PD_ERROR_UNIMPLEMENTED,
						       N_("UpdateDriver without driver-name specified is not implemented"));
		return TRUE; /* handled the method invocation */
	}

	pd_printer_impl_handle_complete_update_driver (_printer,
						       invocation,
						       arg_options);
	return TRUE;
}

static void
pd_printer_iface_init (PdPrinterIface *iface)
{
	iface->handle_set_device_uris = pd_printer_impl_set_device_uris;
	iface->handle_update_defaults = pd_printer_impl_update_defaults;
	iface->handle_create_job = pd_printer_impl_create_job;
	iface->handle_update_driver = pd_printer_impl_handle_update_driver;
}
