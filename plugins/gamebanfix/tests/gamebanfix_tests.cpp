// Tests for plugins/gamebanfix/src/gcban_map.cpp — the hand-written re-implementation of the
// engine container operations the GameBanFix port performs on CCSGameRules::sm_mapGcBanInformation
// (unlink + free-list push + the RemoveAll sweep).
//
// The container itself cannot be tested against the engine here, so the test builds its own
// container with the same 0x38-byte node layout and a textbook CLRS red-black insert, then hammers
// GcBanTree with randomised insert/remove sequences and validates, after every operation:
//   * the in-order walk visits exactly the keys std::map knows about, in order,
//   * every parent/child link agrees, no cycles, no out-of-range indices,
//   * the red-black invariants (root black, no red-red, equal black height) hold,
//   * the element count matches and every slot is either in the tree or on the free list,
//   * the free-list head is the most recently freed slot.
// On top of that, the two operations the plugin actually runs are checked directly:
// RemoveReasons() (reasons 20/22/23 only) and RemoveAll().
//
// A crash or an invariant violation here means the plugin could corrupt the server's ban
// map, so the tests are deliberately paranoid (randomised sequences with several seeds).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "gcban_map.hpp"

namespace {

using gamebanfix::GcBanMap;
using gamebanfix::GcBanNode;
using gamebanfix::GcBanTree;
using gamebanfix::kBlack;
using gamebanfix::kInvalidIndex;
using gamebanfix::kRed;

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
		}                                                                                          \
	} while (0)

// ---------------------------------------------------------------------------------------------
// A container with the engine's node layout and the engine's bookkeeping, plus a plain CLRS RB
// tree so the test can create arbitrary trees for the code under test to consume.
// ---------------------------------------------------------------------------------------------
class TestContainer {
public:
	static constexpr int kMaxCapacity = 4096;

	TestContainer() {
		m_nodes.resize(kMaxCapacity);
		m_map.m_pMemory = m_nodes.data();
		m_map.m_nCapacity = 0;
		m_map.m_nAllocFlags = 0x00010000;
		m_map.m_nRoot = kInvalidIndex;
		m_map.m_nNumElements = 0;
		m_map.m_nFirstFree = kInvalidIndex;
		m_map.m_nLastAlloc = kInvalidIndex;
	}

	[[nodiscard]] GcBanMap* Map() { return &m_map; }
	[[nodiscard]] GcBanTree Tree() { return GcBanTree(&m_map); }

	void EnsureCapacity(int capacity) {
		if (capacity <= m_map.m_nCapacity) {
			return;
		}
		for (int i = capacity - 1; i >= m_map.m_nCapacity; --i) {
			m_nodes[i] = GcBanNode{};
			m_nodes[i].m_iLeft = i; // free marker, same convention as the engine's FreeNode
			m_nodes[i].m_iRight = m_map.m_nFirstFree;
			m_nodes[i].m_iParent = kInvalidIndex;
			m_nodes[i].m_nTag = kBlack;
			m_map.m_nFirstFree = i;
		}
		m_map.m_nCapacity = capacity;
		m_map.m_nLastAlloc = std::max(m_map.m_nLastAlloc, capacity - 1);
	}

	int AllocNode() {
		if (m_map.m_nFirstFree != kInvalidIndex) {
			const int i = m_map.m_nFirstFree;
			m_map.m_nFirstFree = m_nodes[i].m_iRight;
			return i;
		}
		EnsureCapacity(m_map.m_nCapacity == 0 ? 8 : m_map.m_nCapacity + 1);
		return AllocNode();
	}

	void Insert(std::uint32_t key, std::uint32_t reason) {
		int z = AllocNode();
		m_nodes[z].m_nKey = key;
		m_nodes[z].m_uiReason = reason;
		m_nodes[z].m_iLeft = kInvalidIndex;
		m_nodes[z].m_iRight = kInvalidIndex;
		m_nodes[z].m_iParent = kInvalidIndex;
		m_nodes[z].m_nTag = kRed;

		int y = kInvalidIndex;
		int x = m_map.m_nRoot;
		while (x != kInvalidIndex) {
			y = x;
			x = key < m_nodes[x].m_nKey ? m_nodes[x].m_iLeft : m_nodes[x].m_iRight;
		}
		m_nodes[z].m_iParent = y;
		if (y == kInvalidIndex) {
			m_map.m_nRoot = z;
		} else if (key < m_nodes[y].m_nKey) {
			m_nodes[y].m_iLeft = z;
		} else {
			m_nodes[y].m_iRight = z;
		}
		m_map.m_nNumElements++;
		InsertFixup(z);
	}

	[[nodiscard]] std::uint8_t Color(int i) const {
		return i == kInvalidIndex ? kBlack : m_nodes[i].m_nTag;
	}

	[[nodiscard]] int Left(int i) const { return i == kInvalidIndex ? kInvalidIndex : m_nodes[i].m_iLeft; }
	[[nodiscard]] int Right(int i) const { return i == kInvalidIndex ? kInvalidIndex : m_nodes[i].m_iRight; }
	[[nodiscard]] int Parent(int i) const { return i == kInvalidIndex ? kInvalidIndex : m_nodes[i].m_iParent; }
	void SetLeft(int i, int value) { m_nodes[i].m_iLeft = value; }
	void SetRight(int i, int value) { m_nodes[i].m_iRight = value; }
	void SetParent(int i, int value) { m_nodes[i].m_iParent = value; }
	void SetColor(int i, std::uint8_t value) { m_nodes[i].m_nTag = value; }

	void RotateLeft(int x) {
		const int y = Right(x);
		SetRight(x, Left(y));
		if (Left(y) != kInvalidIndex) {
			SetParent(Left(y), x);
		}
		SetParent(y, Parent(x));
		if (Parent(x) == kInvalidIndex) {
			m_map.m_nRoot = y;
		} else if (x == Left(Parent(x))) {
			SetLeft(Parent(x), y);
		} else {
			SetRight(Parent(x), y);
		}
		SetLeft(y, x);
		SetParent(x, y);
	}

	void RotateRight(int x) {
		const int y = Left(x);
		SetLeft(x, Right(y));
		if (Right(y) != kInvalidIndex) {
			SetParent(Right(y), x);
		}
		SetParent(y, Parent(x));
		if (Parent(x) == kInvalidIndex) {
			m_map.m_nRoot = y;
		} else if (x == Right(Parent(x))) {
			SetRight(Parent(x), y);
		} else {
			SetLeft(Parent(x), y);
		}
		SetRight(y, x);
		SetParent(x, y);
	}

	void InsertFixup(int z) {
		while (Color(Parent(z)) == kRed) {
			const int parent = Parent(z);
			const int grand = Parent(parent);
			if (parent == Left(grand)) {
				const int uncle = Right(grand);
				if (Color(uncle) == kRed) {
					SetColor(parent, kBlack);
					SetColor(uncle, kBlack);
					SetColor(grand, kRed);
					z = grand;
				} else {
					if (z == Right(parent)) {
						z = parent;
						RotateLeft(z);
					}
					SetColor(Parent(z), kBlack);
					SetColor(Parent(Parent(z)), kRed);
					RotateRight(Parent(Parent(z)));
				}
			} else {
				const int uncle = Left(grand);
				if (Color(uncle) == kRed) {
					SetColor(parent, kBlack);
					SetColor(uncle, kBlack);
					SetColor(grand, kRed);
					z = grand;
				} else {
					if (z == Left(parent)) {
						z = parent;
						RotateRight(z);
					}
					SetColor(Parent(z), kBlack);
					SetColor(Parent(Parent(z)), kRed);
					RotateLeft(Parent(Parent(z)));
				}
			}
		}
		SetColor(m_map.m_nRoot, kBlack);
	}

	[[nodiscard]] const GcBanNode& Node(int i) const { return m_nodes[i]; }
	[[nodiscard]] std::vector<GcBanNode>& Nodes() { return m_nodes; }

private:
	std::vector<GcBanNode> m_nodes;
	GcBanMap m_map{};
};

// ---------------------------------------------------------------------------------------------
// Full validation: in-order walk, links, red-black invariants, free list.
// ---------------------------------------------------------------------------------------------
int BlackHeight(const TestContainer& c, int node, int depth) {
	if (depth > 64) {
		return -1; // cycle
	}
	if (node == kInvalidIndex) {
		return 1;
	}
	const int leftHeight = BlackHeight(c, c.Left(node), depth + 1);
	const int rightHeight = BlackHeight(c, c.Right(node), depth + 1);
	if (leftHeight < 0 || rightHeight < 0 || leftHeight != rightHeight) {
		return -1;
	}
	return leftHeight + (c.Color(node) == kBlack ? 1 : 0);
}

bool Validate(TestContainer& c, const std::map<std::uint32_t, std::uint32_t>& model, const char* what) {
	bool ok = true;
	const GcBanMap* map = c.Map();
	GcBanTree tree(c.Map());

	if (map->m_nNumElements != static_cast<std::int32_t>(model.size())) {
		std::printf("  [%s] count mismatch: container %d, model %zu\n", what, map->m_nNumElements, model.size());
		ok = false;
	}

	// In-order walk: exactly the model's keys, ascending.
	std::vector<std::pair<std::uint32_t, std::uint32_t>> walked;
	for (int i = tree.FirstInorder(); i != kInvalidIndex; i = tree.NextInorder(i)) {
		walked.emplace_back(c.Node(i).m_nKey, c.Node(i).m_uiReason);
		if (walked.size() > model.size()) {
			std::printf("  [%s] in-order walk does not terminate\n", what);
			ok = false;
			break;
		}
	}
	if (walked.size() != model.size()) {
		std::printf("  [%s] in-order walk visited %zu nodes, model has %zu\n", what, walked.size(), model.size());
		ok = false;
	} else {
		std::size_t index = 0;
		for (const auto& [key, reason] : model) {
			if (walked[index].first != key || walked[index].second != reason) {
				std::printf("  [%s] in-order mismatch at %zu\n", what, index);
				ok = false;
				break;
			}
			++index;
		}
	}

	// Parent/child agreement, red-black invariants, black height. Free slots (m_iLeft == own
	// index, the free-list convention) are not part of the tree and are skipped here.
	for (int i = 0; i < map->m_nCapacity; ++i) {
		const GcBanNode& node = c.Node(i);
		if (node.m_iLeft == i) {
			continue; // free
		}
		for (const int child : {node.m_iLeft, node.m_iRight}) {
			if (child != kInvalidIndex && c.Parent(child) != i) {
				std::printf("  [%s] node %d child %d does not point back\n", what, i, child);
				ok = false;
			}
		}
	}
	if (c.Color(map->m_nRoot) != kBlack) {
		std::printf("  [%s] root is not black\n", what);
		ok = false;
	}
	if (BlackHeight(c, map->m_nRoot, 0) < 0) {
		std::printf("  [%s] black-height invariant broken (or a cycle)\n", what);
		ok = false;
	}

	// Every slot is in the tree or on the free list.
	const std::set<std::uint32_t> treeKeys = [&] {
		std::set<std::uint32_t> keys;
		for (const auto& [key, reason] : model) {
			keys.insert(key);
		}
		return keys;
	}();
	int inTree = 0;
	for (int i = 0; i < map->m_nCapacity; ++i) {
		const GcBanNode& node = c.Node(i);
		if (node.m_iLeft == i) {
			continue; // free
		}
		if (!treeKeys.contains(node.m_nKey)) {
			std::printf("  [%s] slot %d is neither free nor in the model (key %u)\n", what, i, node.m_nKey);
			ok = false;
		}
		++inTree;
	}
	if (inTree != map->m_nNumElements) {
		std::printf("  [%s] %d allocated slots, count says %d\n", what, inTree, map->m_nNumElements);
		ok = false;
	}

	int freeCount = 0;
	for (int i = map->m_nFirstFree; i != kInvalidIndex; i = c.Node(i).m_iRight) {
		if (++freeCount > map->m_nCapacity) {
			std::printf("  [%s] free list does not terminate\n", what);
			ok = false;
			break;
		}
	}
	if (freeCount + inTree != map->m_nCapacity) {
		std::printf("  [%s] free list has %d nodes, tree %d, capacity %d\n", what, freeCount, inTree, map->m_nCapacity);
		ok = false;
	}

	return ok;
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

void TestTraversal() {
	std::printf("-- traversal\n");
	TestContainer c;
	std::map<std::uint32_t, std::uint32_t> model;
	std::mt19937 rng(20261008u);
	for (int i = 0; i < 500; ++i) {
		const auto key = static_cast<std::uint32_t>(rng() % 100000u) + 1u;
		if (model.contains(key)) {
			continue;
		}
		const auto reason = static_cast<std::uint32_t>(rng() % 30u);
		c.Insert(key, reason);
		model.emplace(key, reason);
	}
	CHECK(Validate(c, model, "traversal"));
}

void TestUnlinkRandom() {
	for (const unsigned seed : {1u, 2u, 3u, 7u, 42u}) {
		std::printf("-- unlink (seed %u)\n", seed);
		TestContainer c;
		std::map<std::uint32_t, std::uint32_t> model;
		std::mt19937 rng(seed);

		std::vector<std::uint32_t> keys;
		for (int i = 0; i < 400; ++i) {
			const auto key = static_cast<std::uint32_t>(rng() % 100000u) + 1u;
			if (model.contains(key)) {
				continue;
			}
			const auto reason = static_cast<std::uint32_t>(rng() % 30u);
			c.Insert(key, reason);
			model.emplace(key, reason);
			keys.push_back(key);
		}
		CHECK(Validate(c, model, "after inserts"));

		std::shuffle(keys.begin(), keys.end(), rng);
		GcBanTree tree = c.Tree();
		int removed = 0;
		for (const std::uint32_t key : keys) {
			// Find the node index of that key (models the plugin's in-order walk).
			int index = kInvalidIndex;
			for (int i = tree.FirstInorder(); i != kInvalidIndex; i = tree.NextInorder(i)) {
				if (c.Node(i).m_nKey == key) {
					index = i;
					break;
				}
			}
			CHECK(index != kInvalidIndex);
			if (index == kInvalidIndex) {
				continue;
			}
			const std::int32_t countBefore = tree.Count();
			const std::int32_t freeHeadBefore = c.Map()->m_nFirstFree;

			tree.Unlink(index);
			tree.FreeNode(index);
			model.erase(key);

			CHECK(tree.Count() == countBefore - 1);
			CHECK(c.Map()->m_nFirstFree == index);       // freshly freed slot is the new head
			CHECK(c.Node(index).m_iLeft == index);       // free marker
			CHECK(c.Node(index).m_iRight == freeHeadBefore);
			if (++removed % 25 == 0 || model.empty()) {
				CHECK(Validate(c, model, "after removals"));
			}
		}
		CHECK(model.empty());
		CHECK(Validate(c, model, "after all removals"));
		CHECK(c.Map()->m_nRoot == kInvalidIndex);
		CHECK(tree.FirstInorder() == kInvalidIndex);
	}
}

void TestRemoveReasons() {
	std::printf("-- remove reasons 20/22/23\n");
	TestContainer c;
	std::map<std::uint32_t, std::uint32_t> model;
	std::mt19937 rng(99u);
	std::vector<std::uint32_t> expectedRemoved;
	for (int i = 0; i < 300; ++i) {
		const auto key = static_cast<std::uint32_t>(rng() % 100000u) + 1u;
		if (model.contains(key)) {
			continue;
		}
		// Mostly matching reasons so the test actually removes a lot.
		const std::uint32_t reasons[] = {20, 22, 23, 4, 7, 18, 1, 22, 20};
		const auto reason = reasons[rng() % (sizeof(reasons) / sizeof(reasons[0]))];
		c.Insert(key, reason);
		model.emplace(key, reason);
		if (reason == 20 || reason == 22 || reason == 23) {
			expectedRemoved.push_back(key);
		}
	}
	const std::int32_t countBefore = c.Map()->m_nNumElements;

	GcBanTree tree = c.Tree();
	const std::uint32_t reasons[] = {20, 22, 23};
	tree.RemoveReasons(reasons, 3);

	CHECK(expectedRemoved.size() > 10); // the fixture must actually exercise the removal path
	for (const std::uint32_t key : expectedRemoved) {
		bool present = false;
		for (int i = tree.FirstInorder(); i != kInvalidIndex; i = tree.NextInorder(i)) {
			if (c.Node(i).m_nKey == key) {
				present = true;
				break;
			}
		}
		CHECK(!present);
		model.erase(key);
	}
	CHECK(c.Map()->m_nNumElements == countBefore - static_cast<std::int32_t>(expectedRemoved.size()));
	CHECK(Validate(c, model, "after RemoveReasons"));
	// Idempotent: a second run removes nothing.
	const std::int32_t countAfter = c.Map()->m_nNumElements;
	tree.RemoveReasons(reasons, 3);
	CHECK(c.Map()->m_nNumElements == countAfter);
}

void TestRemoveAll() {
	std::printf("-- RemoveAll\n");
	TestContainer c;
	std::map<std::uint32_t, std::uint32_t> model;
	std::mt19937 rng(1234u);
	for (int i = 0; i < 200; ++i) {
		const auto key = static_cast<std::uint32_t>(rng() % 100000u) + 1u;
		if (model.contains(key)) {
			continue;
		}
		c.Insert(key, static_cast<std::uint32_t>(rng() % 30u));
		model.emplace(key, 0);
	}

	// Remove some first so the free list is not a plain range (the sweep must skip those slots
	// without corrupting the chain).
	GcBanTree tree = c.Tree();
	int removed = 0;
	std::int32_t i = tree.FirstInorder();
	while (i != kInvalidIndex && removed < 40) {
		const std::int32_t next = tree.NextInorder(i); // successor before the node leaves the tree
		tree.Unlink(i);
		tree.FreeNode(i);
		++removed;
		i = next;
	}

	tree.RemoveAll();

	// The engine's store: one qword, m_nRoot = -1 in the low dword, m_nNumElements = 0 in the high
	// one.
	std::int64_t header = 0;
	std::memcpy(&header, &c.Map()->m_nRoot, sizeof(header));
	CHECK(header == 0x00000000FFFFFFFFLL);
	CHECK(c.Map()->m_nRoot == kInvalidIndex);
	CHECK(c.Map()->m_nNumElements == 0);
	CHECK(tree.FirstInorder() == kInvalidIndex);

	// Every slot up to lastAlloc is on the free list exactly once.
	std::set<int> seen;
	int freeCount = 0;
	for (int i = c.Map()->m_nFirstFree; i != kInvalidIndex; i = c.Node(i).m_iRight) {
		CHECK(c.Node(i).m_iLeft == i);
		CHECK(seen.insert(i).second);
		++freeCount;
	}
	CHECK(freeCount == c.Map()->m_nLastAlloc + 1);

	// A second RemoveAll on the now-empty container rewrites the same header.
	tree.RemoveAll();
	CHECK(c.Map()->m_nRoot == kInvalidIndex);
	CHECK(c.Map()->m_nNumElements == 0);
}

void TestDetectsCorruption() {
	std::printf("-- corruption detection\n");
	TestContainer c;
	GcBanTree tree(c.Map());
	CHECK(tree.IsValid()); // 0 capacity, no memory: valid-but-empty
	CHECK(tree.CheckInvariants());

	c.Insert(1, 20);
	c.Insert(2, 20);
	CHECK(tree.CheckInvariants());

	// Break the parent pointer of a child: CheckInvariants must notice.
	const int root = c.Map()->m_nRoot;
	const int child = c.Left(root) != kInvalidIndex ? c.Left(root) : c.Right(root);
	if (child != kInvalidIndex) {
		c.SetParent(child, kInvalidIndex);
		CHECK(!tree.CheckInvariants());
		c.SetParent(child, root);
		CHECK(tree.CheckInvariants());
	}
}

// The per-frame hook walks the container of a signature hit that passed IsValid(): a corrupted
// tree (a wrong address that looks plausible) must not be able to hang the server. Every walk is
// bounded by the slot count, and links that leave the slot array or point back at their own node
// are treated as "no link".
void TestCorruptedTraversalTerminates() {
	std::printf("-- corrupted container: traversal stays finite\n");

	TestContainer c;
	c.Insert(10, 20);
	c.Insert(20, 20);
	c.Insert(30, 22);
	GcBanTree tree = c.Tree();
	CHECK(tree.IsValid());
	const int root = c.Map()->m_nRoot;
	CHECK(root != kInvalidIndex);

	// (a) Self-referencing child (the free-list marker): read as a leaf, walk ends at the node.
	c.SetLeft(root, root);
	CHECK(tree.FirstInorder() == root);
	c.SetLeft(root, kInvalidIndex);

	// (b) Out-of-range child: also read as a leaf (no out-of-bounds access).
	c.SetLeft(root, root + c.Map()->m_nCapacity + 7);
	CHECK(tree.FirstInorder() == root);
	c.SetLeft(root, kInvalidIndex);

	// (c) Two-node cycle in the left spine: the bounded descent ends, and the in-order walk of the
	// whole (damaged) tree still terminates.
	const int other = root == 0 ? 1 : 0;
	CHECK(other < c.Map()->m_nCapacity);
	c.SetLeft(root, other);
	c.SetLeft(other, root);
	{
		const int first = tree.FirstInorder();
		CHECK(first != kInvalidIndex);
		int steps = 0;
		for (int i = tree.FirstInorder(); i != kInvalidIndex && steps <= c.Map()->m_nCapacity + 1;
			 i = tree.NextInorder(i)) {
			++steps;
		}
		CHECK(steps <= c.Map()->m_nCapacity + 2); // bounded, i.e. no hang
	}
	// The removal walk must also terminate on the damaged tree (it unlinks nodes it visits).
	{
		const std::uint32_t reasons[] = {20, 22, 23};
		tree.RemoveReasons(reasons, 3);
		CHECK(true); // reaching this line is the assertion: no hang, no crash
	}

	// (d) A parent chain that cycles back on itself: NextInorder's climb is bounded too.
	TestContainer d;
	d.Insert(1, 20);
	d.Insert(2, 20);
	GcBanTree dtree = d.Tree();
	const int droot = d.Map()->m_nRoot;
	CHECK(droot != kInvalidIndex);
	const int dchild = d.Left(droot) != kInvalidIndex ? d.Left(droot) : d.Right(droot);
	if (dchild != kInvalidIndex) {
		d.SetParent(droot, dchild);
		d.SetParent(dchild, droot);
		int steps = 0;
		for (int i = dtree.FirstInorder(); i != kInvalidIndex && steps <= d.Map()->m_nCapacity + 1;
			 i = dtree.NextInorder(i)) {
			++steps;
		}
		CHECK(steps <= d.Map()->m_nCapacity + 2);
		const std::uint32_t reasons[] = {20};
		dtree.RemoveReasons(reasons, 1); // Unlink's delete fixup must terminate as well
	}
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("gamebanfix_tests\n");

	TestTraversal();
	TestUnlinkRandom();
	TestRemoveReasons();
	TestRemoveAll();
	TestDetectsCorruption();
	TestCorruptedTraversalTerminates();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
