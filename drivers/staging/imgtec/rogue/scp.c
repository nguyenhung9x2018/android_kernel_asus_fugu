/*************************************************************************/ /*!
@File           scp.c
@Title          Software Command Processor
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    The software command processor allows commands queued and
                deferred until their synchronisation requirements have been meet.
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

#include "scp.h"
#include "lists.h"
#include "allocmem.h"
#include "pvr_notifier.h"
#include "pvrsrv.h"
#include "pvr_debug.h"
#include "osfunc.h"
#include "lock.h"
#include "sync_server.h"
#include "sync_internal.h"
#include "rgxhwperf.h"

#if defined(SUPPORT_NATIVE_FENCE_SYNC)

#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0))
#include <linux/sw_sync.h>
#else
#include <../drivers/staging/android/sw_sync.h>
#endif

#include "kernel_compatibility.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0))
static inline int sync_fence_get_status(struct sync_fence *psFence)
{
	return psFence->status;
}

static inline struct sync_timeline *sync_pt_parent(struct sync_pt *pt)
{
	return pt->parent;
}

static inline int sync_pt_get_status(struct sync_pt *pt)
{
	return pt->status;
}

static inline ktime_t sync_pt_get_timestamp(struct sync_pt *pt)
{
	return pt->timestamp;
}

#define for_each_sync_pt(s, f, c)							\
	list_for_each_entry((s), &(f)->pt_list_head, pt_list)
#else
static inline int sync_fence_get_status(struct sync_fence *psFence)
{
	int iStatus = atomic_read(&psFence->status);

	/*
	 * When Android sync was rebased on top of fences the sync_fence status
	 * values changed from 0 meaning 'active' to 'signalled' and, likewise,
	 * values greater than 0 went from meaning 'signalled' to 'active'
	 * (where the value corresponds to the number of active sync points).
	 *
	 * Convert to the old style status values.
	 */
	return iStatus > 0 ? 0 : iStatus ? iStatus : 1;
}

static inline int sync_pt_get_status(struct sync_pt *pt)
{
	/* No error state for raw dma-buf fences */
	return fence_is_signaled(&pt->base) ? 1 : 0;
}

static inline ktime_t sync_pt_get_timestamp(struct sync_pt *pt)
{
	return pt->base.timestamp;
}

#define for_each_sync_pt(s, f, c)							   \
	for ((c) = 0, (s) = (struct sync_pt *)(f)->cbs[0].sync_pt; \
	     (c) < (f)->num_fences;								   \
	     (c)++,   (s) = (struct sync_pt *)(f)->cbs[c].sync_pt)
#endif


static PVRSRV_ERROR AllocReleaseFence(struct sw_sync_timeline *psTimeline, const char *szName, IMG_UINT32 ui32FenceVal, int *piFenceFd)
{
	struct sync_fence *psFence = NULL;
	struct sync_pt *psPt;
	int iFd = get_unused_fd();
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (iFd < 0)
	{
		return PVRSRV_ERROR_RESOURCE_UNAVAILABLE;
	}

	psPt = sw_sync_pt_create(psTimeline, ui32FenceVal);
	if(!psPt)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorPutFd;
	}

	psFence = sync_fence_create(szName, psPt);
	if(!psFence)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorFreePoint;
	}

	sync_fence_install(psFence, iFd);

	*piFenceFd = iFd;

ErrorOut:
	return eError;

ErrorFreePoint:
	sync_pt_free(psPt);

ErrorPutFd:
	put_unused_fd(iFd);

	goto ErrorOut;
}

#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

struct _SCP_CONTEXT_
{
	void				*pvCCB;	            /*!< Pointer to the command circler buffer*/
	volatile IMG_UINT32	ui32DepOffset;      /*!< Dependency offset  */
	volatile IMG_UINT32	ui32ReadOffset;     /*!< Read offset */
	volatile IMG_UINT32	ui32WriteOffset;    /*!< Write offset */
	IMG_UINT32			ui32CCBSize;        /*!< CCB size */
	IMG_UINT32			psSyncRequesterID;	/*!< Sync requester ID, used when taking sync operations */
	POS_LOCK			hLock;				/*!< Lock for this structure */
#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	void                *pvTimeline;
	IMG_UINT32          ui32TimelineVal;
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */
};

typedef struct _SCP_SYNC_DATA_
{
	SERVER_SYNC_PRIMITIVE	*psSync;		/*!< Server sync */
	IMG_UINT32				ui32Fence;		/*!< Fence value to check for */
	IMG_UINT32				ui32Update;		/*!< Fence update value */
	IMG_UINT32				ui32Flags;		/*!< Flags for this sync data */
#define SCP_SYNC_DATA_FENCE		(1<<0)		/*!< This sync has a fence */
#define SCP_SYNC_DATA_UPDATE	(1<<1)		/*!< This sync has an update */
} SCP_SYNC_DATA;


#define SCP_COMMAND_INVALID     0   /*!< Invalid command */
#define SCP_COMMAND_CALLBACK    1   /*!< Command with callbacks */
#define SCP_COMMAND_PADDING     2   /*!< Padding */
typedef struct _SCP_COMMAND_
{
	IMG_UINT32				ui32CmdType;        /*!< Command type */
	IMG_UINT32				ui32CmdSize;		/*!< Total size of the command (i.e. includes header) */
	IMG_UINT32				ui32SyncCount;      /*!< Total number of syncs in pasSync */
	SCP_SYNC_DATA			*pasSCPSyncData;    /*!< Pointer to the array of sync data (allocated in the CCB) */
#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	struct sync_fence       *psAcquireFence;
	struct sync_fence       *psReleaseFence;
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */
	SCPReady				pfnReady;           /*!< Pointer to the function to check if the command is ready */
	SCPDo					pfnDo;           	/*!< Pointer to the function to call when the command is ready to go */
	void					*pvReadyData;        /*!< Data to pass into pfnReady */
	void					*pvCompleteData;     /*!< Data to pass into pfnComplete */
} SCP_COMMAND;

#define GET_CCB_SPACE(WOff, ROff, CCBSize) \
	((((ROff) - (WOff)) + ((CCBSize) - 1)) & ((CCBSize) - 1))

#define UPDATE_CCB_OFFSET(Off, PacketSize, CCBSize) \
	(Off) = (((Off) + (PacketSize)) & ((CCBSize) - 1))

#define PADDING_COMMAND_SIZE	(sizeof(SCP_COMMAND))

#if defined(SCP_DEBUG)
#define SCP_DEBUG_PRINT(fmt, ...) \
	PVRSRVDebugPrintf(PVR_DBG_WARNING, \
					  __FILE__, __LINE__, \
					  fmt, \
					  __VA_ARGS__)
#else
#define SCP_DEBUG_PRINT(fmt, ...)
#endif

/*****************************************************************************
 *                          Internal functions                               *
 *****************************************************************************/

/*************************************************************************/ /*!
@Function       __SCPAlloc

@Description    Allocate space in the software command processor.

@Input          psContext            Context to allocate from

@Input          ui32Size                Size to allocate

@Output         ppvBufferSpace          Pointer to space allocated

@Return         PVRSRV_OK if the allocation was successful
*/
/*****************************************************************************/
static
PVRSRV_ERROR __SCPAlloc(SCP_CONTEXT *psContext,
						IMG_UINT32 ui32Size,
						void **ppvBufferSpace)
{
	IMG_UINT32 ui32FreeSpace;

	ui32FreeSpace = GET_CCB_SPACE(psContext->ui32WriteOffset,
								  psContext->ui32ReadOffset,
								  psContext->ui32CCBSize);
	if (ui32FreeSpace >= ui32Size)
	{
		*ppvBufferSpace = (void *)((IMG_UINT8 *)psContext->pvCCB +
		                  psContext->ui32WriteOffset);
		return PVRSRV_OK;
	}
	else
	{
		return PVRSRV_ERROR_RETRY;
	}
}

/*************************************************************************/ /*!
@Function       _SCPAlloc

@Description    Allocate space in the software command processor, handling the
                case where we wrap around the CCB.

@Input          psContext            Context to allocate from

@Input          ui32Size                Size to allocate

@Output         ppvBufferSpace          Pointer to space allocated

@Return         PVRSRV_OK if the allocation was successful
*/
/*****************************************************************************/
static
PVRSRV_ERROR _SCPAlloc(SCP_CONTEXT *psContext,
					   IMG_UINT32 ui32Size,
					   void **ppvBufferSpace)
{
	if ((ui32Size + PADDING_COMMAND_SIZE) > psContext->ui32CCBSize)
	{
		PVR_DPF((PVR_DBG_WARNING, "Command size (%d) too big for CCB\n", ui32Size));
		return PVRSRV_ERROR_CMD_TOO_BIG;
	}

	/*
		Check we don't overflow the end of the buffer and make sure we have
		enough for the padding command
	*/
	if ((psContext->ui32WriteOffset + ui32Size + PADDING_COMMAND_SIZE) > psContext->ui32CCBSize)
	{
		SCP_COMMAND *psCommand;
		void *pvCommand;
		PVRSRV_ERROR eError;
		IMG_UINT32 ui32Remain = psContext->ui32CCBSize - psContext->ui32WriteOffset;

		/* We're at the end of the buffer without enough contiguous space */
		eError = __SCPAlloc(psContext, ui32Remain, &pvCommand);
		if (eError != PVRSRV_OK)
		{
			PVR_ASSERT(eError == PVRSRV_ERROR_RETRY);
			return eError;
		}
		psCommand = pvCommand;
		psCommand->ui32CmdType = SCP_COMMAND_PADDING;
		psCommand->ui32CmdSize = ui32Remain;

		UPDATE_CCB_OFFSET(psContext->ui32WriteOffset, ui32Remain, psContext->ui32CCBSize);
	}

	return __SCPAlloc(psContext, ui32Size, ppvBufferSpace);
}

/*************************************************************************/ /*!
@Function       _SCPInsert

@Description    Insert the a finished command that was written into the CCB
                space allocated in a previous call to _SCPAlloc.
                This makes the command ready to be processed.

@Input          psContext               Context to allocate from

@Input          ui32Size                Size to allocate

@Return         None
*/
/*****************************************************************************/
static
void _SCPInsert(SCP_CONTEXT *psContext,
				IMG_UINT32 ui32Size)
{
	/*
	 * Update the write offset.
	 */
	UPDATE_CCB_OFFSET(psContext->ui32WriteOffset,
					  ui32Size,
					  psContext->ui32CCBSize);
}

#if defined(SUPPORT_NATIVE_FENCE_SYNC)

static void _SCPDumpFence(const char *psczName, struct sync_fence *psFence,
					DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf,
					void *pvDumpDebugFile)
{
	struct sync_pt *psPt;
	char szTime[16]  = { '\0' };
	char szVal1[64]  = { '\0' };
	char szVal2[64]  = { '\0' };
	char szVal3[132] = { '\0' };
	int iStatus = sync_fence_get_status(psFence);
	int i;

	PVR_UNREFERENCED_PARAMETER(i);

	PVR_DUMPDEBUG_LOG("\t  %s: [%p] %s: %s", psczName, psFence, psFence->name,
					   (iStatus > 0 ? "signalled" : iStatus ? "error" : "active"));

	for_each_sync_pt(psPt, psFence, i)
	{
		struct sync_timeline *psTimeline = sync_pt_parent(psPt);
		ktime_t timestamp = sync_pt_get_timestamp(psPt);
		struct timeval tv = ktime_to_timeval(timestamp);
		int iPtStatus = sync_pt_get_status(psPt);

		snprintf(szTime, sizeof(szTime), "@%ld.%06ld", tv.tv_sec, tv.tv_usec);

		if (psTimeline->ops->pt_value_str &&
			psTimeline->ops->timeline_value_str)
		{
			psTimeline->ops->pt_value_str(psPt, szVal1, sizeof(szVal1));
			psTimeline->ops->timeline_value_str(psTimeline, szVal2, sizeof(szVal2));
			snprintf(szVal3, sizeof(szVal3), ": %s / %s", szVal1, szVal2);
		}

		PVR_DUMPDEBUG_LOG("\t    %s %s%s%s", psTimeline->name,
						   (iPtStatus > 0 ? "signalled" : iPtStatus ? "error" : "active"),
						   (iPtStatus > 0 ? szTime : ""),
						   szVal3);
	}

}

#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

/*************************************************************************/ /*!
@Function       _SCPCommandReady

@Description    Check if a command is ready. Checks to see if the command
                has had its fences met and is ready to go.

@Input          psCommand               Command to check

@Return         PVRSRV_OK if the command is ready
*/
/*****************************************************************************/
static
PVRSRV_ERROR _SCPCommandReady(SCP_COMMAND *psCommand)
{
	IMG_UINT32 i;
	
	PVR_ASSERT(psCommand->ui32CmdType != SCP_COMMAND_INVALID);

	if (psCommand->ui32CmdType == SCP_COMMAND_PADDING)
	{
		return PVRSRV_OK;
	}

	for (i = 0; i < psCommand->ui32SyncCount; i++)
	{
		SCP_SYNC_DATA *psSCPSyncData = &psCommand->pasSCPSyncData[i];

		/*
			If the same sync is used in concurrent command we can skip the check
		*/
		if (psSCPSyncData->ui32Flags & SCP_SYNC_DATA_FENCE)
		{
			if (!ServerSyncFenceIsMet(psSCPSyncData->psSync, psSCPSyncData->ui32Fence))
			{
				return PVRSRV_ERROR_FAILED_DEPENDENCIES;
			}
		}
	}

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	/* Check for the provided acquire fence */
	if (psCommand->psAcquireFence != NULL)
	{
		int err = sync_fence_wait(psCommand->psAcquireFence, 0);
		/* -ETIME means active. In this case we will retry later again. If the
		 * return value is an error or zero we will close this fence and
		 * proceed. This makes sure that we are not getting stuck here when a
		 * fence changes into an error state for whatever reason. */
		if (err == -ETIME)
		{
			return PVRSRV_ERROR_FAILED_DEPENDENCIES;
		}
		else
		{
			if (err)
			{
				PVR_LOG(("SCP: Fence wait failed with %d", err));
				_SCPDumpFence("Acquire Fence", psCommand->psAcquireFence, NULL, NULL);
			}
			/* Put the fence. */
			sync_fence_put(psCommand->psAcquireFence);
			psCommand->psAcquireFence = NULL;
		}
	}
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

	/* Command is ready */
	if (psCommand->pfnReady(psCommand->pvReadyData))
	{
		return PVRSRV_OK;
	}

	/*
		If we got here it means the command is ready to go, but the SCP client
		isn't ready for the command
	*/
	return PVRSRV_ERROR_NOT_READY;
}

/*************************************************************************/ /*!
@Function       _SCPCommandDo

@Description    Run a command

@Input          psCommand               Command to run

@Return         PVRSRV_OK if the command is ready
*/
/*****************************************************************************/
static
void _SCPCommandDo(SCP_COMMAND *psCommand)
{
	if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
	{
		psCommand->pfnDo(psCommand->pvReadyData, psCommand->pvCompleteData);
	}
}

/*************************************************************************/ /*!
@Function       _SCPDumpCommand

@Description    Dump a SCP command

@Input          psCommand               Command to dump

@Return         None
*/
/*****************************************************************************/
static void _SCPDumpCommand(SCP_COMMAND *psCommand,
				DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf,
				void *pvDumpDebugFile)
{
	IMG_UINT32 i;

	PVR_DUMPDEBUG_LOG("\tCommand type = %d (@%p)", psCommand->ui32CmdType, psCommand);

	if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
	{
		for (i = 0; i < psCommand->ui32SyncCount; i++)
		{
			SCP_SYNC_DATA *psSCPSyncData = &psCommand->pasSCPSyncData[i];

			PVR_ASSERT(psCommand->pasSCPSyncData != NULL);
			PVR_ASSERT(psSCPSyncData != NULL);

			/*
				Only dump this sync if there is a fence operation on it
			*/
			if (psSCPSyncData->ui32Flags & SCP_SYNC_DATA_FENCE)
			{
				IMG_UINT32 ui32SyncAddr;

				PVR_ASSERT(psSCPSyncData->psSync != NULL);
				(void)ServerSyncGetFWAddr(psSCPSyncData->psSync, &ui32SyncAddr);
				PVR_DUMPDEBUG_LOG("\t\tFenced on 0x%08x = 0x%08x (?= 0x%08x)",
						ui32SyncAddr,
						psSCPSyncData->ui32Fence,
						ServerSyncGetValue(psSCPSyncData->psSync));
			}
		}
#if defined(SUPPORT_NATIVE_FENCE_SYNC)
		if (psCommand->psAcquireFence)
		{
			_SCPDumpFence("Acquire Fence", psCommand->psAcquireFence,
						  pfnDumpDebugPrintf, pvDumpDebugFile);
		}
		if (psCommand->psReleaseFence)
		{
			_SCPDumpFence("Release Fence", psCommand->psReleaseFence,
						  pfnDumpDebugPrintf, pvDumpDebugFile);
		}
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */
	}
}

/*****************************************************************************
 *                    Public interface functions                             *
 *****************************************************************************/

/*
	SCPCreate
*/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV SCPCreate(IMG_UINT32 ui32CCBSizeLog2,
									SCP_CONTEXT **ppsContext)
{
	SCP_CONTEXT	*psContext;
	IMG_UINT32 ui32Power2QueueSize = 1 << ui32CCBSizeLog2;
	PVRSRV_ERROR eError;

	/* allocate an internal queue info structure */
	psContext = OSAllocZMem(sizeof(SCP_CONTEXT));
	if (psContext == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"SCPCreate: Failed to alloc queue struct"));
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorExit;
	}

	/* allocate the command queue buffer - allow for overrun */
	psContext->pvCCB = OSAllocMem(ui32Power2QueueSize);
	if (psContext->pvCCB == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"SCPCreate: Failed to alloc queue buffer"));
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorExit;
	}

	psContext->ui32CCBSize = ui32Power2QueueSize;

	eError = OSLockCreate(&psContext->hLock, LOCK_TYPE_NONE);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}

	eError = PVRSRVServerSyncRequesterRegisterKM(&psContext->psSyncRequesterID);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}	

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	psContext->pvTimeline = sw_sync_timeline_create("pvr_scp");
	if(psContext->pvTimeline == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"SCPCreate: sw_sync_timeline_create() failed"));
		goto ErrorExit;
	}
	psContext->ui32TimelineVal = 0;
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

	SCP_DEBUG_PRINT("%s: New SCP %p of size %d", 
			__FUNCTION__, psContext, ui32Power2QueueSize);

	*ppsContext = psContext;

	return PVRSRV_OK;

ErrorExit:
	if(psContext)
	{
		if(psContext->pvCCB)
		{
			OSFreeMem(psContext->pvCCB);
			psContext->pvCCB = NULL;
		}

		OSFreeMem(psContext);
	}

	return eError;
}

/*
	SCPAllocCommand
*/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV SCPAllocCommand(SCP_CONTEXT *psContext,
										  IMG_UINT32 ui32SyncPrimCount,
										  SERVER_SYNC_PRIMITIVE **papsSync,
										  IMG_BOOL *pabUpdate,
										  IMG_INT32 i32AcquireFenceFd,
										  SCPReady pfnCommandReady,
										  SCPDo pfnCommandDo,
										  size_t ui32ReadyDataByteSize,
										  size_t ui32CompleteDataByteSize,
										  void **ppvReadyData,
										  void **ppvCompleteData,
										  IMG_INT32 *pi32ReleaseFenceFd)
{
	PVRSRV_ERROR eError;
	SCP_COMMAND *psCommand;
	IMG_UINT32 ui32CommandSize;
	IMG_UINT32 ui32SyncOpSize;
	IMG_UINT32 i;

	/* Round up the incoming data sizes to be pointer granular */
	ui32ReadyDataByteSize = (ui32ReadyDataByteSize & (~(sizeof(void *)-1))) + sizeof(void *);
	ui32CompleteDataByteSize = (ui32CompleteDataByteSize & (~(sizeof(void *)-1))) + sizeof(void *);

	ui32SyncOpSize = (sizeof(PVRSRV_CLIENT_SYNC_PRIM_OP) * ui32SyncPrimCount);

	/* Total command size */
	ui32CommandSize = sizeof(SCP_COMMAND) +
					  ui32SyncOpSize +
					  ui32ReadyDataByteSize +
					  ui32CompleteDataByteSize;

	eError = _SCPAlloc(psContext, ui32CommandSize, (void **) &psCommand);
	if(eError != PVRSRV_OK)
	{
		SCP_DEBUG_PRINT("%s: Failed to allocate command of size %d for ctx %p (%d)", __FUNCTION__, ui32CommandSize, psContext, eError);
		return eError;
	}

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	if (pi32ReleaseFenceFd)
	{
		/* Create a release sync for the caller. */
		eError = AllocReleaseFence(psContext->pvTimeline, "pvr_scp_retire",
								   ++psContext->ui32TimelineVal,
								   pi32ReleaseFenceFd);
		if (eError != PVRSRV_OK)
		{
			return eError;
		}
	}
#else /* defined(SUPPORT_NATIVE_FENCE_SYNC) */
	PVR_UNREFERENCED_PARAMETER(i32AcquireFenceFd);
	PVR_UNREFERENCED_PARAMETER(pi32ReleaseFenceFd);
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

	SCP_DEBUG_PRINT("%s: New Command %p for ctx %p of size %d, syncCount: %d", 
			__FUNCTION__, psCommand, psContext, ui32CommandSize, ui32SyncPrimCount);

	/* setup the command */
	psCommand->ui32CmdSize = ui32CommandSize;
	psCommand->ui32CmdType = SCP_COMMAND_CALLBACK;
	psCommand->ui32SyncCount = ui32SyncPrimCount;

	/* Set up command pointers */
	psCommand->pasSCPSyncData = (SCP_SYNC_DATA *) (((IMG_CHAR *) psCommand) + sizeof(SCP_COMMAND));

	psCommand->pfnReady = pfnCommandReady;
	psCommand->pfnDo = pfnCommandDo;

	psCommand->pvReadyData = ((IMG_CHAR *) psCommand) +
							 sizeof(SCP_COMMAND) + ui32SyncOpSize;
	psCommand->pvCompleteData = ((IMG_CHAR *) psCommand) +
								sizeof(SCP_COMMAND) + ui32SyncOpSize +
								ui32ReadyDataByteSize;

	/* Copy over the sync data */
	for (i=0;i<ui32SyncPrimCount;i++)
	{
		SCP_SYNC_DATA *psSCPSyncData = &psCommand->pasSCPSyncData[i];
		IMG_BOOL bFenceRequired;

		psSCPSyncData->psSync = papsSync[i];

		PVRSRVServerSyncQueueSWOpKM(papsSync[i],
								  &psSCPSyncData->ui32Fence,
								  &psSCPSyncData->ui32Update,
								  psContext->psSyncRequesterID,
								  pabUpdate[i],
								  &bFenceRequired);
		if (bFenceRequired)
		{
			psSCPSyncData->ui32Flags = SCP_SYNC_DATA_FENCE;
		}
		else
		{
			psSCPSyncData->ui32Flags = 0;
		}

		/* Only update if requested */
		if (pabUpdate[i])
		{
			psSCPSyncData->ui32Flags |= SCP_SYNC_DATA_UPDATE;
		}
	}

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	/* Copy over the fences */
	if (i32AcquireFenceFd >= 0)
	{
		psCommand->psAcquireFence = sync_fence_fdget(i32AcquireFenceFd);
	}
	else
	{
		psCommand->psAcquireFence = NULL;
	}

	if (pi32ReleaseFenceFd)
	{
		psCommand->psReleaseFence = sync_fence_fdget(*pi32ReleaseFenceFd);
	}
	else
	{
		psCommand->psReleaseFence = NULL;
	}
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

	*ppvReadyData = psCommand->pvReadyData;
	*ppvCompleteData = psCommand->pvCompleteData;

	return PVRSRV_OK;
}

/*
	SCPSubmitCommand
*/
IMG_EXPORT 
PVRSRV_ERROR SCPSubmitCommand(SCP_CONTEXT *psContext)
{
	SCP_COMMAND *psCommand;

	if (psContext == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psCommand = (SCP_COMMAND *) ((IMG_UINT8 *) psContext->pvCCB
				+ psContext->ui32WriteOffset);

	SCP_DEBUG_PRINT("%s: Submit command %p for ctx %p", 
			__FUNCTION__, psCommand, psContext);

	_SCPInsert(psContext, psCommand->ui32CmdSize);

	return PVRSRV_OK;
}

/*
	SCPRun
*/
IMG_EXPORT
PVRSRV_ERROR SCPRun(SCP_CONTEXT *psContext)
{
	SCP_COMMAND *psCommand;
	PVRSRV_ERROR eError = PVRSRV_OK;


	if (psContext == NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	OSLockAcquire(psContext->hLock);
	while (psContext->ui32DepOffset != psContext->ui32WriteOffset)
	{
		psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
		            psContext->ui32DepOffset);

		/* See if the command is ready to go */
		eError = _SCPCommandReady(psCommand);

		SCP_DEBUG_PRINT("%s: Processes command %p for ctx %p (%d)", 
				__FUNCTION__, psCommand, psContext, eError);

		if (eError == PVRSRV_OK)
		{
			/* processed cmd so update queue */
			UPDATE_CCB_OFFSET(psContext->ui32DepOffset,
							  psCommand->ui32CmdSize,
							  psContext->ui32CCBSize);
		}
		else
		{
			/* As soon as we hit a command that can't run break out */
			break;
		}

		/* Run the command */
		_SCPCommandDo(psCommand);
	}
	OSLockRelease(psContext->hLock);

	return eError;
}

IMG_EXPORT
PVRSRV_ERROR SCPFlush(SCP_CONTEXT *psContext)
{
	if (psContext->ui32ReadOffset != psContext->ui32WriteOffset)
	{
		return PVRSRV_ERROR_RETRY;
	}

	return PVRSRV_OK;
}

/* This looks like a reasonable value. Number of traced syncs should
 * not exceed 20. */
#define MAX_TRACED_UFOS 20

/*
	SCPCommandComplete
*/
IMG_EXPORT
void SCPCommandComplete(SCP_CONTEXT *psContext,
                        IMG_BOOL bIgnoreFences)
{
	SCP_COMMAND *psCommand;
	IMG_UINT32 i;
	IMG_BOOL bContinue = IMG_TRUE;

	if (psContext == NULL)
	{
		return;
	}

	if (psContext->ui32ReadOffset == psContext->ui32DepOffset)
	{
		PVR_DPF((PVR_DBG_ERROR, "SCPCommandComplete: Called with nothing to do!"));
		return;
	}	

	while(bContinue)
	{
		psCommand = (SCP_COMMAND *) ((IMG_UINT8 *) psContext->pvCCB + 
					psContext->ui32ReadOffset);

		if (psCommand->ui32CmdType == SCP_COMMAND_CALLBACK)
		{
			RGX_HWPERF_UFO_DATA_ELEMENT asFenceSyncData[MAX_TRACED_UFOS];
			RGX_HWPERF_UFO_DATA_ELEMENT asUpdateSyncData[MAX_TRACED_UFOS];
			IMG_BOOL   bFenceFailed     = IMG_FALSE;
			IMG_UINT32 ui32FenceUFOIdx  = 0;
			IMG_UINT32 ui32UpdateUFOIdx = 0;

			/* Do any fence checks */
			if (bIgnoreFences == IMG_FALSE)
			{
				for (i=0;i<psCommand->ui32SyncCount;i++)
				{
					SCP_SYNC_DATA *psSCPSyncData = &psCommand->pasSCPSyncData[i];
					IMG_BOOL bFence = (psSCPSyncData->ui32Flags & SCP_SYNC_DATA_FENCE);
				
					if (bFence)
					{
						IMG_UINT32 ui32CurrentValue = ServerSyncGetValue(psSCPSyncData->psSync);
						IMG_UINT32 ui32SyncAddr;

						(void)ServerSyncGetFWAddr(psSCPSyncData->psSync, &ui32SyncAddr);
						PVR_ASSERT(ui32FenceUFOIdx < MAX_TRACED_UFOS);
						asFenceSyncData[ui32FenceUFOIdx].sUpdate.ui32FWAddr = ui32SyncAddr;
						asFenceSyncData[ui32FenceUFOIdx].sUpdate.ui32OldValue = ui32CurrentValue;
						asFenceSyncData[ui32FenceUFOIdx].sUpdate.ui32NewValue = psSCPSyncData->ui32Update;
						ui32FenceUFOIdx++;

						if (ui32CurrentValue != psSCPSyncData->ui32Fence)
						{
							bFenceFailed = IMG_TRUE;
						}
					}
				}

				if (bFenceFailed)
				{
					RGX_HWPERF_HOST_UFO(RGX_HWPERF_UFO_EV_CHECK_FAIL, asFenceSyncData, ui32FenceUFOIdx);
					return;
				}
				else
				{
					RGX_HWPERF_HOST_UFO(RGX_HWPERF_UFO_EV_CHECK_SUCCESS, asFenceSyncData, ui32FenceUFOIdx);
				}
			}
			
			/* Do any fence updates */
			for (i=0;i<psCommand->ui32SyncCount;i++)
			{
				SCP_SYNC_DATA *psSCPSyncData = &psCommand->pasSCPSyncData[i];
				IMG_BOOL bUpdate = (psSCPSyncData->ui32Flags & SCP_SYNC_DATA_UPDATE);

				if (bUpdate)
				{
					IMG_UINT32 ui32SyncAddr;

					(void)ServerSyncGetFWAddr(psSCPSyncData->psSync, &ui32SyncAddr);
					PVR_ASSERT(ui32UpdateUFOIdx < MAX_TRACED_UFOS);
					asUpdateSyncData[ui32UpdateUFOIdx].sUpdate.ui32FWAddr = ui32SyncAddr;
					asUpdateSyncData[ui32UpdateUFOIdx].sUpdate.ui32OldValue = ServerSyncGetValue(psSCPSyncData->psSync);
					asUpdateSyncData[ui32UpdateUFOIdx].sUpdate.ui32NewValue = psSCPSyncData->ui32Update;
					ui32UpdateUFOIdx++;
				}

				ServerSyncCompleteOp(psSCPSyncData->psSync, bUpdate, psSCPSyncData->ui32Update);

				if (bUpdate)
				{
					psSCPSyncData->ui32Flags = 0; /* Stop future interaction with this sync prim. */
					psSCPSyncData->psSync = NULL; /* Clear psSync as it is no longer referenced. */
				}
			}
			if (ui32UpdateUFOIdx > 0)
			{
				RGX_HWPERF_HOST_UFO(RGX_HWPERF_UFO_EV_UPDATE, asUpdateSyncData, ui32UpdateUFOIdx);
			}

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
			if (psCommand->psReleaseFence)
			{
				sw_sync_timeline_inc(psContext->pvTimeline, 1);
				/* Decrease the ref to this fence */
				sync_fence_put(psCommand->psReleaseFence);
				psCommand->psReleaseFence = NULL;
			}
#endif /* defined(SUPPORT_NATIVE_FENCE_SYNC) */

			bContinue = IMG_FALSE;
		}

		/* processed cmd so update queue */
		UPDATE_CCB_OFFSET(psContext->ui32ReadOffset,
						  psCommand->ui32CmdSize,
						  psContext->ui32CCBSize);

		SCP_DEBUG_PRINT("%s: Complete command %p for ctx %p (continue: %d)", 
				__FUNCTION__, psCommand, psContext, bContinue);

	}
}

IMG_EXPORT
IMG_BOOL SCPHasPendingCommand(SCP_CONTEXT *psContext)
{
	return psContext->ui32DepOffset != psContext->ui32WriteOffset;
}

IMG_EXPORT
void IMG_CALLCONV SCPDumpStatus(SCP_CONTEXT *psContext,
					DUMPDEBUG_PRINTF_FUNC *pfnDumpDebugPrintf,
					void *pvDumpDebugFile)
{
	PVR_ASSERT(psContext != NULL);

	/*
		Acquire the lock to ensure that the SCP isn't run while
		while we're dumping info
	*/
	OSLockAcquire(psContext->hLock);

	PVR_DUMPDEBUG_LOG("Pending command:");
	if (psContext->ui32DepOffset == psContext->ui32WriteOffset)
	{
		PVR_DUMPDEBUG_LOG("\tNone");
	}
	else
	{
		SCP_COMMAND *psCommand;
		IMG_UINT32 ui32DepOffset = psContext->ui32DepOffset;

		while (ui32DepOffset != psContext->ui32WriteOffset)
		{
		        /* Dump the command we're pending on */
		        psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
		                ui32DepOffset);

		        _SCPDumpCommand(psCommand, pfnDumpDebugPrintf, pvDumpDebugFile);

		        /* processed cmd so update queue */
		        UPDATE_CCB_OFFSET(ui32DepOffset,
		                                          psCommand->ui32CmdSize,
		                                          psContext->ui32CCBSize);

		}
	}

	PVR_DUMPDEBUG_LOG("Active command(s):");
	if (psContext->ui32DepOffset == psContext->ui32ReadOffset)
	{
		PVR_DUMPDEBUG_LOG("\tNone");
	}
	else
	{
		SCP_COMMAND *psCommand;
		IMG_UINT32 ui32ReadOffset = psContext->ui32ReadOffset;

		while (ui32ReadOffset != psContext->ui32DepOffset)
		{
			psCommand = (SCP_COMMAND *)((IMG_UINT8 *)psContext->pvCCB +
			            ui32ReadOffset);

			_SCPDumpCommand(psCommand, pfnDumpDebugPrintf, pvDumpDebugFile);

			/* processed cmd so update queue */
			UPDATE_CCB_OFFSET(ui32ReadOffset,
							  psCommand->ui32CmdSize,
							  psContext->ui32CCBSize);
		}
	}

	OSLockRelease(psContext->hLock);
}


/*
	SCPDestroy
*/
IMG_EXPORT
void IMG_CALLCONV SCPDestroy(SCP_CONTEXT *psContext)
{
	/*
		The caller must ensure that they completed all queued operations
		before calling this function
	*/
	
	PVR_ASSERT(psContext->ui32ReadOffset == psContext->ui32WriteOffset);

#if defined(SUPPORT_NATIVE_FENCE_SYNC)
	sync_timeline_destroy(psContext->pvTimeline);
#endif

	PVRSRVServerSyncRequesterUnregisterKM(psContext->psSyncRequesterID);
	OSLockDestroy(psContext->hLock);
	psContext->hLock = NULL;
	OSFreeMem(psContext->pvCCB);
	psContext->pvCCB = NULL;
	OSFreeMem(psContext);
}
