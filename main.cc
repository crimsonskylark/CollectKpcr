#include <ntddk.h>

static constexpr ULONG POOL_TAG = 'rcPC';

static UNICODE_STRING SYMLINK_NAME = RTL_CONSTANT_STRING(L"\\??\\CollectKpcr");
static UNICODE_STRING DEVICE_NAME = RTL_CONSTANT_STRING(L"\\Device\\CollectKpcr");

struct EntryCtx {
  LIST_ENTRY Entry;
  ULONG ProcessorIdx;
  PKPCR Kpcr;
};

struct Context {
  LIST_ENTRY Head;
  KEVENT DoneEvent;
  KMUTEX ListMutex;
};

static Context *g_Ctx = nullptr;

extern "C" {

NTKERNELAPI
_IRQL_requires_max_(APC_LEVEL)
    _IRQL_requires_min_(PASSIVE_LEVEL) _IRQL_requires_same_ VOID
    KeGenericCallDpc(_In_ PKDEFERRED_ROUTINE Routine, _In_opt_ PVOID Context);

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL) _IRQL_requires_same_ VOID
    KeSignalCallDpcDone(_In_ PVOID SystemArgument1);

NTKERNELAPI
_IRQL_requires_(DISPATCH_LEVEL) _IRQL_requires_same_ LOGICAL
    KeSignalCallDpcSynchronize(_In_ PVOID SystemArgument2);

void DrvUnload(PDRIVER_OBJECT DrvObj) {
  IoDeleteSymbolicLink(&SYMLINK_NAME);
  IoDeleteDevice(DrvObj->DeviceObject);

  if (g_Ctx) {
    for (auto tail = g_Ctx->Head.Flink; tail != &g_Ctx->Head;
         tail = tail->Flink) {
      auto Entry = CONTAINING_RECORD(tail, EntryCtx, Entry);
      ExFreePoolWithTag(Entry, POOL_TAG);
    }

    ExFreePoolWithTag(g_Ctx, POOL_TAG);
  }
}

VOID GetPcrForCurrentProcessor(PKDPC Dpc, PVOID DeferredContext,
                               PVOID SystemArgument1, PVOID SystemArgument2) {
  (void)Dpc;

  auto ctx = static_cast<Context *>(DeferredContext);

  auto procCtx = static_cast<EntryCtx *>(
      ExAllocatePoolZero(NonPagedPool, sizeof(Context), POOL_TAG));

  if (procCtx) {
    KeWaitForSingleObject(&ctx->ListMutex, Executive, KernelMode, FALSE,
                          nullptr);

    procCtx->Kpcr = KeGetPcr();
    procCtx->ProcessorIdx = KeGetCurrentProcessorNumber();
    InsertHeadList(&ctx->Head, &procCtx->Entry);
    KeReleaseMutex(&ctx->ListMutex, FALSE);

    DbgPrint("[%lu] KPCR address: %p\n", procCtx->ProcessorIdx, procCtx->Kpcr);
  }

  KeSignalCallDpcSynchronize(SystemArgument2);
  KeSignalCallDpcDone(SystemArgument1);

  KeSetEvent(&ctx->DoneEvent, IO_NO_INCREMENT, FALSE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DrvObj, UNICODE_STRING RegPath) {
  UNREFERENCED_PARAMETER(RegPath);

  auto ret = STATUS_SUCCESS;

  auto symLinkExists = false;

  PDEVICE_OBJECT dvcObj = nullptr;

  ret = IoCreateDevice(DrvObj, 0, &DEVICE_NAME, 0, 0, FALSE, &dvcObj);

  if (!NT_SUCCESS(ret)) {
    DbgPrint("Unable to create device: 0x%08x\n", ret);
    goto fail;
  }

  ret = IoCreateSymbolicLink(&SYMLINK_NAME, &DEVICE_NAME);

  if (!NT_SUCCESS(ret)) {
    DbgPrint("Unable to create symbolic link to device: %08x\n", ret);
    goto fail;
  }

  for (auto &irp : DrvObj->MajorFunction) {
    irp = nullptr;
  }

  DrvObj->DriverUnload = DrvUnload;

  auto ctx = static_cast<Context *>(
      ExAllocatePoolZero(NonPagedPool, sizeof(Context), POOL_TAG));

  if (!ctx) {
    DbgPrint("Unable to allocate %llu bytes.\n", sizeof(Context));
    ret = STATUS_INSUFFICIENT_RESOURCES;
    goto fail;
  }

  KeInitializeMutex(&ctx->ListMutex, 0);
  InitializeListHead(&ctx->Head);
  KeInitializeEvent(&ctx->DoneEvent, SynchronizationEvent, FALSE);

  // broadcast a DPC to all processors
  KeGenericCallDpc(GetPcrForCurrentProcessor, ctx);

  // Wait until all DPCs have completed.
  KeWaitForSingleObject(&ctx->DoneEvent, Executive, KernelMode, FALSE, nullptr);

  DbgPrint("Collected all KPRCs.\n");

  for (auto tail = ctx->Head.Flink; tail != &ctx->Head; tail = tail->Flink) {
    auto Entry = CONTAINING_RECORD(tail, EntryCtx, Entry);
    DbgPrint("KPCR: %p (%lu)\n", Entry->Kpcr, Entry->ProcessorIdx);
  }

  return ret;

fail:
  if (symLinkExists) {
    IoDeleteSymbolicLink(&SYMLINK_NAME);
  }
  if (dvcObj) {
    IoDeleteDevice(dvcObj);
  }
  return ret;
}
}