## Introduction

The Windows kernel creates one Kernel Processor Control Region (KPCR) for every logical processor on the system.

This proof-of-concept driver gets the address of all `KPCR`s for a given system and stores them in a linked list. All allocated memory is freeded on `DriverUnload`.

## How it works

1. We broadcast a Deferred Procedure Call (DPC) to every processor on the system using `KeGenericCallDpc`
2. When the DPC executes, a call to `KeGetPcr` will occur and a struct of type `EntryCtx` will be allocated from the non-paged memory pool and added as an entry to the doubly-linked list kept in the global variable `g_Ctx` of type `Context`
3. A call to `KeSignalCallDpcSynchronize` will ensure that all DPCs are synchronized before proceeding to `KeSignalCallDpcDone`
4. The main `DriverEntry` thread will be waiting for `g_Ctx->DoneEvent` to be signalled, which will occur after `KeSignalCallDpcDone`. This informs `DriverEntry` that all DPCs have finished and the linked list can now be traversed.

## Possible improvements

A kernel-based container implementation such as [jxy::vector](https://github.com/jxy-s/stlkrn/blob/e8f73f66cb4e03e47ce2cceb59113b03f8de6a3a/include/jxy/vector.hpp) could instead be used to store all KPCR pointers.

## Credits

The APIs `KeGenericCallDpc`, `KeSignalCallDpcSynchronize` and `KeSignalCallDpcDone` are undocumented and their usage has been taken out of [gbhv/entry.c](https://github.com/Gbps/gbhv/blob/master/gbhv/entry.c#L66-L70=).