/**
*  测试bluepill程序：
*            当未启动bluepill的时候cpuid返回即原始数据值: 传入 0xbabecafe（BP_KNOCK_EAX） 返回  (好像这个ID未定义?)
*            当开启bluepill以后，cpuid返回值可以被bluepill修改成任意值. 传入 0xbabecafe（BP_KNOCK_EAX） 返回 0x69696969 （BP_KNOCK_EAX_ANSWER）
*            CPUID 代码处理函数详见 "nbp-0.32-public/svm/svmtraps.c/SvmDispatchCpuid()"、 "nbp-0.32-public/vmx/vmxtraps.c/VmxDispatchCpuid()"
**/
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

ULONG32 __declspec(naked) NBPCall (ULONG32 knock) { 
	__asm { 
        mov  eax, [esp+4]
	    push 	ebp
	    mov	ebp, esp
	    push	ebx
	    push	ecx
	    push	edx
	    cpuid      // 在开启hypervisor的Client模式下cpuid触发异常陷入VM
	    pop	edx
	    pop	ecx
	    pop	ebx
	    mov	esp, ebp
	    pop	ebp
	    ret
	}
}

// 命令行运行方式 > bphnock.exe  0xbabecafe
int __cdecl main(int argc, char **argv) {
	ULONG32 knock;
	if (argc != 2) {
		printf ("bpknock <magic knock>\n");
		return 0;
	}
	
	knock = strtoul (argv[1], 0, 0);  // 将传入的字符串("0xbabecafe")转成unsig long值 (0xbabecafe)
	__try {
		printf ("knock answer: %#x\n", NBPCall (knock));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		printf ("CPUDID caused exception");
		return 0;
	}
	
	return 0;
}