/*************************************************************************/ /*!
@File
@Title          Device Memory Management
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Front End (nominally Client side part, but now invokable
                from server too) of device memory management
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



#include "devicemem.h"
#include "img_types.h"
#include "pvr_debug.h"
#include "pvrsrv_error.h"
#include "allocmem.h"
#include "ra.h"
#include "osfunc.h"
#include "devicemem_mmap.h"
#include "devicemem_utils.h"
#include "client_mm_bridge.h"
#if defined(PDUMP)
#include "devicemem_pdump.h"
#endif
#if defined(PVR_RI_DEBUG)
#include "client_ri_bridge.h"
#endif 
#if defined(SUPPORT_PAGE_FAULT_DEBUG)
#include "client_devicememhistory_bridge.h"
#endif

#if defined(__KERNEL__)
#include "pvrsrv.h"
#if defined(LINUX)
#include "linux/kernel.h"
#endif
#endif

/** Page size.
 *  Should be initialised to the correct value at driver init time.
 *  Use macros from devicemem.h to access from outside this module.
 */
IMG_UINT32 g_uiLog2PageSize = 0;

/*****************************************************************************
 *                    Sub allocation internals                               *
 *****************************************************************************/

static PVRSRV_ERROR
_AllocateDeviceMemory(SHARED_DEV_CONNECTION hDevConnection,
                      IMG_UINT32 uiLog2Quantum,
                      IMG_DEVMEM_SIZE_T uiSize,
                      IMG_DEVMEM_SIZE_T uiChunkSize,
                      IMG_UINT32 ui32NumPhysChunks,
                      IMG_UINT32 ui32NumVirtChunks,
                      IMG_UINT32 *pui32MappingTable,
                      IMG_DEVMEM_ALIGN_T uiAlign,
                      DEVMEM_FLAGS_T uiFlags,
                      IMG_BOOL bExportable,
                      DEVMEM_IMPORT **ppsImport)
{
	DEVMEM_IMPORT *psImport;
	DEVMEM_FLAGS_T uiPMRFlags;
	IMG_HANDLE hPMR;
	PVRSRV_ERROR eError;

	eError = _DevmemImportStructAlloc(hDevConnection,
									  &psImport);
	if (eError != PVRSRV_OK)
	{
		goto failAlloc;
	}

    /* Check the size is a multiple of the quantum */
    PVR_ASSERT((uiSize & ((1ULL<<uiLog2Quantum)-1)) == 0);

	/* Pass only the PMR flags down */
	uiPMRFlags = uiFlags & PVRSRV_MEMALLOCFLAGS_PMRFLAGSMASK;
    eError = BridgePhysmemNewRamBackedPMR(hDevConnection,
                                          uiSize,
                                          uiChunkSize,
                                          ui32NumPhysChunks,
                                          ui32NumVirtChunks,
                                          pui32MappingTable,
                                          uiLog2Quantum,
                                          uiPMRFlags,
                                          &hPMR);
    if (eError != PVRSRV_OK)
    {
        /* Our check above should have ensured this the "not page
           multiple" error never happens */
        PVR_ASSERT(eError != PVRSRV_ERROR_PMR_NOT_PAGE_MULTIPLE);

        goto failPMR;
    }

    _DevmemImportStructInit(psImport,
							uiSize,
							uiAlign,
							uiFlags,
							hPMR,
							bExportable ? DEVMEM_PROPERTIES_EXPORTABLE : 0);

	*ppsImport = psImport;
	return PVRSRV_OK;

failPMR:
	_DevmemImportDiscard(psImport);
failAlloc:
	PVR_ASSERT(eError != PVRSRV_OK);

	return eError;
}


/*****************************************************************************
 *                    Sub allocation internals                               *
 *****************************************************************************/

IMG_INTERNAL PVRSRV_ERROR
DeviceMemChangeSparse(DEVMEM_MEMDESC *psMemDesc,
					  IMG_UINT32 ui32AllocPageCount,
					  IMG_UINT32 *paui32AllocPageIndices,
					  IMG_UINT32 ui32FreePageCount,
					  IMG_UINT32 *pauiFreePageIndices,
					  SPARSE_MEM_RESIZE_FLAGS uiSparseFlags,
					  IMG_UINT32 *pui32Status)
{
	PVRSRV_ERROR eError=-1;
	IMG_UINT32	ui32Status;
	DEVMEM_IMPORT *psImport=psMemDesc->psImport;
	SHARED_DEV_CONNECTION hDevConnection;
	IMG_HANDLE	hPMR, hSrvDevMemHeap;
	POS_LOCK hLock;
	IMG_DEV_VIRTADDR sDevVAddr;
	IMG_CPU_VIRTADDR sCpuVAddr;

	if(NULL == psImport)
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: Invalid Sparse memory import",__func__));
		goto ChangeSparseError;
	}

	hDevConnection = psImport->hDevConnection;
	hPMR = psImport->hPMR;
	hLock = psImport->hLock;
	sDevVAddr = psImport->sDeviceImport.sDevVAddr;
	sCpuVAddr = psImport->sCPUImport.pvCPUVAddr;

	if(NULL == hDevConnection)
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: Invalid Bridge handle",__func__));
		goto ChangeSparseError;
	}

	if(NULL == hPMR)
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: Invalid PMR handle",__func__));
		goto ChangeSparseError;
	}

	if((uiSparseFlags & SPARSE_RESIZE_BOTH) && (0 == sDevVAddr.uiAddr))
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: Invalid Device Virtual Map",__func__));
		goto ChangeSparseError;
	}

	if((uiSparseFlags & SPARSE_MAP_CPU_ADDR) && (0 == sCpuVAddr))
	{
		PVR_DPF((PVR_DBG_ERROR,"%s: Invalid CPU Virtual Map",__func__));
		goto ChangeSparseError;
	}

	hSrvDevMemHeap  = psImport->sDeviceImport.psHeap->hDevMemServerHeap;

	OSLockAcquire(hLock);

	eError = BridgeChangeSparseMem(hDevConnection,
										hSrvDevMemHeap,
										hPMR,
										ui32AllocPageCount,
										paui32AllocPageIndices,
										ui32FreePageCount,
										pauiFreePageIndices,
										uiSparseFlags,
										psImport->uiFlags,
										sDevVAddr,
										(IMG_UINT64)((uintptr_t)sCpuVAddr),
										&ui32Status);

	 OSLockRelease(hLock);
	 *pui32Status = ui32Status;

#if defined(PVR_RI_DEBUG)
	BridgeRIUpdateMEMDESCBacking(psImport->hDevConnection,
	                             psMemDesc->hRIHandle,
	                             ((IMG_INT32) ui32AllocPageCount - (IMG_INT32) ui32FreePageCount)
	                              * (1 << psImport->sDeviceImport.psHeap->uiLog2Quantum));
#endif

#ifdef PVRSRV_UNMAP_ON_SPARSE_CHANGE
	 if((PVRSRV_OK == eError) && (psMemDesc->sCPUMemDesc.ui32RefCount))
	 {
		 /*
		  * Release the CPU Virtual mapping here
		  * the caller is supposed to map entire range again
		  */
		 DevmemReleaseCpuVirtAddr(psMemDesc);
	 }
#endif

ChangeSparseError:
	return eError;
}

static void
_FreeDeviceMemory(DEVMEM_IMPORT *psImport)
{
	_DevmemImportStructRelease(psImport);
}

static IMG_BOOL
_SubAllocImportAlloc(RA_PERARENA_HANDLE hArena,
                     RA_LENGTH_T uiSize,
                     RA_FLAGS_T _flags,
                     /* returned data */
                     RA_BASE_T *puiBase,
                     RA_LENGTH_T *puiActualSize,
                     RA_PERISPAN_HANDLE *phImport)
{
    /* When suballocations need a new lump of memory, the RA calls
       back here.  Later, in the kernel, we must construct a new PMR
       and a pairing between the new lump of virtual memory and the
       PMR (whether or not such PMR is backed by physical memory) */
    DEVMEM_HEAP *psHeap;
    DEVMEM_IMPORT *psImport;
    IMG_DEVMEM_ALIGN_T uiAlign;
    DEVMEM_FLAGS_T uiFlags;
    PVRSRV_ERROR eError;
    IMG_UINT32 ui32MappingTable = 0;

    uiFlags = (DEVMEM_FLAGS_T) _flags;

    /* Per-arena private handle is, for us, the heap */
    psHeap = hArena;

    /* align to the l.s.b. of the size...  e.g. 96kiB aligned to
       32kiB. NB: There is an argument to say that the RA should never
       ask us for Non-power-of-2 size anyway, but I don't want to make
       that restriction arbitrarily now */
    uiAlign = uiSize & ~(uiSize-1);

    /* The RA should not have invoked us with a size that is not a
       multiple of the quantum anyway */
    PVR_ASSERT((uiSize & ((1ULL<<psHeap->uiLog2Quantum)-1)) == 0);

	eError = _AllocateDeviceMemory(psHeap->psCtx->hDevConnection,
	                               psHeap->uiLog2Quantum,
	                               uiSize,
	                               uiSize,
	                               1,
	                               1,
	                               &ui32MappingTable,
	                               uiAlign,
	                               uiFlags,
	                               IMG_FALSE,
	                               &psImport);
	if (eError != PVRSRV_OK)
	{
		goto failAlloc;
	}

#if defined(PVR_RI_DEBUG)
	{
		eError = BridgeRIWritePMREntry (psImport->hDevConnection,
										psImport->hPMR,
										sizeof("PMR sub-allocated"),
										"PMR sub-allocated",
										psImport->uiSize);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWritePMREntry failed (eError=%d)", __func__, eError));
		}
	}
#endif
	/*
		Suballocations always get mapped into the device was we need to
		key the RA off something and as we can't export suballocations
		there is no valid reason to request an allocation an not map it
	*/
	eError = _DevmemImportStructDevMap(psHeap,
									   IMG_TRUE,
									   psImport);
	if (eError != PVRSRV_OK)
	{
		goto failMap;
	}

	*puiBase = psImport->sDeviceImport.sDevVAddr.uiAddr;
	*puiActualSize = uiSize;
	*phImport = psImport;

    return IMG_TRUE;

    /*
      error exit paths follow
    */
failMap:
    _FreeDeviceMemory(psImport);
failAlloc:

    return IMG_FALSE;
}

static void
_SubAllocImportFree(RA_PERARENA_HANDLE hArena,
                    RA_BASE_T uiBase,
                    RA_PERISPAN_HANDLE hImport)
{
    DEVMEM_IMPORT *psImport = hImport;

    PVR_ASSERT(psImport != NULL);
    PVR_ASSERT(hArena == psImport->sDeviceImport.psHeap);
    PVR_ASSERT(uiBase == psImport->sDeviceImport.sDevVAddr.uiAddr);

    _DevmemImportStructDevUnmap(psImport);  
	_DevmemImportStructRelease(psImport);
}

/*****************************************************************************
 *                    Devmem context internals                               *
 *****************************************************************************/

static PVRSRV_ERROR
_PopulateContextFromBlueprint(struct _DEVMEM_CONTEXT_ *psCtx,
                              DEVMEM_HEAPCFGID uiHeapBlueprintID)
{
    PVRSRV_ERROR eError;
    PVRSRV_ERROR eError2;
    struct _DEVMEM_HEAP_ **ppsHeapArray;
    IMG_UINT32 uiNumHeaps;
    IMG_UINT32 uiHeapsToUnwindOnError;
    IMG_UINT32 uiHeapIndex;
    IMG_DEV_VIRTADDR sDevVAddrBase;
    IMG_CHAR aszHeapName[DEVMEM_HEAPNAME_MAXLENGTH];
    IMG_DEVMEM_SIZE_T uiHeapLength;
    IMG_DEVMEM_LOG2ALIGN_T uiLog2DataPageSize;
    IMG_DEVMEM_LOG2ALIGN_T uiLog2ImportAlignment;

    eError = DevmemHeapCount(psCtx->hDevConnection,
                             uiHeapBlueprintID,
                             &uiNumHeaps);
    if (eError != PVRSRV_OK)
    {
        goto e0;
    }

    if (uiNumHeaps == 0)
    {
        ppsHeapArray = NULL;
    }
    else
    {
        ppsHeapArray = OSAllocMem(sizeof(*ppsHeapArray) * uiNumHeaps);
        if (ppsHeapArray == NULL)
        {
            eError = PVRSRV_ERROR_OUT_OF_MEMORY;
            goto e0;
        }
    }

    uiHeapsToUnwindOnError = 0;

    for (uiHeapIndex = 0; uiHeapIndex < uiNumHeaps; uiHeapIndex++)
    {
        eError = DevmemHeapDetails(psCtx->hDevConnection,
                                   uiHeapBlueprintID,
                                   uiHeapIndex,
                                   &aszHeapName[0],
                                   sizeof(aszHeapName),
                                   &sDevVAddrBase,
                                   &uiHeapLength,
                                   &uiLog2DataPageSize,
                                   &uiLog2ImportAlignment);
        if (eError != PVRSRV_OK)
        {
            goto e1;
        }

        eError = DevmemCreateHeap(psCtx,
                                  sDevVAddrBase,
                                  uiHeapLength,
                                  uiLog2DataPageSize,
                                  uiLog2ImportAlignment,
                                  aszHeapName,
                                  uiHeapBlueprintID,
                                  &ppsHeapArray[uiHeapIndex]);
        if (eError != PVRSRV_OK)
        {
            goto e1;
        }

        uiHeapsToUnwindOnError = uiHeapIndex + 1;
    }

    psCtx->uiAutoHeapCount = uiNumHeaps;
    psCtx->ppsAutoHeapArray = ppsHeapArray;

    PVR_ASSERT(psCtx->uiNumHeaps >= psCtx->uiAutoHeapCount);
    PVR_ASSERT(psCtx->uiAutoHeapCount == uiNumHeaps);

    return PVRSRV_OK;

    /*
      error exit paths
    */
 e1:
    for (uiHeapIndex = 0; uiHeapIndex < uiHeapsToUnwindOnError; uiHeapIndex++)
    {
        eError2 = DevmemDestroyHeap(ppsHeapArray[uiHeapIndex]);
        PVR_ASSERT(eError2 == PVRSRV_OK);
    }

    if (uiNumHeaps != 0)
    {
        OSFreeMem(ppsHeapArray);
    }

 e0:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

static void
_UnpopulateContextFromBlueprint(struct _DEVMEM_CONTEXT_ *psCtx)
{
    PVRSRV_ERROR eError2;
    IMG_UINT32 uiHeapIndex;
    IMG_BOOL bDoCheck = IMG_TRUE;
#if defined(__KERNEL__)
    PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
    if (psPVRSRVData->eServicesState != PVRSRV_SERVICES_STATE_OK)
    {
    	bDoCheck = IMG_FALSE;
    }
#endif

    PVR_ASSERT(psCtx->uiNumHeaps >= psCtx->uiAutoHeapCount);

    for (uiHeapIndex = 0; uiHeapIndex < psCtx->uiAutoHeapCount; uiHeapIndex++)
    {
        eError2 = DevmemDestroyHeap(psCtx->ppsAutoHeapArray[uiHeapIndex]);
        if (bDoCheck)
        {
        	PVR_ASSERT(eError2 == PVRSRV_OK);
        }
    }

    if (psCtx->uiAutoHeapCount != 0)
    {
        OSFreeMem(psCtx->ppsAutoHeapArray);
        psCtx->ppsAutoHeapArray = NULL;
    }
    psCtx->uiAutoHeapCount = 0;

    PVR_ASSERT(psCtx->uiAutoHeapCount == 0);
    PVR_ASSERT(psCtx->ppsAutoHeapArray == NULL);
}


/*****************************************************************************
 *                    Devmem context functions                               *
 *****************************************************************************/

IMG_INTERNAL PVRSRV_ERROR
DevmemCreateContext(SHARED_DEV_CONNECTION hDevConnection,
                    DEVMEM_HEAPCFGID uiHeapBlueprintID,
                   DEVMEM_CONTEXT **ppsCtxPtr)
{
    PVRSRV_ERROR eError;
    DEVMEM_CONTEXT *psCtx;
    /* handle to the server-side counterpart of the device memory
       context (specifically, for handling mapping to device MMU) */
    IMG_HANDLE hDevMemServerContext;
    IMG_HANDLE hPrivData;
    IMG_BOOL bHeapCfgMetaId = (uiHeapBlueprintID == DEVMEM_HEAPCFG_META);

    if (ppsCtxPtr == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto e0;
    }

    psCtx = OSAllocMem(sizeof *psCtx);
    if (psCtx == NULL)
    {
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto e0;
    }

    psCtx->uiNumHeaps = 0;

    psCtx->hDevConnection = hDevConnection;

    /* Create (server-side) Device Memory context */
    eError = BridgeDevmemIntCtxCreate(psCtx->hDevConnection,
                                      bHeapCfgMetaId,
                                      &hDevMemServerContext,
                                      &hPrivData);
    if (eError != PVRSRV_OK)
    {
        goto e1;
    }

    psCtx->hDevMemServerContext = hDevMemServerContext;
    psCtx->hPrivData = hPrivData;

    /* automagic heap creation */
    psCtx->uiAutoHeapCount = 0;

    eError = _PopulateContextFromBlueprint(psCtx, uiHeapBlueprintID);
    if (eError != PVRSRV_OK)
    {
        goto e2;
    }


    *ppsCtxPtr = psCtx;


    PVR_ASSERT(psCtx->uiNumHeaps == psCtx->uiAutoHeapCount);
    return PVRSRV_OK;

    /*
      error exit paths follow
    */

 e2:
    PVR_ASSERT(psCtx->uiAutoHeapCount == 0);
    PVR_ASSERT(psCtx->uiNumHeaps == 0);
    BridgeDevmemIntCtxDestroy(psCtx->hDevConnection, hDevMemServerContext);

 e1:
    OSFreeMem(psCtx);

 e0:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemAcquireDevPrivData(DEVMEM_CONTEXT *psCtx,
                         IMG_HANDLE *hPrivData)
{
	PVRSRV_ERROR eError;

	if ((psCtx == NULL) || (hPrivData == NULL))
	{
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto e0;
	}

	*hPrivData = psCtx->hPrivData;
	return PVRSRV_OK;

e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemReleaseDevPrivData(DEVMEM_CONTEXT *psCtx)
{
	PVRSRV_ERROR eError;

	if (psCtx == NULL)
	{
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto e0;
	}
	return PVRSRV_OK;

e0:
	PVR_ASSERT(eError != PVRSRV_OK);
	return eError;
}


IMG_INTERNAL PVRSRV_ERROR
DevmemFindHeapByName(const struct _DEVMEM_CONTEXT_ *psCtx,
                     const IMG_CHAR *pszHeapName,
                     struct _DEVMEM_HEAP_ **ppsHeapRet)
{
    IMG_UINT32 uiHeapIndex;

    /* N.B.  This func is only useful for finding "automagic" heaps by name */
    for (uiHeapIndex = 0;
         uiHeapIndex < psCtx->uiAutoHeapCount;
         uiHeapIndex++)
    {
        if (!OSStringCompare(psCtx->ppsAutoHeapArray[uiHeapIndex]->pszName, pszHeapName))
        {
            *ppsHeapRet = psCtx->ppsAutoHeapArray[uiHeapIndex];
            return PVRSRV_OK;
        }
    }

    return PVRSRV_ERROR_DEVICEMEM_INVALID_HEAP_INDEX;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemDestroyContext(DEVMEM_CONTEXT *psCtx)
{
    PVRSRV_ERROR eError;
    IMG_BOOL bDoCheck = IMG_TRUE;

#if defined(__KERNEL__)
    PVRSRV_DATA *psPVRSRVData = PVRSRVGetPVRSRVData();
    if (psPVRSRVData->eServicesState != PVRSRV_SERVICES_STATE_OK)
    {
    	bDoCheck = IMG_FALSE;
    }
#endif

    if (psCtx == NULL)
    {
        return PVRSRV_ERROR_INVALID_PARAMS;
    }

    /* should be only the automagically instantiated heaps left */
    if (psCtx->uiNumHeaps != psCtx->uiAutoHeapCount)
    {
        return PVRSRV_ERROR_DEVICEMEM_ALLOCATIONS_REMAIN_IN_HEAP;
    }

    _UnpopulateContextFromBlueprint(psCtx);

    if (bDoCheck)
    {
		PVR_ASSERT(psCtx->uiAutoHeapCount == 0);
		PVR_ASSERT(psCtx->uiNumHeaps == 0);
    }
    eError = BridgeDevmemIntCtxDestroy(psCtx->hDevConnection,
                                       psCtx->hDevMemServerContext);
    if (bDoCheck)
    {
    	PVR_ASSERT (eError == PVRSRV_OK);
    }

    OSFreeMem(psCtx);

    return PVRSRV_OK;
}

/*****************************************************************************
 *                 Devmem heap query functions                               *
 *****************************************************************************/

IMG_INTERNAL PVRSRV_ERROR
DevmemHeapConfigCount(SHARED_DEV_CONNECTION hDevConnection,
                      IMG_UINT32 *puiNumHeapConfigsOut)
{
    PVRSRV_ERROR eError;

    eError = BridgeHeapCfgHeapConfigCount(hDevConnection,
                                          puiNumHeapConfigsOut);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemHeapCount(SHARED_DEV_CONNECTION hDevConnection,
                IMG_UINT32 uiHeapConfigIndex,
                IMG_UINT32 *puiNumHeapsOut)
{
    PVRSRV_ERROR eError;

    eError = BridgeHeapCfgHeapCount(hDevConnection,
                                    uiHeapConfigIndex,
                                    puiNumHeapsOut);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemHeapConfigName(SHARED_DEV_CONNECTION hDevConnection,
                     IMG_UINT32 uiHeapConfigIndex,
                     IMG_CHAR *pszConfigNameOut,
                     IMG_UINT32 uiConfigNameBufSz)
{
    PVRSRV_ERROR eError;

    eError = BridgeHeapCfgHeapConfigName(hDevConnection,
                                         uiHeapConfigIndex,
                                         uiConfigNameBufSz,
                                         pszConfigNameOut);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemHeapDetails(SHARED_DEV_CONNECTION hDevConnection,
                  IMG_UINT32 uiHeapConfigIndex,
                  IMG_UINT32 uiHeapIndex,
                  IMG_CHAR *pszHeapNameOut,
                  IMG_UINT32 uiHeapNameBufSz,
                  IMG_DEV_VIRTADDR *psDevVAddrBaseOut,
                  IMG_DEVMEM_SIZE_T *puiHeapLengthOut,
                  IMG_UINT32 *puiLog2DataPageSizeOut,
                  IMG_UINT32 *puiLog2ImportAlignmentOut)
{
    PVRSRV_ERROR eError;

    eError = BridgeHeapCfgHeapDetails(hDevConnection,
                                      uiHeapConfigIndex,
                                      uiHeapIndex,
                                      uiHeapNameBufSz,
                                      pszHeapNameOut,
                                      psDevVAddrBaseOut,
                                      puiHeapLengthOut,
                                      puiLog2DataPageSizeOut,
                                      puiLog2ImportAlignmentOut);

    VG_MARK_INITIALIZED(pszHeapNameOut,uiHeapNameBufSz);

    return eError;
}

/*****************************************************************************
 *                    Devmem heap functions                                  *
 *****************************************************************************/
 
/* See devicemem.h for important notes regarding the arguments
   to this function */
IMG_INTERNAL PVRSRV_ERROR
DevmemCreateHeap(DEVMEM_CONTEXT *psCtx,
                 IMG_DEV_VIRTADDR sBaseAddress,
                 IMG_DEVMEM_SIZE_T uiLength,
                 IMG_UINT32 ui32Log2Quantum,
                 IMG_UINT32 ui32Log2ImportAlignment,
                 const IMG_CHAR *pszName,
                 DEVMEM_HEAPCFGID uiHeapBlueprintID,
                 DEVMEM_HEAP **ppsHeapPtr)
{
    PVRSRV_ERROR eError = PVRSRV_OK;
    PVRSRV_ERROR eError2;
    DEVMEM_HEAP *psHeap;
    /* handle to the server-side counterpart of the device memory
       heap (specifically, for handling mapping to device MMU */
    IMG_HANDLE hDevMemServerHeap;
    IMG_BOOL bRANoSplit = IMG_FALSE;

    IMG_CHAR aszBuf[100];
    IMG_CHAR *pszStr;

    if (ppsHeapPtr == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto e0;
    }

    psHeap = OSAllocMem(sizeof *psHeap);
    if (psHeap == NULL)
    {
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto e0;
    }

    /* Need to keep local copy of heap name, so caller may free
       theirs */
    pszStr = OSAllocMem(OSStringLength(pszName)+1);
    if (pszStr == NULL)
    {
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto e1;
    }
    OSStringCopy(pszStr, pszName);
    psHeap->pszName = pszStr;

    psHeap->sBaseAddress = sBaseAddress;
    OSAtomicWrite(&psHeap->hImportCount,0);

    OSSNPrintf(aszBuf, sizeof(aszBuf),
               "NDM heap '%s' (suballocs) ctx:%p",
               pszName, psCtx);
    pszStr = OSAllocMem(OSStringLength(aszBuf)+1);
    if (pszStr == NULL)
    {
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto e2;
    }
    OSStringCopy(pszStr, aszBuf);
    psHeap->pszSubAllocRAName = pszStr;

#if defined(PDUMP) && defined(ANDROID)
    /* the META heap is shared globally so a single
     * physical memory import may be used to satisfy
     * allocations of different processes.
     * This is problematic when PDumping because the
     * physical memory import used to satisfy a new allocation
     * may actually have been imported (and thus the PDump MALLOC
     * generated) before the PDump client was started, leading to the
     * MALLOC being missing.
     * This is solved by disabling splitting of imports for the META physmem
     * RA, meaning that every firmware allocation gets its own import, thus
     * ensuring the MALLOC is present for every allocation made within the
     * pdump capture range
     */
    if(uiHeapBlueprintID == DEVMEM_HEAPCFG_META)
    {
    	bRANoSplit = IMG_TRUE;
    }
#else
    PVR_UNREFERENCED_PARAMETER(uiHeapBlueprintID);
#endif


    psHeap->psSubAllocRA = RA_Create(psHeap->pszSubAllocRAName,
                       /* Subsequent imports: */
                       ui32Log2Quantum,
					   RA_LOCKCLASS_2,
                       _SubAllocImportAlloc,
                       _SubAllocImportFree,
                       (RA_PERARENA_HANDLE) psHeap,
                       bRANoSplit);
    if (psHeap->psSubAllocRA == NULL)
    {
        eError = PVRSRV_ERROR_DEVICEMEM_UNABLE_TO_CREATE_ARENA;
        goto e3;
    }

    psHeap->uiLog2ImportAlignment = ui32Log2ImportAlignment;
    psHeap->uiLog2Quantum = ui32Log2Quantum;

    OSSNPrintf(aszBuf, sizeof(aszBuf),
               "NDM heap '%s' (QVM) ctx:%p",
               pszName, psCtx);
    pszStr = OSAllocMem(OSStringLength(aszBuf)+1);
    if (pszStr == NULL)
    {
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto e4;
    }
    OSStringCopy(pszStr, aszBuf);
    psHeap->pszQuantizedVMRAName = pszStr;

    psHeap->psQuantizedVMRA = RA_Create(psHeap->pszQuantizedVMRAName,
                       /* Subsequent import: */
                                       0, RA_LOCKCLASS_1, NULL, NULL,
                       (RA_PERARENA_HANDLE) psHeap,
                       IMG_FALSE);

    if (psHeap->psQuantizedVMRA == NULL)
    {
        eError = PVRSRV_ERROR_DEVICEMEM_UNABLE_TO_CREATE_ARENA;
        goto e5;
    }

	if (!RA_Add(psHeap->psQuantizedVMRA,
                       (RA_BASE_T)sBaseAddress.uiAddr,
                       (RA_LENGTH_T)uiLength,
                       (RA_FLAGS_T)0, /* This RA doesn't use or need flags */
				NULL /* per ispan handle */))
	{
		RA_Delete(psHeap->psQuantizedVMRA);
        eError = PVRSRV_ERROR_DEVICEMEM_UNABLE_TO_CREATE_ARENA;
        goto e5;
	}


    psHeap->psCtx = psCtx;


    /* Create server-side counterpart of Device Memory heap */
    eError = BridgeDevmemIntHeapCreate(psCtx->hDevConnection,
                                      psCtx->hDevMemServerContext,
                                      sBaseAddress,
                                      uiLength,
                                      ui32Log2Quantum,
                                      &hDevMemServerHeap);
    if (eError != PVRSRV_OK)
    {
        goto e6;
    }
    psHeap->hDevMemServerHeap = hDevMemServerHeap;

	eError = OSLockCreate(&psHeap->hLock, LOCK_TYPE_PASSIVE);
	if (eError != PVRSRV_OK)
	{
		goto e7;
	}

    psHeap->psCtx->uiNumHeaps ++;
    *ppsHeapPtr = psHeap;

#if defined PVRSRV_NEWDEVMEM_SUPPORT_MEM_TRACKING
    psHeap->psMemDescList = NULL;
#endif  /* PVRSRV_NEWDEVMEM_SUPPORT_MEM_TRACKING */

    return PVRSRV_OK;

    /*
      error exit paths
    */
 e7:
    eError2 = BridgeDevmemIntHeapDestroy(psCtx->hDevConnection,
                                       psHeap->hDevMemServerHeap);
    PVR_ASSERT (eError2 == PVRSRV_OK);
 e6:
    RA_Delete(psHeap->psQuantizedVMRA);
 e5:
    OSFreeMem(psHeap->pszQuantizedVMRAName);
 e4:
    RA_Delete(psHeap->psSubAllocRA);
 e3:
    OSFreeMem(psHeap->pszSubAllocRAName);
 e2:
    OSFreeMem(psHeap->pszName);
 e1:
    OSFreeMem(psHeap);
 e0:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemGetHeapBaseDevVAddr(struct _DEVMEM_HEAP_ *psHeap,
			  IMG_DEV_VIRTADDR *pDevVAddr)
{
	if (psHeap == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	*pDevVAddr = psHeap->sBaseAddress;

	return PVRSRV_OK;
}

IMG_INTERNAL void
DevmemExportalignAdjustSizeAndAlign(DEVMEM_HEAP *psHeap, IMG_DEVMEM_SIZE_T *puiSize, IMG_DEVMEM_ALIGN_T *puiAlign)
{
	IMG_DEVMEM_SIZE_T uiSize = *puiSize;
	IMG_DEVMEM_ALIGN_T uiAlign = *puiAlign;
	IMG_UINT32 uiLog2Quantum;

	if (psHeap)
	{
		uiLog2Quantum = psHeap->uiLog2Quantum;
	}
	else
	{
		uiLog2Quantum = GET_LOG2_PAGESIZE();
	}

    if ((1ULL << uiLog2Quantum) > uiAlign)
    {
		uiAlign = 1ULL << uiLog2Quantum;
    }
    uiSize = (uiSize + uiAlign - 1) & ~(uiAlign - 1);

	*puiSize = uiSize;
	*puiAlign = uiAlign;
}


IMG_INTERNAL PVRSRV_ERROR
DevmemDestroyHeap(DEVMEM_HEAP *psHeap)
{
    PVRSRV_ERROR eError;
	IMG_INT uiImportCount;

    if (psHeap == NULL)
    {
        return PVRSRV_ERROR_INVALID_PARAMS;
    }

	uiImportCount = OSAtomicRead(&psHeap->hImportCount);
    if (uiImportCount > 0)
    {
        PVR_DPF((PVR_DBG_ERROR, "%d(%s) leaks remain", uiImportCount, psHeap->pszName));
        return PVRSRV_ERROR_DEVICEMEM_ALLOCATIONS_REMAIN_IN_HEAP;
    }

	OSLockDestroy(psHeap->hLock);

    PVR_ASSERT(psHeap->psCtx->uiNumHeaps > 0);
    psHeap->psCtx->uiNumHeaps --;

    eError = BridgeDevmemIntHeapDestroy(psHeap->psCtx->hDevConnection,
                                        psHeap->hDevMemServerHeap);
    PVR_ASSERT (eError == PVRSRV_OK);

    RA_Delete(psHeap->psQuantizedVMRA);
    OSFreeMem(psHeap->pszQuantizedVMRAName);

    RA_Delete(psHeap->psSubAllocRA);
    OSFreeMem(psHeap->pszSubAllocRAName);

    OSFreeMem(psHeap->pszName);

    OSFreeMem(psHeap);

    return PVRSRV_OK;
}

/*****************************************************************************
 *                Devmem allocation/free functions                           *
 *****************************************************************************/

IMG_INTERNAL PVRSRV_ERROR
DevmemAllocate(DEVMEM_HEAP *psHeap,
               IMG_DEVMEM_SIZE_T uiSize,
               IMG_DEVMEM_ALIGN_T uiAlign,
               DEVMEM_FLAGS_T uiFlags,
               const IMG_PCHAR pszText,
			   DEVMEM_MEMDESC **ppsMemDescPtr)
{
    IMG_BOOL bStatus; /* eError for RA */
    RA_BASE_T uiAllocatedAddr;
    RA_LENGTH_T uiAllocatedSize;
    RA_PERISPAN_HANDLE hImport; /* the "import" from which this sub-allocation came */
    RA_FLAGS_T uiFlagsForRA;
    PVRSRV_ERROR eError;
    DEVMEM_MEMDESC *psMemDesc = NULL;
	IMG_DEVMEM_OFFSET_T uiOffset = 0;
	DEVMEM_IMPORT *psImport;
	void *pvAddr;

	if (uiFlags & PVRSRV_MEMALLOCFLAG_NO_OSPAGES_ON_ALLOC)
	{
		/* Deferred Allocation not supported on SubAllocs*/
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto failParams;
	}

    if (psHeap == NULL || ppsMemDescPtr == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto failParams;
    }

	eError = _DevmemValidateParams(uiSize,
								   uiAlign,
								   uiFlags);
	if (eError != PVRSRV_OK)
	{
		goto failParams;
	}

	eError =_DevmemMemDescAlloc(&psMemDesc);
    if (eError != PVRSRV_OK)
    {
        goto failMemDescAlloc;
    }

    /*
        If zero flag is set we have to have write access to the page.
    */
    uiFlags |= (uiFlags & PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC) ? PVRSRV_MEMALLOCFLAG_CPU_WRITEABLE : 0;

	/*
		No request for exportable memory so use the RA
	*/
    uiFlagsForRA = (RA_FLAGS_T)(uiFlags & PVRSRV_MEMALLOCFLAGS_RA_DIFFERENTIATION_MASK);
    /* Check that the cast didn't lose any flags due to different integer widths */
    PVR_ASSERT(uiFlagsForRA == (uiFlags & PVRSRV_MEMALLOCFLAGS_RA_DIFFERENTIATION_MASK));

	/* 
	   When the RA suballocates memory from a Span it does not zero it. It only zeroes the
	   memory if it allocates a new Span; but we don't know what is going to happen for this
	   RA_Alloc call. Therefore, we zero the mem after the allocation below.
	*/
	uiFlagsForRA &= ~PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC;
	
	bStatus = RA_Alloc(psHeap->psSubAllocRA,
					   uiSize,
					   uiFlagsForRA,
					   uiAlign,
					   &uiAllocatedAddr,
					   &uiAllocatedSize,
					   &hImport);
	if (!bStatus)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto failDeviceMemAlloc;
	}

	psImport = hImport;

	/* This assignment is assuming the RA returns an hImport where suballocations
	 * can be made from if uiSize is NOT a page multiple of the passed heap.
	 *
	 * So we check if uiSize is a page multiple and mark it as suballocatable
	 * if it is not.
	 * */
	if (uiSize & ((1 << psHeap->uiLog2Quantum) - 1) )
	{
		psImport->uiProperties |= DEVMEM_PROPERTIES_SUBALLOCATABLE;
	}

	uiOffset = uiAllocatedAddr - psImport->sDeviceImport.sDevVAddr.uiAddr;

	_DevmemMemDescInit(psMemDesc,
					   uiOffset,
					   psImport,
					   uiSize);

	/* zero the memory */
	if (uiFlags & PVRSRV_MEMALLOCFLAG_ZERO_ON_ALLOC)

	{
		eError = DevmemAcquireCpuVirtAddr(psMemDesc, &pvAddr);
		if (eError != PVRSRV_OK)
		{
			goto failZero;
		}

#if (defined(_WIN32) && !defined(_WIN64)) || (defined(LINUX) && defined(__i386__))
		PVR_ASSERT(uiSize<IMG_UINT32_MAX);
#endif

		OSDeviceMemSet(pvAddr, 0x0, (size_t) uiSize);
	    
		DevmemReleaseCpuVirtAddr(psMemDesc);

#if defined(PDUMP)
		DevmemPDumpLoadZeroMem(psMemDesc, 0, uiSize, PDUMP_FLAGS_CONTINUOUS);
#endif
	}

#if defined(SUPPORT_PAGE_FAULT_DEBUG)
	/* copy the allocation descriptive name and size so it can be passed to DevicememHistory when
	 * the allocation gets mapped/unmapped
	 */
	OSStringNCopy(psMemDesc->sTraceData.szText, pszText, sizeof(psMemDesc->sTraceData.szText) - 1);
#endif

#if defined(PVR_RI_DEBUG)
	{
		/* Attach RI information */
		eError = BridgeRIWriteMEMDESCEntry (psMemDesc->psImport->hDevConnection,
											psMemDesc->psImport->hPMR,
											OSStringNLength(pszText, RI_MAX_TEXT_LEN),
											pszText,
											psMemDesc->uiOffset,
											uiAllocatedSize,
											uiAllocatedSize,
											IMG_FALSE,
											IMG_FALSE,
											&(psMemDesc->hRIHandle));
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWriteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#else  /* if defined(PVR_RI_DEBUG) */
	PVR_UNREFERENCED_PARAMETER (pszText);
#endif /* if defined(PVR_RI_DEBUG) */

	*ppsMemDescPtr = psMemDesc;

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

failZero:
	_DevmemMemDescRelease(psMemDesc);
	psMemDesc = NULL;	/* Make sure we don't do a discard after the release */
failDeviceMemAlloc:
	if (psMemDesc)
		_DevmemMemDescDiscard(psMemDesc);
failMemDescAlloc:
failParams:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}



IMG_INTERNAL PVRSRV_ERROR
DevmemAllocateExportable(SHARED_DEV_CONNECTION hDevConnection,
						 IMG_DEVMEM_SIZE_T uiSize,
						 IMG_DEVMEM_ALIGN_T uiAlign,
						 DEVMEM_FLAGS_T uiFlags,
						 const IMG_PCHAR pszText,
						 DEVMEM_MEMDESC **ppsMemDescPtr)
{
    PVRSRV_ERROR eError;
    DEVMEM_MEMDESC *psMemDesc = NULL;
	DEVMEM_IMPORT *psImport;
    IMG_UINT32 ui32MappingTable = 0;

	DevmemExportalignAdjustSizeAndAlign(NULL,
										&uiSize,
										&uiAlign);

	eError = _DevmemValidateParams(uiSize,
								   uiAlign,
								   uiFlags);
	if (eError != PVRSRV_OK)
	{
		goto failParams;
	}


	eError =_DevmemMemDescAlloc(&psMemDesc);
    if (eError != PVRSRV_OK)
    {
        goto failMemDescAlloc;
    }

	/*
		Note:
		In the case of exportable memory we have no heap to
		query the pagesize from, so we assume host pagesize.
	*/
	eError = _AllocateDeviceMemory(hDevConnection,
								   GET_LOG2_PAGESIZE(),
								   uiSize,
								   uiSize,
								   1,
								   1,
								   &ui32MappingTable,
								   uiAlign,
								   uiFlags,
								   IMG_TRUE,
								   &psImport);
	if (eError != PVRSRV_OK)
	{
		goto failDeviceMemAlloc;
	}

	_DevmemMemDescInit(psMemDesc,
					   0,
					   psImport,
					   uiSize);

    *ppsMemDescPtr = psMemDesc;

#if defined(SUPPORT_PAGE_FAULT_DEBUG)
	/* copy the allocation descriptive name and size so it can be passed to DevicememHistory when
	 * the allocation gets mapped/unmapped
	 */
	OSStringNCopy(psMemDesc->sTraceData.szText, pszText, sizeof(psMemDesc->sTraceData.szText) - 1);
#endif

#if defined(PVR_RI_DEBUG)
	{
		eError = BridgeRIWritePMREntry (psImport->hDevConnection,
										psImport->hPMR,
										OSStringNLength(pszText, RI_MAX_TEXT_LEN),
										(IMG_CHAR *)pszText,
										psImport->uiSize);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWritePMREntry failed (eError=%d)", __func__, eError));
		}

		 /* Attach RI information */
		eError = BridgeRIWriteMEMDESCEntry (psImport->hDevConnection,
											psImport->hPMR,
											sizeof("^"),
											"^",
											psMemDesc->uiOffset,
											uiSize,
											uiSize,
											IMG_FALSE,
											IMG_TRUE,
											&psMemDesc->hRIHandle);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWriteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#else  /* if defined(PVR_RI_DEBUG) */
	PVR_UNREFERENCED_PARAMETER (pszText);
#endif /* if defined(PVR_RI_DEBUG) */

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

failDeviceMemAlloc:
    _DevmemMemDescDiscard(psMemDesc);

failMemDescAlloc:
failParams:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemAllocateSparse(SHARED_DEV_CONNECTION hDevConnection,
					 IMG_DEVMEM_SIZE_T uiSize,
					 IMG_DEVMEM_SIZE_T uiChunkSize,
					 IMG_UINT32 ui32NumPhysChunks,
					 IMG_UINT32 ui32NumVirtChunks,
					 IMG_UINT32 *pui32MappingTable,
					 IMG_DEVMEM_ALIGN_T uiAlign,
					 DEVMEM_FLAGS_T uiFlags,
					 const IMG_PCHAR pszText,
					 DEVMEM_MEMDESC **ppsMemDescPtr)
{
    PVRSRV_ERROR eError;
    DEVMEM_MEMDESC *psMemDesc = NULL;
	DEVMEM_IMPORT *psImport;

	DevmemExportalignAdjustSizeAndAlign(NULL,
										&uiSize,
										&uiAlign);

	eError = _DevmemValidateParams(uiSize,
								   uiAlign,
								   uiFlags);
	if (eError != PVRSRV_OK)
	{
		goto failParams;
	}

	eError =_DevmemMemDescAlloc(&psMemDesc);
    if (eError != PVRSRV_OK)
    {
        goto failMemDescAlloc;
    }

	/*
		Note:
		In the case of sparse memory we have no heap to
		query the pagesize from, so we assume host pagesize.
	*/
    eError = _AllocateDeviceMemory(hDevConnection,
								   GET_LOG2_PAGESIZE(),
								   uiSize,
								   uiChunkSize,
								   ui32NumPhysChunks,
								   ui32NumVirtChunks,
								   pui32MappingTable,
								   uiAlign,
								   uiFlags,
								   IMG_TRUE,
								   &psImport);
	if (eError != PVRSRV_OK)
	{
		goto failDeviceMemAlloc;
	}

	_DevmemMemDescInit(psMemDesc,
					   0,
					   psImport,
					   uiSize);

#if defined(SUPPORT_PAGE_FAULT_DEBUG)
	/* copy the allocation descriptive name and size so it can be passed to DevicememHistory when
	 * the allocation gets mapped/unmapped
	 */
	OSStringNCopy(psMemDesc->sTraceData.szText, pszText, sizeof(psMemDesc->sTraceData.szText) - 1);
#endif

#if defined(PVR_RI_DEBUG)
	{
		eError = BridgeRIWritePMREntry (psImport->hDevConnection,
										psImport->hPMR,
										OSStringNLength(pszText, RI_MAX_TEXT_LEN),
										(IMG_CHAR *)pszText,
										psImport->uiSize);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWritePMREntry failed (eError=%d)", __func__, eError));
		}

		/* Attach RI information */
    	eError = BridgeRIWriteMEMDESCEntry (psMemDesc->psImport->hDevConnection,
											psMemDesc->psImport->hPMR,
											sizeof("^"),
											"^",
											psMemDesc->uiOffset,
											uiSize,
											ui32NumPhysChunks * uiChunkSize,
											IMG_FALSE,
											IMG_TRUE,
											&psMemDesc->hRIHandle);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWriteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#else  /* if defined(PVR_RI_DEBUG) */
	PVR_UNREFERENCED_PARAMETER (pszText);
#endif /* if defined(PVR_RI_DEBUG) */

	*ppsMemDescPtr = psMemDesc;

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

failDeviceMemAlloc:
    _DevmemMemDescDiscard(psMemDesc);

failMemDescAlloc:
failParams:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemMakeLocalImportHandle(SHARED_DEV_CONNECTION hBridge,
                            IMG_HANDLE hServerHandle,
                            IMG_HANDLE *hLocalImportHandle)
{
	return BridgePMRMakeLocalImportHandle(hBridge,
	                                      hServerHandle,
	                                      hLocalImportHandle);
}

IMG_INTERNAL PVRSRV_ERROR
DevmemUnmakeLocalImportHandle(SHARED_DEV_CONNECTION hBridge,
                              IMG_HANDLE hLocalImportHandle)
{
	return BridgePMRUnmakeLocalImportHandle(hBridge, hLocalImportHandle);
}

/*****************************************************************************
 *                Devmem unsecure export functions                           *
 *****************************************************************************/

#if defined(SUPPORT_INSECURE_EXPORT)

static PVRSRV_ERROR
_Mapping_Export(DEVMEM_IMPORT *psImport,
                DEVMEM_EXPORTHANDLE *phPMRExportHandlePtr,
                DEVMEM_EXPORTKEY *puiExportKeyPtr,
                DEVMEM_SIZE_T *puiSize,
                DEVMEM_LOG2ALIGN_T *puiLog2Contig)
{
    /* Gets an export handle and key for the PMR used for this mapping */
    /* Can only be done if there are no suballocations for this mapping */

    PVRSRV_ERROR eError;
    DEVMEM_EXPORTHANDLE hPMRExportHandle;
    DEVMEM_EXPORTKEY uiExportKey;
    IMG_DEVMEM_SIZE_T uiSize;
    IMG_DEVMEM_LOG2ALIGN_T uiLog2Contig;

    if (psImport == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto failParams;
    }

	if ((psImport->uiProperties & DEVMEM_PROPERTIES_EXPORTABLE) == 0)
    {
		eError = PVRSRV_ERROR_DEVICEMEM_CANT_EXPORT_SUBALLOCATION;
        goto failParams;
    }

    eError = BridgePMRExportPMR(psImport->hDevConnection,
                                psImport->hPMR,
                                &hPMRExportHandle,
                                &uiSize,
                                &uiLog2Contig,
                                &uiExportKey);
    if (eError != PVRSRV_OK)
    {
        goto failExport;
    }

    PVR_ASSERT(uiSize == psImport->uiSize);

    *phPMRExportHandlePtr = hPMRExportHandle;
    *puiExportKeyPtr = uiExportKey;
    *puiSize = uiSize;
    *puiLog2Contig = uiLog2Contig;

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

failExport:
failParams:

    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;

}

static void
_Mapping_Unexport(DEVMEM_IMPORT *psImport,
                  DEVMEM_EXPORTHANDLE hPMRExportHandle)
{
    PVRSRV_ERROR eError;

    PVR_ASSERT (psImport != NULL);

    eError = BridgePMRUnexportPMR(psImport->hDevConnection,
                                  hPMRExportHandle);
    PVR_ASSERT(eError == PVRSRV_OK);
}

IMG_INTERNAL PVRSRV_ERROR
DevmemExport(DEVMEM_MEMDESC *psMemDesc,
             DEVMEM_EXPORTCOOKIE *psExportCookie)
{
    /* Caller to provide storage for export cookie struct */
    PVRSRV_ERROR eError;
    IMG_HANDLE hPMRExportHandle = 0;
    IMG_UINT64 uiPMRExportPassword = 0;
    IMG_DEVMEM_SIZE_T uiSize = 0;
    IMG_DEVMEM_LOG2ALIGN_T uiLog2Contig = 0;

    if (psMemDesc == NULL || psExportCookie == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto e0;
    }

    eError = _Mapping_Export(psMemDesc->psImport,
                             &hPMRExportHandle,
                             &uiPMRExportPassword,
                             &uiSize,
                             &uiLog2Contig);
    if (eError != PVRSRV_OK)
    {
		psExportCookie->uiSize = 0;
        goto e0;
    }

    psExportCookie->hPMRExportHandle = hPMRExportHandle;
    psExportCookie->uiPMRExportPassword = uiPMRExportPassword;
    psExportCookie->uiSize = uiSize;
    psExportCookie->uiLog2ContiguityGuarantee = uiLog2Contig;

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

 e0:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

IMG_INTERNAL void
DevmemUnexport(DEVMEM_MEMDESC *psMemDesc,
               DEVMEM_EXPORTCOOKIE *psExportCookie)
{
    _Mapping_Unexport(psMemDesc->psImport,
                      psExportCookie->hPMRExportHandle);

    psExportCookie->uiSize = 0;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemImport(SHARED_DEV_CONNECTION hDevConnection,
			 DEVMEM_EXPORTCOOKIE *psCookie,
			 DEVMEM_FLAGS_T uiFlags,
			 DEVMEM_MEMDESC **ppsMemDescPtr)
{
    DEVMEM_MEMDESC *psMemDesc = NULL;
    DEVMEM_IMPORT *psImport;
    IMG_HANDLE hPMR;
    PVRSRV_ERROR eError;

	if (ppsMemDescPtr == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto failParams;
    }

	eError =_DevmemMemDescAlloc(&psMemDesc);
    if (eError != PVRSRV_OK)
    {
        goto failMemDescAlloc;
    }

	eError = _DevmemImportStructAlloc(hDevConnection,
									  &psImport);
    if (eError != PVRSRV_OK)
    {
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto failImportAlloc;
    }

    /* Get a handle to the PMR (inc refcount) */
    eError = BridgePMRImportPMR(hDevConnection,
                                psCookie->hPMRExportHandle,
                                psCookie->uiPMRExportPassword,
                                psCookie->uiSize, /* not trusted - just for sanity checks */
                                psCookie->uiLog2ContiguityGuarantee, /* not trusted - just for sanity checks */
                                &hPMR);
    if (eError != PVRSRV_OK)
    {
        goto failImport;
    }

	_DevmemImportStructInit(psImport,
							psCookie->uiSize,
							1ULL << psCookie->uiLog2ContiguityGuarantee,
							uiFlags,
							hPMR,
							DEVMEM_PROPERTIES_IMPORTED |
							DEVMEM_PROPERTIES_EXPORTABLE);

	_DevmemMemDescInit(psMemDesc,
					   0,
					   psImport,
					   psImport->uiSize);

    *ppsMemDescPtr = psMemDesc;

#if defined(PVR_RI_DEBUG)
	{
		/* Attach RI information */
		eError = BridgeRIWriteMEMDESCEntry (psMemDesc->psImport->hDevConnection,
											psMemDesc->psImport->hPMR,
											sizeof("^"),
											"^",
											psMemDesc->uiOffset,
											psMemDesc->psImport->uiSize,
											psMemDesc->psImport->uiSize,
											IMG_TRUE,
											IMG_FALSE,
											&psMemDesc->hRIHandle);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWriteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#endif /* if defined(PVR_RI_DEBUG) */

	return PVRSRV_OK;

    /*
      error exit paths follow
    */

failImport:
    _DevmemImportDiscard(psImport);
failImportAlloc:
    _DevmemMemDescDiscard(psMemDesc);
failMemDescAlloc:
failParams:
    PVR_ASSERT(eError != PVRSRV_OK);

    return eError;
}

#endif /* SUPPORT_INSECURE_EXPORT */

/*****************************************************************************
 *                   Common MemDesc functions                                *
 *****************************************************************************/
IMG_INTERNAL PVRSRV_ERROR
DevmemUnpin(DEVMEM_MEMDESC *psMemDesc)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	DEVMEM_IMPORT *psImport = psMemDesc->psImport;

	/* Stop if the allocation might have suballocations. */
	if (psImport->uiProperties & DEVMEM_PROPERTIES_SUBALLOCATABLE)
	{
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		PVR_DPF((PVR_DBG_ERROR,
		         "%s: The passed allocation is not valid to unpin because "
		         "there might be suballocations on it. Make sure you allocate a page multiple "
		         "of the heap when using PVRSRVAllocDeviceMem()",
		         __FUNCTION__));

		goto e_exit;
	}

	/* Stop if the Import is still mapped to CPU */
	if (psImport->sCPUImport.ui32RefCount)
	{
		eError = PVRSRV_ERROR_STILL_MAPPED;
		PVR_DPF((PVR_DBG_ERROR,
		         "%s: There are still %u references on the CPU mapping. "
		         "Please remove all CPU mappings before unpinning.",
		         __FUNCTION__,
		         psImport->sCPUImport.ui32RefCount));

		goto e_exit;
	}

	/* Only unpin if it is not already unpinned
	 * Return PVRSRV_OK */
	if (psImport->uiProperties & DEVMEM_PROPERTIES_UNPINNED)
	{
		goto e_exit;
	}

	/* Unpin it and invalidate mapping */
	if (psImport->sDeviceImport.bMapped == IMG_TRUE)
	{
		eError = BridgeDevmemIntUnpinInvalidate(psImport->hDevConnection,
		                                        psImport->sDeviceImport.hMapping,
		                                        psImport->hPMR);
	}
	else
	{
		/* Or just unpin it */
		eError = BridgeDevmemIntUnpin(psImport->hDevConnection,
		                              psImport->hPMR);
	}

	/* Update flags and RI when call was successful */
	if (eError == PVRSRV_OK)
	{
		psImport->uiProperties |= DEVMEM_PROPERTIES_UNPINNED;
#if defined(PVR_RI_DEBUG)
		if (psMemDesc->hRIHandle)
		{
			PVRSRV_ERROR eError2;

			eError2 = BridgeRIUpdateMEMDESCPinning(psMemDesc->psImport->hDevConnection,
			                                       psMemDesc->hRIHandle,
			                                       IMG_FALSE);

			if( eError2 != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIUpdateMEMDESCPinningKM failed (eError=%d)",
				         __func__,
				         eError));
			}
		}
#endif
	}
	else
	{
		/* Or just show what went wrong */
		PVR_DPF((PVR_DBG_ERROR, "%s: Unpin aborted because of error %d",
		         __func__,
		         eError));
	}

e_exit:
	return eError;
}


IMG_INTERNAL PVRSRV_ERROR
DevmemPin(DEVMEM_MEMDESC *psMemDesc)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	DEVMEM_IMPORT *psImport = psMemDesc->psImport;

	/* Only pin if it is unpinned */
	if ((psImport->uiProperties & DEVMEM_PROPERTIES_UNPINNED) == 0)
	{
		goto e_exit;
	}

	/* Pin it and make mapping valid */
	if (psImport->sDeviceImport.bMapped)
	{
		eError = BridgeDevmemIntPinValidate(psImport->hDevConnection,
		                                    psImport->sDeviceImport.hMapping,
		                                    psImport->hPMR);
	}
	else
	{
		/* Or just pin it */
		eError = BridgeDevmemIntPin(psImport->hDevConnection,
		                            psImport->hPMR);
	}

	if ( (eError == PVRSRV_OK) || (eError == PVRSRV_ERROR_PMR_NEW_MEMORY) )
	{
		psImport->uiProperties &= ~DEVMEM_PROPERTIES_UNPINNED;
#if defined(PVR_RI_DEBUG)
		if (psMemDesc->hRIHandle)
		{
			PVRSRV_ERROR eError2;

			eError2 = BridgeRIUpdateMEMDESCPinning(psMemDesc->psImport->hDevConnection,
			                                       psMemDesc->hRIHandle,
			                                       IMG_TRUE);

			if( eError2 != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIUpdateMEMDESCPinningKM failed (eError=%d)",
				         __func__,
				         eError));
			}
		}
#endif
	}
	else
	{
		/* Or just show what went wrong */
		PVR_DPF((PVR_DBG_ERROR, "%s: Pin aborted because of error %d",
		         __func__,
		         eError));
	}

e_exit:
	return eError;
}

/*
	This function is called for freeing any class of memory
*/
IMG_INTERNAL void
DevmemFree(DEVMEM_MEMDESC *psMemDesc)
{
#if defined(PVR_RI_DEBUG)
	if (psMemDesc->hRIHandle)
	{
	    PVRSRV_ERROR eError;

	    eError = BridgeRIDeleteMEMDESCEntry(psMemDesc->psImport->hDevConnection,
					   	   	   	   psMemDesc->hRIHandle);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIDeleteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#endif  /* if defined(PVR_RI_DEBUG) */
	_DevmemMemDescRelease(psMemDesc);
}

IMG_INTERNAL PVRSRV_ERROR
DevmemMapToDevice(DEVMEM_MEMDESC *psMemDesc,
				  DEVMEM_HEAP *psHeap,
				  IMG_DEV_VIRTADDR *psDevVirtAddr)
{
	DEVMEM_IMPORT *psImport;
	IMG_DEV_VIRTADDR sDevVAddr;
	PVRSRV_ERROR eError;
	IMG_BOOL bMap = IMG_TRUE;

	/* Do not try to map unpinned memory */
	if (psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_UNPINNED)
	{
		eError = PVRSRV_ERROR_INVALID_MAP_REQUEST;
		goto failFlags;
	}

	OSLockAcquire(psMemDesc->sDeviceMemDesc.hLock);
	if (psHeap == NULL)
	{
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto failParams;
	}

	if (psMemDesc->sDeviceMemDesc.ui32RefCount != 0)
	{
		eError = PVRSRV_ERROR_DEVICEMEM_ALREADY_MAPPED;
		goto failCheck;
	}

	/* Don't map memory for deferred allocations */
	if (psMemDesc->psImport->uiFlags & PVRSRV_MEMALLOCFLAG_NO_OSPAGES_ON_ALLOC)
	{
		PVR_ASSERT(psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_EXPORTABLE);
		bMap = IMG_FALSE;
	}

	DEVMEM_REFCOUNT_PRINT("%s (%p) %d->%d",
					__FUNCTION__,
					psMemDesc,
					psMemDesc->sDeviceMemDesc.ui32RefCount,
					psMemDesc->sDeviceMemDesc.ui32RefCount+1);

	psImport = psMemDesc->psImport;
	_DevmemMemDescAcquire(psMemDesc);

	eError = _DevmemImportStructDevMap(psHeap,
									   bMap,
									   psImport);
	if (eError != PVRSRV_OK)
	{
		goto failMap;
	}

	sDevVAddr.uiAddr = psImport->sDeviceImport.sDevVAddr.uiAddr;
	sDevVAddr.uiAddr += psMemDesc->uiOffset;
	psMemDesc->sDeviceMemDesc.sDevVAddr = sDevVAddr;
	psMemDesc->sDeviceMemDesc.ui32RefCount++;

    *psDevVirtAddr = psMemDesc->sDeviceMemDesc.sDevVAddr;

    OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);

#if defined(SUPPORT_PAGE_FAULT_DEBUG)
	BridgeDevicememHistoryMap(psMemDesc->psImport->hDevConnection,
						psMemDesc->sDeviceMemDesc.sDevVAddr,
						psMemDesc->uiAllocSize,
						psMemDesc->sTraceData.szText);
#endif

#if defined(PVR_RI_DEBUG)
	if (psMemDesc->hRIHandle)
    {
		 eError = BridgeRIUpdateMEMDESCAddr(psImport->hDevConnection,
    									   psMemDesc->hRIHandle,
    									   psImport->sDeviceImport.sDevVAddr);
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIUpdateMEMDESCAddr failed (eError=%d)", __func__, eError));
		}
	}
#endif

    return PVRSRV_OK;

failMap:
	_DevmemMemDescRelease(psMemDesc);
failCheck:
failParams:
	OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);
	PVR_ASSERT(eError != PVRSRV_OK);

failFlags:
	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemAcquireDevVirtAddr(DEVMEM_MEMDESC *psMemDesc,
                         IMG_DEV_VIRTADDR *psDevVirtAddr)
{
	PVRSRV_ERROR eError;

	/* Do not try to map unpinned memory */
	if (psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_UNPINNED)
	{
		eError = PVRSRV_ERROR_INVALID_MAP_REQUEST;
		goto failCheck;
	}

	OSLockAcquire(psMemDesc->sDeviceMemDesc.hLock);
	DEVMEM_REFCOUNT_PRINT("%s (%p) %d->%d",
					__FUNCTION__,
					psMemDesc,
					psMemDesc->sDeviceMemDesc.ui32RefCount,
					psMemDesc->sDeviceMemDesc.ui32RefCount+1);

	if (psMemDesc->sDeviceMemDesc.ui32RefCount == 0)
	{
		eError = PVRSRV_ERROR_DEVICEMEM_NO_MAPPING;
		goto failRelease;
	}
	psMemDesc->sDeviceMemDesc.ui32RefCount++;

    *psDevVirtAddr = psMemDesc->sDeviceMemDesc.sDevVAddr;
	OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);

    return PVRSRV_OK;

failRelease:
	OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);
	PVR_ASSERT(eError != PVRSRV_OK);
failCheck:
	return eError;
}

IMG_INTERNAL void
DevmemReleaseDevVirtAddr(DEVMEM_MEMDESC *psMemDesc)
{
	PVR_ASSERT(psMemDesc != NULL);

	OSLockAcquire(psMemDesc->sDeviceMemDesc.hLock);
	DEVMEM_REFCOUNT_PRINT("%s (%p) %d->%d",
					__FUNCTION__,
					psMemDesc,
					psMemDesc->sDeviceMemDesc.ui32RefCount,
					psMemDesc->sDeviceMemDesc.ui32RefCount-1);

	PVR_ASSERT(psMemDesc->sDeviceMemDesc.ui32RefCount != 0);

	if (--psMemDesc->sDeviceMemDesc.ui32RefCount == 0)
	{
#if defined(SUPPORT_PAGE_FAULT_DEBUG)
		BridgeDevicememHistoryUnmap(psMemDesc->psImport->hDevConnection,
							psMemDesc->sDeviceMemDesc.sDevVAddr,
							psMemDesc->uiAllocSize,
							psMemDesc->sTraceData.szText);
#endif
		_DevmemImportStructDevUnmap(psMemDesc->psImport);
		OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);

		_DevmemMemDescRelease(psMemDesc);
	}
	else
	{
		OSLockRelease(psMemDesc->sDeviceMemDesc.hLock);
	}
}

IMG_INTERNAL PVRSRV_ERROR
DevmemAcquireCpuVirtAddr(DEVMEM_MEMDESC *psMemDesc,
                         void **ppvCpuVirtAddr)
{
	PVRSRV_ERROR eError;

	/* Do not try to map unpinned memory */
	if (psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_UNPINNED)
	{
		eError = PVRSRV_ERROR_INVALID_MAP_REQUEST;
		goto failFlags;
	}

	OSLockAcquire(psMemDesc->sCPUMemDesc.hLock);
	DEVMEM_REFCOUNT_PRINT("%s (%p) %d->%d",
					__FUNCTION__,
					psMemDesc,
					psMemDesc->sCPUMemDesc.ui32RefCount,
					psMemDesc->sCPUMemDesc.ui32RefCount+1);

	if (psMemDesc->sCPUMemDesc.ui32RefCount++ == 0)
	{
		DEVMEM_IMPORT *psImport = psMemDesc->psImport;
		IMG_UINT8 *pui8CPUVAddr;

		_DevmemMemDescAcquire(psMemDesc);
		eError = _DevmemImportStructCPUMap(psImport);
		if (eError != PVRSRV_OK)
		{
			goto failMap;
		}

		pui8CPUVAddr = psImport->sCPUImport.pvCPUVAddr;
		pui8CPUVAddr += psMemDesc->uiOffset;
		psMemDesc->sCPUMemDesc.pvCPUVAddr = pui8CPUVAddr;
	}
    *ppvCpuVirtAddr = psMemDesc->sCPUMemDesc.pvCPUVAddr;

    VG_MARK_INITIALIZED(*ppvCpuVirtAddr, psMemDesc->psImport->uiSize);

    OSLockRelease(psMemDesc->sCPUMemDesc.hLock);

    return PVRSRV_OK;

failMap:
	PVR_ASSERT(eError != PVRSRV_OK);
	psMemDesc->sCPUMemDesc.ui32RefCount--;
	_DevmemMemDescRelease(psMemDesc);
	OSLockRelease(psMemDesc->sCPUMemDesc.hLock);
failFlags:
	return eError;
}

IMG_INTERNAL void
DevmemReleaseCpuVirtAddr(DEVMEM_MEMDESC *psMemDesc)
{
	PVR_ASSERT(psMemDesc != NULL);

	OSLockAcquire(psMemDesc->sCPUMemDesc.hLock);
	DEVMEM_REFCOUNT_PRINT("%s (%p) %d->%d",
					__FUNCTION__,
					psMemDesc,
					psMemDesc->sCPUMemDesc.ui32RefCount,
					psMemDesc->sCPUMemDesc.ui32RefCount-1);

	PVR_ASSERT(psMemDesc->sCPUMemDesc.ui32RefCount != 0);

	if (--psMemDesc->sCPUMemDesc.ui32RefCount == 0)
	{
		OSLockRelease(psMemDesc->sCPUMemDesc.hLock);
		_DevmemImportStructCPUUnmap(psMemDesc->psImport);
		_DevmemMemDescRelease(psMemDesc);
	}
	else
	{
		OSLockRelease(psMemDesc->sCPUMemDesc.hLock);
	}
}

IMG_INTERNAL PVRSRV_ERROR
DevmemLocalGetImportHandle(DEVMEM_MEMDESC *psMemDesc,
			   IMG_HANDLE *phImport)
{
	if ((psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_EXPORTABLE) == 0)
	{
		return PVRSRV_ERROR_DEVICEMEM_CANT_EXPORT_SUBALLOCATION;
	}

	*phImport = psMemDesc->psImport->hPMR;

	return PVRSRV_OK;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemGetImportUID(DEVMEM_MEMDESC *psMemDesc,
						   IMG_UINT64 *pui64UID)
{
	DEVMEM_IMPORT *psImport = psMemDesc->psImport;
	PVRSRV_ERROR eError;

	eError = BridgePMRGetUID(psImport->hDevConnection,
							 psImport->hPMR,
							 pui64UID);

	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemGetReservation(DEVMEM_MEMDESC *psMemDesc,
				IMG_HANDLE *hReservation)
{
	DEVMEM_IMPORT *psImport;

	PVR_ASSERT(psMemDesc);
	psImport = psMemDesc->psImport;

	PVR_ASSERT(psImport);
	*hReservation = psImport->sDeviceImport.hReservation;

	return PVRSRV_OK;
}

PVRSRV_ERROR
DevmemGetPMRData(DEVMEM_MEMDESC *psMemDesc,
		IMG_HANDLE *phPMR,
		IMG_DEVMEM_OFFSET_T *puiPMROffset)
{
	DEVMEM_IMPORT *psImport;

	PVR_ASSERT(psMemDesc);
	*puiPMROffset = psMemDesc->uiOffset;
	psImport = psMemDesc->psImport;

	PVR_ASSERT(psImport);
	*phPMR = psImport->hPMR;

	return PVRSRV_OK;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemGetFlags(DEVMEM_MEMDESC *psMemDesc,
				DEVMEM_FLAGS_T *puiFlags)
{
	DEVMEM_IMPORT *psImport;

	PVR_ASSERT(psMemDesc);
	psImport = psMemDesc->psImport;

	PVR_ASSERT(psImport);
	*puiFlags = psImport->uiFlags;

	return PVRSRV_OK;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemLocalImport(IMG_HANDLE hBridge,
				  IMG_HANDLE hExtHandle,
				  DEVMEM_FLAGS_T uiFlags,
				  DEVMEM_MEMDESC **ppsMemDescPtr,
				  IMG_DEVMEM_SIZE_T *puiSizePtr)
{
    DEVMEM_MEMDESC *psMemDesc = NULL;
    DEVMEM_IMPORT *psImport;
    IMG_DEVMEM_SIZE_T uiSize;
    IMG_DEVMEM_ALIGN_T uiAlign;
    IMG_HANDLE hPMR;
    PVRSRV_ERROR eError;

    if (ppsMemDescPtr == NULL)
    {
        eError = PVRSRV_ERROR_INVALID_PARAMS;
        goto failParams;
    }	

	eError =_DevmemMemDescAlloc(&psMemDesc);
    if (eError != PVRSRV_OK)
    {
        goto failMemDescAlloc;
    }

	eError = _DevmemImportStructAlloc(hBridge,
									  &psImport);
    if (eError != PVRSRV_OK)
    {
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
        goto failImportAlloc;
    }

	/* Get the PMR handle and it's size from the server */
	eError = BridgePMRLocalImportPMR(hBridge,
									 hExtHandle,
									 &hPMR,
									 &uiSize,
									 &uiAlign);
	if (eError != PVRSRV_OK)
	{
		goto failImport;
	}

	_DevmemImportStructInit(psImport,
							uiSize,
							uiAlign,
							uiFlags,
							hPMR,
							DEVMEM_PROPERTIES_IMPORTED |
							DEVMEM_PROPERTIES_EXPORTABLE);

	_DevmemMemDescInit(psMemDesc,
					   0,
					   psImport,
					   uiSize);

    *ppsMemDescPtr = psMemDesc;
	if (puiSizePtr)
		*puiSizePtr = uiSize;

#if defined(PVR_RI_DEBUG)
	{
		/* Attach RI information */
		eError = BridgeRIWriteMEMDESCEntry (psMemDesc->psImport->hDevConnection,
											psMemDesc->psImport->hPMR,
											sizeof("^"),
											"^",
											psMemDesc->uiOffset,
											psMemDesc->psImport->uiSize,
											psMemDesc->psImport->uiSize,
											IMG_TRUE,
											IMG_FALSE,
											&(psMemDesc->hRIHandle));
		if( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: call to BridgeRIWriteMEMDESCEntry failed (eError=%d)", __func__, eError));
		}
	}
#endif /* if defined(PVR_RI_DEBUG) */
	return PVRSRV_OK;

failImport:
    _DevmemImportDiscard(psImport);
failImportAlloc:
	_DevmemMemDescDiscard(psMemDesc);
failMemDescAlloc:
failParams:
	PVR_ASSERT(eError != PVRSRV_OK);

	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
DevmemIsDevVirtAddrValid(DEVMEM_CONTEXT *psContext,
                         IMG_DEV_VIRTADDR sDevVAddr)
{
    return BridgeDevmemIsVDevAddrValid(psContext->hDevConnection,
                                       psContext->hDevMemServerContext,
                                       sDevVAddr);
}

IMG_INTERNAL IMG_UINT32
DevmemGetHeapLog2PageSize(DEVMEM_HEAP *psHeap)
{
	return psHeap->uiLog2Quantum;
}

IMG_INTERNAL IMG_UINT32
DevmemGetHeapLog2ImportAlignment(DEVMEM_HEAP *psHeap)
{
	return psHeap->uiLog2ImportAlignment;
}

