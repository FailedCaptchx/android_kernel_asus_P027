/*************************************************************************/ /*!
@File
@Title          Device Memory Management
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Server-side component of the Device Memory Management.
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
/* our exported API */
#include "devicemem_server.h"
#include "devicemem_utils.h"
#include "devicemem.h"

#include "device.h" /* For device node */
#include "img_types.h"
#include "pvr_debug.h"
#include "pvrsrv_error.h"

#include "mmu_common.h"
#include "pdump_km.h"
#include "pmr.h"
#include "physmem.h"

#include "allocmem.h"
#include "osfunc.h"
#include "lock.h"

#if defined(SUPPORT_BUFFER_SYNC)
#include <linux/sched.h>
#include "pvr_buffer_sync.h"
#endif

struct _DEVMEMINT_CTX_
{
    PVRSRV_DEVICE_NODE *psDevNode;

    /* MMU common code needs to have a context.  There's a one-to-one
       correspondence between device memory context and MMU context,
       but we have the abstraction here so that we don't need to care
       what the MMU does with its context, and the MMU code need not
       know about us at all. */
    MMU_CONTEXT *psMMUContext;

    ATOMIC_T hRefCount;

    /* This handle is for devices that require notification when a new
       memory context is created and they need to store private data that
       is associated with the context. */
    IMG_HANDLE hPrivData;
};

struct _DEVMEMINT_CTX_EXPORT_ 
{
	DEVMEMINT_CTX *psDevmemCtx;
};

struct _DEVMEMINT_HEAP_
{
    struct _DEVMEMINT_CTX_ *psDevmemCtx;
    IMG_UINT32 uiLog2PageSize;
    ATOMIC_T hRefCount;
};

struct _DEVMEMINT_RESERVATION_
{
    struct _DEVMEMINT_HEAP_ *psDevmemHeap;
    IMG_DEV_VIRTADDR sBase;
    IMG_DEVMEM_SIZE_T uiLength;
};

struct _DEVMEMINT_MAPPING_
{
    struct _DEVMEMINT_RESERVATION_ *psReservation;
    PMR *psPMR;
    IMG_UINT32 uiNumPages;
};

/*************************************************************************/ /*!
@Function       _DevmemIntCtxAcquire
@Description    Acquire a reference to the provided device memory context.
@Return         None
*/ /**************************************************************************/
static INLINE void _DevmemIntCtxAcquire(DEVMEMINT_CTX *psDevmemCtx)
{
	OSAtomicIncrement(&psDevmemCtx->hRefCount);
}

/*************************************************************************/ /*!
@Function       _DevmemIntCtxRelease
@Description    Release the reference to the provided device memory context.
                If this is the last reference which was taken then the
                memory context will be freed.
@Return         None
*/ /**************************************************************************/
static INLINE void _DevmemIntCtxRelease(DEVMEMINT_CTX *psDevmemCtx)
{
	if (OSAtomicDecrement(&psDevmemCtx->hRefCount) == 0)
	{
		/* The last reference has gone, destroy the context */
		PVRSRV_DEVICE_NODE *psDevNode = psDevmemCtx->psDevNode;
	
		if (psDevNode->pfnUnregisterMemoryContext)
		{
			psDevNode->pfnUnregisterMemoryContext(psDevmemCtx->hPrivData);
		}
	    MMU_ContextDestroy(psDevmemCtx->psMMUContext);
	
		PVR_DPF((PVR_DBG_MESSAGE, "%s: Freed memory context %p", __FUNCTION__, psDevmemCtx));
		OSFreeMem(psDevmemCtx);
	}
}

/*************************************************************************/ /*!
@Function       _DevmemIntHeapAcquire
@Description    Acquire a reference to the provided device memory heap.
@Return         None
*/ /**************************************************************************/
static INLINE void _DevmemIntHeapAcquire(DEVMEMINT_HEAP *psDevmemHeap)
{
	OSAtomicIncrement(&psDevmemHeap->hRefCount);
}

/*************************************************************************/ /*!
@Function       _DevmemIntHeapRelease
@Description    Release the reference to the provided device memory heap.
                If this is the last reference which was taken then the
                memory context will be freed.
@Return         None
*/ /**************************************************************************/
static INLINE void _DevmemIntHeapRelease(DEVMEMINT_HEAP *psDevmemHeap)
{
	OSAtomicDecrement(&psDevmemHeap->hRefCount);
}

PVRSRV_ERROR
DevmemIntUnpin(PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	/* Unpin */
	eError = PMRUnpinPMR(psPMR, IMG_FALSE);

	return eError;
}

PVRSRV_ERROR
DevmemIntUnpinInvalidate(DEVMEMINT_MAPPING *psDevmemMapping, PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	eError = PMRUnpinPMR(psPMR, IMG_TRUE);
	if (eError != PVRSRV_OK)
	{
		goto e_exit;
	}

	/* Invalidate mapping */
	eError = MMU_ChangeValidity(psDevmemMapping->psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
	                            psDevmemMapping->psReservation->sBase,
	                            psDevmemMapping->uiNumPages,
	                            psDevmemMapping->psReservation->psDevmemHeap->uiLog2PageSize,
	                            IMG_FALSE, /* !< Choose to invalidate PT entries */
	                            psPMR);

e_exit:
	return eError;
}

PVRSRV_ERROR
DevmemIntPin(PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	/* Start the pinning */
	eError = PMRPinPMR(psPMR);

	return eError;
}

PVRSRV_ERROR
DevmemIntPinValidate(DEVMEMINT_MAPPING *psDevmemMapping, PMR *psPMR)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_ERROR eErrorMMU = PVRSRV_OK;
	IMG_UINT32 uiLog2PageSize = psDevmemMapping->psReservation->psDevmemHeap->uiLog2PageSize;

	/* Start the pinning */
	eError = PMRPinPMR(psPMR);

	if (eError == PVRSRV_OK)
	{
		/* Make mapping valid again */
		eErrorMMU = MMU_ChangeValidity(psDevmemMapping->psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
		                            psDevmemMapping->psReservation->sBase,
		                            psDevmemMapping->uiNumPages,
		                            uiLog2PageSize,
		                            IMG_TRUE, /* !< Choose to make PT entries valid again */
		                            psPMR);
	}
	else if (eError == PVRSRV_ERROR_PMR_NEW_MEMORY)
	{
		/* If we lost the physical baking we have to map it again because
		 * the old physical addresses are not valid anymore. */
		IMG_UINT32 uiFlags;
		uiFlags = PMR_Flags(psPMR);

        eErrorMMU = MMU_MapPages(psDevmemMapping->psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
                                 uiFlags,
                                 psDevmemMapping->psReservation->sBase,
                                 psPMR,
                                 0,
                                 psDevmemMapping->uiNumPages,
                                 NULL,
                                 uiLog2PageSize);
	}

	/* Just overwrite eError if the mappings failed.
	 * PMR_NEW_MEMORY has to be propagated to the user. */
	if (eErrorMMU != PVRSRV_OK)
	{
		eError = eErrorMMU;
	}

	return eError;
}

/*************************************************************************/ /*!
@Function       DevmemServerGetImportHandle
@Description    For given exportable memory descriptor returns PMR handle.
@Return         Memory is exportable - Success
                PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemServerGetImportHandle(DEVMEM_MEMDESC *psMemDesc,
						   IMG_HANDLE *phImport)
{
	PVRSRV_ERROR eError;

	if ((psMemDesc->psImport->uiProperties & DEVMEM_PROPERTIES_EXPORTABLE) == 0)
	{
        eError = PVRSRV_ERROR_DEVICEMEM_CANT_EXPORT_SUBALLOCATION;
        goto e0;
	}

	*phImport = psMemDesc->psImport->hPMR;
	return PVRSRV_OK;

e0:
	return eError;
}

/*************************************************************************/ /*!
@Function       DevmemServerGetHeapHandle
@Description    For given reservation returns the Heap handle.
@Return         PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemServerGetHeapHandle(DEVMEMINT_RESERVATION *psReservation,
						   IMG_HANDLE *phHeap)
{
	*phHeap = psReservation->psDevmemHeap;
	return PVRSRV_OK;
}



/*************************************************************************/ /*!
@Function       DevmemIntCtxCreate
@Description    Creates and initialises a device memory context.
@Return         valid Device Memory context handle - Success
                PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntCtxCreate(CONNECTION_DATA *psConnection,
                   PVRSRV_DEVICE_NODE *psDeviceNode,
                   IMG_BOOL bKernelMemoryCtx,
                   DEVMEMINT_CTX **ppsDevmemCtxPtr,
                   IMG_HANDLE *hPrivData
                   )
{
	PVRSRV_ERROR eError;
	DEVMEMINT_CTX *psDevmemCtx;
	IMG_HANDLE hPrivDataInt = NULL;
	MMU_DEVICEATTRIBS      *psMMUDevAttrs;

#if defined(RGX_FEATURE_META)
	psMMUDevAttrs = psDeviceNode->psMMUDevAttrs;
	PVR_UNREFERENCED_PARAMETER(bKernelMemoryCtx);
#else
	psMMUDevAttrs = bKernelMemoryCtx ? psDeviceNode->psFirmwareMMUDevAttrs:
									   psDeviceNode->psMMUDevAttrs;
#endif


	PVR_DPF((PVR_DBG_MESSAGE, "%s", __FUNCTION__));
	PVR_UNREFERENCED_PARAMETER(psConnection);

	/* allocate a Devmem context */
	psDevmemCtx = OSAllocMem(sizeof *psDevmemCtx);
	if (psDevmemCtx == NULL)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		PVR_DPF ((PVR_DBG_ERROR, "%s: Alloc failed", __FUNCTION__));
        	goto fail_alloc;
	}

	OSAtomicWrite(&psDevmemCtx->hRefCount, 1);
   	psDevmemCtx->psDevNode = psDeviceNode;

	/* Call down to MMU context creation */

   	eError = MMU_ContextCreate(psDeviceNode,
                                   &psDevmemCtx->psMMUContext,
                                   psMMUDevAttrs);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: MMU_ContextCreate failed", __FUNCTION__));
		goto fail_mmucontext;
	}


	if (psDeviceNode->pfnRegisterMemoryContext)
	{
		eError = psDeviceNode->pfnRegisterMemoryContext(psDeviceNode, psDevmemCtx->psMMUContext, &hPrivDataInt);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Failed to register MMU context", __FUNCTION__));
			goto fail_register;
		}
	}

	/* Store the private data as it is required to unregister the memory context */
	psDevmemCtx->hPrivData = hPrivDataInt;
	*hPrivData = hPrivDataInt;
	*ppsDevmemCtxPtr = psDevmemCtx;

	return PVRSRV_OK;

fail_register:
    MMU_ContextDestroy(psDevmemCtx->psMMUContext);
fail_mmucontext:
	OSFREEMEM(psDevmemCtx);
fail_alloc:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

/*************************************************************************/ /*!
@Function       DevmemIntHeapCreate
@Description    Creates and initialises a device memory heap.
@Return         valid Device Memory heap handle - Success
                PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntHeapCreate(
                    DEVMEMINT_CTX *psDevmemCtx,
                    IMG_DEV_VIRTADDR sHeapBaseAddr,
                    IMG_DEVMEM_SIZE_T uiHeapLength,
                    IMG_UINT32 uiLog2DataPageSize,
                    DEVMEMINT_HEAP **ppsDevmemHeapPtr
                    )
{
    PVRSRV_ERROR eError;
    DEVMEMINT_HEAP *psDevmemHeap;

	PVR_DPF((PVR_DBG_MESSAGE, "%s: DevmemIntHeap_Create", __FUNCTION__));

	/* allocate a Devmem context */
	psDevmemHeap = OSAllocMem(sizeof *psDevmemHeap);
    if (psDevmemHeap == NULL)
	{
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		PVR_DPF ((PVR_DBG_ERROR, "%s: Alloc failed", __FUNCTION__));
        goto fail_alloc;
	}

    psDevmemHeap->psDevmemCtx = psDevmemCtx;

	_DevmemIntCtxAcquire(psDevmemHeap->psDevmemCtx);

	OSAtomicWrite(&psDevmemHeap->hRefCount, 1);

	psDevmemHeap->uiLog2PageSize = uiLog2DataPageSize;

    *ppsDevmemHeapPtr = psDevmemHeap;

	return PVRSRV_OK;

fail_alloc:
    return eError;
}

static PVRSRV_ERROR DevmemIntAllocDummyPage(DEVMEMINT_HEAP *psDevmemHeap)
{

	IMG_UINT32 ui32Dummyref = 0;
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_DEVICE_NODE *psDevNode;

	psDevNode = psDevmemHeap->psDevmemCtx->psDevNode;

	/* We know there will not be 4G number of sparse PMR's */
	/* Also this function depends on the fact that its called under the global lock &
	 * pmr lock and thus is safe from being re-entrant */
	ui32Dummyref = OSAtomicIncrement(&psDevNode->sDummyPage.atRefCounter);
	if(1 == ui32Dummyref)
	{
		IMG_UINT8 u8Value = 0;
		IMG_BOOL bInitPage = IMG_FALSE;
		IMG_DEV_PHYADDR	sDevPhysAddr={0};

		/*Acquire the lock */
		OSLockAcquire(psDevNode->sDummyPage.psDummyPgLock);

#if defined(PVR_DUMMY_PAGE_INIT_VALUE)
		u8Value = PVR_DUMMY_PAGE_INIT_VALUE;
		bInitPage = IMG_TRUE;
#else
		bInitPage = IMG_FALSE;
#endif

#if defined(PDUMP)
		PDUMPCOMMENT("Alloc Dummy page object");
#endif
		/*Allocate the dummy page required for sparse backing */
		eError = DevPhysMemAlloc(psDevNode,
				(1 << psDevNode->sDummyPage.ui32Log2DummyPgSize),
				u8Value,
				bInitPage,
#if	defined(PDUMP)
				psDevNode->pszMMUPxPDumpMemSpaceName,
				DUMMY_PAGE,
				&psDevNode->sDummyPage.hPdumpDummyPg,
#endif
				&psDevNode->sDummyPage.sDummyPageHandle,
				&sDevPhysAddr);
		if(PVRSRV_OK != eError)
		{
			OSAtomicDecrement(&psDevNode->sDummyPage.atRefCounter);
		}
		else
		{
			psDevNode->sDummyPage.ui64DummyPgPhysAddr = sDevPhysAddr.uiAddr;
		}

		/*Release the lock */
		OSLockRelease(psDevNode->sDummyPage.psDummyPgLock);
	}
	return eError;
}

static void DevmemIntFreeDummyPage(DEVMEMINT_HEAP *psDevmemHeap)
{
	PVRSRV_DEVICE_NODE *psDevNode;
	IMG_UINT32 ui32Dummyref = 0;

	psDevNode = psDevmemHeap->psDevmemCtx->psDevNode;
	ui32Dummyref = OSAtomicRead(&psDevNode->sDummyPage.atRefCounter);

	/* For the cases where the dummy page allocation fails due to lack of memory
	 * The refcount can still be 0 even for a sparse allocation */
	if(0 != ui32Dummyref)
	{
		OSLockAcquire(psDevNode->sDummyPage.psDummyPgLock);

		/* We know there will not be 4G number of sparse PMR's */
		ui32Dummyref = OSAtomicDecrement(&psDevNode->sDummyPage.atRefCounter);

		if(0 == ui32Dummyref)
		{
			PDUMPCOMMENT("Free Dummy page object");

			/* Free the dummy page when refcount reaches zero */
			DevPhysMemFree(psDevNode,
#if defined(PDUMP)
					psDevNode->sDummyPage.hPdumpDummyPg,
#endif
					&psDevNode->sDummyPage.sDummyPageHandle);

#if	defined(PDUMP)
			psDevNode->sDummyPage.hPdumpDummyPg = NULL;
#endif
			psDevNode->sDummyPage.ui64DummyPgPhysAddr = MMU_BAD_PHYS_ADDR;
		}

		OSLockRelease(psDevNode->sDummyPage.psDummyPgLock);
	}

}

PVRSRV_ERROR
DevmemIntMapPages(DEVMEMINT_RESERVATION *psReservation,
                  PMR *psPMR,
                  IMG_UINT32 ui32PageCount,
                  IMG_UINT32 ui32PhysicalPgOffset,
                  PVRSRV_MEMALLOCFLAGS_T uiFlags,
                  IMG_DEV_VIRTADDR sDevVAddrBase)
{
	PVRSRV_ERROR eError;

	eError = MMU_MapPages(psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
	                      uiFlags,
	                      sDevVAddrBase,
	                      psPMR,
	                      ui32PhysicalPgOffset,
	                      ui32PageCount,
	                      NULL,
	                      psReservation->psDevmemHeap->uiLog2PageSize);

	return eError;
}

PVRSRV_ERROR
DevmemIntUnmapPages(DEVMEMINT_RESERVATION *psReservation,
                    IMG_DEV_VIRTADDR sDevVAddrBase,
                    IMG_UINT32 ui32PageCount)
{
	/*Unmap the pages and mark them invalid in the MMU PTE */
	MMU_UnmapPages (psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
					0,
	                sDevVAddrBase,
	                ui32PageCount,
	                NULL,
	                psReservation->psDevmemHeap->uiLog2PageSize,
	                IMG_FALSE);

	return PVRSRV_OK;
}

PVRSRV_ERROR
DevmemIntMapPMR(DEVMEMINT_HEAP *psDevmemHeap,
                DEVMEMINT_RESERVATION *psReservation,
                PMR *psPMR,
                PVRSRV_MEMALLOCFLAGS_T uiMapFlags,
                DEVMEMINT_MAPPING **ppsMappingPtr)
{
    PVRSRV_ERROR eError;
    DEVMEMINT_MAPPING *psMapping;
    /* number of pages (device pages) that allocation spans */
    IMG_UINT32 ui32NumDevPages;
    /* device virtual address of start of allocation */
    IMG_DEV_VIRTADDR sAllocationDevVAddr;
    /* and its length */
    IMG_DEVMEM_SIZE_T uiAllocationSize;
    IMG_UINT32 uiLog2Contiguity = psDevmemHeap->uiLog2PageSize;
    IMG_BOOL bIsSparse = IMG_FALSE, bNeedBacking = IMG_FALSE;
	PVRSRV_DEVICE_NODE *psDevNode;
	 PMR_FLAGS_T uiPMRFlags;

	psDevNode = psDevmemHeap->psDevmemCtx->psDevNode;

	/* allocate memory to record the mapping info */
	psMapping = OSAllocMem(sizeof *psMapping);
    if (psMapping == NULL)
	{
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		PVR_DPF ((PVR_DBG_ERROR, "DevmemIntMapPMR: Alloc failed"));
        goto e0;
	}

    uiAllocationSize = psReservation->uiLength;


    ui32NumDevPages = 0xffffffffU & ( ( (uiAllocationSize - 1) >> uiLog2Contiguity) + 1);
    PVR_ASSERT(ui32NumDevPages << uiLog2Contiguity == uiAllocationSize);

    eError = PMRLockSysPhysAddresses(psPMR,
                                     uiLog2Contiguity);
    if (eError != PVRSRV_OK)
	{
        goto e2;
	}

    sAllocationDevVAddr = psReservation->sBase;

    /*Check if the PMR that need to be mapped is sparse */
	bIsSparse = PMR_IsSparse(psPMR);
	if(bIsSparse)
	{
		/*Get the flags*/
		uiPMRFlags = PMR_Flags(psPMR);
		bNeedBacking = PVRSRV_IS_SPARSE_DUMMY_BACKING_REQUIRED(uiPMRFlags);

		if(bNeedBacking)
		{
			/*Error is logged with in the function if any failures.
			 * As the allocation fails we need to fail the map request and
			 * return appropriate error
			 *
			 * Wondering if we do dummy allocation first and then physically lock pages later.
			 * But on further thought failure of dummy allocation has lesser chance than
			 * failing of physically locking down of the pages.
			 * Hence we unlock the locked pages if dummy allocation fails. yes this is 
			 * time consuming but very unlikely to occur */
			eError = DevmemIntAllocDummyPage(psDevmemHeap);
			if(PVRSRV_OK != eError)
			{
			  	goto e3;
			}
		}

        /*  N.B.  We pass mapping permission flags to MMU_MapPages and let
           it reject the mapping if the permissions on the PMR are not compatible. */
        eError = MMU_MapPages(psDevmemHeap->psDevmemCtx->psMMUContext,
                              uiMapFlags,
                              sAllocationDevVAddr,
                              psPMR,
                              0,
                              ui32NumDevPages,
                              NULL,
                              uiLog2Contiguity);
        if(PVRSRV_OK != eError)
        {
            goto e4;
        }
    }
    else
    {
        eError = MMU_MapPMRFast(psDevmemHeap->psDevmemCtx->psMMUContext,
                             sAllocationDevVAddr,
                             psPMR,
                             ui32NumDevPages << uiLog2Contiguity,
                             uiMapFlags,
                             uiLog2Contiguity);
        if(PVRSRV_OK != eError)
        {
            goto e3;
        }
    }

    psMapping->psReservation = psReservation;
    psMapping->uiNumPages = ui32NumDevPages;
    psMapping->psPMR = psPMR;
    /* Don't bother with refcount on reservation, as a reservation
       only ever holds one mapping, so we directly increment the
       refcount on the heap instead */
    _DevmemIntHeapAcquire(psMapping->psReservation->psDevmemHeap);

    *ppsMappingPtr = psMapping;

    return PVRSRV_OK;
 e4:
 	 if(bNeedBacking)
 	 {
	 	/*if the mapping failed, the allocated dummy ref count need
	 	 * to be handled accordingly */
		 DevmemIntFreeDummyPage(psDevmemHeap);
	 }
 e3:
 	 {
		 PVRSRV_ERROR eError1=PVRSRV_OK;
		 eError1 = PMRUnlockSysPhysAddresses(psPMR);
		 if(PVRSRV_OK != eError1)
		 {
			PVR_DPF ((PVR_DBG_ERROR, "%s: Failed to unlock the physical addresses",__func__));
		 }
	  	*ppsMappingPtr = NULL;
 	 }
 e2:
	OSFreeMem(psMapping);

 e0:
    PVR_ASSERT (eError != PVRSRV_OK);
    return eError;
}


PVRSRV_ERROR
DevmemIntUnmapPMR(DEVMEMINT_MAPPING *psMapping)
{
    PVRSRV_ERROR eError;
    DEVMEMINT_HEAP *psDevmemHeap;
    /* device virtual address of start of allocation */
    IMG_DEV_VIRTADDR sAllocationDevVAddr;
    /* number of pages (device pages) that allocation spans */
    IMG_UINT32 ui32NumDevPages;
    IMG_BOOL bIsSparse = IMG_FALSE, bNeedBacking = IMG_FALSE;
	PVRSRV_DEVICE_NODE *psDevNode;
	PMR_FLAGS_T uiPMRFlags;
#if defined(SUPPORT_BUFFER_SYNC)
	bool bInterruptible = true;
	unsigned long ulTimeout = MAX_SCHEDULE_TIMEOUT;
	IMG_INT iErr;

retry:
	iErr = pvr_buffer_sync_wait(psMapping->psPMR, bInterruptible, ulTimeout);
	if (iErr)
	{
		if (iErr == -ERESTARTSYS)
		{
			PVR_DPF((PVR_DBG_MESSAGE, "%s: Buffer sync wait interrupted (retrying)",
					 __FUNCTION__));
			bInterruptible = false;
			ulTimeout = 30 * HZ;
			goto retry;
		}

		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to unmap PMR from device (errno=%d)",
				 __FUNCTION__, iErr));
		return PVRSRV_ERROR_STILL_MAPPED;
	}
#endif

    psDevmemHeap = psMapping->psReservation->psDevmemHeap;
	psDevNode = psDevmemHeap->psDevmemCtx->psDevNode;

    ui32NumDevPages = psMapping->uiNumPages;
    sAllocationDevVAddr = psMapping->psReservation->sBase;

    /*Check if the PMR that need to be mapped is sparse */
  	bIsSparse = PMR_IsSparse(psMapping->psPMR);

  	if(bIsSparse)
  	{
		/*Get the flags*/
  		uiPMRFlags = PMR_Flags(psMapping->psPMR);
		bNeedBacking = PVRSRV_IS_SPARSE_DUMMY_BACKING_REQUIRED(uiPMRFlags);

		if(bNeedBacking)
		{
			DevmemIntFreeDummyPage(psDevmemHeap);
		}

		MMU_UnmapPages (psDevmemHeap->psDevmemCtx->psMMUContext,
						0,
						sAllocationDevVAddr,
						ui32NumDevPages,
						NULL,
						psMapping->psReservation->psDevmemHeap->uiLog2PageSize,
						IMG_FALSE);
  	}
    else
    {
        MMU_UnmapPMRFast(psDevmemHeap->psDevmemCtx->psMMUContext,
                        sAllocationDevVAddr,
                        ui32NumDevPages,
                        psMapping->psReservation->psDevmemHeap->uiLog2PageSize);
    }



    eError = PMRUnlockSysPhysAddresses(psMapping->psPMR);
    PVR_ASSERT(eError == PVRSRV_OK);

    /* Don't bother with refcount on reservation, as a reservation
       only ever holds one mapping, so we directly decrement the
       refcount on the heap instead */
    _DevmemIntHeapRelease(psDevmemHeap);

	OSFreeMem(psMapping);

    return PVRSRV_OK;
}


PVRSRV_ERROR
DevmemIntReserveRange(DEVMEMINT_HEAP *psDevmemHeap,
                      IMG_DEV_VIRTADDR sAllocationDevVAddr,
                      IMG_DEVMEM_SIZE_T uiAllocationSize,
                      DEVMEMINT_RESERVATION **ppsReservationPtr)
{
    PVRSRV_ERROR eError;
    DEVMEMINT_RESERVATION *psReservation;

	/* allocate memory to record the reservation info */
	psReservation = OSAllocMem(sizeof *psReservation);
    if (psReservation == NULL)
	{
        eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		PVR_DPF ((PVR_DBG_ERROR, "DevmemIntReserveRange: Alloc failed"));
        goto e0;
	}

    psReservation->sBase = sAllocationDevVAddr;
    psReservation->uiLength = uiAllocationSize;

    eError = MMU_Alloc (psDevmemHeap->psDevmemCtx->psMMUContext,
                        uiAllocationSize,
                        &uiAllocationSize,
                        0, /* IMG_UINT32 uiProtFlags */
                        0, /* alignment is n/a since we supply devvaddr */
                        &sAllocationDevVAddr,
                        psDevmemHeap->uiLog2PageSize);
    if (eError != PVRSRV_OK)
    {
        goto e1;
    }

    /* since we supplied the virt addr, MMU_Alloc shouldn't have
       chosen a new one for us */
    PVR_ASSERT(sAllocationDevVAddr.uiAddr == psReservation->sBase.uiAddr);

	_DevmemIntHeapAcquire(psDevmemHeap);

    psReservation->psDevmemHeap = psDevmemHeap;
    *ppsReservationPtr = psReservation;

    return PVRSRV_OK;

    /*
      error exit paths follow
    */

 e1:
	OSFreeMem(psReservation);

 e0:
    PVR_ASSERT(eError != PVRSRV_OK);
    return eError;
}

PVRSRV_ERROR
DevmemIntUnreserveRange(DEVMEMINT_RESERVATION *psReservation)
{
	IMG_DEV_VIRTADDR sBase        = psReservation->sBase;
	IMG_UINT32 uiLength           = psReservation->uiLength;
	IMG_UINT32 uiLog2DataPageSize = psReservation->psDevmemHeap->uiLog2PageSize;

    MMU_Free (psReservation->psDevmemHeap->psDevmemCtx->psMMUContext,
              sBase,
              uiLength,
              uiLog2DataPageSize);

	_DevmemIntHeapRelease(psReservation->psDevmemHeap);
	OSFreeMem(psReservation);

    return PVRSRV_OK;
}


PVRSRV_ERROR
DevmemIntHeapDestroy(
                     DEVMEMINT_HEAP *psDevmemHeap
                     )
{
    if (OSAtomicRead(&psDevmemHeap->hRefCount) != 1)
    {
        PVR_DPF((PVR_DBG_ERROR, "BUG!  %s called but has too many references (%d) "
                 "which probably means allocations have been made from the heap and not freed",
                 __FUNCTION__,
                 OSAtomicRead(&psDevmemHeap->hRefCount)));

        /*
	 * Try again later when you've freed all the memory
	 *
	 * Note:
	 * We don't expect the application to retry (after all this call would
	 * succeed if the client had freed all the memory which it should have
	 * done before calling this function). However, given there should be
	 * an associated handle, when the handle base is destroyed it will free
	 * any allocations leaked by the client and then it will retry this call,
	 * which should then succeed.
	 */
        return PVRSRV_ERROR_RETRY;
    }

    PVR_ASSERT(OSAtomicRead(&psDevmemHeap->hRefCount) == 1);

	_DevmemIntCtxRelease(psDevmemHeap->psDevmemCtx);

	PVR_DPF((PVR_DBG_MESSAGE, "%s: Freed heap %p", __FUNCTION__, psDevmemHeap));
	OSFreeMem(psDevmemHeap);

	return PVRSRV_OK;
}

PVRSRV_ERROR
DeviceMemChangeSparseServer(DEVMEMINT_HEAP *psDevmemHeap,
					PMR *psPMR,
					IMG_UINT32 ui32AllocPageCount,
					IMG_UINT32 *pai32AllocIndices,
					IMG_UINT32 ui32FreePageCount,
					IMG_UINT32 *pai32FreeIndices,
					SPARSE_MEM_RESIZE_FLAGS uiSparseFlags,
					PVRSRV_MEMALLOCFLAGS_T uiFlags,
					IMG_DEV_VIRTADDR sDevVAddrBase,
					IMG_UINT64 sCpuVAddrBase,
					IMG_UINT32 *pui32Status)
{
	PVRSRV_ERROR eError = ~PVRSRV_OK;

	IMG_UINT32 uiLog2PageSize = GET_LOG2_PAGESIZE(),uiPageSize=0;

	uiPageSize = (1 << uiLog2PageSize);
	/*
	 * The order of steps in which this request is done is given below. The order of
	 * operations is very important in this case
	 * 1. The parameters are validated in function PMR_ChangeSparseMem below.
	 * 	   A successful response indicates all the parameters are correct.
	 * 	   In failure case we bail out from here with out processing further.
	 * 2. On success, get the PMR specific operations done. this includes page alloc, page free
	 *    and the corresponding PMR status changes.
	 *    when this call fails, it is ensured that the state of the PMR before is
	 *    not disturbed. If it succeeds, then we can go ahead with the subsequent steps.
	 * 3. Dewire the GPU page table entries for the pages to be freed.
	 * 4. Write the GPU page table entries for the pages that got allocated.
	 * 5. Change the corresponding CPU space map.
	 *
	 * The above steps can be selectively controlled using flags.
	 */
	*pui32Status = PVRSRV_OK;

	{
		if(uiSparseFlags & (SPARSE_REMAP_MEM | SPARSE_RESIZE_BOTH))
		{
			/*
			 * Do the PMR specific changes first
			 */
			eError = PMR_ChangeSparseMem(psPMR,
					ui32AllocPageCount,
					pai32AllocIndices,
					ui32FreePageCount,
					pai32FreeIndices,
					uiSparseFlags,
					pui32Status);
			if(PVRSRV_OK != eError)
			{
				PVR_DPF((PVR_DBG_MESSAGE,"%s: Failed to do PMR specific changes......",__func__));
				goto SparseChangeError;
			}

			/*
			 * Dewire the page table entries for the free pages
			 * Optimization later would be not to touch the ones that gets re-mapped
			 */
			if((0 != ui32FreePageCount) && (uiSparseFlags & SPARSE_RESIZE_FREE))
			{
				PMR_FLAGS_T uiPMRFlags;
				IMG_BOOL bNeedBacking = IMG_FALSE;
				/*Get the flags*/
				uiPMRFlags = PMR_Flags(psPMR);
				bNeedBacking = PVRSRV_IS_SPARSE_DUMMY_BACKING_REQUIRED(uiPMRFlags);

				if(SPARSE_REMAP_MEM != (uiSparseFlags & SPARSE_REMAP_MEM))
				{
					/*Unmap the pages and mark them invalid in the MMU PTE */
					MMU_UnmapPages (psDevmemHeap->psDevmemCtx->psMMUContext,
							uiFlags,
							sDevVAddrBase,
							ui32FreePageCount,
							pai32FreeIndices,
							uiLog2PageSize,
							bNeedBacking);
				}
			}

			/*
			 *  Wire the pages tables that got allocated
			 */
			if((0 != ui32AllocPageCount) && (uiSparseFlags & SPARSE_RESIZE_ALLOC))
			{
				/*Map the pages and mark them Valid in the MMU PTE */
				eError = MMU_MapPages (psDevmemHeap->psDevmemCtx->psMMUContext,
						uiFlags,
						sDevVAddrBase,
						psPMR,
						0,
						ui32AllocPageCount,
						pai32AllocIndices,
						uiLog2PageSize);

				if(PVRSRV_OK != eError)
				{
					PVR_DPF((PVR_DBG_MESSAGE,"%s: Failed to map alloc indices......",__func__));
					goto SparseChangeError;
				}
			}
			
			/*Should this be a debug feature or ever used in real scenario */
			if(SPARSE_REMAP_MEM == (uiSparseFlags & SPARSE_REMAP_MEM))
			{
				eError = MMU_MapPages (psDevmemHeap->psDevmemCtx->psMMUContext,
						uiFlags,
						sDevVAddrBase,
						psPMR,
						0,
						ui32AllocPageCount,
						pai32FreeIndices,
						uiLog2PageSize);
				if(PVRSRV_OK != eError)
				{
					PVR_DPF((PVR_DBG_MESSAGE,"%s: Failed to map Free indices......",__func__));
					goto SparseChangeError;
				}
			}
		}

	}
#ifndef PVRSRV_UNMAP_ON_SPARSE_CHANGE
	/*
	 * Do the changes in sparse on to the CPU virtual map accordingly
	 */

	if(uiSparseFlags & SPARSE_MAP_CPU_ADDR)
	{
		if(sCpuVAddrBase != 0)
		{
			eError = PMR_ChangeSparseMemCPUMap(psPMR,
												sCpuVAddrBase,
												ui32AllocPageCount,
												pai32AllocIndices,
												ui32FreePageCount,
												pai32FreeIndices,
												pui32Status);
			if(PVRSRV_OK != eError)
			{
				PVR_DPF((PVR_DBG_MESSAGE,"%s: Failed to map to CPU addr space......",__func__));
				goto SparseChangeError;
			}
		}
	}
#endif
	SparseChangeError:
	return eError;	
}

/*************************************************************************/ /*!
@Function       DevmemIntCtxDestroy
@Description    Destroy that created by DevmemIntCtxCreate
@Input          psDevmemCtx   Device Memory context
@Return         cannot fail.
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntCtxDestroy(
                    DEVMEMINT_CTX *psDevmemCtx
                    )
{
	/*
		We can't determine if we should be freeing the context here
		as it refcount!=1 could be due to either the fact that heap(s)
		remain with allocations on them, or that this memory context
		has been exported.
		As the client couldn???t do anything useful with this information
		anyway and the fact that the refcount will ensure we only
		free the context when _all_ references have been released
		don't bother checking and just return OK regardless.
	*/
	_DevmemIntCtxRelease(psDevmemCtx);
	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       DevmemIntCtxExport
@Description    Exports a device memory context.
@Return         valid Device Memory context handle - Success
                PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntCtxExport(DEVMEMINT_CTX *psDevmemCtx,
                   DEVMEMINT_CTX_EXPORT **ppsExport)
{
	DEVMEMINT_CTX_EXPORT *psExport;

	psExport = OSAllocMem(sizeof(*psExport));
	if (psExport == NULL)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}
	_DevmemIntCtxAcquire(psDevmemCtx);
	psExport->psDevmemCtx = psDevmemCtx;
	
	*ppsExport = psExport;
	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       DevmemIntCtxUnexport
@Description    Unexport an exported a device memory context.
@Return         None
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntCtxUnexport(DEVMEMINT_CTX_EXPORT *psExport)
{
	_DevmemIntCtxRelease(psExport->psDevmemCtx);
	OSFreeMem(psExport);
	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       DevmemIntCtxImport
@Description    Import an exported a device memory context.
@Return         valid Device Memory context handle - Success
                PVRSRV_ERROR failure code
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemIntCtxImport(DEVMEMINT_CTX_EXPORT *psExport,
				   DEVMEMINT_CTX **ppsDevmemCtxPtr,
				   IMG_HANDLE *hPrivData)
{
	DEVMEMINT_CTX *psDevmemCtx = psExport->psDevmemCtx;

	_DevmemIntCtxAcquire(psDevmemCtx);

	*ppsDevmemCtxPtr = psDevmemCtx;
	*hPrivData = psDevmemCtx->hPrivData;

	return PVRSRV_OK;
}

/*************************************************************************/ /*!
@Function       DevmemSLCFlushInvalRequest
@Description    Requests a SLC Flush and Invalidate
@Input          psDeviceNode    Device node
@Input          psPmr           PMR
@Return         PVRSRV_OK
*/ /**************************************************************************/
PVRSRV_ERROR
DevmemSLCFlushInvalRequest(PVRSRV_DEVICE_NODE *psDeviceNode,
							PMR *psPmr)
{

	/* invoke SLC flush and invalidate request */
	psDeviceNode->pfnSLCCacheInvalidateRequest(psDeviceNode, psPmr);

	return PVRSRV_OK;
}

PVRSRV_ERROR DevmemIntIsVDevAddrValid(DEVMEMINT_CTX *psDevMemContext,
                                      IMG_DEV_VIRTADDR sDevAddr)
{
    return MMU_IsVDevAddrValid(psDevMemContext->psMMUContext,
                               GET_LOG2_PAGESIZE(),
                               sDevAddr) ? PVRSRV_OK : PVRSRV_ERROR_INVALID_GPU_ADDR;
}

#if defined (PDUMP)
IMG_UINT32 DevmemIntMMUContextID(DEVMEMINT_CTX *psDevMemContext)
{
	IMG_UINT32 ui32MMUContextID;
	MMU_AcquirePDumpMMUContext(psDevMemContext->psMMUContext, &ui32MMUContextID);
	return ui32MMUContextID;
}

PVRSRV_ERROR
DevmemIntPDumpSaveToFileVirtual(DEVMEMINT_CTX *psDevmemCtx,
                                IMG_DEV_VIRTADDR sDevAddrStart,
                                IMG_DEVMEM_SIZE_T uiSize,
                                IMG_UINT32 ui32ArraySize,
                                const IMG_CHAR *pszFilename,
								IMG_UINT32 ui32FileOffset,
								IMG_UINT32 ui32PDumpFlags)
{
    PVRSRV_ERROR eError;
    IMG_UINT32 uiPDumpMMUCtx;


	PVR_UNREFERENCED_PARAMETER(ui32ArraySize);

	eError = MMU_AcquirePDumpMMUContext(psDevmemCtx->psMMUContext,
										&uiPDumpMMUCtx);

    PVR_ASSERT(eError == PVRSRV_OK);

    /*
      The following SYSMEM refers to the 'MMU Context', hence it
      should be the MMU context, not the PMR, that says what the PDump
      MemSpace tag is?
      From a PDump P.O.V. it doesn't matter which name space we use as long
      as that MemSpace is used on the 'MMU Context' we're dumping from
    */
    eError = PDumpMMUSAB(psDevmemCtx->psDevNode->sDevId.pszPDumpDevName,
                            uiPDumpMMUCtx,
                            sDevAddrStart,
                            uiSize,
                            pszFilename,
                            ui32FileOffset,
                            ui32PDumpFlags);
    PVR_ASSERT(eError == PVRSRV_OK);

	MMU_ReleasePDumpMMUContext(psDevmemCtx->psMMUContext);
    return PVRSRV_OK;
}


PVRSRV_ERROR
DevmemIntPDumpBitmap(CONNECTION_DATA * psConnection,
                     PVRSRV_DEVICE_NODE *psDeviceNode,
                     IMG_CHAR *pszFileName,
                     IMG_UINT32 ui32FileOffset,
                     IMG_UINT32 ui32Width,
                     IMG_UINT32 ui32Height,
                     IMG_UINT32 ui32StrideInBytes,
                     IMG_DEV_VIRTADDR sDevBaseAddr,
                     DEVMEMINT_CTX *psDevMemContext,
                     IMG_UINT32 ui32Size,
                     PDUMP_PIXEL_FORMAT ePixelFormat,
                     IMG_UINT32 ui32AddrMode,
                     IMG_UINT32 ui32PDumpFlags)
{
	IMG_UINT32 ui32ContextID;
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psConnection);
	
	eError = MMU_AcquirePDumpMMUContext(psDevMemContext->psMMUContext, &ui32ContextID);

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "DevmemIntPDumpBitmap: Failed to acquire MMU context"));
		return PVRSRV_ERROR_FAILED_TO_ALLOC_MMUCONTEXT_ID;
	}

	eError = PDumpBitmapKM(psDeviceNode,
							pszFileName,
							ui32FileOffset,
							ui32Width,
							ui32Height,
							ui32StrideInBytes,
							sDevBaseAddr,
							ui32ContextID,
							ui32Size,
							ePixelFormat,
							ui32AddrMode,
							ui32PDumpFlags);

	/* Don't care about return value */
	MMU_ReleasePDumpMMUContext(psDevMemContext->psMMUContext);

	return eError;
}
#endif
