/**
*  ����bluepill����
*            ��δ����bluepill��ʱ��cpuid���ؼ�ԭʼ����ֵ: ���� 0xbabecafe��BP_KNOCK_EAX�� ����  (�������IDδ����?)
*            ������bluepill�Ժ�cpuid����ֵ���Ա�bluepill�޸ĳ�����ֵ. ���� 0xbabecafe��BP_KNOCK_EAX�� ���� 0x69696969 ��BP_KNOCK_EAX_ANSWER��
*            CPUID ���봦������� "nbp-0.32-public/svm/svmtraps.c/SvmDispatchCpuid()"�� "nbp-0.32-public/vmx/vmxtraps.c/VmxDispatchCpuid()"
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
	    cpuid      // �ڿ���hypervisor��Clientģʽ��cpuid�����쳣����VM
	    pop	edx
	    pop	ecx
	    pop	ebx
	    mov	esp, ebp
	    pop	ebp
	    ret
	}
}

// ���������з�ʽ > bphnock.exe  0xbabecafe
int __cdecl main(int argc, char **argv) {
	ULONG32 knock;
	if (argc != 2) {
		printf ("bpknock <magic knock>\n");
		return 0;
	}
	
	knock = strtoul (argv[1], 0, 0);  // ��������ַ���("0xbabecafe")ת��unsig longֵ (0xbabecafe)
	__try {
		printf ("knock answer: %#x\n", NBPCall (knock));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		printf ("CPUDID caused exception");
		return 0;
	}
	
	return 0;
}