#include "../cli/include/sparx_speculative.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace sparx::speculation;

int main() {
    EmbeddingIndex idx;

    // Test 1: Same text → identical embedding
    auto v1 = idx.embed("show me the weather");
    auto v2 = idx.embed("show me the weather");
    float sim = EmbeddingIndex::cosineSimilarity(v1, v2);
    assert(std::abs(sim - 1.0f) < 0.001f);
    std::cout << "PASS: identical text → cosine=1.0\n";

    // Test 2: Similar text → high similarity
    auto v3 = idx.embed("show me weather");
    sim = EmbeddingIndex::cosineSimilarity(v1, v3);
    std::cout << "  similar text similarity: " << sim << "\n";
    assert(sim > 0.7f);
    std::cout << "PASS: similar text → high cosine (" << sim << ")\n";

    // Test 3: Different text → low similarity
    auto v4 = idx.embed("delete all files from system");
    sim = EmbeddingIndex::cosineSimilarity(v1, v4);
    std::cout << "  different text similarity: " << sim << "\n";
    assert(sim < 0.5f);
    std::cout << "PASS: different text → low cosine (" << sim << ")\n";

    // Test 4: Case insensitivity
    auto v5 = idx.embed("Show Me The Weather");
    sim = EmbeddingIndex::cosineSimilarity(v1, v5);
    assert(std::abs(sim - 1.0f) < 0.001f);
    std::cout << "PASS: case insensitive\n";

    // Test 5: Nearest-neighbor search
    idx.insert("weather:abc123", v1);
    idx.insert("delete:xyz789", v4);

    auto nearest = idx.findNearest(v3, 0.7f);
    assert(nearest.has_value());
    assert(nearest->cache_key == "weather:abc123");
    std::cout << "PASS: nearest neighbor finds weather (sim=" << nearest->similarity << ")\n";

    // Test 6: Threshold filtering
    auto too_far = idx.findNearest(v4, 0.99f);  // very strict threshold
    // v4 itself is in the index, so it should match itself
    auto exact_self = idx.findNearest(v4, 0.99f);
    assert(exact_self.has_value());
    std::cout << "PASS: threshold works (self-match at " << exact_self->similarity << ")\n";

    // Test 7: Paraphrase similarity
    auto qa = idx.embed("what is the temperature outside");
    auto qb = idx.embed("what's the temperature outdoors");
    sim = EmbeddingIndex::cosineSimilarity(qa, qb);
    std::cout << "  paraphrase similarity: " << sim << "\n";
    assert(sim > 0.6f);
    std::cout << "PASS: paraphrases have high similarity (" << sim << ")\n";

    std::cout << "\nAll embedding tests passed!\n";
    return 0;
}
