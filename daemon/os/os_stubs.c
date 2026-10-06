/* SPDX-License-Identifier: MIT */
/*
 * Digital call progress (DCP) audio and the diagnostics manager.
 *
 * Both were optional services of the kernel driver (a player daemon and a
 * diagnostics character device). They are inert for now, but must return
 * valid handles: the legacy versions returned NULL only when out of memory.
 */
#include "hsf_os.h"

#include <stdlib.h>

struct dcp {
	HANDLE devnode;
	int volume;
	unsigned long bytes;
};

HSF_EXPORT HANDLE DcpCreate(HANDLE hDevNode)
{
	struct dcp *d = calloc(1, sizeof(*d));

	HSF_TRACE("DcpCreate", "%p) = (%p", hDevNode, (void *)d);
	if (d)
		d->devnode = hDevNode;
	return d;
}

HSF_EXPORT VOID DcpDestroy(HANDLE hDcp)
{
	struct dcp *d = hDcp;

	HSF_TRACE("DcpDestroy", "%p: %lu bytes of call-progress audio", hDcp, d ? d->bytes : 0ul);
	free(d);
}

HSF_EXPORT VOID DcpSetVolume(HANDLE hDcp, int nVolume)
{
	struct dcp *d = hDcp;

	HSF_TRACE("DcpSetVolume", "%p, %d", hDcp, nVolume);
	if (d)
		d->volume = nVolume;
}

HSF_EXPORT VOID DcpCallback(HANDLE hDcp, PVOID pData, UINT32 dwSize)
{
	struct dcp *d = hDcp;

	(void)pData;
	HSF_TRACE_HOT("DcpCallback", "%p, %u", hDcp, dwSize);
	if (d)
		d->bytes += dwSize;
}

HSF_EXPORT HANDLE OsDiagMgrOpen(PSYSTEM_INSTANCES_T pSys, PI_DIAG_MGR_T pDiagMgrInterface)
{
	static int handle;

	HSF_TRACE("OsDiagMgrOpen", "%p, %p", (void *)pSys, (void *)pDiagMgrInterface);
	return &handle;
}

HSF_EXPORT COM_STATUS OsDiagMgrClose(HANDLE hOsDiagModem)
{
	HSF_TRACE("OsDiagMgrClose", "%p", hOsDiagModem);
	return COM_STATUS_SUCCESS;
}

HSF_EXPORT COM_STATUS OsDiagMgrNotify(HANDLE hOsDiagMgr, CNXT_DIAG_CODES Code, UINT32 dwData)
{
	HSF_TRACE_HOT("OsDiagMgrNotify", "%p, %d, 0x%x", hOsDiagMgr, (int)Code, dwData);
	return COM_STATUS_SUCCESS;
}
