/*
 * Copyright (c) 2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * History:
 *  2001-05-30 	gvdl	Initial implementation of the vtable patcher.
 */

#include <sys/cdefs.h>

__BEGIN_DECLS

#if KERNEL
extern Boolean kld_file_map(const char *pathName,
			    unsigned char *map, size_t mapSize,
			    Boolean isKmem);
#else
extern Boolean kld_file_map(const char *pathName);

extern void *
    kld_file_lookupsymbol(const char *pathName, const char *symbolname);

Boolean kld_file_debug_dump(const char *pathName, const char *outName);
#endif /* KERNEL */

extern void *kld_file_getaddr(const char *pathName, long *size);

extern Boolean kld_file_merge_OSObjects(const char *pathName);

extern Boolean kld_file_patch_OSObjects(const char *pathName);

extern Boolean kld_file_prepare_for_link();

extern void kld_file_cleanup_all_resources();

__END_DECLS
