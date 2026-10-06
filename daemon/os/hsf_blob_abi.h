/* SPDX-License-Identifier: MIT */
/*
 * The Conexant/Linuxant ABI headers, included unchanged and in the order they
 * need. osuniqredef.h comes first: it maps every Os* name onto the versioned
 * symbol the blobs import (cnxthsf_<version>_OsAllocate, ...).
 */
#ifndef HSF_BLOB_ABI_H
#define HSF_BLOB_ABI_H

#include "osuniqredef.h"
#include "typedefs.h"
#include "comtypes.h"
#include "osservices.h"
#include "ostime_ex.h"
#include "osmemory_ex.h"
#include "comctrl_ex.h"
#include "osresour_ex.h"
#include "oshda.h"
#include "osnvm.h"
#include "dcp.h"
#include "intfctrl_ex.h"
#include "osdiag.h"
#include "configtypes.h"
#include "dpaloem.h"

/* Blob entry points that no header declares. */
__shimcall__ int HsfEngineInit(void);
__shimcall__ void HsfEngineExit(void);
__shimcall__ void *GetHwFuncs(void);

/* Every function the blobs call is defined with this annotation (regparm(0)
 * on i386, empty on x86_64) so its type matches the blob headers. */
#define HSF_EXPORT __shimcall__

#endif /* HSF_BLOB_ABI_H */
