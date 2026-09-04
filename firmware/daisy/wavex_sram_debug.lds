/*
 * WaveX direct-SWD SRAM layout.
 *
 * Based on libDaisy v8.1.0's core/STM32H750IB_sram.lds, with WaveX's
 * project-specific sections and memory budgets folded in. The stock layout
 * places all .data and .bss in 128 KiB of DTCM; WaveX's foreground state is
 * larger, so ordinary .bss lives in D2 SRAM and a small, explicitly-cleared
 * spill range lives in D3 SRAM. Code and initialization images remain wholly
 * in internal SRAM -- this script never places or loads a section in QSPI.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH       (RX)  : ORIGIN = 0x08000000, LENGTH = 128K
    DTCMRAM     (RWX) : ORIGIN = 0x20000000, LENGTH = 128K
    SRAM        (RWX) : ORIGIN = 0x24000000, LENGTH = 512K - 32K
    RAM_D2_DMA  (RWX) : ORIGIN = 0x30000000, LENGTH = 32K
    RAM_D2      (RWX) : ORIGIN = 0x30008000, LENGTH = 256K
    RAM_D3      (RWX) : ORIGIN = 0x38000000, LENGTH = 64K
    BACKUP_SRAM (RWX) : ORIGIN = 0x38800000, LENGTH = 4K
    ITCMRAM     (RWX) : ORIGIN = 0x00000000, LENGTH = 64K
    SDRAM       (RWX) : ORIGIN = 0xc0000000, LENGTH = 64M
}

_estack = 0x20020000;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > SRAM

    .text :
    {
        . = ALIGN(4);
        _stext = .;
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        *(.qspiflash_text)
        *(.qspiflash_text*)
        *(.glue_7)
        *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > SRAM

    .ARM.extab :
    {
        . = ALIGN(4);
        *(.ARM.extab)
        *(.gnu.linkonce.armextab.*)
        . = ALIGN(4);
    } > SRAM

    .exidx :
    {
        . = ALIGN(4);
        PROVIDE(__exidx_start = .);
        *(.ARM.exidx*)
        . = ALIGN(4);
        PROVIDE(__exidx_end = .);
    } > SRAM

    .ARM.attributes 0 : { *(.ARM.attributes) }

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > SRAM

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > SRAM

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array*))
        KEEP(*(SORT(.fini_array.*)))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > SRAM

    /*
     * SDMMC1's internal DMA is a D1-domain bus master and cannot reach D2 or
     * D3 SRAM on STM32H750. Keep every FatFS sector buffer and every direct
     * read/write staging buffer in AXI SRAM. The normal QSPI layout already
     * puts these objects here; this section preserves that hardware contract
     * when the SRAM-only image spills ordinary .bss into D2/D3.
     */
    .sram_debug_sd_axi_bss (NOLOAD) :
    {
        . = ALIGN(32);
        PROVIDE(__wavex_sram_debug_sd_axi_bss_start__ = .);
        *(.bss._ZN5WaveX7Storage6SdSdioL5s_fsiE)
        *(.bss._ZL15s_metadata_file)
        *(.bss._ZL14s_metadata_buf)
        *(.bss._ZN5WaveX11AudioEngineL12s_cvcal_fileE)
        *(.bss._ZN5WaveX11AudioEngineL5s_wavE)
        *(.bss._ZN5WaveX11AudioEngineL18s_sample_load_fileE)
        *(.bss._ZN5WaveX11AudioEngineL12s_sd_buffersE)
        *(.bss._ZN5WaveX11AudioEngineL14s_prebuffer_sdE)
        *(.bss._ZN5WaveX11AudioEngineL11s_sample_ioE)
        *(.bss._ZN5WaveX11AudioEngine9SfzLoader12_GLOBAL__N_1L6s_fileE)
        . = ALIGN(32);
        PROVIDE(__wavex_sram_debug_sd_axi_bss_end__ = .);
    } > SRAM

    .sram1_bss (NOLOAD) :
    {
        . = ALIGN(4);
        _ssram1_bss = .;
        PROVIDE(__sram1_bss_start__ = _ssram1_bss);
        *(.sram1_bss)
        *(.sram1_bss*)
        . = ALIGN(4);
        _esram1_bss = .;
        PROVIDE(__sram1_bss_end__ = _esram1_bss);
    } > RAM_D2_DMA

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        PROVIDE(__data_start__ = _sdata);
        *(.data)
        *(.data*)
        *(.qspiflash_data)
        *(.qspiflash_data*)
        . = ALIGN(4);
        _edata = .;
        PROVIDE(__data_end__ = _edata);
    } > DTCMRAM AT > SRAM

    _sidata = LOADADDR(.data);

    /*
     * Keep the startup file's automatic .bss range empty. Its Reset_Handler
     * clears that range before SystemInit() enables D2 SRAM; WaveX clears the
     * actual D2/D3 ranges from a preinit hook after SystemInit instead.
     */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        PROVIDE(__bss_start__ = _sbss);
        _ebss = .;
        PROVIDE(__bss_end__ = _ebss);
    } > DTCMRAM

    /*
     * These are large foreground/parser buffers rather than DMA or callback
     * state. Moving them makes the remaining SRAM-only state fit in D2. The
     * preinit hook clears every SRAM-only BSS range before constructors.
     */
    .sram_debug_bss (NOLOAD) :
    {
        . = ALIGN(32);
        PROVIDE(__wavex_sram_debug_bss_start__ = .);
        *(.bss._ZN5WaveX11AudioEngine9SfzLoader12_GLOBAL__N_1L8s_parserE)
        *(.bss._ZN5WaveX11AudioEngine9SfzLoader12_GLOBAL__N_1L18s_fallback_subdirsE)
        . = ALIGN(32);
        PROVIDE(__wavex_sram_debug_bss_end__ = .);
    } > RAM_D3

    .sram_debug_d2_bss (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE(__wavex_sram_debug_d2_bss_start__ = .);
        *(.bss)
        *(.bss*)
        *(.qspiflash_bss)
        *(.qspiflash_bss*)
        *(COMMON)
        . = ALIGN(4);
        PROVIDE(__wavex_sram_debug_d2_bss_end__ = .);
    } > RAM_D2

    .dtcmram_bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sdtcmram_bss = .;
        PROVIDE(__dtcmram_bss_start__ = _sdtcmram_bss);
        *(.dtcmram_bss)
        *(.dtcmram_bss*)
        . = ALIGN(4);
        _edtcmram_bss = .;
        PROVIDE(__dtcmram_bss_end__ = _edtcmram_bss);
    } > DTCMRAM

    .itcm_text : ALIGN(4)
    {
        __itcm_text_start__ = .;
        *(.itcm_text)
        *(.itcm_text*)
        . = ALIGN(4);
        __itcm_text_end__ = .;
    } > ITCMRAM AT > SRAM
    __itcm_text_load__ = LOADADDR(.itcm_text);

    .sdram_bss (NOLOAD) :
    {
        . = ALIGN(4);
        _ssdram_bss = .;
        PROVIDE(__sdram_bss_start = _ssdram_bss);
        *(.sdram_bss)
        *(.sdram_bss*)
        . = ALIGN(4);
        _esdram_bss = .;
        PROVIDE(__sdram_bss_end = _esdram_bss);
    } > SDRAM

    .backup_sram (NOLOAD) :
    {
        . = ALIGN(4);
        _sbackup_sram = .;
        PROVIDE(__backup_sram_start = _sbackup_sram);
        *(.backup_sram)
        *(.backup_sram*)
        . = ALIGN(4);
        _ebackup_sram = .;
        PROVIDE(__backup_sram_end = _ebackup_sram);
    } > BACKUP_SRAM

    .heap (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE(__heap_start__ = .);
        PROVIDE(__heap_start = .);
        KEEP(*(.heap))
        . = ALIGN(4);
        PROVIDE(__heap_end = .);
        PROVIDE(__heap_end__ = .);
    } > SRAM

    PROVIDE(end = .);
    PROVIDE(__wavex_heap_limit = ORIGIN(SRAM) + LENGTH(SRAM));

    .reserved_for_stack (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE(__reserved_for_stack_start__ = .);
        KEEP(*(.reserved_for_stack))
        . = ALIGN(4);
        PROVIDE(__reserved_for_stack_end__ = .);
    } > DTCMRAM

    /* libDaisy's scripts end with an output section named DISCARD listing
     * libc.a, libm.a and libgcc.a. Naming an archive as an input section
     * makes ld open it, which pulls the full newlib libc.a in ahead of the
     * -lc_nano the specs select. Deliberately omitted - see CMakeLists.txt. */
}

ASSERT(SIZEOF(.sram1_bss) <= 30K,
       "WaveX D2 DMA budget exceeded: .sram1_bss must remain <= 30 KiB")
ASSERT(SIZEOF(.sram_debug_sd_axi_bss) <= 80K,
       "WaveX SRAM debug SD/AXI budget exceeded 80 KiB")
ASSERT(SIZEOF(.sram_debug_bss) <= LENGTH(RAM_D3),
       "WaveX SRAM debug D3 spill exceeded 64 KiB")
ASSERT(SIZEOF(.data) + SIZEOF(.dtcmram_bss) <= 96K,
       "WaveX SRAM debug DTCM budget exceeded: preserve at least 32 KiB for stacks")
