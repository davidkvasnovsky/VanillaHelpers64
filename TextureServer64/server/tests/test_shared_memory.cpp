// test_shared_memory.cpp - Unit tests for TexServer::SharedMemory
// Build with MSVC: cl /EHsc /std:c++17 /I../../shared /I../src
//   test_shared_memory.cpp ../src/SharedMemory.cpp /link /SUBSYSTEM:CONSOLE

#include "../src/SharedMemory.h"
#include "../../shared/Protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// ── Test 1: Create and verify header fields ────────────────────────────
static void test_create_and_open() {
    TexServer::SharedMemory shm;

    assert(!shm.IsValid());
    bool ok = shm.Create();
    assert(ok);
    assert(shm.IsValid());

    auto* hdr = shm.GetHeader();
    assert(hdr != nullptr);
    assert(hdr->magic      == TexProto::SHM_MAGIC);
    assert(hdr->version    == TexProto::SHM_VERSION);
    assert(hdr->slot_count == TexProto::SLOT_COUNT);
    assert(hdr->slot_data_size == TexProto::SLOT_DATA_SIZE);
    assert(hdr->server_pid == static_cast<uint64_t>(GetCurrentProcessId()));
    assert(hdr->sequence   == 0);

    // Verify all slots start as free (state == 0).
    for (int32_t i = 0; i < static_cast<int32_t>(TexProto::SLOT_COUNT); ++i) {
        auto* sh = shm.GetSlotHeader(i);
        assert(sh != nullptr);
        assert(sh->state == 0);
    }

    // Bounds checking: out-of-range slots return nullptr.
    assert(shm.GetSlotHeader(-1) == nullptr);
    assert(shm.GetSlotHeader(static_cast<int32_t>(TexProto::SLOT_COUNT)) == nullptr);
    assert(shm.GetSlotData(-1) == nullptr);
    assert(shm.GetSlotData(static_cast<int32_t>(TexProto::SLOT_COUNT)) == nullptr);

    shm.Destroy();
    assert(!shm.IsValid());

    std::printf("[PASS] test_create_and_open\n");
}

// ── Test 2: Slot lifecycle — alloc, write, mark ready, free ────────────
static void test_slot_lifecycle() {
    TexServer::SharedMemory shm;
    bool ok = shm.Create();
    assert(ok);

    // Allocate a slot.
    int32_t slot = shm.AllocateSlot();
    assert(slot >= 0 && slot < static_cast<int32_t>(TexProto::SLOT_COUNT));

    auto* sh = shm.GetSlotHeader(slot);
    assert(sh != nullptr);
    assert(sh->state == 1);  // allocated / loading

    // Write some data into the slot.
    uint8_t* data = shm.GetSlotData(slot);
    assert(data != nullptr);
    const char test_payload[] = "Hello, shared memory!";
    std::memcpy(data, test_payload, sizeof(test_payload));

    // Fill in slot header metadata.
    sh->width      = 256;
    sh->height     = 256;
    sh->data_size  = sizeof(test_payload);
    sh->format     = TexProto::PixelFormat::RGBA8;
    sh->mip_levels = 1;
    sh->path_hash  = TexProto::HashPath("textures\\test.blp");

    // Mark slot ready.
    shm.MarkSlotReady(slot);
    assert(sh->state == 3);  // SlotState::Ready

    // Sequence counter should have incremented.
    auto* hdr = shm.GetHeader();
    assert(hdr->sequence == 1);

    // Verify data is still intact.
    assert(std::memcmp(data, test_payload, sizeof(test_payload)) == 0);

    // Free the slot.
    shm.FreeSlot(slot);
    assert(sh->state == 0);  // free

    shm.Destroy();
    std::printf("[PASS] test_slot_lifecycle\n");
}

// ── Test 3: Slot exhaustion and reclaim ────────────────────────────────
static void test_slot_exhaustion_and_reclaim() {
    TexServer::SharedMemory shm;
    bool ok = shm.Create();
    assert(ok);

    int32_t slots[TexProto::SLOT_COUNT];

    // Allocate all 64 slots.
    for (uint32_t i = 0; i < TexProto::SLOT_COUNT; ++i) {
        slots[i] = shm.AllocateSlot();
        assert(slots[i] >= 0);
    }

    // Next allocation must fail.
    int32_t overflow = shm.AllocateSlot();
    assert(overflow == -1);

    // Free one slot and verify we can reclaim it.
    int32_t freed_slot = slots[32];  // pick one in the middle
    shm.FreeSlot(freed_slot);

    int32_t reclaimed = shm.AllocateSlot();
    assert(reclaimed == freed_slot);

    // Trying again should fail (all slots occupied again).
    assert(shm.AllocateSlot() == -1);

    // Clean up: free all.
    for (uint32_t i = 0; i < TexProto::SLOT_COUNT; ++i) {
        shm.FreeSlot(slots[i]);
    }

    shm.Destroy();
    std::printf("[PASS] test_slot_exhaustion_and_reclaim\n");
}

// ── Main ───────────────────────────────────────────────────────────────
int main() {
    test_create_and_open();
    test_slot_lifecycle();
    test_slot_exhaustion_and_reclaim();

    std::printf("\nAll SharedMemory tests passed.\n");
    return 0;
}
