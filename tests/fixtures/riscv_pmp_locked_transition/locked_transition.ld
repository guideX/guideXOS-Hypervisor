OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
	. = 0x80000000;
	.text : ALIGN(4)
	{
		KEEP(*(.text.boot))
		*(.text .text.*)
	} :text

	/DISCARD/ :
	{
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
