/*************************************************************************/ /*!
@File
@Title          RGX Compute routines
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    RGX Compute routines
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

#include "srvkm.h"
#include "pdump_km.h"
#include "pvr_debug.h"
#include "rgxutils.h"
#include "rgxfwutils.h"
#include "rgxcompute.h"
#include "rgxmem.h"
#include "allocmem.h"
#include "devicemem.h"
#include "devicemem_pdump.h"
#include "osfunc.h"
#include "rgxccb.h"
#include "rgxhwperf.h"
#include "rgxtimerquery.h"
#include "htbuffer.h"

#include "sync_server.h"
#include "sync_internal.h"
#include "rgx_memallocflags.h"
#include "rgxsync.h"

struct _RGX_SERVER_COMPUTE_CONTEXT_ {
	PVRSRV_DEVICE_NODE			*psDeviceNode;
	RGX_SERVER_COMMON_CONTEXT	*psServerCommonContext;
	DEVMEM_MEMDESC				*psFWFrameworkMemDesc;
	DEVMEM_MEMDESC				*psFWComputeContextStateMemDesc;
	PVRSRV_CLIENT_SYNC_PRIM		*psSync;
	DLLIST_NODE					sListNode;
	SYNC_ADDR_LIST				sSyncAddrListFence;
	SYNC_ADDR_LIST				sSyncAddrListUpdate;
};

IMG_EXPORT
PVRSRV_ERROR PVRSRVRGXCreateComputeContextKM(CONNECTION_DATA			*psConnection,
											 PVRSRV_DEVICE_NODE			*psDeviceNode,
											 IMG_UINT32					ui32Priority,
											 IMG_DEV_VIRTADDR			sMCUFenceAddr,
											 IMG_UINT32					ui32FrameworkCommandSize,
											 IMG_PBYTE					pbyFrameworkCommand,
											 IMG_HANDLE					hMemCtxPrivData,
											 RGX_SERVER_COMPUTE_CONTEXT	**ppsComputeContext)
{
	PVRSRV_RGXDEV_INFO 			*psDevInfo = psDeviceNode->pvDevice;
	DEVMEM_MEMDESC				*psFWMemContextMemDesc = RGXGetFWMemDescFromMemoryContextHandle(hMemCtxPrivData);
	RGX_SERVER_COMPUTE_CONTEXT	*psComputeContext;
	RGX_COMMON_CONTEXT_INFO		sInfo;
	PVRSRV_ERROR				eError = PVRSRV_OK;

	/* Prepare cleanup struct */
	*ppsComputeContext = NULL;
	psComputeContext = OSAllocMem(sizeof(*psComputeContext));
	if (psComputeContext == NULL)
	{
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	OSMemSet(psComputeContext, 0, sizeof(*psComputeContext));

	psComputeContext->psDeviceNode = psDeviceNode;

	/* Allocate cleanup sync */
	eError = SyncPrimAlloc(psDeviceNode->hSyncPrimContext,
						   &psComputeContext->psSync,
						   "compute cleanup");
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXCreateComputeContextKM: Failed to allocate cleanup sync (0x%x)",
				eError));
		goto fail_syncalloc;
	}

	/*
		Allocate device memory for the firmware GPU context suspend state.
		Note: the FW reads/writes the state to memory by accessing the GPU register interface.
	*/
	PDUMPCOMMENT("Allocate RGX firmware compute context suspend state");

	eError = DevmemFwAllocate(psDevInfo,
							  sizeof(RGXFWIF_COMPUTECTX_STATE),
							  RGX_FWCOMCTX_ALLOCFLAGS,
							  "FwComputeContextState",
							  &psComputeContext->psFWComputeContextStateMemDesc);

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXCreateComputeContextKM: Failed to allocate firmware GPU context suspend state (%u)",
				eError));
		goto fail_contextsuspendalloc;
	}

	/* 
	 * Create the FW framework buffer
	 */
	eError = PVRSRVRGXFrameworkCreateKM(psDeviceNode,
										&psComputeContext->psFWFrameworkMemDesc,
										ui32FrameworkCommandSize);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXCreateComputeContextKM: Failed to allocate firmware GPU framework state (%u)",
				eError));
		goto fail_frameworkcreate;
	}

	/* Copy the Framework client data into the framework buffer */
	eError = PVRSRVRGXFrameworkCopyCommand(psComputeContext->psFWFrameworkMemDesc,
										   pbyFrameworkCommand,
										   ui32FrameworkCommandSize);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXCreateComputeContextKM: Failed to populate the framework buffer (%u)",
				eError));
		goto fail_frameworkcopy;
	}
	
	sInfo.psFWFrameworkMemDesc = psComputeContext->psFWFrameworkMemDesc;
	sInfo.psMCUFenceAddr = &sMCUFenceAddr;

	eError = FWCommonContextAllocate(psConnection,
									 psDeviceNode,
									 REQ_TYPE_CDM,
									 RGXFWIF_DM_CDM,
									 NULL,
									 0,
									 psFWMemContextMemDesc,
									 psComputeContext->psFWComputeContextStateMemDesc,
									 RGX_CDM_CCB_SIZE_LOG2,
									 ui32Priority,
									 &sInfo,
									 &psComputeContext->psServerCommonContext);
	if (eError != PVRSRV_OK)
	{
		goto fail_contextalloc;
	}

	SyncAddrListInit(&psComputeContext->sSyncAddrListFence);
	SyncAddrListInit(&psComputeContext->sSyncAddrListUpdate);

	{
		PVRSRV_RGXDEV_INFO			*psDevInfo = psDeviceNode->pvDevice;

		OSWRLockAcquireWrite(psDevInfo->hComputeCtxListLock);
		dllist_add_to_tail(&(psDevInfo->sComputeCtxtListHead), &(psComputeContext->sListNode));
		OSWRLockReleaseWrite(psDevInfo->hComputeCtxListLock);
	}

	*ppsComputeContext = psComputeContext;
	return PVRSRV_OK;

fail_contextalloc:
fail_frameworkcopy:
	DevmemFwFree(psComputeContext->psFWFrameworkMemDesc);
fail_frameworkcreate:
	DevmemFwFree(psComputeContext->psFWComputeContextStateMemDesc);
fail_contextsuspendalloc:
	SyncPrimFree(psComputeContext->psSync);
fail_syncalloc:
	OSFreeMem(psComputeContext);
	return eError;
}

IMG_EXPORT
PVRSRV_ERROR PVRSRVRGXDestroyComputeContextKM(RGX_SERVER_COMPUTE_CONTEXT *psComputeContext)
{
	PVRSRV_ERROR				eError = PVRSRV_OK;
	PVRSRV_RGXDEV_INFO *psDevInfo = psComputeContext->psDeviceNode->pvDevice;

	/* Check if the FW has finished with this resource ... */
	eError = RGXFWRequestCommonContextCleanUp(psComputeContext->psDeviceNode,
											  FWCommonContextGetFWAddress(psComputeContext->psServerCommonContext),
											  psComputeContext->psSync,
											  RGXFWIF_DM_CDM);

	if (eError == PVRSRV_ERROR_RETRY)
	{
		return eError;
	}
	else if (eError != PVRSRV_OK)
	{
		PVR_LOG(("%s: Unexpected error from RGXFWRequestCommonContextCleanUp (%s)",
				__FUNCTION__,
				PVRSRVGetErrorStringKM(eError)));
		return eError;
	}

	/* ... it has so we can free its resources */

	OSWRLockAcquireWrite(psDevInfo->hComputeCtxListLock);
	dllist_remove_node(&(psComputeContext->sListNode));
	OSWRLockReleaseWrite(psDevInfo->hComputeCtxListLock);

	FWCommonContextFree(psComputeContext->psServerCommonContext);
	DevmemFwFree(psComputeContext->psFWFrameworkMemDesc);
	DevmemFwFree(psComputeContext->psFWComputeContextStateMemDesc);
	SyncPrimFree(psComputeContext->psSync);
	OSFreeMem(psComputeContext);

	return PVRSRV_OK;
}


IMG_EXPORT
PVRSRV_ERROR PVRSRVRGXKickCDMKM(RGX_SERVER_COMPUTE_CONTEXT	*psComputeContext,
								IMG_UINT32					ui32ClientFenceCount,
								SYNC_PRIMITIVE_BLOCK			**pauiClientFenceUFOSyncPrimBlock,
								IMG_UINT32					*paui32ClientFenceSyncOffset,
								IMG_UINT32					*paui32ClientFenceValue,
								IMG_UINT32					ui32ClientUpdateCount,
								SYNC_PRIMITIVE_BLOCK			**pauiClientUpdateUFOSyncPrimBlock,
								IMG_UINT32					*paui32ClientUpdateSyncOffset,
								IMG_UINT32					*paui32ClientUpdateValue,
								IMG_UINT32					ui32ServerSyncPrims,
								IMG_UINT32					*paui32ServerSyncFlags,
								SERVER_SYNC_PRIMITIVE		**pasServerSyncs,
								IMG_UINT32					ui32CmdSize,
								IMG_PBYTE					pui8DMCmd,
								IMG_BOOL					bPDumpContinuous,
							    IMG_UINT32					ui32ExtJobRef,
								IMG_UINT32					ui32IntJobRef)
{
	RGXFWIF_KCCB_CMD		sCmpKCCBCmd;
	RGX_CCB_CMD_HELPER_DATA	asCmdHelperData[1];
	PVRSRV_ERROR			eError;
	PVRSRV_ERROR			eError2;
	IMG_UINT32				i;
	IMG_UINT32				ui32CDMCmdOffset = 0;

	PRGXFWIF_TIMESTAMP_ADDR pPreAddr;
	PRGXFWIF_TIMESTAMP_ADDR pPostAddr;
	PRGXFWIF_UFO_ADDR       pRMWUFOAddr;

	eError = SyncAddrListPopulate(&psComputeContext->sSyncAddrListFence,
									ui32ClientFenceCount,
									pauiClientFenceUFOSyncPrimBlock,
									paui32ClientFenceSyncOffset);
	if(eError != PVRSRV_OK)
	{
		goto err_populate_sync_addr_list;
	}

	eError = SyncAddrListPopulate(&psComputeContext->sSyncAddrListUpdate,
									ui32ClientUpdateCount,
									pauiClientUpdateUFOSyncPrimBlock,
									paui32ClientUpdateSyncOffset);
	if(eError != PVRSRV_OK)
	{
		goto err_populate_sync_addr_list;
	}


	/* Sanity check the server fences */
	for (i=0;i<ui32ServerSyncPrims;i++)
	{
		if (!(paui32ServerSyncFlags[i] & PVRSRV_CLIENT_SYNC_PRIM_OP_CHECK))
		{
			PVR_DPF((PVR_DBG_ERROR, "%s: Server fence (on CDM) must fence", __FUNCTION__));
			return PVRSRV_ERROR_INVALID_SYNC_PRIM_OP;
		}
	}

	RGX_GetTimestampCmdHelper((PVRSRV_RGXDEV_INFO*) psComputeContext->psDeviceNode->pvDevice,
	                          & pPreAddr,
	                          & pPostAddr,
	                          & pRMWUFOAddr);

	eError = RGXCmdHelperInitCmdCCB(FWCommonContextGetClientCCB(psComputeContext->psServerCommonContext),
	                                ui32ClientFenceCount,
	                                psComputeContext->sSyncAddrListFence.pasFWAddrs,
	                                paui32ClientFenceValue,
	                                ui32ClientUpdateCount,
	                                psComputeContext->sSyncAddrListUpdate.pasFWAddrs,
	                                paui32ClientUpdateValue,
	                                ui32ServerSyncPrims,
	                                paui32ServerSyncFlags,
	                                pasServerSyncs,
	                                ui32CmdSize,
	                                pui8DMCmd,
	                                & pPreAddr,
	                                & pPostAddr,
	                                & pRMWUFOAddr,
	                                RGXFWIF_CCB_CMD_TYPE_CDM,
	                                ui32ExtJobRef,
	                                ui32IntJobRef,
	                                bPDumpContinuous,
	                                "Compute",
	                                asCmdHelperData);
	if (eError != PVRSRV_OK)
	{
		goto fail_cmdinit;
	}

	eError = RGXCmdHelperAcquireCmdCCB(IMG_ARR_NUM_ELEMS(asCmdHelperData), 
	                                   asCmdHelperData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "RGXKickCDM: Failed to acquire space for client CCB command"));
		goto fail_cmdaquire;
	}


	/*
		We should reserved space in the kernel CCB here and fill in the command
		directly.
		This is so if there isn't space in the kernel CCB we can return with
		retry back to services client before we take any operations
	*/

	/*
		We might only be kicking for flush out a padding packet so only submit
		the command if the create was successful
	*/
	if (eError == PVRSRV_OK)
	{
		/*
			All the required resources are ready at this point, we can't fail so
			take the required server sync operations and commit all the resources
		*/

		ui32CDMCmdOffset = RGXGetHostWriteOffsetCCB(FWCommonContextGetClientCCB(psComputeContext->psServerCommonContext));
		RGXCmdHelperReleaseCmdCCB(1, asCmdHelperData, "CDM", FWCommonContextGetFWAddress(psComputeContext->psServerCommonContext).ui32Addr);
	}

	/* Construct the kernel compute CCB command. */
	sCmpKCCBCmd.eCmdType = RGXFWIF_KCCB_CMD_KICK;
	sCmpKCCBCmd.uCmdData.sCmdKickData.psContext = FWCommonContextGetFWAddress(psComputeContext->psServerCommonContext);
	sCmpKCCBCmd.uCmdData.sCmdKickData.ui32CWoffUpdate = RGXGetHostWriteOffsetCCB(FWCommonContextGetClientCCB(psComputeContext->psServerCommonContext));
	sCmpKCCBCmd.uCmdData.sCmdKickData.ui32NumCleanupCtl = 0;

	HTBLOGK(HTB_SF_MAIN_KICK_CDM,
			sCmpKCCBCmd.uCmdData.sCmdKickData.psContext,
			ui32CDMCmdOffset
			);

	/*
	 * Submit the compute command to the firmware.
	 */
	LOOP_UNTIL_TIMEOUT(MAX_HW_TIME_US)
	{
		eError2 = RGXScheduleCommand(psComputeContext->psDeviceNode->pvDevice,
									RGXFWIF_DM_CDM,
									&sCmpKCCBCmd,
									sizeof(sCmpKCCBCmd),
									bPDumpContinuous);
		if (eError2 != PVRSRV_ERROR_RETRY)
		{
			break;
		}
		OSWaitus(MAX_HW_TIME_US/WAIT_TRY_COUNT);
	} END_LOOP_UNTIL_TIMEOUT();
	
	if (eError2 != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVRGXKickCDMKM failed to schedule kernel CCB command. (0x%x)", eError));
	}
	else
	{
#if defined(SUPPORT_GPUTRACE_EVENTS)
		RGXHWPerfFTraceGPUEnqueueEvent(psComputeContext->psDeviceNode->pvDevice,
				ui32ExtJobRef, ui32IntJobRef, "CDM");
#endif
		RGX_HWPERF_HOST_ENQ(psComputeContext, OSGetCurrentClientProcessIDKM(),
				ui32ExtJobRef, ui32IntJobRef, RGX_HWPERF_HOST_ENQ_KICK_TYPE_CDM);
	}
	/*
	 * Now check eError (which may have returned an error from our earlier call
	 * to RGXCmdHelperAcquireCmdCCB) - we needed to process any flush command first
	 * so we check it now...
	 */
	if (eError != PVRSRV_OK )
	{
		goto fail_cmdaquire;
	}

	return PVRSRV_OK;

fail_cmdaquire:
fail_cmdinit:
err_populate_sync_addr_list:
	return eError;
}

IMG_EXPORT PVRSRV_ERROR PVRSRVRGXFlushComputeDataKM(RGX_SERVER_COMPUTE_CONTEXT *psComputeContext)
{
	RGXFWIF_KCCB_CMD sFlushCmd;
	PVRSRV_ERROR eError = PVRSRV_OK;

#if defined(PDUMP)
	PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_CONTINUOUS, "Submit Compute flush");
#endif
	sFlushCmd.eCmdType = RGXFWIF_KCCB_CMD_SLCFLUSHINVAL;
	sFlushCmd.uCmdData.sSLCFlushInvalData.bInval = IMG_FALSE;
	sFlushCmd.uCmdData.sSLCFlushInvalData.bDMContext = IMG_TRUE;
	sFlushCmd.uCmdData.sSLCFlushInvalData.eDM = RGXFWIF_DM_CDM;
	sFlushCmd.uCmdData.sSLCFlushInvalData.psContext = FWCommonContextGetFWAddress(psComputeContext->psServerCommonContext);

	LOOP_UNTIL_TIMEOUT(MAX_HW_TIME_US)
	{
		eError = RGXScheduleCommand(psComputeContext->psDeviceNode->pvDevice,
									RGXFWIF_DM_GP,
									&sFlushCmd,
									sizeof(sFlushCmd),
									IMG_TRUE);
		if (eError != PVRSRV_ERROR_RETRY)
		{
			break;
		}
		OSWaitus(MAX_HW_TIME_US/WAIT_TRY_COUNT);
	} END_LOOP_UNTIL_TIMEOUT();

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXFlushComputeDataKM: Failed to schedule SLC flush command with error (%u)", eError));
	}
	else
	{
		/* Wait for the SLC flush to complete */
		eError = RGXWaitForFWOp(psComputeContext->psDeviceNode->pvDevice,
								RGXFWIF_DM_GP,
								psComputeContext->psSync,
								IMG_TRUE);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"PVRSRVRGXFlushComputeDataKM: Compute flush aborted with error (%u)", eError));
		}
	}
	return eError;
}

PVRSRV_ERROR PVRSRVRGXSetComputeContextPriorityKM(CONNECTION_DATA *psConnection,
                                                  PVRSRV_DEVICE_NODE * psDeviceNode,
												  RGX_SERVER_COMPUTE_CONTEXT *psComputeContext,
												  IMG_UINT32 ui32Priority)
{
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psDeviceNode);

	eError = ContextSetPriority(psComputeContext->psServerCommonContext,
								psConnection,
								psComputeContext->psDeviceNode->pvDevice,
								ui32Priority,
								RGXFWIF_DM_CDM);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to set the priority of the compute context (%s)", __FUNCTION__, PVRSRVGetErrorStringKM(eError)));
	}
	return eError;
}

/*
 * PVRSRVRGXGetLastComputeContextResetReasonKM
 */
PVRSRV_ERROR PVRSRVRGXGetLastComputeContextResetReasonKM(RGX_SERVER_COMPUTE_CONTEXT *psComputeContext,
                                                         IMG_UINT32 *peLastResetReason,
														 IMG_UINT32 *pui32LastResetJobRef)
{
	PVR_ASSERT(psComputeContext != NULL);
	PVR_ASSERT(peLastResetReason != NULL);
	PVR_ASSERT(pui32LastResetJobRef != NULL);
	
	*peLastResetReason = FWCommonContextGetLastResetReason(psComputeContext->psServerCommonContext,
	                                                       pui32LastResetJobRef);

	return PVRSRV_OK;
}

void CheckForStalledComputeCtxt(PVRSRV_RGXDEV_INFO *psDevInfo,
								DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf)
{
	DLLIST_NODE *psNode, *psNext;
	OSWRLockAcquireRead(psDevInfo->hComputeCtxListLock);
	dllist_foreach_node(&psDevInfo->sComputeCtxtListHead, psNode, psNext)
	{
		RGX_SERVER_COMPUTE_CONTEXT *psCurrentServerComputeCtx =
			IMG_CONTAINER_OF(psNode, RGX_SERVER_COMPUTE_CONTEXT, sListNode);
		DumpStalledFWCommonContext(psCurrentServerComputeCtx->psServerCommonContext,
								   pfnDumpDebugPrintf);
	}
	OSWRLockReleaseRead(psDevInfo->hComputeCtxListLock);
}

IMG_BOOL CheckForStalledClientComputeCtxt(PVRSRV_RGXDEV_INFO *psDevInfo)
{
	PVRSRV_ERROR eError = PVRSRV_OK;
	DLLIST_NODE *psNode, *psNext;
	OSWRLockAcquireRead(psDevInfo->hComputeCtxListLock);
	dllist_foreach_node(&psDevInfo->sComputeCtxtListHead, psNode, psNext)
	{
		RGX_SERVER_COMPUTE_CONTEXT *psCurrentServerComputeCtx =
			IMG_CONTAINER_OF(psNode, RGX_SERVER_COMPUTE_CONTEXT, sListNode);

		if (CheckStalledClientCommonContext(psCurrentServerComputeCtx->psServerCommonContext)
			== PVRSRV_ERROR_CCCB_STALLED)
		{
			eError = PVRSRV_ERROR_CCCB_STALLED;
		}
	}
	OSWRLockReleaseRead(psDevInfo->hComputeCtxListLock);
	return (PVRSRV_ERROR_CCCB_STALLED == eError)? IMG_TRUE: IMG_FALSE;
}

/******************************************************************************
 End of file (rgxcompute.c)
******************************************************************************/
