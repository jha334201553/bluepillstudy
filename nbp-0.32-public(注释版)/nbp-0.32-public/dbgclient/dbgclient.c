/*
*  ���ڽ���bluepill��־����ӡģ��
*/
#include "dbgclient.h"
#include "dbgclient_ioctl.h"

LIST_ENTRY	g_DebugWindowsList;  // ��Ϣ�б� 
KMUTEX	g_DebugWindowsListMutex;   // ���� g_DebugWindowsList �б��� 
KEVENT	g_ShutdownEvent;        // �رձ��������¼� 
PETHREAD	g_pScanWindowsThread = NULL; // �� bluepill ������־�߳� 

PUCHAR	g_pDebugString = NULL;  // ��־�ַ�����ʱ�ڴ�(�޸�ʱע����߳���ռ����) 

//////////////////////////////////////////////////////////////////////////
// ���� g_DebugWindowsList �б�DbgPrint ��ӡ��־��Ϣ 
//////////////////////////////////////////////////////////////////////////
static VOID PrintData()
{
	PDEBUG_WINDOW_ENTRY	pRegisteredWindow;
	PUCHAR	pData,pString;
	ULONG	i,uWindowSize;


	KeWaitForSingleObject(&g_DebugWindowsListMutex, Executive, KernelMode, FALSE, NULL);

    // �����б� 
//     for (
//         PDEBUG_WINDOW_ENTRY	pRegisteredWindow = (PDEBUG_WINDOW_ENTRY)g_DebugWindowsList.Flink;
//         pRegisteredWindow != (PDEBUG_WINDOW_ENTRY)&g_DebugWindowsList;
//         pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)pRegisteredWindow->le.Flink
//         ) 
	pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)g_DebugWindowsList.Flink;
	while (pRegisteredWindow != (PDEBUG_WINDOW_ENTRY)&g_DebugWindowsList) {

		pRegisteredWindow = CONTAINING_RECORD(pRegisteredWindow, DEBUG_WINDOW_ENTRY, le);
		pData = pRegisteredWindow->DebugWindow.pWindowVA;

		if (pData[0]) {
			uWindowSize = min(pRegisteredWindow->DebugWindow.uWindowSize-1, DEBUG_WINDOW_IN_PAGES*PAGE_SIZE); // ��Խ�� 
			RtlCopyMemory(g_pDebugString, &pData[1], uWindowSize); // �Ȱ������ַ������ݿ����� g_pDebugString �У�����ɨ�账�� 
			pData[0] = 0;

			pString = g_pDebugString; // pString ��Զָ����һ��Ҫ��ӡ���ַ������׵�ַ 
			for (i=0;i<uWindowSize,g_pDebugString[i];i++) {
                // ����л��з����򵥶�һ����� 
				if (g_pDebugString[i]==0x0a) {
					g_pDebugString[i]=0;
					DbgPrint("<%02X>:  %s\n",pRegisteredWindow->DebugWindow.bBpId,pString);
					pString = &g_pDebugString[i+1];
				}
			}
            // �ַ������Ի��з���β�����ٴ�ӡ�����ַ��� 
			if (*pString)
				DbgPrint("<%02X>:  %s\n", pRegisteredWindow->DebugWindow.bBpId, &pString);

			RtlZeroMemory(g_pDebugString, DEBUG_WINDOW_IN_PAGES*PAGE_SIZE);
		}
		pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)pRegisteredWindow->le.Flink;
	}
	KeReleaseMutex(&g_DebugWindowsListMutex,FALSE);	
}

//////////////////////////////////////////////////////////////////////////
// ��־��ӡ�̣߳� 10ms ����һ�� PrintData() ��ӡ��־ 
//////////////////////////////////////////////////////////////////////////
static VOID NTAPI ScanWindowsThread(PVOID Param)
{
	LARGE_INTEGER	Interval;
	Interval.QuadPart = RELATIVE(MILLISECONDS(10));

	while (STATUS_TIMEOUT==KeWaitForSingleObject(
							&g_ShutdownEvent,
							Executive,
							KernelMode,
							FALSE,
							&Interval)) {
		PrintData();
	}
	DbgPrint("ScanWindowsThread(): Shutting down\n");
	PsTerminateSystemThread(STATUS_SUCCESS);
}

///////////////////////////////////////////////////////////////////////////////
// ���� IOCTL_REGISTER_WINDOW ע��(����)��Ϣ, IOCTL_UNREGISTER_WINDOW ע����Ϣ 
///////////////////////////////////////////////////////////////////////////////
NTSTATUS DeviceControl(
			IN PFILE_OBJECT pFileObject,
			IN PVOID pInputBuffer,
			IN ULONG uInputBufferLength,
			OUT PVOID pOutputBuffer,
			IN ULONG uOutputBufferLength,
			IN ULONG uIoControlCode,
			OUT PIO_STATUS_BLOCK pIoStatusBlock,
			IN PDEVICE_OBJECT pDeviceObject)
{
	PDEBUG_WINDOW	pDebugWindow=pInputBuffer;
	PDEBUG_WINDOW_ENTRY	pDwe,pRegisteredWindow;
	BOOLEAN	bFound;
	NTSTATUS	Status = STATUS_SUCCESS;


	switch (uIoControlCode) {
		case IOCTL_REGISTER_WINDOW:

			if (!pInputBuffer || uInputBufferLength!=sizeof(DEBUG_WINDOW) || pDebugWindow->pWindowVA<MM_SYSTEM_RANGE_START) {
				pIoStatusBlock->Status=STATUS_INVALID_DEVICE_REQUEST;
				break;
			}

			pDwe = ExAllocatePool(PagedPool, sizeof(DEBUG_WINDOW_ENTRY));
			if (!pDwe) {
				pIoStatusBlock->Status=STATUS_INSUFFICIENT_RESOURCES;
				break;
			}

			pDwe->DebugWindow = *pDebugWindow;

			KeWaitForSingleObject(&g_DebugWindowsListMutex,Executive,KernelMode,FALSE,NULL);

            // �ȼ�����Ϣ(ͨ��pWindowVA��Ϣ�ڴ��ַ�ж�)�ǲ����Ѿ��洢�� g_DebugWindowsList �б��У�����Ѿ����������� 
			bFound=FALSE;
			pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)g_DebugWindowsList.Flink;
			while (pRegisteredWindow!=(PDEBUG_WINDOW_ENTRY)&g_DebugWindowsList) {
				pRegisteredWindow=CONTAINING_RECORD(pRegisteredWindow,DEBUG_WINDOW_ENTRY,le);

				if (pRegisteredWindow->DebugWindow.pWindowVA == pDebugWindow->pWindowVA) {
					bFound=TRUE;
					break;
				}
				pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)pRegisteredWindow->le.Flink;
			}

            // g_DebugWindowsList �в����ڣ�������б� 
			if (!bFound) {
                // MDL�ڴ���ӳ�� 
				pDwe->pWindowMdl = IoAllocateMdl(pDwe->DebugWindow.pWindowVA, pDwe->DebugWindow.uWindowSize, FALSE, FALSE, NULL);
				if (!pDwe) {
					ExFreePool(pDwe);
					Status=STATUS_INSUFFICIENT_RESOURCES;
				} else {
					try {
						MmProbeAndLockPages(pDwe->pWindowMdl,KernelMode,IoReadAccess);
						InsertTailList(&g_DebugWindowsList,&pDwe->le);

						DbgPrint("dbgclient: NBP <%02X> registered, window at 0x%p, size: 0x%X\n",
							pDwe->DebugWindow.bBpId,
							pDwe->DebugWindow.pWindowVA,
							pDwe->DebugWindow.uWindowSize);

					} except(EXCEPTION_EXECUTE_HANDLER) {
						Status=STATUS_UNSUCCESSFUL;
						ExFreePool(pDwe);
					}
				}
			} else
				ExFreePool(pDwe);

			KeReleaseMutex(&g_DebugWindowsListMutex,FALSE);

			pIoStatusBlock->Information=0;
			pIoStatusBlock->Status=Status;
			break;

		case IOCTL_UNREGISTER_WINDOW:


			if (!pInputBuffer || uInputBufferLength!=sizeof(DEBUG_WINDOW)) {
				pIoStatusBlock->Status=STATUS_INVALID_DEVICE_REQUEST;
				break;
			}

			PrintData();

			KeWaitForSingleObject(&g_DebugWindowsListMutex,Executive,KernelMode,FALSE,NULL);

			bFound=FALSE;
			pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)g_DebugWindowsList.Flink;
			while (pRegisteredWindow!=(PDEBUG_WINDOW_ENTRY)&g_DebugWindowsList) {
				pRegisteredWindow=CONTAINING_RECORD(pRegisteredWindow,DEBUG_WINDOW_ENTRY,le);

				if (pRegisteredWindow->DebugWindow.pWindowVA==pDebugWindow->pWindowVA) {
					RemoveEntryList(&pRegisteredWindow->le);

					MmUnlockPages(pRegisteredWindow->pWindowMdl);
					IoFreeMdl(pRegisteredWindow->pWindowMdl);

					ExFreePool(pRegisteredWindow);
					bFound=TRUE;

					DbgPrint("dbgclient: NBP <%02X> unregistered\n",pRegisteredWindow->DebugWindow.bBpId);
					break;
				}

				pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)pRegisteredWindow->le.Flink;
			}
			KeReleaseMutex(&g_DebugWindowsListMutex,FALSE);

			pIoStatusBlock->Information=0;

			if (!bFound) {
				pIoStatusBlock->Status=STATUS_UNSUCCESSFUL;
			} else {
				pIoStatusBlock->Status=STATUS_SUCCESS;
			}
			break;
		default:
			pIoStatusBlock->Status=STATUS_INVALID_DEVICE_REQUEST;
	}

	return pIoStatusBlock->Status;
}

//////////////////////////////////////////////////////////////////////////
// ����IRP���ȷַ�����(ֻ��Ӧ IRP_MJ_DEVICE_CONTROL ����)
//////////////////////////////////////////////////////////////////////////
NTSTATUS DriverDispatcher(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	PIO_STACK_LOCATION	pIrpStack;
	PVOID	pInputBuffer,pOutputBuffer;
	ULONG	uInputBufferLength,uOutputBufferLength,uIoControlCode;
	NTSTATUS	Status;


	Status=pIrp->IoStatus.Status=STATUS_SUCCESS;
	pIrp->IoStatus.Information=0;

	pIrpStack=IoGetCurrentIrpStackLocation(pIrp);

	pInputBuffer             = pIrp->AssociatedIrp.SystemBuffer;
	uInputBufferLength       = pIrpStack->Parameters.DeviceIoControl.InputBufferLength;
	pOutputBuffer            = pIrp->AssociatedIrp.SystemBuffer;
	uOutputBufferLength      = pIrpStack->Parameters.DeviceIoControl.OutputBufferLength;
	uIoControlCode           = pIrpStack->Parameters.DeviceIoControl.IoControlCode;

	switch (pIrpStack->MajorFunction) {
		case IRP_MJ_DEVICE_CONTROL:
			Status=DeviceControl(
					pIrpStack->FileObject,
					pInputBuffer,
					uInputBufferLength,
					pOutputBuffer,
					uOutputBufferLength,
					uIoControlCode,
					&pIrp->IoStatus,
					pDeviceObject);
		break;
	}

	IoCompleteRequest(pIrp,IO_NO_INCREMENT);
	return Status;
}


VOID NTAPI DriverUnload(IN PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING	DeviceLink;
	PDEBUG_WINDOW_ENTRY	pRegisteredWindow;

    // ���ùر���־�¼�,Ȼ��ȴ� ScanWindowsThread �߳��˳� 
	KeSetEvent(&g_ShutdownEvent,0,FALSE);
	if (g_pScanWindowsThread) {
		KeWaitForSingleObject(g_pScanWindowsThread,Executive,KernelMode,FALSE,NULL);
		ObDereferenceObject(g_pScanWindowsThread);
	}

    // ɾ���������� 
	RtlInitUnicodeString(&DeviceLink,L"\\DosDevices\\itldbgclient");
	IoDeleteSymbolicLink(&DeviceLink);
    // ɾ���豸���� 
	if (DriverObject->DeviceObject)
		IoDeleteDevice(DriverObject->DeviceObject);

    // ��û���յ� IOCTL_UNREGISTER_WINDOW �������ж����������Ҫ������Դ 
    // ��ӡû�д�ӡ����־��Ϣ 
	PrintData();
    // �ͷ� g_DebugWindowsList ��Ϣ�б��ڴ� 
	KeWaitForSingleObject(&g_DebugWindowsListMutex,Executive,KernelMode,FALSE,NULL);
	while (!IsListEmpty(&g_DebugWindowsList)) {
		pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)RemoveHeadList(&g_DebugWindowsList);
		pRegisteredWindow=CONTAINING_RECORD(pRegisteredWindow,DEBUG_WINDOW_ENTRY,le);

		MmUnlockPages(pRegisteredWindow->pWindowMdl);
		IoFreeMdl(pRegisteredWindow->pWindowMdl);

		ExFreePool(pRegisteredWindow);
	}
	KeReleaseMutex(&g_DebugWindowsListMutex,FALSE);

	if (g_pDebugString)
		ExFreePool(g_pDebugString);

	DbgPrint("dbgclient: Shut down\n");
}

// BluePill Debug��Ϣ�������ߣ���ע�������ķ�ʽ���֣�Debug��Ϣ�������߻����Ϣд��\\Device\\itldbgclient�Ĺ����ڴ�ռ���
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS	Status;
	UNICODE_STRING	DeviceLink,DeviceName;
	PDEVICE_OBJECT	pDeviceObject;
	HANDLE	hThread;

	DriverObject->DriverUnload=DriverUnload;

	RtlInitUnicodeString(&DeviceName,L"\\Device\\itldbgclient"); // �����������豸������豸��common/dbgclient.c��Ҳ�У���Ϊ�Ǹ����ж�����豸�ķ���debug��Ϣ������
	RtlInitUnicodeString(&DeviceLink,L"\\DosDevices\\itldbgclient");

	g_pDebugString=ExAllocatePool(PagedPool,DEBUG_WINDOW_IN_PAGES*PAGE_SIZE);
	if (!g_pDebugString) {
		DbgPrint("dbgclient: Failed to allocate %d bytes for debug window buffer\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// The IoCreateDevice routine creates a device object for use by a driver.
    // IoCreateDevice ��������һ��������ʹ�õ��豸���� 
	Status=IoCreateDevice(DriverObject,0,&DeviceName,DBGCLIENT_DEVICE,0,FALSE,&pDeviceObject);
	if (!NT_SUCCESS(Status)) {
		DbgPrint("dbgclient: IoCreateDevice() failed with status 0x%08X\n",Status);
		return Status;
	}

    // IoCreateSymbolicLink ����һ���������ӣ���Ӧ�ò�ʹ�� (bluepill��Ŀδʹ�ã��˴�����Բ���Ҫ) 
	Status=IoCreateSymbolicLink(&DeviceLink,&DeviceName); 
	if (!NT_SUCCESS(Status)) {
		IoDeleteDevice(DriverObject->DeviceObject);
		DbgPrint("dbgclient: IoCreateSymbolicLink() failed with status 0x%08X\n",Status);
		return Status;
	}

	InitializeListHead(&g_DebugWindowsList); // ��ʼ���б�(Ϊ��) 
	KeInitializeMutex(&g_DebugWindowsListMutex,0); // ��Ϣ�б��� 

	//The KeInitializeEvent routine initializes an event object as a synchronization (single waiter) or notification type event and sets it to a signaled or not signaled state.
	// ��ʼ��ж���¼�,δ����״̬ ( �ȵ���DriverUnload����ʱ�ټ�������������ScanWindowsThread �̼߳�ʱ�˳� )
	KeInitializeEvent(&g_ShutdownEvent, NotificationEvent, FALSE);

	//The PsCreateSystemThread routine creates a system thread that executes in kernel mode and returns a handle for the thread.	
	// ����һ���ں����е��߳� ScanWindowsThread 
    // ���ڲ��ϲ鿴 g_DebugWindowsList �������Ƿ����µ���Ϣ��Ҫ��ӡ (DeviceControl IOCTL_REGISTER_WINDOW �¼��н��յ���Ϣ) 
	if (!NT_SUCCESS(Status=PsCreateSystemThread(&hThread,
							(ACCESS_MASK)0L,
							NULL,
							0,
							NULL,
							ScanWindowsThread,
							NULL))) {
            // ʧ��ɾ��������Դ�˳� 
			DbgPrint("dbgclient: Failed to start ScanWindowsThread, status 0x%08X\n",Status);
			IoDeleteDevice(DriverObject->DeviceObject);
			IoDeleteSymbolicLink(&DeviceLink);
			return Status;
		}

    // �����߳̾��(HANDLE)��ȡ��Ӧ�� _ETHREAD �������ڵȴ�����(KeWaitForSingleObject) 
	if (!NT_SUCCESS(Status=ObReferenceObjectByHandle(
							hThread,
							THREAD_ALL_ACCESS,
							NULL,
							KernelMode,
							&g_pScanWindowsThread,
							NULL))) {
		DbgPrint("HelloWorldDriver: Failed to get thread object of the ScanWindowsThread, status 0x%08X\n",Status);
		ZwClose(hThread);
		IoDeleteDevice(DriverObject->DeviceObject);
		IoDeleteSymbolicLink(&DeviceLink);
		return Status;
	}
	ZwClose(hThread);

	DriverObject->MajorFunction[IRP_MJ_CREATE]          =
	DriverObject->MajorFunction[IRP_MJ_CLOSE]           =
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]  = DriverDispatcher;

	DbgPrint("dbgclient: Initialized\n");
	return STATUS_SUCCESS;
}