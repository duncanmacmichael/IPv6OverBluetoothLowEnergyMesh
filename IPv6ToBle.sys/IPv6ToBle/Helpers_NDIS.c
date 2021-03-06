/*++

Module Name:

	Helpers_NDIS.c

Abstract:

	This file contains the implementations for helper functions to work with
	NDIS 6.

Environment:

	Kernel-mode Driver Framework

--*/

#include "Includes.h"
#include "Helpers_NDIS.tmh" // auto-generated tracing file

//_Use_decl_annotations_
//NTSTATUS
//IPv6ToBleNDISRegisterAsIfProvider()
///*++
//Routine Description:
//
//    Registers this driver as a bare-minimum interface NDIS interface provider.    
//
//    This function simlpy registers this driver as an NDIS IF provider and gets
//    a NET_LUID index to identify us. This driver is not a miniport driver, so
//    the system Tcpip.sys driver won't bind to us and we aren't heavy enough to
//    actually do network I/O. That is fine for the purposes of being a filter
//    driver. This process is only required for inbound packet injection into the
//    network stack.
//
//    Note: the IF provider handle must be freed during driver unload.
//
//Arguments:
//
//    None. Accesses global variables defined in Driver.h for the NDIS IF
//    provider handle and NET_LUID index.
//
//Return Value:
//
//    NDIS_STATUS_SUCCESS if successful, appropriate NTSTATUS error codes 
//    otherwise.
//
//--*/
//{
//    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Entry");
//
//    NDIS_STATUS ndisStatus = NDIS_STATUS_SUCCESS;
//
//    //
//    // Step 1
//    // Allocate and initialize an interface characteristics structure
//    //
//    NDIS_IF_PROVIDER_CHARACTERISTICS providerCharacteristics;
//    
//    providerCharacteristics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
//    providerCharacteristics.Header.Revision = NDIS_OBJECT_REVISION_1;
//    providerCharacteristics.Header.Size = NDIS_SIZEOF_IF_PROVIDER_CHARACTERISTICS_REVISION_1;
//
//    // Set handlers for interface query and set operations to NULL because we
//    // are a light-weight interface that doesn't actually handle network I/O
//    providerCharacteristics.QueryObjectHandler = NULL;
//    providerCharacteristics.SetObjectHandler = NULL;
//
//    //
//    // Step 2
//    // Register as an interface provider
//    //
//    ndisStatus = NdisIfRegisterProvider(&providerCharacteristics,
//                                        NULL,
//                                        &gNdisIfProviderHandle
//                                        );
//    if (ndisStatus != NDIS_STATUS_SUCCESS)
//    {
//        TraceEvents(TRACE_LEVEL_ERROR, TRACE_HELPERS_NDIS, "Registering as an NDIS interface provider failed %!STATUS!", (NTSTATUS)ndisStatus);
//        goto Exit;
//    }
//
//    //
//    // Step 3
//    // Request for NDIS to give us a 24-bit NET_LUID index if we don't have one
//    // already
//    //
//
//
//Exit:
//
//    // Deregister the IF provider interface handle if we failed after 
//    // registration
//    if (ndisStatus != NDIS_STATUS_SUCCESS)
//    {
//        if (gNdisIfProviderHandle)
//        {
//            NdisIfDeregisterProvider(gNdisIfProviderHandle);
//        }
//    }
//
//    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Exit");
//
//    return (NTSTATUS)ndisStatus;
//}

_Use_decl_annotations_
NTSTATUS
IPv6ToBleNDISPoolDataCreate(
	_Out_    NDIS_POOL_DATA*	ndisPoolData,
	_In_opt_ UINT32				memoryTag
)
/*++
Routine Description:

	Creates NDIS memory pool information required for allocating
	NET_BUFFER_LIST structures, which are required to translate the user mode
	data to kernel mode data.

	This function is heavily based on the "KrnlHlprNDISPoolDataCreate" helper
	function in the WFPSAMPLER sample driver from Microsoft.

Arguments:

	ndisPoolData - the ndisPoolData to create. This includes information for
	the pools from which to allocate NET_BUFFER_LISTs and NET_BUFFERs.

Return Value:

	STATUS_SUCCESS if successful, appropriate NTSTATUS error codes otherwise.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Entry");


	//NT_ASSERT(ndisPoolData);

	NTSTATUS status = STATUS_SUCCESS;

	//
	// Step 1
	// Allocate the memory for the pool data structure. Use nonpaged pool
	// because this memory is for a network data packet that cannot be paged
	// out (will be used in OS operations).
	//
	ndisPoolData = (NDIS_POOL_DATA*)ExAllocatePoolWithTag(NonPagedPoolNx,
		                                                 sizeof(NDIS_POOL_DATA),
		                                                 memoryTag
	                                                     );
	if (!ndisPoolData) {
		status = STATUS_INSUFFICIENT_RESOURCES;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_HELPERS_NDIS, "NDIS_POOL_DATA memory allocation failed %!STATUS!", status);
		goto Exit;
	}

	//
	// Step 2
	// Populate the pools that the pool data structure contains
	//
	status = IPv6ToBleNDISPoolDataPopulate(ndisPoolData, memoryTag);
	if (!NT_SUCCESS(status) && ndisPoolData != NULL)
	{
		IPv6ToBleNDISPoolDataDestroy(ndisPoolData);
	}

Exit:

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Exit");

	return status;
}

_Use_decl_annotations_
NTSTATUS
IPv6ToBleNDISPoolDataPopulate(
	_Inout_	 NDIS_POOL_DATA*	ndisPoolData,
	_In_opt_ UINT32				memoryTag
)
/*++
Routine Description:

	Populates an NDIS_POOL_DATA structure with the NET_BUFFER_LIST_POOL and
	the NET_BUFFER_POOL.

	This function is heavily based on the "KrnlHlprNDISPoolDataPopulate" helper
	function in the WFPSAMPLER sample driver from Microsoft.

Arguments:

	ndisPoolData - the ndisPoolData to populate. This includes information for
	the pools from which to allocate NET_BUFFER_LISTs and NET_BUFFERs.

Return Value:

	STATUS_SUCCESS if successful, appropriate NTSTATUS error codes otherwise.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Entry");

	NT_ASSERT(ndisPoolData);

	NTSTATUS status = STATUS_SUCCESS;

	// Parameters structures for the NBL pool and NB pool
	NET_BUFFER_LIST_POOL_PARAMETERS nblPoolParameters = { 0 };
	NET_BUFFER_POOL_PARAMETERS nbPoolParameters = { 0 };

	//
	// Step 1
	// Allocate the NDIS handle for the NDIS_POOL_DATA structure
	//
	ndisPoolData->ndisHandle = NdisAllocateGenericObject(0,
														 memoryTag,
														 0
														 );
	if (ndisPoolData->ndisHandle == 0)
	{
		status = STATUS_INVALID_HANDLE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_HELPERS_NDIS, "NDIS_POOL_DATA main NDIS handle allocation failed %!STATUS!", status);
		goto Exit;
	}

	//
	// Step 2
	// Configure the NBL pool parameters and allocate the NBL pool
	//
	nblPoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
	nblPoolParameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	nblPoolParameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	
	// If fAllocateNetBuffer is true and DataSize is 0, NDIS allocates the
	// NET_BUFFER but not the data buffer. We build the data buffer later
	// with a memory descriptor list that is constructed from the byte array
	// passed to us by usermode.
	nblPoolParameters.fAllocateNetBuffer = TRUE;
	nblPoolParameters.DataSize = 0;
	nblPoolParameters.PoolTag = memoryTag;

	ndisPoolData->nblPoolHandle = NdisAllocateNetBufferListPool(
		                            ndisPoolData->ndisHandle,
		                            &nblPoolParameters
	                              );

	if (ndisPoolData->nblPoolHandle == 0)
	{
	    status = STATUS_INVALID_HANDLE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_HELPERS_NDIS, "NDIS_POOL_DATA NBL pool handle allocation failed %!STATUS!", status);
	    goto Exit;
	}

	//
	// Step 3
	// Configure the NB pool parameters and allocate the NB pool
	// 
	nbPoolParameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
	nbPoolParameters.Header.Revision = NET_BUFFER_POOL_PARAMETERS_REVISION_1;
	nbPoolParameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_POOL_PARAMETERS_REVISION_1;
	nbPoolParameters.PoolTag = memoryTag;
	nbPoolParameters.DataSize = 0;

	ndisPoolData->nbPoolHandle = NdisAllocateNetBufferPool(
		                             ndisPoolData->ndisHandle,
		                             &nbPoolParameters
	                             );

	if (ndisPoolData->nbPoolHandle == 0)
	{
		status = STATUS_INVALID_HANDLE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_HELPERS_NDIS, "NDIS_POOL_DATA NB pool handle allocation failed %!STATUS!", status);
		goto Exit;
	}

Exit:

	if (!NT_SUCCESS(status))
	{
		IPv6ToBleNDISPoolDataPurge(ndisPoolData);
	}

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Exit");

	return status;
}

_Use_decl_annotations_
inline VOID
IPv6ToBleNDISPoolDataDestroy(
	_Inout_ NDIS_POOL_DATA*	ndisPoolData
)
/*++
Routine Description:

	Cleans up an NDIS_POOL_DATA structure. Calls IPv6ToBleNDISPoolDataPurge()
	to clean up the memory pools it contains first, then cleans up the
	NDIS_POOL_DATA structure itself.

	This function is heavily based on the "KrnlHlprNDISPoolDataDestroy" helper
	function in the WFPSAMPLER sample driver from Microsoft.

Arguments:

	ndisPoolData - the ndisPoolData to destroy. This includes information for
	the pools from which to allocate NET_BUFFER_LISTs and NET_BUFFERs.

Return Value:

	None.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Entry");

	//NT_ASSERT(ndisPoolData);

	if (ndisPoolData)
	{
		// Clean up the pools
		IPv6ToBleNDISPoolDataPurge(ndisPoolData);

		// Clean up the structure
		ExFreePoolWithTag(ndisPoolData, IPV6_TO_BLE_NDIS_TAG);
		ndisPoolData = 0;
	}

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Exit");

	return;
}

_Use_decl_annotations_
inline VOID
IPv6ToBleNDISPoolDataPurge(
	_Inout_	NDIS_POOL_DATA*	ndisPoolData
)
/*++
Routine Description:

	Cleans up memory pools inside an NDIS_POOL_DATA structure.

	This function is heavily based on the "KrnlHlprNDISPoolDataPurge" helper
	function in the WFPSAMPLER sample driver from Microsoft.

Arguments:

	ndisPoolData - the ndisPoolData to destroy. This includes information for
	the pools from which to allocate NET_BUFFER_LISTs and NET_BUFFERs.

Return Value:

	None.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Entry");

	//NT_ASSERT(ndisPoolData);

	if (ndisPoolData->ndisHandle)
	{
		// Free the NB and NBL pools
		if (ndisPoolData->nbPoolHandle)
		{
			NdisFreeNetBufferPool(ndisPoolData->nbPoolHandle);
			ndisPoolData->nbPoolHandle = 0;
		}

		if (ndisPoolData->nblPoolHandle)
		{
			NdisFreeNetBufferListPool(ndisPoolData->nblPoolHandle);
			ndisPoolData->nblPoolHandle = 0;
		}

		// Free the NDIS handle for the pools
		NdisFreeGenericObject(ndisPoolData->ndisHandle);
		ndisPoolData->ndisHandle = 0;
	}

	// Zero the memory for the structure
	RtlZeroMemory(ndisPoolData, sizeof(NDIS_POOL_DATA));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_HELPERS_NDIS, "%!FUNC! Exit");

	return;
}