/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Richard Hughes <richard@hughsie.com>
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

#ifndef __PD_MANAGER_IMPL_H__
#define __PD_MANAGER_IMPL_H__

#include "pd-daemontypes.h"

G_BEGIN_DECLS

#define PD_TYPE_MANAGER_IMPL	(pd_manager_impl_get_type ())
#define PD_MANAGER_IMPL(o)	(G_TYPE_CHECK_INSTANCE_CAST ((o), PD_TYPE_MANAGER_IMPL, PdManagerImpl))
#define PD_IS_MANAGER_IMPL(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), PD_TYPE_MANAGER_IMPL))

GType		 pd_manager_impl_get_type	(void) G_GNUC_CONST;
PdManager	*pd_manager_impl_new		(PdDaemon	*daemon);
PdDaemon	*pd_manager_impl_get_daemon	(PdManagerImpl	*manager);

G_END_DECLS

#endif /* __PD_MANAGER_IMPL_H__ */
