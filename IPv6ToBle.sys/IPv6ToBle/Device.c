/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.
    
Environment:

    Kernel-mode Driver Framework

--*/

#include "Includes.h"
#include "device.tmh"   // auto-generated tracing file

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, IPv6ToBleControlDeviceCreate)
#endif

_Use_decl_annotations_
NTSTATUS
IPv6ToBleControlDeviceCreate(
    _In_	WDFDRIVER		Driver
)
/*++

Routine Description:

    Routine called to create a device and its software resources.

Arguments:

	Driver - The WDF driver object to which to associate this device.

Return Value:

    STATUS_SUCCESS if the device was successfully created and initialized;
    other appropriate NTSTATUS error codes otherwise.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

	NTSTATUS status = STATUS_SUCCESS;

#if DBG
    KIRQL irql = KeGetCurrentIrql();
#endif // DBG

	// Objects for device creation
    WDF_OBJECT_ATTRIBUTES deviceAttributes;    
	PWDFDEVICE_INIT deviceInit;
	PIPV6_TO_BLE_DEVICE_CONTEXT deviceContext;    

	//
	// Step 1
	// Prepare for device creation
	//

    PAGED_CODE();

	// Initialize the device attributes with the context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, 
											IPV6_TO_BLE_DEVICE_CONTEXT
											);

	// Set the device cleanup callback for when the device is unloaded
	deviceAttributes.EvtCleanupCallback = IPv6ToBleEvtDeviceCleanup;

	// Allocate the device initialization structure
	deviceInit = WdfControlDeviceInitAllocate(Driver,
											  &SDDL_DEVOBJ_KERNEL_ONLY
											  );
	
	if (!deviceInit)
	{
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Device init allocation failed %!STATUS!", status);
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Exit;
	}

	// Set the device type
	WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_NETWORK);

	// Set the security descriptor characteristics
	WdfDeviceInitSetCharacteristics(deviceInit, 
									FILE_DEVICE_SECURE_OPEN,
									FALSE
									);

    // Previous two functions can be called at DISPATCH_LEVEL; verify IRQL
    // did not change
    NT_ASSERT(irql == KeGetCurrentIrql());

	// Define a native name for the device and assign it to the device
	DECLARE_CONST_UNICODE_STRING(nativeDeviceName, L"\\Device\\IPv6ToBle");

	status = WdfDeviceInitAssignName(deviceInit, &nativeDeviceName);
	
	if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Device init assigning native device name failed %!STATUS!", status);
        WdfDeviceInitFree(deviceInit);
		goto Exit;
	}

    NT_ASSERT(irql == KeGetCurrentIrql());

	//
	// Step 2 
	// Create the framework device object
	//
	status = WdfDeviceCreate(&deviceInit, 
							 &deviceAttributes,
							 &globalWdfDeviceObject
							 );
	
	if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Device creation failed %!STATUS!", status);
		WdfDeviceInitFree(deviceInit);
		goto Exit;
	}

	// Finish initializing the control device object
	WdfControlFinishInitializing(globalWdfDeviceObject);	

    NT_ASSERT(irql == KeGetCurrentIrql());

	//
	// Step 3
	// Make the device accessible to usermode apps
	//

	// Define a friendly name for user mode apps to access the device
	DECLARE_CONST_UNICODE_STRING(userDeviceName, L"\\Global??\\IPv6ToBle");

	// Create a symbolic link to the created device object, as one way that
	// usermode apps can talk to us
	status = WdfDeviceCreateSymbolicLink(globalWdfDeviceObject, 
										 &userDeviceName
										 );
	
	if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Device creating symbolic link failed %!STATUS!", status);
		goto Exit;
	}

	//
	// Step 4
	// Initialize the device context
	// 

	// Get the context
	deviceContext = IPv6ToBleGetContextFromDevice(globalWdfDeviceObject);

    //
    // Initialize the spin locks
    //

    // Listen request queue spinlock
    WDF_OBJECT_ATTRIBUTES listenRequestQueueLockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&listenRequestQueueLockAttributes);
    listenRequestQueueLockAttributes.ParentObject = globalWdfDeviceObject;

    status = WdfSpinLockCreate(&listenRequestQueueLockAttributes,
                               &deviceContext->listenRequestQueueLock
                               );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Creating listen request queue spin lock failed %!STATUS!", status);
        goto Exit;
    }

#ifdef BORDER_ROUTER

    // White list spinlock
    WDF_OBJECT_ATTRIBUTES whiteListModifiedLockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&whiteListModifiedLockAttributes);
    whiteListModifiedLockAttributes.ParentObject = globalWdfDeviceObject;

    status = WdfSpinLockCreate(&whiteListModifiedLockAttributes,
                               &deviceContext->whiteListModifiedLock
                               );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Creating white list modified spin lock failed %!STATUS!", status);
        goto Exit;
    }

    // Mesh list spinlock
    WDF_OBJECT_ATTRIBUTES meshListModifiedLockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&meshListModifiedLockAttributes);
    meshListModifiedLockAttributes.ParentObject = globalWdfDeviceObject;

    status = WdfSpinLockCreate(&meshListModifiedLockAttributes,
                               &deviceContext->meshListModifiedLock
                               );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Creating mesh list modified spin lock failed %!STATUS!", status);
        goto Exit;
    }

    NT_ASSERT(irql == KeGetCurrentIrql());

    // Set that callouts are not registered yet
    deviceContext->calloutsRegistered = FALSE;

    // Initialize the list heads
    InitializeListHead(deviceContext->whiteListHead);
    InitializeListHead(deviceContext->meshListHead);

    // Initialize the list booleans
    deviceContext->whiteListModified = FALSE;
    deviceContext->meshListModified = FALSE;

#endif  // BORDER_ROUTER

	// Create the NDIS pool data structure, which also populates it
	status = IPv6ToBleNDISPoolDataCreate(deviceContext->ndisPoolData,
										 IPV6_TO_BLE_NDIS_TAG
										 );
	if (!NT_SUCCESS(status) && deviceContext->ndisPoolData != NULL)
	{
		IPv6ToBleNDISPoolDataDestroy(deviceContext->ndisPoolData);
		goto Exit;
	}

    NT_ASSERT(irql == KeGetCurrentIrql());

	//
	// Step 5
	// Initialize the I/O queues
	//
	status = IPv6ToBleQueuesInitialize(globalWdfDeviceObject);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    //
    // Step 6
    // Initialize the timer object for flushing the runtime lists to the
    // registry periodically (if they've changed)
    //
    // Note: this only applies on the gateway device.
    //

#ifdef BORDER_ROUTER

    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES timerAttributes;

    // Initialize the timer configuration object with the timer event callback
    // and a period of 5 seconds (5000 milliseconds)
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig,
                                   IPv6ToBleDeviceTimerCheckAndFlushLists,
                                   5000
                                   );

    // Set the framework to automatically synchronize this with callbacks under
    // the parent object (the device), at least at DISPATCH_LEVEL
    timerConfig.AutomaticSerialization = TRUE;

    // Initialize the timer attributes to make the device object its parent
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = globalWdfDeviceObject;

    // Create the timer
    status = WdfTimerCreate(&timerConfig, 
                            &timerAttributes, 
                            &deviceContext->registryTimer
                            );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Timer creation failed failed %!STATUS!", status);
        goto Exit;
    }

    // Start the timer
    WdfTimerStart(deviceContext->registryTimer, WDF_REL_TIMEOUT_IN_MS(5000));

#endif  // BORDER_ROUTER

Exit:	

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");

    return status;
}

_Use_decl_annotations_
VOID
IPv6ToBleEvtDeviceCleanup(
	WDFOBJECT Object
)
/*++

Routine Description:

	Routine called to free any memory allocated in the device's context space.
	This is called when the device is unloaded.

Arguments:

	Device - the main WDF control device for this driver.

Return Value:

	None.

--*/
{
    UNREFERENCED_PARAMETER(Object);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

#ifdef BORDER_ROUTER

	// Clean up the runtime lists
	IPv6ToBleRuntimeListDestroyWhiteList();
	IPv6ToBleRuntimeListDestroyMeshList();

#endif  // BORDER_ROUTER

	// Clean up the NDIS memory pool data structure in the device context
	PIPV6_TO_BLE_DEVICE_CONTEXT deviceContext = IPv6ToBleGetContextFromDevice(
		                                            globalWdfDeviceObject
	                                            );
	IPv6ToBleNDISPoolDataDestroy(deviceContext->ndisPoolData);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
}

_Use_decl_annotations_
VOID
IPv6ToBleDeviceTimerCheckAndFlushLists(
    _In_    WDFTIMER    Timer
)
/*++
Routine Description:

    The framework calls this timer function every 5  seconds to check if the 
    runtime lists have changed. If they have, flush the runtime lists to the 
    registry.

    This behavior is to prevent loss of state; the driver generally works with
    the runtime lists so it doesn't have to open and close the registry keys 
    all the time, but if the lists are modified during runtime then we need to 
    save that state in the registry at some point.
    
    Since there is no way to guarantee that you will be able to flush to the
    registry once during device or driver unload, such as an unexpected
    shutdown, the solution is to flush every 5 seconds, and only if the lists
    have been modified. This is not a major load on the system.

    This function is called at DISPATCH_LEVEL. If the lists have changed, it
    queues a work item to assign the list to the registry at PASSIVE_LEVEL.

Arguments:

    Timer - the WDF timer object created during device creation

Return Value:

    None. The two functions called by this callback do return NTSTATUS, so if
    they fail we log the error and continue because this function returns VOID.

--*/
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TIMER, "%!FUNC! Entry");

    UNREFERENCED_PARAMETER(Timer);

#if DBG
    KIRQL irql = KeGetCurrentIrql();
#endif // DBG

    // Get the device context
    PIPV6_TO_BLE_DEVICE_CONTEXT deviceContext = IPv6ToBleGetContextFromDevice(
                                                    globalWdfDeviceObject
                                                );

    //
    // Step 1
    // Flush the white list if it has changed by scheduling a PASSIVE_LEVEL
    // system worker thread. We use a system worker thread because assigning
    // the list to the registry is expected to be very infrequent and doesn't
    // take long to do (no delayed processing, etc.).
    //
    WdfSpinLockAcquire(deviceContext->whiteListModifiedLock);
    if (deviceContext->whiteListModified)
    {
        PIO_WORKITEM workItem = IoAllocateWorkItem(globalWdmDeviceObject);
        if (workItem)
        {
            IoQueueWorkItemEx(workItem,
                              IPv6ToBleRegistryFlushWhiteListWorkItemEx,
                              DelayedWorkQueue,
                              NULL
                              );
        }        
    }
    WdfSpinLockRelease(deviceContext->whiteListModifiedLock);

    NT_ASSERT(irql == KeGetCurrentIrql());

    //
    // Step 2
    // Flush the mesh list if it has changed, also with a system worker thread.
    //
    WdfSpinLockAcquire(deviceContext->meshListModifiedLock);
    if (deviceContext->meshListModified)
    {
        PIO_WORKITEM workItem = IoAllocateWorkItem(globalWdmDeviceObject);
        if (workItem)
        {
            IoQueueWorkItemEx(workItem,
                              IPv6ToBleRegistryFlushMeshListWorkItemEx,
                              DelayedWorkQueue,
                              NULL
                              );
        }
    }
    WdfSpinLockRelease(deviceContext->meshListModifiedLock);

    NT_ASSERT(irql == KeGetCurrentIrql());

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TIMER, "%!FUNC! Exit");

    return;
}