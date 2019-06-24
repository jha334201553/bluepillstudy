#pragma once

#include <ntddk.h>
#include "dbgclient_ioctl.h"

#define ABSOLUTE(wait) (wait)

#define RELATIVE(wait) (-(wait))

// 纳秒 
#define NANOSECONDS(nanos)   \
	 (((signed __int64)(nanos)) / 100L)

// 微秒
#define MICROSECONDS(micros) \
	 (((signed __int64)(micros)) * NANOSECONDS(1000L))

// 毫秒 
#define MILLISECONDS(milli)  \
	 (((signed __int64)(milli)) * MICROSECONDS(1000L))

// 秒 
#define SECONDS(seconds)	 \
	 (((signed __int64)(seconds)) * MILLISECONDS(1000L))

// 分钟 
#define MINUTES(minutes)	 \
	 (((signed __int64)(minutes)) * SECONDS(60L))

// 小时 
#define HOURS(hours)		 \
	 (((signed __int64)(hours)) * MINUTES(60L))


// 用于异步打印 bluepill 信息 
// 接收线程先把DEBUG_WINDOW存储于DEBUG_WINDOW_ENTRY列表 
// 然后再慢慢调用 PrintData() 打印 
typedef struct _DEBUG_WINDOW_ENTRY {
	LIST_ENTRY	le;
	PMDL	pWindowMdl;
	DEBUG_WINDOW	DebugWindow;
} DEBUG_WINDOW_ENTRY, *PDEBUG_WINDOW_ENTRY;