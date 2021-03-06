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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __PD_PRINTER_IMPL_H__
#define __PD_PRINTER_IMPL_H__

#include "pd-daemontypes.h"

G_BEGIN_DECLS

#define PD_TYPE_PRINTER_IMPL	(pd_printer_impl_get_type ())
#define PD_PRINTER_IMPL(o)	(G_TYPE_CHECK_INSTANCE_CAST ((o), PD_TYPE_PRINTER_IMPL, PdPrinterImpl))
#define PD_IS_PRINTER_IMPL(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), PD_TYPE_PRINTER_IMPL))

/* File descriptors for filters and backends */
#define PD_FD_BACK	3
#define PD_FD_SIDE	4
#define PD_FD_MAX	5

GType		 pd_printer_impl_get_type	(void) G_GNUC_CONST;
PdDaemon	*pd_printer_impl_get_daemon	(PdPrinterImpl	*printer);
const gchar	*pd_printer_impl_get_id		(PdPrinterImpl	*printer);
void		 pd_printer_impl_set_id		(PdPrinterImpl	*printer,
						 const gchar	*id);
void		 pd_printer_impl_do_update_defaults (PdPrinterImpl *printer,
						     GVariant	*defaults);
void		 pd_printer_impl_add_state_reason (PdPrinterImpl *printer,
						   const gchar *reason);
void		 pd_printer_impl_remove_state_reason (PdPrinterImpl *printer,
						      const gchar *reason);
const gchar	*pd_printer_impl_get_uri	(PdPrinterImpl	*printer);
PdJob		*pd_printer_impl_get_next_job	(PdPrinterImpl	*printer);
gboolean	 pd_printer_impl_set_driver (PdPrinterImpl *printer,
					     const gchar *driver);
gboolean	 pd_printer_impl_dup_final_content_type (PdPrinterImpl *printer,
							 gchar **content_type,
							 gchar **filter,
							 GError **error);

G_END_DECLS

#endif /* __PD_PRINTER_IMPL_H__ */
