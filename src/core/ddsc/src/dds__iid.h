/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#ifndef _DDS_IID_H_
#define _DDS_IID_H_

#include "dds__types.h"

#if defined (__cplusplus)
extern "C" {
#endif

void dds_iid_init (void);
void dds_iid_fini (void);
uint64_t dds_iid_gen (void);

#if defined (__cplusplus)
}
#endif
#endif
