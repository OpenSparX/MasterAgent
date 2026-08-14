#include "../cli/include/sparx_mesh.h"
#include <cassert>
#include <iostream>

using namespace sparx::mesh;

int main() {
    // Test 1: Two nodes add same element — both tags survive merge
    CrdtStateSync nodeA("nodeA");
    CrdtStateSync nodeB("nodeB");

    auto opA = nodeA.mutate("fruits", CrdtType::ORSet, "apple");
    auto opB = nodeB.mutate("fruits", CrdtType::ORSet, "apple");

    // Cross-merge
    nodeA.merge(opB);
    nodeB.merge(opA);

    auto stateA = nodeA.get("fruits");
    assert(stateA.has_value());
    // Both tags present — apple appears in the alive section
    assert(stateA->value.find("apple") != std::string::npos);
    std::cout << "PASS: concurrent add - element survives\n";

    // Test 2: Multiple elements across nodes
    auto opBanana = nodeB.mutate("fruits", CrdtType::ORSet, "banana");
    auto opCherry = nodeA.mutate("fruits", CrdtType::ORSet, "cherry");

    nodeA.merge(opBanana);
    nodeB.merge(opCherry);

    stateA = nodeA.get("fruits");
    assert(stateA->value.find("apple") != std::string::npos);
    assert(stateA->value.find("banana") != std::string::npos);
    assert(stateA->value.find("cherry") != std::string::npos);
    std::cout << "PASS: multi-element ORSet\n";

    // Test 3: Tag uniqueness — same element added twice gets different tags
    CrdtStateSync node("solo");
    node.mutate("items", CrdtType::ORSet, "x");
    node.mutate("items", CrdtType::ORSet, "x");
    auto state = node.get("items");
    // Should have two tags for "x" (solo#1 and solo#2)
    assert(state->value.find("solo#1") != std::string::npos);
    assert(state->value.find("solo#2") != std::string::npos);
    std::cout << "PASS: multiple adds generate unique tags\n";

    // Test 4: single-element propagation
    CrdtStateSync empty1("e1");
    CrdtStateSync empty2("e2");
    auto op1 = empty1.mutate("k", CrdtType::ORSet, "v");
    empty2.merge(op1);
    auto s = empty2.get("k");
    assert(s.has_value());
    assert(s->value.find("v") != std::string::npos);
    std::cout << "PASS: single-element propagation\n";

    // Test 5: Remove element — observed-remove semantics
    {
        CrdtStateSync n1("n1");
        CrdtStateSync n2("n2");

        auto addOp = n1.mutate("set", CrdtType::ORSet, "item");
        n2.merge(addOp);

        // n2 observes the element, then removes it
        auto rmOp = n2.removeFromORSet("set", "item");
        n1.merge(rmOp);

        // Both nodes should no longer see the item
        auto s1 = n1.get("set");
        auto s2 = n2.get("set");
        // The element's tag is tombstoned — "item" shouldn't appear in alive section
        assert(s1->value.find("item\x1f") == std::string::npos);
        assert(s2->value.find("item\x1f") == std::string::npos);
        std::cout << "PASS: remove propagates and element disappears\n";
    }

    // Test 6: Add-wins — concurrent add + remove
    {
        CrdtStateSync n1("n1");
        CrdtStateSync n2("n2");

        auto addOp1 = n1.mutate("set", CrdtType::ORSet, "item");
        n2.merge(addOp1);

        // n2 removes, n1 concurrently re-adds
        auto rmOp = n2.removeFromORSet("set", "item");
        auto addOp2 = n1.mutate("set", CrdtType::ORSet, "item");  // new tag

        // Cross-merge
        n1.merge(rmOp);
        n2.merge(addOp2);

        // The re-add's tag is NOT tombstoned → item survives (add-wins)
        auto s1 = n1.get("set");
        auto s2 = n2.get("set");
        assert(s1->value.find("item") != std::string::npos);
        assert(s2->value.find("item") != std::string::npos);
        std::cout << "PASS: add-wins — concurrent re-add survives remove\n";
    }

    std::cout << "\nAll ORSet tests passed!\n";
    return 0;
}
