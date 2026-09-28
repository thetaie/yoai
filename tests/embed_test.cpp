// llm/embed/local.hpp and store/vector_index.hpp together: store a few diary
// lines, ask questions, and check each question finds its line.
//
// The pairs are chosen so that each question has exactly one answer among the
// lines, and two of them cross languages -- a Chinese question for an English
// line and the other way round -- since the app's users will write both. A
// model that loads and returns vectors of the right length can still rank
// badly, and ranking is the only thing retrieval is for.
//
//     build/yoai-embed-test <Qwen3-Embedding-0.6B.gguf> [-t threads]
//
// Without YOAI_ONDEVICE_LLM it says so and exits cleanly.

#include "llm/embed/local.hpp"
#include "store/db.hpp"
#include "store/vector_index.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using yoai::llm::EmbedKind;
using yoai::llm::Embedder;
using yoai::llm::LocalEmbedOptions;
using yoai::llm::LocalEmbedder;
using yoai::store::Db;
using yoai::store::Hit;
using yoai::store::VectorIndex;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

bool did(bool ok, const std::string& err, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s: %s\n", what, err.c_str());
        ++g_failures;
    }
    return ok;
}

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Line {
    long long   id;
    const char* text;
};

const Line kLines[] = {
    { 1, "Had beef noodles at the canteen for lunch." },
    { 2, "Ran five kilometers along the river before work." },
    { 3, "Design review with the team about the timeline screen." },
    { 4, "晚上十一点半才睡，睡前一直在刷手机。" },
    { 5, "下午带孩子去公园放风筝。" },
};

struct Question {
    const char* text;
    long long   answer;
};

const Question kQuestions[] = {
    { "What did I eat for lunch?",   1 },
    { "我今天运动了吗？",               2 },   // Chinese question, English line
    { "What did the meeting cover?", 3 },
    { "昨晚几点睡的？",                 4 },
    { "What did I do with my kid?",  5 },   // English question, Chinese line
};

const char* kTask = "Given a question about the user's day, retrieve the diary line that answers it";

}  // namespace

int main(int argc, char** argv) {
    if (!LocalEmbedder::available()) {
        std::printf("this build has no on-device engine -- configure with "
                    "-DYOAI_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=...\n");
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <embedding-model.gguf> [-t threads]\n", argv[0]);
        return 2;
    }

    LocalEmbedOptions opt;
    opt.model_path = argv[1];
    opt.query_task = kTask;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            opt.n_threads = std::atoi(argv[++i]);
        }
    }

    std::string   err;
    LocalEmbedder local(opt);
    // Everything below goes through the interface, the way callers will.
    Embedder&     emb = local;

    // --- load ------------------------------------------------------------------
    double t0 = now_ms();
    if (!did(emb.load(err), err, "load")) {
        return 1;
    }
    std::printf("      loaded in %.0f ms, %d dimensions\n", now_ms() - t0, emb.dim());
    check(emb.dim() > 0, "the model reports a vector length");

    // --- one vector ------------------------------------------------------------
    std::vector<float> a, b;
    did(emb.embed(kLines[0].text, EmbedKind::Document, &a, err), err, "embed");
    check(static_cast<int>(a.size()) == emb.dim(), "the vector has dim() floats");
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += static_cast<double>(a[i]) * a[i];
    }
    check(std::fabs(sum - 1.0) < 1e-4, "the vector is unit length");

    did(emb.embed(kLines[0].text, EmbedKind::Document, &b, err), err, "embed again");
    check(a == b, "the same text gives the same vector");

    // The instruction goes in front of queries only. Equal vectors here would
    // mean it never reached the model -- retrieval would still mostly work,
    // just worse, which is why it is checked rather than trusted.
    did(emb.embed(kLines[0].text, EmbedKind::Query, &b, err), err, "embed as a query");
    check(a != b, "a query is embedded differently from a document");

    // --- retrieval -------------------------------------------------------------
    std::shared_ptr<Db> store = Db::open(":memory:", &err);
    if (!did(store != nullptr, err, "Db::open")) {
        return 1;
    }
    const std::string create = "CREATE VIRTUAL TABLE embeddings USING vec0(embedding float[" +
                               std::to_string(emb.dim()) + "]);";
    if (sqlite3_exec(store->handle(), create.c_str(), 0, 0, 0) != SQLITE_OK) {
        std::printf("FAIL  create vec0 table: %s\n", sqlite3_errmsg(store->handle()));
        return 1;
    }
    VectorIndex ix(store);

    // Lines are Documents and questions are Queries; the embedder decides
    // what that means for this model.
    t0 = now_ms();
    const int n_lines = static_cast<int>(sizeof(kLines) / sizeof(kLines[0]));
    for (int i = 0; i < n_lines; ++i) {
        std::vector<float> v;
        if (did(emb.embed(kLines[i].text, EmbedKind::Document, &v, err), err, "embed a line")) {
            did(ix.put(kLines[i].id, v, &err), err, "store a line");
        }
    }
    std::printf("      %d lines embedded in %.0f ms\n", n_lines, now_ms() - t0);

    const int n_questions = static_cast<int>(sizeof(kQuestions) / sizeof(kQuestions[0]));
    for (int i = 0; i < n_questions; ++i) {
        std::vector<float> q;
        std::vector<Hit>   hits;
        if (!did(emb.embed(kQuestions[i].text, EmbedKind::Query, &q, err),
                 err, "embed a question") ||
            !did(ix.nearest(q, 2, &hits, &err), err, "search")) {
            continue;
        }
        const bool right = !hits.empty() && hits[0].id == kQuestions[i].answer;
        std::printf("%s  %s -> line %lld (%.3f), runner-up line %lld (%.3f)\n",
                    right ? "ok  " : "FAIL", kQuestions[i].text,
                    hits.empty() ? -1 : hits[0].id, hits.empty() ? 0.0 : hits[0].distance,
                    hits.size() < 2 ? -1 : hits[1].id, hits.size() < 2 ? 0.0 : hits[1].distance);
        if (!right) {
            ++g_failures;
        }
    }

    // --- the length limit ------------------------------------------------------
    // Refused, not cut: cutting would drop the EOS the vector is pooled from.
    LocalEmbedOptions small = opt;
    small.n_ctx = 32;
    LocalEmbedder short_ctx(small);
    std::string long_text;
    for (int i = 0; i < 100; ++i) {
        long_text += "word ";
    }
    std::vector<float> v;
    check(!short_ctx.embed(long_text, EmbedKind::Document, &v, err) && v.empty(),
          "text over n_ctx tokens is refused");
    std::printf("      (%s)\n", err.c_str());
    did(short_ctx.embed("short enough", EmbedKind::Document, &v, err), err,
        "text under n_ctx is embedded");

    std::printf("\n%s\n", g_failures ? "FAILED" : "all ok");
    return g_failures ? 1 : 0;
}
