ENTRY(_start)

SECTIONS
{
	__KERNEL_VIRTUAL_BASE = 0xC0000000;
	. = __KERNEL_VIRTUAL_BASE + 1M;
	__KERNEL_MEMORY_START = .;
	.text ALIGN(4096) : AT(ADDR(.text) - __KERNEL_VIRTUAL_BASE)
	{
		*(.multiboot)
		*(.text)
	}

	.rodata ALIGN(4K) : AT(ADDR(.rodata) - __KERNEL_VIRTUAL_BASE)
	{
		*(.rodata)
	}

	.data ALIGN(4K) : AT(ADDR(.data) - __KERNEL_VIRTUAL_BASE)
	{
		*(COMMON)
		*(.data)
		*(.bss)
	}

	. = ALIGN(4K) ;
	__KERNEL_MEMORY_END = .;
}