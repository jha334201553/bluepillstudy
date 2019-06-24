#ifndef _DBGCLIENT_IOCTL_
#define _DBGCLIENT_IOCTL_


#define DBGCLIENT_DEVICE	FILE_DEVICE_UNKNOWN

#define IOCTL_REGISTER_WINDOW CTL_CODE(DBGCLIENT_DEVICE, 0x1, METHOD_BUFFERED, FILE_WRITE_ACCESS)     // 注册一个会话 
#define IOCTL_UNREGISTER_WINDOW CTL_CODE(DBGCLIENT_DEVICE, 0x2, METHOD_BUFFERED, FILE_WRITE_ACCESS)   // 注销一个会话 

#define DEBUG_WINDOW_IN_PAGES	5   // 定义 DEBUG_WINDOW::pWindowVA 使用内存的最大页大小 

// bluepill 与 dbgclient 驱动之间传送的数据结构 
typedef struct _DEBUG_WINDOW {
	UCHAR	bBpId;            // bluepill传入的(随机值)，用户表示当前会话 
	PVOID	pWindowVA;        // 字符串buf指针，第一字节0表示没有需要打印数据，1表示有需要打印数据 
	ULONG	uWindowSize;      // 指示 pWindowVA 数据长度 (字节) 
} DEBUG_WINDOW, *PDEBUG_WINDOW;

#endif // _DBGCLIENT_IOCTL_