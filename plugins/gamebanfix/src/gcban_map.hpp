// gamebanfix — the engine's ban map (CCSGameRules::sm_mapGcBanInformation), as the original
// Metamod plugin operates on it (see BEHAVIOR.md).
//
// The original hands the raw map pointer to its own copy of the engine container template and
// calls Unlink/FreeNode/RemoveAll on it. We cannot use the engine headers here, so the few
// operations the fix needs are re-implemented against the exact layout the original relies on
// (node size 0x38, field offsets verified against libserver.so, see BEHAVIOR.md §Layout).
//
// Scope: the plugin only ever reads the tree (in-order walk), unlinks nodes and rebuilds the
// free list. Insert/AllocNode are deliberately absent — they live in the tests only, so the
// shipped code cannot grow the map by accident.
//
// Everything here is plain memory access: no exceptions, no allocation, no engine calls.

#pragma once

#include <cstdint>

namespace gamebanfix {

// ---------------------------------------------------------------------------------------------
// Layout (all offsets verified against the analysed binary, BEHAVIOR.md §Layout):
//
//   GcBanMap + 0x08  int32  m_nCapacity     number of node slots
//            + 0x0C  int32  m_nAllocFlags   engine "has memory" flag (masked with 0x7FFFFFFF)
//            + 0x10  ptr    m_pMemory       node array
//            + 0x18  int32  m_nRoot         RB tree root index (-1 = empty)
//            + 0x1C  int32  m_nNumElements  element count
//            + 0x20  int32  m_nFirstFree    free list head (-1 = none)
//            + 0x24  int32  m_nLastAlloc    last slot the container ever touched (-1 = none);
//                                           the original's RemoveAll stops its sweep here
//
//   node  + 0x00  int32  m_iLeft     left child; == own index while the node is on the free list
//         + 0x04  int32  m_iRight    right child; next free node index while free
//         + 0x08  int32  m_iParent
//         + 0x0C  uint8  m_nTag      RB colour (0 = red, 1 = black)
//         + 0x10  uint32 m_nKey      map key (Steam account id)
//         + 0x18  uint32 m_uiReason  CGcBanInformation_t::m_uiReason (the "ban type")
//         + 0x20  double m_flUnknown
//         + 0x28  double m_flExpiration
//         + 0x30  uint32 m_uiAccountId
// ---------------------------------------------------------------------------------------------

constexpr std::int32_t kInvalidIndex = -1;
constexpr std::size_t kNodeSize = 0x38;
constexpr std::uint8_t kRed = 0;
constexpr std::uint8_t kBlack = 1;

struct GcBanNode {
	std::int32_t m_iLeft;        // +0x00
	std::int32_t m_iRight;       // +0x04
	std::int32_t m_iParent;      // +0x08
	std::uint8_t m_nTag;         // +0x0C
	std::uint8_t m_pad0[3];      // +0x0D
	std::uint32_t m_nKey;        // +0x10
	std::uint32_t m_pad1;        // +0x14
	std::uint32_t m_uiReason;    // +0x18
	std::uint32_t m_pad2;        // +0x1C
	double m_flUnknown;          // +0x20
	double m_flExpiration;       // +0x28
	std::uint32_t m_uiAccountId; // +0x30
	std::uint32_t m_pad3;        // +0x34
};
static_assert(sizeof(GcBanNode) == kNodeSize, "the engine node is 0x38 bytes");

struct GcBanMap {
	std::uint8_t m_pad0[8];      // +0x00 (less-functor storage, never touched here)
	std::int32_t m_nCapacity;    // +0x08
	std::int32_t m_nAllocFlags;  // +0x0C
	GcBanNode* m_pMemory;        // +0x10
	std::int32_t m_nRoot;        // +0x18
	std::int32_t m_nNumElements; // +0x1C
	std::int32_t m_nFirstFree;   // +0x20
	std::int32_t m_nLastAlloc;   // +0x24
};
static_assert(sizeof(GcBanMap) == 0x28, "the engine map header is 0x28 bytes");

// The map as the plugin sees it. The engine instance lives in the game's static storage; this
// class only wraps the pointer, it never owns it.
class GcBanTree {
public:
	explicit GcBanTree(GcBanMap* map) : m_pMap(map) {}

	[[nodiscard]] bool IsValid() const;

	// --- read-only traversal -----------------------------------------------------------------
	[[nodiscard]] std::int32_t FirstInorder() const;
	[[nodiscard]] std::int32_t NextInorder(std::int32_t i) const;
	[[nodiscard]] std::uint32_t Reason(std::int32_t i) const;
	[[nodiscard]] std::int32_t Count() const;

	// --- mutation (mirrors the engine's CUtlRBTree operations) --------------------------------
	// Unlink: removes the node from the tree, the RB invariants are restored (CLRS delete fixup).
	void Unlink(std::int32_t elem);
	// FreeNode: pushes the (already unlinked) slot back onto the container's free list.
	void FreeNode(std::int32_t i);
	// The engine's RemoveAll(): every slot is pushed onto the free list and the tree header is
	// marked empty. Used verbatim from the original's post hook.
	void RemoveAll();
	// Removes every entry whose reason is in `reasons` (in-order walk, successor computed before
	// the removal — the pattern the original uses).
	void RemoveReasons(const std::uint32_t* reasons, std::size_t count);

	// --- self-check (tests only; cheap enough to also run once at plugin start) ----------------
	[[nodiscard]] bool CheckInvariants() const;

private:
	[[nodiscard]] GcBanNode* NodePtr(std::int32_t i) const;
	[[nodiscard]] std::int32_t Left(std::int32_t i) const;
	[[nodiscard]] std::int32_t Right(std::int32_t i) const;
	[[nodiscard]] std::int32_t Parent(std::int32_t i) const;
	[[nodiscard]] std::uint8_t Color(std::int32_t i) const;
	void SetLeft(std::int32_t i, std::int32_t value) const;
	void SetRight(std::int32_t i, std::int32_t value) const;
	void SetParent(std::int32_t i, std::int32_t value) const;
	void SetColor(std::int32_t i, std::uint8_t value) const;
	[[nodiscard]] std::int32_t LeftMost(std::int32_t i) const;

	void RotateLeft(std::int32_t elem);
	void RotateRight(std::int32_t elem);
	void DeleteFixup(std::int32_t x, std::int32_t xParent);

	GcBanMap* m_pMap;
};

} // namespace gamebanfix
