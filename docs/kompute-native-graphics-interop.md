# Extending Kompute for Native Graphics Pipeline Interop

## Overview

This document proposes an extension to Kompute so compute workloads can interoperate with native Vulkan graphics pipelines **without CPU-side synchronization** and **without device-host-device transfers**.

The core idea is to evolve Kompute from a compute-only submit helper into a submission node that can participate in a GPU frame graph via semaphores/timeline semaphores and explicit queue-family ownership transfers.

## Problem Statement

Today, Kompute sequences are easy to use for compute dispatch, but interop with native graphics is constrained:

- `Sequence::evalAsync()` currently submits with no public wait/signal semaphore controls.
- `Sequence` internally manages a fence and exposes `evalAwait()` / `isRunning()`, which encourages CPU-side waits.
- `Manager` is compute-queue-centric, not frame-graph-centric.

This blocks clean GPU-only chaining like:

1. Compute sequence writes image/buffer.
2. Graphics submit waits on compute completion.
3. Graphics pass samples/reads compute output.
4. Present.

## Goals

- Enable **GPU-only synchronization** between Kompute and native Vulkan submits.
- Preserve Kompute ergonomics for compute users.
- Keep graphics functionality optional (interop-first, not full renderer replacement).
- Avoid breaking existing `eval()` / `evalAsync()` workflows.

## Non-Goals

- Replacing full native Vulkan graphics pipeline creation.
- Hiding all Vulkan synchronization complexity.
- Building a complete frame graph system inside Kompute.

## Requirements for Interop

To interop safely and efficiently, Kompute must support:

1. **Submit-time waits and signals** (binary and timeline semaphores).
2. **Queue selection** across compute/transfer (and optionally graphics).
3. **Queue-family ownership transfer support** in ops/barriers.
4. Optional export of synchronization primitives for external native submits.

## Proposed API Additions

### 1) Submission Synchronization Types

```cpp
struct SubmitWait {
  vk::Semaphore semaphore;
  vk::PipelineStageFlags2 stageMask;
  uint64_t value = 0; // timeline value (ignored for binary)
};

struct SubmitSignal {
  vk::Semaphore semaphore;
  uint64_t value = 0; // timeline value (ignored for binary)
};

struct SubmitSync {
  std::vector<SubmitWait> waits;
  std::vector<SubmitSignal> signals;
  vk::Fence fence = VK_NULL_HANDLE; // optional external fence
};
```

### 2) Sequence Submission Overloads

```cpp
std::shared_ptr<Sequence> evalAsync(const SubmitSync& sync);
std::shared_ptr<Sequence> eval(const SubmitSync& sync); // evalAsync + evalAwait
```

Backward-compatible behavior:

- Existing `evalAsync()` behaves as today.
- Existing `eval()` remains blocking convenience.

### 3) Optional Timeline Support in Manager

```cpp
vk::Semaphore Manager::createTimelineSemaphore(uint64_t initialValue = 0);
```

This allows apps to centralize cross-submit ordering with timeline values.

### 4) Queue Accessors (Optional but practical)

```cpp
std::shared_ptr<vk::Queue> Manager::getQueue(uint32_t queueIndex) const;
uint32_t Manager::getQueueFamilyIndex(uint32_t queueIndex) const;
```

These help external native submit code compose dependencies correctly.

## Synchronization Model

### A) Same queue family

Use:

- semaphore wait/signal between submits for execution ordering
- memory/image barriers for visibility/layout transitions

### B) Different queue families

Use both:

- semaphore wait/signal for execution dependency
- ownership transfer barriers (`srcQueueFamilyIndex`, `dstQueueFamilyIndex`) for resource ownership

## Op-Level Extensions

`OpMemoryBarrier` should be extended for ownership transfers:

```cpp
OpMemoryBarrier(
  memObjects,
  srcAccess, dstAccess,
  srcStage, dstStage,
  barrierOnPrimary,
  srcQueueFamilyIndex,   // optional
  dstQueueFamilyIndex    // optional
);
```

This keeps ownership transfer explicit and composable in sequences.

## Example Interop Flow (GPU-only)

1. Kompute sequence dispatches compute and signals `computeDone` timeline value `N`.
2. Native graphics submit waits on `computeDone >= N`.
3. Graphics pass samples compute output image/buffer.
4. Present.

No `evalAwait()`, no host sync, no CPU readback.

## Suggested Incremental Implementation Plan

### Phase 1: Submit Sync Primitives

- Add `SubmitSync` and `Sequence::evalAsync(const SubmitSync&)`.
- Support binary semaphores first.
- Keep old API untouched.

### Phase 2: Timeline Semaphores

- Add timeline semaphore waits/signals.
- Use `vk::SubmitInfo2` path where available.

### Phase 3: Queue-Family Ownership

- Extend `OpMemoryBarrier` for ownership transfer.
- Add validation checks for cross-family usage.

### Phase 4: Native Vulkan Integration Helpers

- Expose queue/family metadata from `Manager`.
- Optional helper wrappers for common interop patterns.

## Compatibility Strategy

- Preserve existing public methods and behavior.
- Add new overloads instead of changing old signatures.
- Default `SubmitSync{}` should match current submission behavior.

## Testing Strategy

- Unit tests for:
  - binary wait/signal sequencing
  - timeline value ordering
  - invalid sync configurations
- Integration tests:
  - compute sequence -> native graphics submit
  - same-family and cross-family paths
  - no-host-sync correctness checks
- Validation-layer clean runs on all tested paths.

## Risks and Mitigations

- **API complexity growth**  
  Mitigate via minimal structs and defaults.
- **Cross-platform queue family behavior**  
  Mitigate with explicit ownership APIs and validation.
- **Backwards compatibility**  
  Mitigate with additive API only.

## Recommended Architecture for Applications

Use Kompute as a compute subsystem and native Vulkan as graphics subsystem under one shared Vulkan context:

- shared instance/device
- explicit semaphores/timeline between compute and graphics submits
- explicit barriers/layout transitions
- no CPU-side waits in frame loop except debug/fallback modes

## Conclusion

Kompute can be made fully compatible with native graphics pipelines by extending **submission and synchronization primitives**, not by forcing graphics pipeline construction into Kompute first.

The most valuable upgrade is first-class semaphore/timeline interop in `Sequence::evalAsync()` plus queue-family-aware barriers.
