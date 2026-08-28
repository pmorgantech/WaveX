SECTIONS
{
    .itcm_text : ALIGN(4)
    {
        __itcm_text_start__ = .;
        *(.itcm_text)
        *(.itcm_text*)
        . = ALIGN(4);
        __itcm_text_end__ = .;
    } > ITCMRAM AT > QSPIFLASH
    __itcm_text_load__ = LOADADDR(.itcm_text);
}

/* Preserve at least 2 KiB for late DMA bookkeeping/alignment growth. */
ASSERT(SIZEOF(.sram1_bss) <= 30K,
       "WaveX D2 DMA budget exceeded: .sram1_bss must remain <= 30 KiB")

/* DTCM grows upward while the main/IRQ stack descends from 0x20020000. */
ASSERT(SIZEOF(.dtcmram_bss) <= 64K,
       "WaveX DTCM static budget exceeded: preserve at least 64 KiB for stacks")
