/* **********************************************************
 * Copyright (c) 2012 Google, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Windows Kernel Handle Leak Checks */

#ifndef _HANDLECHECK_H_
#define _HANDLECHECK_H_

#include "dr_api.h"
#include "windefs.h"

enum {
    HANDLE_TYPE_KERNEL,
    HANDLE_TYPE_GDI,
    HANDLE_TYPE_USER,
};

void
handlecheck_init(void);

void
handlecheck_exit(void);

void
handlecheck_create_handle(void *drcontext, HANDLE handle, int type,
                          int sysnum, app_pc pc, dr_mcontext_t *mc);

void *
handlecheck_delete_handle(void *drcontext, HANDLE handle, int type,
                          int sysnum, app_pc pc, dr_mcontext_t *mc);

void
handlecheck_delete_handle_post_syscall(void *drcontext, HANDLE handle,
                                       int type, void *handle_info,
                                       bool success);

#ifdef STATISTICS
void
handlecheck_dump_statistics(void);
#endif /* STATISTICS */

void
handlecheck_nudge(void *drcontext);

#endif /* _HANDLECHECK_H_ */
