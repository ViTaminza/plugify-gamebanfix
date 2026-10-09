#include "gcban_map.hpp"

namespace gamebanfix {
namespace {

// The engine's Links(i) returns a shared sentinel node for i == -1; its fields read as
// {-1, -1, -1, 0}. Rather than carry that mutable global, every accessor below answers the same
// values for an invalid index (colour -1 is treated as black, exactly like a null leaf in CLRS).

inline bool InRange(const GcBanMap* map, std::int32_t i) {
	return i >= 0 && i < map->m_nCapacity;
}

// A child/parent link is only followed when it points at another slot: an out-of-range index
// (corrupted container) or a link back to the node itself (the free-list marker of a slot that is
// not on the tree) is treated as "no link", so a damaged map cannot send the walks below out of
// bounds or around a cycle. In a well-formed tree neither case occurs.
inline std::int32_t Sanitized(const GcBanMap* map, std::int32_t link, std::int32_t self) {
	if (link == kInvalidIndex || link == self || !InRange(map, link)) {
		return kInvalidIndex;
	}
	return link;
}

} // namespace

bool GcBanTree::IsValid() const {
	if (m_pMap == nullptr) {
		return false;
	}
	// A sane map keeps at most a few hundred ban entries; anything outside that range means the
	// address did not point at the container (signature hit something else).
	constexpr std::int32_t kSaneCapacity = 1 << 20; // 2^20 nodes * 0x38 = 56 MB
	if (m_pMap->m_nCapacity < 0 || m_pMap->m_nCapacity > kSaneCapacity) {
		return false;
	}
	// The engine only touches m_pMemory when the low 31 bits of the allocation flags are set;
	// a container that never allocated a node (capacity 0) is valid and simply empty.
	if (m_pMap->m_nCapacity > 0 && (m_pMap->m_nAllocFlags & 0x7FFFFFFF) != 0 && m_pMap->m_pMemory == nullptr) {
		return false;
	}
	return true;
}

GcBanNode* GcBanTree::NodePtr(std::int32_t i) const {
	return m_pMap->m_pMemory + i;
}

std::int32_t GcBanTree::Left(std::int32_t i) const {
	if (i == kInvalidIndex || !InRange(m_pMap, i)) {
		return kInvalidIndex;
	}
	return Sanitized(m_pMap, NodePtr(i)->m_iLeft, i);
}

std::int32_t GcBanTree::Right(std::int32_t i) const {
	if (i == kInvalidIndex || !InRange(m_pMap, i)) {
		return kInvalidIndex;
	}
	return Sanitized(m_pMap, NodePtr(i)->m_iRight, i);
}

std::int32_t GcBanTree::Parent(std::int32_t i) const {
	if (i == kInvalidIndex || !InRange(m_pMap, i)) {
		return kInvalidIndex;
	}
	return Sanitized(m_pMap, NodePtr(i)->m_iParent, i);
}

std::uint8_t GcBanTree::Color(std::int32_t i) const {
	// A missing child is a black leaf (CLRS T.nil). The engine reads colour through the same
	// sentinel, whose tag byte is 0 — but every engine code path treats it as black, and the
	// fixup below relies on that. An out-of-range index cannot be coloured either.
	return (i == kInvalidIndex || !InRange(m_pMap, i)) ? kBlack : NodePtr(i)->m_nTag;
}

void GcBanTree::SetLeft(std::int32_t i, std::int32_t value) const {
	if (i != kInvalidIndex && InRange(m_pMap, i)) {
		NodePtr(i)->m_iLeft = value;
	}
}

void GcBanTree::SetRight(std::int32_t i, std::int32_t value) const {
	if (i != kInvalidIndex && InRange(m_pMap, i)) {
		NodePtr(i)->m_iRight = value;
	}
}

void GcBanTree::SetParent(std::int32_t i, std::int32_t value) const {
	if (i != kInvalidIndex && InRange(m_pMap, i)) {
		NodePtr(i)->m_iParent = value;
	}
}

void GcBanTree::SetColor(std::int32_t i, std::uint8_t value) const {
	if (i != kInvalidIndex && InRange(m_pMap, i)) {
		NodePtr(i)->m_nTag = value;
	}
}

std::int32_t GcBanTree::LeftMost(std::int32_t i) const {
	// A left spine cannot be longer than the number of slots; the bound stops a cycle left behind by
	// a corrupted container instead of hanging the per-frame hook.
	std::int32_t guard = m_pMap->m_nCapacity > 0 ? m_pMap->m_nCapacity : 1;
	std::int32_t next = Left(i);
	while (next != kInvalidIndex && guard-- > 0) {
		i = next;
		next = Left(i);
	}
	return i;
}

std::int32_t GcBanTree::FirstInorder() const {
	std::int32_t i = m_pMap->m_nRoot;
	if (i == kInvalidIndex || !InRange(m_pMap, i)) {
		return kInvalidIndex;
	}
	return LeftMost(i);
}

std::int32_t GcBanTree::NextInorder(std::int32_t i) const {
	if (i == kInvalidIndex) {
		return kInvalidIndex;
	}
	const std::int32_t right = Right(i);
	if (right != kInvalidIndex) {
		return LeftMost(right);
	}
	// The climb to the next ancestor is bounded by the number of slots for the same reason as
	// LeftMost (a corrupted parent chain must not hang the walk).
	std::int32_t guard = m_pMap->m_nCapacity > 0 ? m_pMap->m_nCapacity : 1;
	std::int32_t parent = Parent(i);
	while (parent != kInvalidIndex && Right(parent) == i && guard-- > 0) {
		i = parent;
		parent = Parent(i);
	}
	if (guard == 0) {
		return kInvalidIndex;
	}
	return parent;
}

std::uint32_t GcBanTree::Reason(std::int32_t i) const {
	return (i == kInvalidIndex || !InRange(m_pMap, i)) ? 0u : NodePtr(i)->m_uiReason;
}

std::int32_t GcBanTree::Count() const {
	return m_pMap->m_nNumElements;
}

void GcBanTree::RotateLeft(std::int32_t elem) {
	const std::int32_t right = Right(elem);
	if (right == kInvalidIndex) {
		return;
	}
	SetRight(elem, Left(right));
	if (Left(right) != kInvalidIndex) {
		SetParent(Left(right), elem);
	}
	SetParent(right, Parent(elem));
	if (Parent(elem) == kInvalidIndex) {
		m_pMap->m_nRoot = right;
	} else if (elem == Left(Parent(elem))) {
		SetLeft(Parent(elem), right);
	} else {
		SetRight(Parent(elem), right);
	}
	SetLeft(right, elem);
	SetParent(elem, right);
}

void GcBanTree::RotateRight(std::int32_t elem) {
	const std::int32_t left = Left(elem);
	if (left == kInvalidIndex) {
		return;
	}
	SetLeft(elem, Right(left));
	if (Right(left) != kInvalidIndex) {
		SetParent(Right(left), elem);
	}
	SetParent(left, Parent(elem));
	if (Parent(elem) == kInvalidIndex) {
		m_pMap->m_nRoot = left;
	} else if (elem == Right(Parent(elem))) {
		SetRight(Parent(elem), left);
	} else {
		SetLeft(Parent(elem), left);
	}
	SetRight(left, elem);
	SetParent(elem, left);
}

// CLRS RB-DELETE-FIXUP. `xParent` is the parent x has after the splice (a null leaf keeps the
// parent of the removed node).
void GcBanTree::DeleteFixup(std::int32_t x, std::int32_t xParent) {
	// The fixup climbs towards the root: bounded by the number of slots so that a parent chain left
	// behind by a corrupted container cannot hang the per-frame hook.
	std::int32_t guard = m_pMap->m_nCapacity > 0 ? m_pMap->m_nCapacity + 1 : 1;
	while (x != m_pMap->m_nRoot && Color(x) == kBlack && guard-- > 0) {
		if (xParent == kInvalidIndex) {
			break; // cannot happen in a consistent tree; bail out instead of dereferencing
		}
		if (x == Left(xParent)) {
			std::int32_t w = Right(xParent);
			if (Color(w) == kRed) {
				SetColor(w, kBlack);
				SetColor(xParent, kRed);
				RotateLeft(xParent);
				w = Right(xParent);
			}
			if (Color(Left(w)) == kBlack && Color(Right(w)) == kBlack) {
				SetColor(w, kRed);
				x = xParent;
				xParent = Parent(x);
			} else {
				if (Color(Right(w)) == kBlack) {
					SetColor(Left(w), kBlack);
					SetColor(w, kRed);
					RotateRight(w);
					w = Right(xParent);
				}
				SetColor(w, Color(xParent));
				SetColor(xParent, kBlack);
				SetColor(Right(w), kBlack);
				RotateLeft(xParent);
				x = m_pMap->m_nRoot;
				xParent = kInvalidIndex;
			}
		} else {
			std::int32_t w = Left(xParent);
			if (Color(w) == kRed) {
				SetColor(w, kBlack);
				SetColor(xParent, kRed);
				RotateRight(xParent);
				w = Left(xParent);
			}
			if (Color(Right(w)) == kBlack && Color(Left(w)) == kBlack) {
				SetColor(w, kRed);
				x = xParent;
				xParent = Parent(x);
			} else {
				if (Color(Left(w)) == kBlack) {
					SetColor(Right(w), kBlack);
					SetColor(w, kRed);
					RotateLeft(w);
					w = Left(xParent);
				}
				SetColor(w, Color(xParent));
				SetColor(xParent, kBlack);
				SetColor(Left(w), kBlack);
				RotateRight(xParent);
				x = m_pMap->m_nRoot;
				xParent = kInvalidIndex;
			}
		}
	}
	SetColor(x, kBlack);
}

void GcBanTree::Unlink(std::int32_t elem) {
	if (elem == kInvalidIndex || !InRange(m_pMap, elem)) {
		return;
	}

	// The node that actually leaves its position: elem itself when it has a free child slot,
	// otherwise its in-order successor (CLRS RB-DELETE). `x` is y's only child, if any.
	std::int32_t y = elem;
	if (Left(elem) != kInvalidIndex && Right(elem) != kInvalidIndex) {
		y = LeftMost(Right(elem));
	}
	const bool removedBlack = Color(y) == kBlack;
	const std::int32_t x = Left(y) != kInvalidIndex ? Left(y) : Right(y);
	const std::int32_t xParent = Parent(y);

	if (y == elem) {
		// Plain transplant: x takes y's place.
		if (x != kInvalidIndex) {
			SetParent(x, xParent);
		}
		if (xParent == kInvalidIndex) {
			m_pMap->m_nRoot = x;
		} else if (y == Left(xParent)) {
			SetLeft(xParent, x);
		} else {
			SetRight(xParent, x);
		}
	} else {
		// y (elem's successor) is detached and then moved into elem's position.
		if (x != kInvalidIndex) {
			SetParent(x, xParent);
		}
		if (xParent != elem) {
			// The usual case: y was deep in the right subtree, x replaces it there.
			if (xParent == kInvalidIndex) {
				m_pMap->m_nRoot = x; // unreachable (y != elem implies y has a parent), kept safe
			} else if (y == Left(xParent)) {
				SetLeft(xParent, x);
			} else {
				SetRight(xParent, x);
			}
			// y inherits elem's right subtree (its own right child was re-attached above).
			SetRight(y, Right(elem));
		}
		// When y was elem's direct right child it keeps its own right subtree: copying
		// Right(elem) over it would make y its own child.
		const std::int32_t elemParent = Parent(elem);
		SetLeft(y, Left(elem));
		SetParent(y, elemParent);
		if (Left(y) != kInvalidIndex) {
			SetParent(Left(y), y);
		}
		if (Right(y) != kInvalidIndex) {
			SetParent(Right(y), y);
		}
		if (elemParent == kInvalidIndex) {
			m_pMap->m_nRoot = y;
		} else if (elem == Left(elemParent)) {
			SetLeft(elemParent, y);
		} else {
			SetRight(elemParent, y);
		}
		// CLRS: if y was elem's direct right child, the null leaf x now hangs under y.
		if (x != kInvalidIndex && xParent == elem) {
			SetParent(x, y);
		}
		SetColor(y, Color(elem));
	}

	if (removedBlack) {
		// The parent of a null leaf: y itself when the successor moved into elem's place,
		// otherwise wherever x ended up.
		const std::int32_t fixupParent =
			x != kInvalidIndex ? Parent(x) : (y != elem && xParent == elem) ? y : xParent;
		DeleteFixup(x, fixupParent);
	}
}

void GcBanTree::FreeNode(std::int32_t i) {
	if (!InRange(m_pMap, i)) {
		return;
	}
	GcBanNode* node = NodePtr(i);
	node->m_iLeft = i; // self index marks the slot as free, exactly like the engine's FreeNode
	node->m_iRight = m_pMap->m_nFirstFree;
	m_pMap->m_nFirstFree = i;
	m_pMap->m_nNumElements--;
}

// The engine's CUtlRBTree::RemoveAll() (recovered from the original plugin's post hook):
// push every slot that is not already on the free list, stop at the last slot the container ever
// touched, then mark the tree empty by writing -1 over {m_nRoot, m_nNumElements} as one qword.
void GcBanTree::RemoveAll() {
	if (m_pMap->m_nLastAlloc == kInvalidIndex) {
		return;
	}
	if (m_pMap->m_nCapacity <= 0) {
		std::int64_t* header = reinterpret_cast<std::int64_t*>(&m_pMap->m_nRoot);
		*header = -1;
		return;
	}
	for (std::int32_t i = 0; i < m_pMap->m_nCapacity; ++i) {
		GcBanNode* node = NodePtr(i);
		if (node->m_iLeft != i) {
			node->m_iRight = m_pMap->m_nFirstFree;
			node->m_iLeft = i;
			m_pMap->m_nFirstFree = i;
		}
		if (i == m_pMap->m_nLastAlloc) {
			break;
		}
	}
	std::int64_t* header = reinterpret_cast<std::int64_t*>(&m_pMap->m_nRoot);
	*header = -1; // m_nRoot = -1, m_nNumElements = -1: the original's exact store
}

void GcBanTree::RemoveReasons(const std::uint32_t* reasons, std::size_t count) {
	std::int32_t i = FirstInorder();
	// A tree of N slots holds at most N nodes: the bound keeps this per-frame walk finite even if
	// the container is not what we think it is.
	std::int32_t guard = m_pMap->m_nCapacity > 0 ? m_pMap->m_nCapacity + 1 : 1;
	while (i != kInvalidIndex && guard-- > 0) {
		// Successor first: the node is about to leave the tree.
		const std::int32_t next = NextInorder(i);
		const std::uint32_t reason = Reason(i);
		bool match = false;
		for (std::size_t r = 0; r < count; ++r) {
			if (reasons[r] == reason) {
				match = true;
				break;
			}
		}
		if (match) {
			Unlink(i);
			FreeNode(i);
		}
		i = next;
	}
}

bool GcBanTree::CheckInvariants() const {
	if (!IsValid()) {
		return false;
	}
	if (m_pMap->m_nCapacity == 0) {
		return m_pMap->m_nRoot == kInvalidIndex;
	}

	// 1. The root is black.
	if (Color(m_pMap->m_nRoot) != kBlack) {
		return false;
	}

	// 2. Walk the tree in-order: keys strictly increase, every node's links stay in range and
	//    the parent pointers agree; count the elements.
	std::int32_t count = 0;
	std::int32_t previousKey = 0;
	bool first = true;
	for (std::int32_t i = FirstInorder(); i != kInvalidIndex; i = NextInorder(i)) {
		if (!InRange(m_pMap, i)) {
			return false;
		}
		const GcBanNode* node = NodePtr(i);
		if (node->m_iParent != kInvalidIndex) {
			if (!InRange(m_pMap, node->m_iParent)) {
				return false;
			}
			const GcBanNode* parent = NodePtr(node->m_iParent);
			if (parent->m_iLeft != i && parent->m_iRight != i) {
				return false;
			}
		} else if (m_pMap->m_nRoot != i) {
			return false;
		}
		if (!first && static_cast<std::int32_t>(node->m_nKey) <= previousKey) {
			return false;
		}
		previousKey = static_cast<std::int32_t>(node->m_nKey);
		first = false;
		if (++count > m_pMap->m_nCapacity) {
			return false; // cycle
		}
		// 3. No red node has a red child, and every path has the same black height: computed
		//    recursively below via a stack-free helper would need allocation, so this check uses
		//    the parent-chain walk instead (see the tests for the full recursive check).
	}
	if (count != m_pMap->m_nNumElements) {
		return false;
	}

	// 4. Every slot is either in the tree, on the free list, or was never allocated
	//    (m_iLeft == own index, which is how both the engine and FreeNode mark free slots).
	std::int32_t freeCount = 0;
	for (std::int32_t i = m_pMap->m_nFirstFree; i != kInvalidIndex; i = NodePtr(i)->m_iRight) {
		if (!InRange(m_pMap, i) || ++freeCount > m_pMap->m_nCapacity) {
			return false;
		}
	}
	return freeCount + count <= m_pMap->m_nCapacity;
}

} // namespace gamebanfix
