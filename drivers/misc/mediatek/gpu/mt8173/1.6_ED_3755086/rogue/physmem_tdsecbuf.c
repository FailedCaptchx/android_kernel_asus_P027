/*************************************************************************/ /*!
@File
@Title          Implementation of PMR functions for Trusted Device secure memory
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Part of the memory management. This module is responsible for
                implementing the function callbacks for physical memory imported
                from a trusted environment. This memory is not accessible from
                within the driver, so the only purpose of this PMR is to
                provide physical addresses for the MMU page tables.
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include "pvr_debug.h"
#include "pvrsrv.h"
#include "physmem_tdsecbuf.h"
#include "physheap.h"
#include "rgxdevice.h"

#if defined(PVR_RI_DEBUG)
#include "ri_server.h"
#endif


#if defined (SUPPORT_TRUSTED_DEVICE)

typedef struct _PMR_TDSECBUF_DATA_ {
	PHYS_HEAP             *psTDSecBufPhysHeap;
	PFN_TD_SECUREBUF_FREE pfnTDSecureBufFree;
	IMG_CPU_PHYADDR       sCpuPAddr;
	IMG_DEV_PHYADDR       sDevPAddr;
	IMG_UINT64            ui64Size;
	IMG_UINT64            ui64SecBufHandle;
} PMR_TDSECBUF_DATA;


/*
 * Implementation of callback functions
 */

static PVRSRV_ERROR PMRSysPhysAddrTDSecBufMem(PMR_IMPL_PRIVDATA pvPriv,
                                              IMG_UINT32 ui32NumOfPages,
                                              IMG_DEVMEM_OFFSET_T *puiOffset,
                                              IMG_BOOL *pbValid,
                                              IMG_DEV_PHYADDR *psDevPAddr)
{
	PMR_TDSECBUF_DATA *psPrivData = pvPriv;
	IMG_UINT32 i;

	for (i = 0; i < ui32NumOfPages; i++)
	{
		psDevPAddr[i].uiAddr = psPrivData->sDevPAddr.uiAddr + puiOffset[i];
	}

	return PVRSRV_OK;
}

static PVRSRV_ERROR PMRFinalizeTDSecBufMem(PMR_IMPL_PRIVDATA pvPriv)
{
	PMR_TDSECBUF_DATA *psPrivData = pvPriv;
	PVRSRV_ERROR eError;

	eError = psPrivData->pfnTDSecureBufFree(psPrivData->ui64SecBufHandle);
	if (eError != PVRSRV_OK)
	{
		if (eError == PVRSRV_ERROR_NOT_IMPLEMENTED)
		{
			PVR_DPF((PVR_DBG_ERROR, "PMRFinalizeTDSecBufMem: TDSecBufFree not implemented on the Trusted Device!"));
		}
		else
		{
			PVR_DPF((PVR_DBG_ERROR, "PMRFinalizeTDSecBufMem: TDSecBufFree cannot free the resource!"));
		}
		return eError;
	}

	PhysHeapRelease(psPrivData->psTDSecBufPhysHeap);
	OSFreeMem(psPrivData);

	return PVRSRV_OK;
}

static PMR_IMPL_FUNCTAB _sPMRTDSecBufFuncTab = {
	/* pfnLockPhysAddresses */
	NULL,
	/* pfnUnlockPhysAddresses */
	NULL,
	/* pfnDevPhysAddr */
	&PMRSysPhysAddrTDSecBufMem,
	/* pfnPDumpSymbolicAddr */
	NULL,
	/* pfnAcquireKernelMappingData */
	NULL,
	/* pfnReleaseKernelMappingData */
	NULL,
	/* pfnReadBytes */
	NULL,
	/* pfnWriteBytes */
	NULL,
	/* pfnUnpinMem */
	NULL,
	/* pfnPinMem */
	NULL,
	/* pfnChangeSparseMem */
	NULL,
	/* pfnChangeSparseMemCPUMap */
	NULL,
	/* pfnMMap */
	NULL,
	/* pfnFinalize */
	&PMRFinalizeTDSecBufMem
};


/*
 * Public functions
 */
PVRSRV_ERROR PhysmemNewTDSecureBufPMR(CONNECTION_DATA *psConnection,
                                      PVRSRV_DEVICE_NODE *psDevNode,
                                      IMG_DEVMEM_SIZE_T uiSize,
                                      PVRSRV_MEMALLOCFLAGS_T uiFlags,
                                      IMG_UINT32 *pui32Align,
                                      PMR **ppsPMRPtr,
                                      IMG_UINT64 *pui64SecBufHandle)
{
	PVRSRV_DEVICE_CONFIG *psDevConfig = psDevNode->psDevConfig;
	RGX_DATA *psRGXData = (RGX_DATA *)(psDevConfig->hDevData);
	PMR_TDSECBUF_DATA *psPrivData = NULL;
	PMR *psPMR = NULL;
	IMG_HANDLE hPDumpAllocInfo = NULL;
	PMR_LOG2ALIGN_T uiLog2PageSize = OSGetPageShift();
	IMG_BOOL bMappingTable = IMG_TRUE;
	PMR_FLAGS_T uiPMRFlags;
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psConnection);

	/* In this instance, we simply pass flags straight through.
	 * Generically, uiFlags can include things that control the PMR
	 * factory, but we don't need any such thing (at the time of
	 * writing!), and our caller specifies all PMR flags so we don't
	 * need to meddle with what was given to us.
	 */
	uiPMRFlags = (PMR_FLAGS_T)(uiFlags & PVRSRV_MEMALLOCFLAGS_PMRFLAGSMASK);

	/* Check no significant bits were lost in cast due to different bit widths for flags */
	PVR_ASSERT(uiPMRFlags == (uiFlags & PVRSRV_MEMALLOCFLAGS_PMRFLAGSMASK));

	/* Many flags can be dropped as the driver cannot access this memory
	 * and it is assumed that the trusted zone is physically contiguous
	 */
	uiPMRFlags &= ~(PVRSRV_MEMALLOCFLAG_KERNEL_CPU_MAPPABLE |
	                PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC |
	                PVRSRV_MEMALLOCFLAG_POISON_ON_ALLOC |
	                PVRSRV_MEMALLOCFLAG_POISON_ON_FREE |
	                PVRSRV_MEMALLOCFLAGS_CPU_MMUFLAGSMASK);

	psPrivData = OSAllocZMem(sizeof(PMR_TDSECBUF_DATA));
	if (psPrivData == NULL)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto errorOnAllocData;
	}

	/* Get required info for the TD Secure Buffer physical heap */
	if (!psRGXData->bHasTDSecureBufPhysHeap)
	{
		PVR_DPF((PVR_DBG_ERROR, "Trusted Device physical heap not available!"));
		eError = PVRSRV_ERROR_REQUEST_TDSECUREBUF_PAGES_FAIL;
		goto errorOnAcquireHeap;
	}

	eError = PhysHeapAcquire(psRGXData->uiTDSecureBufPhysHeapID,
	                         &psPrivData->psTDSecBufPhysHeap);
	if (eError != PVRSRV_OK) goto errorOnAcquireHeap;

	psPrivData->ui64Size = uiSize;

	if (psDevConfig->pfnTDSecureBufAlloc && psDevConfig->pfnTDSecureBufFree)
	{
		PVRSRV_TD_SECBUF_PARAMS sTDSecBufParams;

		psPrivData->pfnTDSecureBufFree = psDevConfig->pfnTDSecureBufFree;

		/* Ask the Trusted Device to allocate secure memory */
		sTDSecBufParams.uiSize = uiSize;
		sTDSecBufParams.uiAlign = ROGUE_CACHE_LINE_SIZE;

		/* These will be returned by pfnTDSecureBufAlloc on success */
		sTDSecBufParams.psSecBufAddr = &psPrivData->sCpuPAddr;
		sTDSecBufParams.pui64SecBufHandle = &psPrivData->ui64SecBufHandle;

		eError = psDevConfig->pfnTDSecureBufAlloc(&sTDSecBufParams);
		if (eError != PVRSRV_OK)
		{
			if (eError == PVRSRV_ERROR_NOT_IMPLEMENTED)
			{
				PVR_DPF((PVR_DBG_ERROR, "PhysmemNewTDSecureBufPMR: TDSecBufAlloc not implemented on the Trusted Device!"));
			}
			else
			{
				PVR_DPF((PVR_DBG_ERROR, "PhysmemNewTDSecureBufPMR: TDSecBufAlloc cannot allocate the resource!"));
			}
			goto errorOnAlloc;
		}
	}
	else
	{
		PVR_DPF((PVR_DBG_ERROR, "PhysmemNewTDSecureBufPMR: TDSecBufAlloc/Free not implemented!"));
		eError = PVRSRV_ERROR_NOT_IMPLEMENTED;
		goto errorOnAlloc;
	}

	PhysHeapCpuPAddrToDevPAddr(psPrivData->psTDSecBufPhysHeap,
	                           1,
	                           &psPrivData->sDevPAddr,
	                           &psPrivData->sCpuPAddr);

	/* Check that the secure buffer is aligned to a Rogue cache line */
	if ((psPrivData->sDevPAddr.uiAddr & (ROGUE_CACHE_LINE_SIZE - 1)) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "Trusted Device physical heap not aligned to a Rogue cache line!"));
		eError = PVRSRV_ERROR_REQUEST_TDSECUREBUF_PAGES_FAIL;
		goto errorOnCheckAlign;
	}


	eError = PMRCreatePMR(psPrivData->psTDSecBufPhysHeap,
	                      psPrivData->ui64Size,
	                      psPrivData->ui64Size,
	                      1,                 /* ui32NumPhysChunks */
	                      1,                 /* ui32NumVirtChunks */
	                      &bMappingTable,    /* pui32MappingTable (not used) */
	                      uiLog2PageSize,
	                      uiPMRFlags,
	                      "PMRTDSECUREBUF",
	                      &_sPMRTDSecBufFuncTab,
	                      psPrivData,
	                      &psPMR,
	                      &hPDumpAllocInfo,
	                      IMG_FALSE);
	if (eError != PVRSRV_OK)
	{
		goto errorOnCreatePMR;
	}

#if defined(PVR_RI_DEBUG)
	eError = RIWritePMREntryKM(psPMR,
	                           sizeof("TDSecureBuffer"),
	                           "TDSecureBuffer",
	                           psPrivData->ui64Size);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_WARNING,
		         "%s: Failed to write PMR entry (%s)",
		         __func__, PVRSRVGetErrorStringKM(eError)));
	}
#endif

	*pui32Align = 1 << uiLog2PageSize;
	*ppsPMRPtr = psPMR;
	*pui64SecBufHandle = psPrivData->ui64SecBufHandle;

	return PVRSRV_OK;


errorOnCreatePMR:
errorOnCheckAlign:
	eError = psDevConfig->pfnTDSecureBufFree(psPrivData->ui64SecBufHandle);
	if (eError != PVRSRV_OK)
	{
		if (eError == PVRSRV_ERROR_NOT_IMPLEMENTED)
		{
			PVR_DPF((PVR_DBG_ERROR, "PhysmemNewTDSecureBufPMR: TDSecBufFree not implemented on the Trusted Device!"));
		}
		else
		{
			PVR_DPF((PVR_DBG_ERROR, "PhysmemNewTDSecureBufPMR: TDSecBufFree cannot free the resource!"));
		}
	}
errorOnAlloc:
	PhysHeapRelease(psPrivData->psTDSecBufPhysHeap);
errorOnAcquireHeap:
	OSFreeMem(psPrivData);

errorOnAllocData:
	PVR_ASSERT(eError != PVRSRV_OK);

	return eError;
}

#else /* SUPPORT_TRUSTED_DEVICE */

PVRSRV_ERROR PhysmemNewTDSecureBufPMR(CONNECTION_DATA *psConnection,
                                      PVRSRV_DEVICE_NODE *psDevNode,
                                      IMG_DEVMEM_SIZE_T uiSize,
                                      PVRSRV_MEMALLOCFLAGS_T uiFlags,
                                      IMG_UINT32 *pui32Align,
                                      PMR **ppsPMRPtr,
                                      IMG_UINT64 *pui64SecBufHandle)
{
	PVR_UNREFERENCED_PARAMETER(psConnection);
	PVR_UNREFERENCED_PARAMETER(psDevNode);
	PVR_UNREFERENCED_PARAMETER(uiSize);
	PVR_UNREFERENCED_PARAMETER(uiFlags);
	PVR_UNREFERENCED_PARAMETER(pui32Align);
	PVR_UNREFERENCED_PARAMETER(ppsPMRPtr);
	PVR_UNREFERENCED_PARAMETER(pui64SecBufHandle);

	return PVRSRV_ERROR_NOT_IMPLEMENTED;
}

#endif

PVRSRV_ERROR PhysmemImportSecBuf(CONNECTION_DATA *psConnection,
                                 PVRSRV_DEVICE_NODE *psDevNode,
                                 IMG_DEVMEM_SIZE_T uiSize,
                                 PVRSRV_MEMALLOCFLAGS_T uiFlags,
                                 IMG_UINT32 *pui32Align,
                                 PMR **ppsPMRPtr,
                                 IMG_UINT64 *pui64SecBufHandle)
{
	return PhysmemNewTDSecureBufPMR(psConnection,
	                                psDevNode,
	                                uiSize,
	                                uiFlags,
	                                pui32Align,
	                                ppsPMRPtr,
	                                pui64SecBufHandle);
};

