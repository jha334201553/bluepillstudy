/*
*  用于接收bluepill日志并打印模块
*/
#include "dbgclient.h"
#include "dbgclient_ioctl.h"

LIST_ENTRY	g_DebugWindowsList;  // 消息列表 
KMUTEX	g_DebugWindowsListMutex;   // 操作 g_DebugWindowsList 列表锁 
KEVENT	g_ShutdownEvent;        // 关闭本驱动的事件 
PETHREAD	g_pScanWindowsThread = NULL; // 从 bluepill 接收日志线程 

PUCHAR	g_pDebugString = NULL;  // 日志字符串临时内存(修改时注意多线程抢占问题) 

//////////////////////////////////////////////////////////////////////////
// 遍历 g_DebugWindowsList 列表，DbgPrint 打印日志信息 
//////////////////////////////////////////////////////////////////////////
static VOID PrintData()
{
	PDEBUG_WINDOW_ENTRY	pRegisteredWindow;
	PUCHAR	pData,pString;
	ULONG	i,uWindowSize;


	KeWaitForSingleObject(&g_DebugWindowsListMutex, Executive, KernelMode, FALSE, NULL);

    // 遍历列表 
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
			uWindowSize = min(pRegisteredWindow->DebugWindow.uWindowSize-1, DEBUG_WINDOW_IN_PAGES*PAGE_SIZE); // 防越界 
			RtlCopyMemory(g_pDebugString, &pData[1], uWindowSize); // 先把所有字符串数据拷贝到 g_pDebugString 中，再做扫描处理 
			pData[0] = 0;

			pString = g_pDebugString; // pString 永远指向下一次要打印的字符串的首地址 
			for (i=0;i<uWindowSize,g_pDebugString[i];i++) {
                // 如果有换行符，则单独一行输出 
				if (g_pDebugString[i]==0x0a) {
					g_pDebugString[i]=0;
					DbgPrint("<%02X>:  %s\n",pRegisteredWindow->DebugWindow.bBpId,pString);
					pString = &g_pDebugString[i+1];
				}
			}
            // 字符串不以换行符结尾，则再打印最后的字符串 
			if (*pString)
				DbgPrint("<%02X>:  %s\n", pRegisteredWindow->DebugWindow.bBpId, &pString);

			RtlZeroMemory(g_pDebugString, DEBUG_WINDOW_IN_PAGES*PAGE_SIZE);
		}
		pRegisteredWindow=(PDEBUG_WINDOW_ENTRY)pRegisteredWindow->le.Flink;
	}
	KeReleaseMutex(&g_DebugWindowsListMutex,FALSE);	
}

//////////////////////////////////////////////////////////////////////////
// 日志打印线程： 10ms 调用一次 PrintData() 打印日志 
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
// 处理 IOCTL_REGISTER_WINDOW 注册(接收)消息, IOCTL_UNREGISTER_WINDOW 注销消息 
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

            // 先检测此消息(通过pWindowVA消息内存地址判断)是不是已经存储于 g_DebugWindowsList 列表中，如果已经存在则跳过 
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

            // g_DebugWindowsList 中不存在，则插入列表 
			if (!bFound) {
                // MDL内存重映射 
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
// 驱动IRP调度分发函数(只响应 IRP_MJ_DEVICE_CONTROL 请求)
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

    // 设置关闭日志事件,然后等待 ScanWindowsThread 线程退出 
	KeSetEvent(&g_ShutdownEvent,0,FALSE);
	if (g_pScanWindowsThread) {
		KeWaitForSingleObject(g_pScanWindowsThread,Executive,KernelMode,FALSE,NULL);
		ObDereferenceObject(g_pScanWindowsThread);
	}

    // 删除符号链接 
	RtlInitUnicodeString(&DeviceLink,L"\\DosDevices\\itldbgclient");
	IoDeleteSymbolicLink(&DeviceLink);
    // 删除设备对象 
	if (DriverObject->DeviceObject)
		IoDeleteDevice(DriverObject->DeviceObject);

    // 在没有收到 IOCTL_UNREGISTER_WINDOW 的情况下卸载驱动，需要清理资源 
    // 打印没有打印的日志信息 
	PrintData();
    // 释放 g_DebugWindowsList 消息列表内存 
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

// BluePill Debug信息的消费者，以注册驱动的方式出现，Debug信息的生产者会把信息写到\\Device\\itldbgclient的共享内存空间上
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	NTSTATUS	Status;
	UNICODE_STRING	DeviceLink,DeviceName;
	PDEVICE_OBJECT	pDeviceObject;
	HANDLE	hThread;

	DriverObject->DriverUnload=DriverUnload;

	RtlInitUnicodeString(&DeviceName,L"\\Device\\itldbgclient"); // 声明了两个设备（这个设备在common/dbgclient.c中也有，因为那个是有对这个设备的发送debug信息操作）
	RtlInitUnicodeString(&DeviceLink,L"\\DosDevices\\itldbgclient");

	g_pDebugString=ExAllocatePool(PagedPool,DEBUG_WINDOW_IN_PAGES*PAGE_SIZE);
	if (!g_pDebugString) {
		DbgPrint("dbgclient: Failed to allocate %d bytes for debug window buffer\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// The IoCreateDevice routine creates a device object for use by a driver.
    // IoCreateDevice 方法创建一个给驱动使用的设备对象 
	Status=IoCreateDevice(DriverObject,0,&DeviceName,DBGCLIENT_DEVICE,0,FALSE,&pDeviceObject);
	if (!NT_SUCCESS(Status)) {
		DbgPrint("dbgclient: IoCreateDevice() failed with status 0x%08X\n",Status);
		return Status;
	}

    // IoCreateSymbolicLink 创建一个符号连接，给应用层使用 (bluepill项目未使用，此代码可以不需要) 
	Status=IoCreateSymbolicLink(&DeviceLink,&DeviceName); 
	if (!NT_SUCCESS(Status)) {
		IoDeleteDevice(DriverObject->DeviceObject);
		DbgPrint("dbgclient: IoCreateSymbolicLink() failed with status 0x%08X\n",Status);
		return Status;
	}

	InitializeListHead(&g_DebugWindowsList); // 初始化列表(为空) 
	KeInitializeMutex(&g_DebugWindowsListMutex,0); // 消息列表锁 

	//The KeInitializeEvent routine initializes an event object as a synchronization (single waiter) or notification type event and sets it to a signaled or not signaled state.
	// 初始化卸载事件,未激活状态 ( 等调用DriverUnload方法时再激活它，以至让ScanWindowsThread 线程及时退出 )
	KeInitializeEvent(&g_ShutdownEvent, NotificationEvent, FALSE);

	//The PsCreateSystemThread routine creates a system thread that executes in kernel mode and returns a handle for the thread.	
	// 创建一个内核运行的线程 ScanWindowsThread 
    // 用于不断查看 g_DebugWindowsList 链表中是否有新的消息需要打印 (DeviceControl IOCTL_REGISTER_WINDOW 事件中接收的消息) 
	if (!NT_SUCCESS(Status=PsCreateSystemThread(&hThread,
							(ACCESS_MASK)0L,
							NULL,
							0,
							NULL,
							ScanWindowsThread,
							NULL))) {
            // 失败删除所有资源退出 
			DbgPrint("dbgclient: Failed to start ScanWindowsThread, status 0x%08X\n",Status);
			IoDeleteDevice(DriverObject->DeviceObject);
			IoDeleteSymbolicLink(&DeviceLink);
			return Status;
		}

    // 利用线程句柄(HANDLE)获取对应的 _ETHREAD 对象，用于等待函数(KeWaitForSingleObject) 
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