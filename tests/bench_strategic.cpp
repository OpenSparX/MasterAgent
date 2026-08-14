#include "../cli/include/sparx_speculative.h"
#include "../cli/include/sparx_mesh.h"
#include "../cli/include/sparx_formal_verify.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>

using namespace std::chrono;
using namespace sparx;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template<typename F>
double benchmarkUs(F&& fn, int iterations = 1000) {
    auto start = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) fn();
    auto end = high_resolution_clock::now();
    return duration_cast<nanoseconds>(end - start).count() / (iterations * 1000.0);
}

void printResult(const char* name, double us, int iterations = 1000) {
    std::cout << std::left << std::setw(45) << name
              << std::right << std::setw(10) << std::fixed << std::setprecision(2)
              << us << " μs"
              << "  (" << iterations << " iters)\n";
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

void benchEmbedding() {
    std::cout << "\n=== Embedding Index ===\n";
    speculation::EmbeddingIndex idx;

    std::string short_text = "show weather";
    std::string medium_text = "what is the current temperature outside today";
    std::string long_text = "please check the weather forecast for tomorrow "
                            "and tell me if I need an umbrella for my commute";

    printResult("embed(short, 12 chars)", benchmarkUs([&]{ idx.embed(short_text); }));
    printResult("embed(medium, 46 chars)", benchmarkUs([&]{ idx.embed(medium_text); }));
    printResult("embed(long, 95 chars)", benchmarkUs([&]{ idx.embed(long_text); }));

    // Index 16 entries then search
    for (int i = 0; i < 16; ++i) {
        auto v = idx.embed("entry number " + std::to_string(i));
        idx.insert("key" + std::to_string(i), v);
    }
    auto query = idx.embed("entry number 7");
    printResult("findNearest(16 entries)", benchmarkUs([&]{ idx.findNearest(query, 0.8f); }));

    auto v1 = idx.embed("hello world");
    auto v2 = idx.embed("goodbye world");
    printResult("cosineSimilarity()", benchmarkUs([&]{
        speculation::EmbeddingIndex::cosineSimilarity(v1, v2);
    }, 10000), 10000);
}

void benchMerkle() {
    std::cout << "\n=== Merkle Anti-Entropy ===\n";
    mesh::CrdtStateSync node("bench");

    // Build state with N keys
    std::map<std::string, mesh::StateEntry> state100, state1000;

    for (int i = 0; i < 100; ++i) {
        node.mutate("k" + std::to_string(i), mesh::CrdtType::LWWRegister,
                    "v" + std::to_string(i));
    }
    for (const auto& e : node.allState()) state100[e.key] = e;

    for (int i = 100; i < 1000; ++i) {
        node.mutate("k" + std::to_string(i), mesh::CrdtType::LWWRegister,
                    "v" + std::to_string(i));
    }
    for (const auto& e : node.allState()) state1000[e.key] = e;

    mesh::MerkleAntiEntropy merkle100, merkle1000;

    printResult("rebuild(100 keys)", benchmarkUs([&]{ merkle100.rebuild(state100); }, 100), 100);
    printResult("rebuild(1000 keys)", benchmarkUs([&]{ merkle1000.rebuild(state1000); }, 100), 100);

    merkle100.rebuild(state100);
    merkle1000.rebuild(state1000);

    auto digest100 = merkle100.digest();
    auto digest1000 = merkle1000.digest();

    printResult("digest(100 keys)", benchmarkUs([&]{ merkle100.digest(); }));
    printResult("digest(1000 keys)", benchmarkUs([&]{ merkle1000.digest(); }, 500), 500);

    printResult("compare(same, 100 keys) [O(1)]", benchmarkUs([&]{
        merkle100.compare(digest100);
    }, 10000), 10000);
    printResult("compare(same, 1000 keys) [O(1)]", benchmarkUs([&]{
        merkle1000.compare(digest1000);
    }, 10000), 10000);

    // Simulate 1-key divergence
    mesh::MerkleAntiEntropy merkle100b;
    auto state100b = state100;
    state100b["k50"].value = "changed";
    state100b["k50"].last_modified = 99999;
    merkle100b.rebuild(state100b);
    auto digest100b = merkle100b.digest();

    printResult("compare(1-key divergence, 100 keys)", benchmarkUs([&]{
        merkle100.compare(digest100b);
    }));
}

void benchORSet() {
    std::cout << "\n=== ORSet CRDT ===\n";
    mesh::CrdtStateSync nodeA("A");
    mesh::CrdtStateSync nodeB("B");

    printResult("mutate(ORSet add)", benchmarkUs([&]{
        nodeA.mutate("set", mesh::CrdtType::ORSet, "elem");
    }));

    auto op = nodeA.mutate("mergeset", mesh::CrdtType::ORSet, "x");
    printResult("merge(ORSet op)", benchmarkUs([&]{ nodeB.merge(op); }));

    // Build up 50 elements then remove
    for (int i = 0; i < 50; ++i) {
        nodeA.mutate("bigset", mesh::CrdtType::ORSet, "item" + std::to_string(i));
    }
    printResult("removeFromORSet(50 elements)", benchmarkUs([&]{
        nodeA.removeFromORSet("bigset", "item25");
    }, 100), 100);
}

void benchPOR() {
    std::cout << "\n=== Formal Verification (POR) ===\n";

    // Build a plan with independent nodes (best case for POR)
    std::vector<formal::PlanNode> plan;
    for (int i = 0; i < 10; ++i) {
        formal::PlanNode n;
        n.id = "node" + std::to_string(i);
        n.tool_name = "tool" + std::to_string(i);
        n.service = "svc" + std::to_string(i % 5);
        n.resources = {"res" + std::to_string(i)};  // unique resources
        plan.push_back(n);
    }

    formal::VerifierConfig config;
    config.enable_por = true;
    formal::PlanVerifier verifierPOR(config);

    config.enable_por = false;
    formal::PlanVerifier verifierNoPOR(config);

    printResult("verify(10 nodes, POR enabled)", benchmarkUs([&]{
        verifierPOR.verify(plan);
    }, 500), 500);
    printResult("verify(10 nodes, POR disabled)", benchmarkUs([&]{
        verifierNoPOR.verify(plan);
    }, 500), 500);

    // Larger plan with dependencies (worst case)
    std::vector<formal::PlanNode> depPlan;
    for (int i = 0; i < 20; ++i) {
        formal::PlanNode n;
        n.id = "n" + std::to_string(i);
        n.tool_name = "t" + std::to_string(i);
        n.service = "shared_svc";
        n.is_destructive = (i % 3 == 0);
        if (i > 0) n.deps.push_back("n" + std::to_string(i - 1));
        depPlan.push_back(n);
    }

    printResult("verify(20 nodes, dependent, POR)", benchmarkUs([&]{
        verifierPOR.verify(depPlan);
    }, 200), 200);
}

void benchSpeculation() {
    std::cout << "\n=== Speculative Execution ===\n";
    speculation::IntentPredictor predictor;

    // Warm up predictor
    for (int i = 0; i < 50; ++i) {
        speculation::IntentRecord rec;
        rec.intent_name = "intent" + std::to_string(i % 5);
        rec.raw_input = "input " + std::to_string(i);
        rec.hour_of_day = 10;
        predictor.observe(rec);
    }

    speculation::IntentRecord rec;
    rec.intent_name = "intent3";
    rec.raw_input = "test input";
    rec.hour_of_day = 10;

    printResult("observe()", benchmarkUs([&]{ predictor.observe(rec); }));
    printResult("predict(top-3)", benchmarkUs([&]{ predictor.predict(3); }));

    // Cache operations
    speculation::SpeculationCache cache;
    speculation::SpeculativeResult sr;
    sr.cache_key = "intent0:hash123";
    sr.raw_output = std::string(500, 'x');
    sr.intent_name = "intent0";
    sr.prediction_confidence = 0.9f;
    sr.computed_at_utc = time(nullptr);
    sr.ttl_seconds = 300;
    sr.context_hash = "ctx1";
    sr.model_id = "local";
    sr.token_count = 125;
    cache.put(sr);

    printResult("cache.get(exact hit)", benchmarkUs([&]{
        cache.get("intent0", "hash123", "ctx1");
    }));
    printResult("cache.get(miss → similarity fallback)", benchmarkUs([&]{
        cache.get("intent0", "hash999", "ctx1");
    }));
}

int main() {
    std::cout << "OpenSparX Strategic Features — Performance Benchmark\n";
    std::cout << "====================================================\n";

    benchEmbedding();
    benchMerkle();
    benchORSet();
    benchPOR();
    benchSpeculation();

    std::cout << "\n====================================================\n";
    std::cout << "Done. All latencies in microseconds (μs).\n";
    return 0;
}
