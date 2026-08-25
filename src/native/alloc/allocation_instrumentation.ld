SECTIONS
{
    spark_allocation_hooks :
    {
        PROVIDE_HIDDEN(__spark_allocation_hooks_start = .);
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl10hookMallocEm*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl10hookCallocEmm*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl11hookReallocEPvm*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl8hookFreeEPv*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl16hookReallocArrayEPvmm*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl16hookAlignedAllocEmm*))
        KEEP(*(.text._ZN5spark17AllocationSampler4Impl17hookPosixMemalignEPPvmm*))
        PROVIDE_HIDDEN(__spark_allocation_hooks_end = .);
    }
}
INSERT AFTER .text;
