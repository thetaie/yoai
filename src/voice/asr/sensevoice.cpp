#include "voice/asr/sensevoice.hpp"

#include "voice/algorithms/fbank_funasr.hpp"
#include "utils/log.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef YOAI_ASR
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "gguf.h"
#include <map>
#endif

namespace yoai {
namespace voice {

#ifdef YOAI_ASR

namespace {

// The marker SentencePiece puts at the start of a word. Three bytes of UTF-8 for
// U+2581 LOWER ONE EIGHTH BLOCK, not an underscore.
const char* const kWordStart = "\xe2\x96\x81";

bool is_tag(const std::string& piece) {
    return piece.size() >= 4 && piece[0] == '<' && piece[1] == '|' &&
           piece[piece.size() - 2] == '|' && piece[piece.size() - 1] == '>';
}

std::string tag_body(const std::string& piece) {
    return piece.substr(2, piece.size() - 4);
}

bool in_list(const std::string& s, const char* const* list) {
    for (int i = 0; list[i] != nullptr; ++i) {
        if (s == list[i]) {
            return true;
        }
    }
    return false;
}

// The model's own label spaces, spelled as they appear in its vocabulary. They
// are listed rather than pattern-matched because a tag that is not one of these
// should be ignored, not guessed at -- and because the emotion list is worth
// being able to read: every label in it is high-arousal except SAD, which is
// why design/voice.md takes arousal from the signal instead of from here.
const char* const kLanguages[] = {"zh", "en", "yue", "ja", "ko", "nospeech", nullptr};
const char* const kEmotions[]  = {"HAPPY", "SAD", "ANGRY", "NEUTRAL", "FEARFUL",
                                  "DISGUSTED", "SURPRISED", "EMO_UNKNOWN", nullptr};
const char* const kEvents[]    = {"Speech", "BGM", "Applause", "Laughter", "Cry",
                                  "Sneeze", "Breath", "Cough", "Event_UNK", nullptr};

// A token is a stray-capital candidate when it is one ASCII capital and nothing
// else, with or without the word-start marker.
bool is_lone_capital(const std::string& piece) {
    std::string s = piece;
    if (s.compare(0, 3, kWordStart) == 0) {
        s = s.substr(3);
    }
    return s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z';
}

// Runs shorter than this are kept. "I" and "A" are English words and initials
// are real, so a single capital is not evidence of anything; the failure this
// guards against emitted them in runs -- SESE, SNB, NCT -- at sentence
// boundaries. Two is the smallest threshold that leaves ordinary text alone.
const int kMinStrayRun = 2;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        return std::string();
    }
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// Fill the tag fields from the tokens, and build the text from what is left.
//
// The stray-capital filter runs here, on TOKENS rather than on the finished
// string, because at this level the signature is exact: a token whose entire
// content is one capital letter. Applied to text it would have to guess where
// word boundaries were, and would take "OK" or an initialled name with it.
void assemble(Transcript* out) {
    std::vector<bool> drop(out->tokens.size(), false);

    size_t i = 0;
    while (i < out->tokens.size()) {
        if (is_tag(out->tokens[i].piece) || !is_lone_capital(out->tokens[i].piece)) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < out->tokens.size() && !is_tag(out->tokens[j].piece) &&
               is_lone_capital(out->tokens[j].piece)) {
            ++j;
        }
        if (static_cast<int>(j - i) >= kMinStrayRun) {
            for (size_t k = i; k < j; ++k) {
                drop[k] = true;
                out->tokens[k].dropped = true;
            }
            out->filtered_stray_capitals = true;
        }
        i = j;
    }

    std::string text;
    for (size_t k = 0; k < out->tokens.size(); ++k) {
        const std::string& piece = out->tokens[k].piece;
        if (is_tag(piece)) {
            const std::string body = tag_body(piece);
            if (in_list(body, kLanguages)) {
                out->language = body;
            } else if (in_list(body, kEmotions)) {
                out->emotion = body;
            } else if (in_list(body, kEvents)) {
                out->event = body;
            } else if (body == "withitn") {
                out->itn = true;
            } else if (body == "woitn") {
                out->itn = false;
            }
            continue;
        }
        if (drop[k]) {
            continue;
        }
        text += piece;
    }

    size_t pos;
    while ((pos = text.find(kWordStart)) != std::string::npos) {
        text.replace(pos, 3, " ");
    }
    out->text = trim(text);

    if (out->filtered_stray_capitals) {
        utils::log_warn(
            "sensevoice: dropped a run of isolated capital letters from the transcript; "
            "this model does that on some audio at every precision");
    }
}

const float kLayerNormEps = 1e-5f;

// Encoder frames are one LFR hop: 6 mel frames of 10 ms each.
const float kFrameSeconds = 0.06f;

// Graph budget. The encoder is 70 layers and the FSMN memory block alone adds
// five nodes per tap per layer, so the node count is a few thousand; this is
// sized well above it and checked at build time rather than discovered as a
// corrupt graph. The context memory is reused across calls -- the graph has to
// be rebuilt every time because its shape follows the clip length, but the
// buffer it is built in does not have to be reallocated.
const size_t kGraphNodes = 32768;

// The two embedding rows SenseVoiceSmall uses as the text-normalization query.
// They are indices into the same table the features are compared against, not
// vocabulary entries with these meanings -- row 14 happens to spell an ordinary
// word. From FunASR's textnorm_dict; the GGUF ships 14.
const int kQueryWithItn = 14;
const int kQueryWoItn   = 15;

struct Config {
    int d_model    = 512;
    int n_head     = 4;
    int num_blocks = 50;
    int tp_blocks  = 20;
    int kernel     = 11;
    int vocab      = 25055;
    int blank      = 0;
};

ggml_tensor* linear(ggml_context* ctx, ggml_tensor* w, ggml_tensor* b, ggml_tensor* x) {
    ggml_tensor* y = ggml_mul_mat(ctx, w, x);
    return b ? ggml_add(ctx, y, b) : y;
}

ggml_tensor* layer_norm(ggml_context* ctx, ggml_tensor* x, ggml_tensor* g, ggml_tensor* b) {
    return ggml_add(ctx, ggml_mul(ctx, ggml_norm(ctx, x, kLayerNormEps), g), b);
}

}  // namespace

struct SenseVoice::Impl {
    Config       cfg;
    ggml_context*     weights = nullptr;
    ggml_backend_t    backend = nullptr;
    ggml_gallocr_t    alloc   = nullptr;
    std::vector<char> graph_buf;

    std::map<std::string, ggml_tensor*> tensors;
    std::vector<std::string>            vocab;
    std::vector<int>                    query_tokens;
    std::vector<float>                  query_embed;  // query_tokens.size() * 560
    int                                 threads  = 4;
    int                                 itn_slot = -1;   // index into query_tokens, or -1

    FunAsrFbank fbank;

    ~Impl() {
        if (alloc)   ggml_gallocr_free(alloc);
        if (backend) ggml_backend_free(backend);
        if (weights) ggml_free(weights);
    }

    // Copy the query rows out of the embedding table into query_embed.
    //
    // Done once at load and again whenever a query token changes, so the hot
    // path never reads the embedding tensor -- which also means an unexpected
    // tensor type is an error here rather than silent noise prepended to every
    // clip. Leaves the model untouched on failure; the caller decides.
    bool fill_query_embed() {
        const int width = fbank.dim();
        ggml_tensor* embed = get("embed.weight");
        if (!embed) {
            utils::log_error("sensevoice: model has no tensor 'embed.weight'");
            return false;
        }
        if (embed->ne[0] != width) {
            utils::log_error("sensevoice: embed.weight is %d wide, front end produces %d",
                                static_cast<int>(embed->ne[0]), width);
            return false;
        }
        query_embed.assign(query_tokens.size() * width, 0.0f);
        for (size_t i = 0; i < query_tokens.size(); ++i) {
            const int row = query_tokens[i];
            if (row < 0 || row >= static_cast<int>(embed->ne[1])) {
                utils::log_error("sensevoice: query token %d is outside embed.weight", row);
                return false;
            }
            float* dst = &query_embed[i * width];
            if (embed->type == GGML_TYPE_F32) {
                const float* src =
                    static_cast<const float*>(embed->data) + static_cast<size_t>(row) * width;
                std::memcpy(dst, src, width * sizeof(float));
            } else if (embed->type == GGML_TYPE_F16) {
                const ggml_fp16_t* src =
                    static_cast<const ggml_fp16_t*>(embed->data) + static_cast<size_t>(row) * width;
                for (int j = 0; j < width; ++j) {
                    dst[j] = ggml_fp16_to_fp32(src[j]);
                }
            } else {
                utils::log_error("sensevoice: embed.weight is %s, expected F32 or F16",
                                    ggml_type_name(embed->type));
                return false;
            }
        }
        return true;
    }

    ggml_tensor* get(const std::string& name) const {
        std::map<std::string, ggml_tensor*>::const_iterator it = tensors.find(name);
        return it == tensors.end() ? nullptr : it->second;
    }

    // Missing weights are fatal for the pass but must not be fatal for the
    // process: a host that shipped the wrong GGUF gets an error, not a crash.
    ggml_tensor* need(const std::string& name, bool* ok) const {
        ggml_tensor* t = get(name);
        if (!t) {
            utils::log_error("sensevoice: model has no tensor '%s'", name.c_str());
            *ok = false;
        }
        return t;
    }

    ggml_tensor* sanm_attention(ggml_context* ctx, const std::string& p,
                                ggml_tensor* x, int n, bool* ok) {
        const int d  = cfg.d_model;
        const int h  = cfg.n_head;
        const int dk = d / h;
        const int k  = cfg.kernel;

        ggml_tensor* qkv = linear(ctx, need(p + "linear_q_k_v.weight", ok),
                                  need(p + "linear_q_k_v.bias", ok), x);
        if (!*ok) {
            return x;
        }
        // One projection produces q, k and v side by side in each row.
        const size_t stride = qkv->nb[1];
        ggml_tensor* q = ggml_cont(ctx, ggml_view_2d(ctx, qkv, d, n, stride, 0));
        ggml_tensor* k_in = ggml_cont(
            ctx, ggml_view_2d(ctx, qkv, d, n, stride, static_cast<size_t>(d) * sizeof(float)));
        ggml_tensor* v_in = ggml_cont(
            ctx, ggml_view_2d(ctx, qkv, d, n, stride, static_cast<size_t>(2 * d) * sizeof(float)));

        // The FSMN memory block: a depthwise convolution over time added to the
        // attention output. This is the "M" in SAN-M and the reason the encoder
        // can see local context without paying for it in attention.
        const int pad = (k - 1) / 2;
        ggml_tensor* fsmn_w = need(p + "fsmn_block.weight", ok);
        if (!*ok) {
            return x;
        }
        ggml_tensor* padded = ggml_pad_ext(ctx, v_in, 0, 0, pad, pad, 0, 0, 0, 0);
        ggml_tensor* fsmn = v_in;
        for (int j = 0; j < k; ++j) {
            ggml_tensor* slice = ggml_view_2d(ctx, padded, d, n, padded->nb[1],
                                              static_cast<size_t>(j) * padded->nb[1]);
            ggml_tensor* tap = ggml_view_1d(ctx, fsmn_w, d, static_cast<size_t>(j) * fsmn_w->nb[1]);
            fsmn = ggml_add(ctx, fsmn, ggml_mul(ctx, ggml_cont(ctx, slice), tap));
        }

        ggml_tensor* qh = ggml_permute(ctx, ggml_reshape_3d(ctx, q, dk, h, n), 0, 2, 1, 3);
        ggml_tensor* kh = ggml_permute(ctx, ggml_reshape_3d(ctx, k_in, dk, h, n), 0, 2, 1, 3);
        ggml_tensor* vh =
            ggml_cont(ctx, ggml_permute(ctx, ggml_reshape_3d(ctx, v_in, dk, h, n), 1, 2, 0, 3));

        ggml_tensor* scores = ggml_soft_max(
            ctx, ggml_scale(ctx, ggml_mul_mat(ctx, kh, qh), 1.0f / std::sqrt(static_cast<float>(dk))));
        ggml_tensor* attended =
            ggml_cont_2d(ctx, ggml_permute(ctx, ggml_mul_mat(ctx, vh, scores), 0, 2, 1, 3), d, n);

        ggml_tensor* out = linear(ctx, need(p + "linear_out.weight", ok),
                                  need(p + "linear_out.bias", ok), attended);
        return *ok ? ggml_add(ctx, out, fsmn) : x;
    }

    // `residual` is false only for the very first block, whose input is the
    // 560-wide feature and whose output is 512-wide: there is nothing to add.
    ggml_tensor* sanm_layer(ggml_context* ctx, const std::string& p, ggml_tensor* x,
                            int n, bool residual, bool* ok) {
        ggml_tensor* r = x;
        ggml_tensor* h = layer_norm(ctx, x, need(p + "norm1.weight", ok), need(p + "norm1.bias", ok));
        if (!*ok) {
            return x;
        }
        ggml_tensor* attended = sanm_attention(ctx, p + "self_attn.", h, n, ok);
        if (!*ok) {
            return x;
        }
        x = residual ? ggml_add(ctx, r, attended) : attended;

        r = x;
        h = layer_norm(ctx, x, need(p + "norm2.weight", ok), need(p + "norm2.bias", ok));
        h = linear(ctx, need(p + "feed_forward.w_1.weight", ok),
                   need(p + "feed_forward.w_1.bias", ok), h);
        if (!*ok) {
            return x;
        }
        h = ggml_relu(ctx, h);
        h = linear(ctx, need(p + "feed_forward.w_2.weight", ok),
                   need(p + "feed_forward.w_2.bias", ok), h);
        return *ok ? ggml_add(ctx, r, h) : x;
    }
};

bool SenseVoice::available() {
    return true;
}

SenseVoice::SenseVoice() : impl_(new Impl()) {}

SenseVoice::~SenseVoice() {
    delete impl_;
}

bool SenseVoice::loaded() const {
    return impl_->weights != nullptr;
}

void SenseVoice::set_threads(int n) {
    impl_->threads = n > 0 ? n : 1;
}

bool SenseVoice::set_inverse_text_normalization(bool on) {
    Impl* m = impl_;
    if (!m->weights) {
        utils::log_error("sensevoice: set_inverse_text_normalization before a model is loaded");
        return false;
    }
    if (m->itn_slot < 0) {
        utils::log_warn("sensevoice: this model carries no text-normalization query slot");
        return false;
    }
    const int want = on ? kQueryWithItn : kQueryWoItn;
    if (m->query_tokens[m->itn_slot] == want) {
        return true;
    }
    const int previous = m->query_tokens[m->itn_slot];
    m->query_tokens[m->itn_slot] = want;
    if (!m->fill_query_embed()) {
        m->query_tokens[m->itn_slot] = previous;
        m->fill_query_embed();
        return false;
    }
    return true;
}

bool SenseVoice::load(const std::string& gguf_path) {
    Impl* m = impl_;

    if (m->weights) {
        ggml_free(m->weights);
        m->weights = nullptr;
        m->tensors.clear();
        m->vocab.clear();
        m->query_tokens.clear();
        m->query_embed.clear();
    }

    gguf_init_params params;
    params.no_alloc = false;
    params.ctx      = &m->weights;
    gguf_context* gguf = gguf_init_from_file(gguf_path.c_str(), params);
    if (!gguf) {
        utils::log_error("sensevoice: cannot read '%s'", gguf_path.c_str());
        return false;
    }

    // The architecture is read from the file rather than assumed, so a
    // differently-shaped SAN-M model on the same runtime works without a code
    // change. The fallbacks are SenseVoiceSmall's own values.
    struct Reader {
        gguf_context* g;
        int operator()(const char* key, int fallback) const {
            const int64_t i = gguf_find_key(g, key);
            return i < 0 ? fallback : static_cast<int>(gguf_get_val_u32(g, i));
        }
    } read = {gguf};

    m->cfg.d_model    = read("sv.output_size", 512);
    m->cfg.n_head     = read("sv.attention_heads", 4);
    m->cfg.num_blocks = read("sv.num_blocks", 50);
    m->cfg.tp_blocks  = read("sv.tp_blocks", 20);
    m->cfg.kernel     = read("sv.kernel_size", 11);
    m->cfg.vocab      = read("sv.vocab_size", 25055);
    m->cfg.blank      = read("sv.blank_id", 0);

    const int64_t qi = gguf_find_key(gguf, "sv.query_tokens");
    if (qi >= 0) {
        const int n = static_cast<int>(gguf_get_arr_n(gguf, qi));
        const int32_t* ids = static_cast<const int32_t*>(gguf_get_arr_data(gguf, qi));
        m->query_tokens.assign(ids, ids + n);
    }

    const int64_t vi = gguf_find_key(gguf, "sv.vocab");
    if (vi >= 0) {
        const int n = static_cast<int>(gguf_get_arr_n(gguf, vi));
        m->vocab.resize(n);
        for (int i = 0; i < n; ++i) {
            const char* s = gguf_get_arr_str(gguf, vi, i);
            m->vocab[i] = s ? s : "";
        }
    }

    for (int64_t i = 0; i < gguf_get_n_tensors(gguf); ++i) {
        const char* name = gguf_get_tensor_name(gguf, i);
        m->tensors[name] = ggml_get_tensor(m->weights, name);
    }
    gguf_free(gguf);

    if (m->vocab.empty()) {
        utils::log_error("sensevoice: '%s' carries no sv.vocab, so it cannot produce text",
                            gguf_path.c_str());
        ggml_free(m->weights);
        m->weights = nullptr;
        return false;
    }

    if (!m->fill_query_embed()) {
        ggml_free(m->weights);
        m->weights = nullptr;
        return false;
    }

    // Which slot carries the text-normalization query is found rather than
    // assumed to be the last one: it is a property of the conversion, and a
    // hard-coded index would quietly switch the wrong control on a model that
    // ordered them differently.
    m->itn_slot = -1;
    for (size_t i = 0; i < m->query_tokens.size(); ++i) {
        if (m->query_tokens[i] == kQueryWithItn || m->query_tokens[i] == kQueryWoItn) {
            m->itn_slot = static_cast<int>(i);
        }
    }

    if (!m->backend) {
        m->backend = ggml_backend_cpu_init();
        m->alloc   = ggml_gallocr_new(ggml_backend_cpu_buffer_type());
    }
    if (m->graph_buf.empty()) {
        m->graph_buf.resize(ggml_tensor_overhead() * kGraphNodes * 2 +
                            ggml_graph_overhead_custom(kGraphNodes, false) + (1u << 20));
    }

    utils::log_info("sensevoice: loaded %s (%d blocks + %d tp, vocab %d)",
                       gguf_path.c_str(), m->cfg.num_blocks, m->cfg.tp_blocks,
                       static_cast<int>(m->vocab.size()));

    // The query tokens are an INPUT, not just something the model answers with:
    // they are prepended to the features and are what the four tag outputs are
    // responses to. Which ones the GGUF carries is therefore a property of the
    // conversion, and it decides things the caller may care about -- notably
    // whether the transcript comes back with inverse text normalization applied.
    // Worth being able to see.
    //
    // Printed as IDS, not as vocabulary pieces. These are row indices into the
    // embedding table used as control slots, and the rows they pick are also
    // ordinary words -- so spelling them out reads as "<unk> <s> </s> you",
    // which looks like a bug and is not one.
    {
        std::string ids;
        for (size_t i = 0; i < m->query_tokens.size(); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s%d", i ? " " : "", m->query_tokens[i]);
            ids += buf;
        }
        utils::log_debug("sensevoice: query token ids are [%s]", ids.c_str());
    }
    return true;
}

bool SenseVoice::transcribe(const float* pcm, size_t n_samples, Transcript* out) {
    if (!out) {
        return false;
    }
    *out = Transcript();

    Impl* m = impl_;
    if (!m->weights) {
        utils::log_error("sensevoice: transcribe called with no model loaded");
        return false;
    }

    int frames = 0;
    std::vector<float> features = m->fbank.compute(pcm, n_samples, &frames);
    if (frames <= 0) {
        utils::log_warn("sensevoice: %zu samples is shorter than one analysis window",
                           n_samples);
        return false;
    }
    out->audio_seconds =
        static_cast<float>(n_samples) / m->fbank.options().sample_rate;

    const int feature_dim = m->fbank.dim();
    const int n_query     = static_cast<int>(m->query_tokens.size());
    const int n           = n_query + frames;
    const int d_model     = m->cfg.d_model;
    const int vocab       = m->cfg.vocab;

    // Input: the query rows, then the features. Scaled and given a sinusoidal
    // position, both exactly as the reference does -- note that the scale is
    // sqrt(d_model) applied to a feature that is 560 wide, not 512. It looks
    // like a mismatch and is what the model was trained with.
    std::vector<float> input(static_cast<size_t>(n) * feature_dim);
    if (n_query > 0) {
        std::memcpy(&input[0], &m->query_embed[0],
                    m->query_embed.size() * sizeof(float));
    }
    std::memcpy(&input[static_cast<size_t>(n_query) * feature_dim], &features[0],
                static_cast<size_t>(frames) * feature_dim * sizeof(float));

    const float scale = std::sqrt(static_cast<float>(d_model));
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] *= scale;
    }
    {
        const double inc = std::log(10000.0) / (feature_dim / 2.0 - 1.0);
        for (int t = 0; t < n; ++t) {
            const double pos = t + 1;
            for (int i = 0; i < feature_dim / 2; ++i) {
                const double angle = pos * std::exp(i * -inc);
                input[static_cast<size_t>(t) * feature_dim + i] +=
                    static_cast<float>(std::sin(angle));
                input[static_cast<size_t>(t) * feature_dim + feature_dim / 2 + i] +=
                    static_cast<float>(std::cos(angle));
            }
        }
    }

    ggml_init_params cp;
    cp.mem_size   = m->graph_buf.size();
    cp.mem_buffer = &m->graph_buf[0];
    cp.no_alloc   = true;
    ggml_context* ctx = ggml_init(cp);
    if (!ctx) {
        utils::log_error("sensevoice: could not create the graph context");
        return false;
    }

    bool ok = true;
    ggml_tensor* x = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, feature_dim, n);
    ggml_set_input(x);

    ggml_tensor* h = m->sanm_layer(ctx, "encoder.encoders0.0.", x, n, false, &ok);
    for (int i = 0; ok && i < m->cfg.num_blocks - 1; ++i) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "encoder.encoders.%d.", i);
        h = m->sanm_layer(ctx, prefix, h, n, true, &ok);
    }
    if (ok) {
        h = layer_norm(ctx, h, m->need("encoder.after_norm.weight", &ok),
                       m->need("encoder.after_norm.bias", &ok));
    }
    for (int i = 0; ok && i < m->cfg.tp_blocks; ++i) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "encoder.tp_encoders.%d.", i);
        h = m->sanm_layer(ctx, prefix, h, n, true, &ok);
    }
    if (ok) {
        h = layer_norm(ctx, h, m->need("encoder.tp_norm.weight", &ok),
                       m->need("encoder.tp_norm.bias", &ok));
    }
    ggml_tensor* logits = nullptr;
    if (ok) {
        logits = linear(ctx, m->need("ctc.ctc_lo.weight", &ok),
                        m->need("ctc.ctc_lo.bias", &ok), h);
    }
    if (!ok) {
        ggml_free(ctx);
        return false;
    }
    ggml_set_output(logits);

    ggml_cgraph* graph = ggml_new_graph_custom(ctx, kGraphNodes, false);
    ggml_build_forward_expand(graph, logits);
    if (!ggml_gallocr_alloc_graph(m->alloc, graph)) {
        utils::log_error("sensevoice: could not allocate the compute graph");
        ggml_free(ctx);
        return false;
    }

    ggml_backend_tensor_set(x, &input[0], 0, ggml_nbytes(x));
    ggml_backend_cpu_set_n_threads(m->backend, m->threads);
    if (ggml_backend_graph_compute(m->backend, graph) != GGML_STATUS_SUCCESS) {
        utils::log_error("sensevoice: the encoder failed to run");
        ggml_free(ctx);
        return false;
    }

    std::vector<float> scores(static_cast<size_t>(vocab) * n);
    ggml_backend_tensor_get(logits, &scores[0], 0, ggml_nbytes(logits));
    ggml_free(ctx);

    // Greedy CTC: argmax per frame, collapse repeats, drop the blank. The frame
    // each surviving token fired on is kept -- that is where the timestamps come
    // from, and it costs nothing to carry.
    int previous = -1;
    for (int t = 0; t < n; ++t) {
        const float* column = &scores[static_cast<size_t>(t) * vocab];
        int   best_id    = 0;
        float best_score = column[0];
        for (int v = 1; v < vocab; ++v) {
            if (column[v] > best_score) {
                best_score = column[v];
                best_id    = v;
            }
        }
        if (best_id != previous && best_id != m->cfg.blank) {
            Token token;
            token.id    = best_id;
            token.frame = t - n_query;  // negative for the query rows
            token.start = token.frame > 0 ? token.frame * kFrameSeconds : 0.0f;
            if (best_id < static_cast<int>(m->vocab.size())) {
                token.piece = m->vocab[best_id];
            }
            out->tokens.push_back(token);
        }
        previous = best_id;
    }

    assemble(out);
    return true;
}

#else   // !YOAI_ASR

// The lane compiles out to a truthful stub rather than disappearing, so a host
// links the same symbols either way and asks available() before offering voice
// input at all. Same shape as the on-device LLM lane in src/llm/chat/local.cpp.

struct SenseVoice::Impl {
    int unused;
};

bool SenseVoice::available() {
    return false;
}

SenseVoice::SenseVoice() : impl_(nullptr) {}
SenseVoice::~SenseVoice() {}
bool SenseVoice::loaded() const { return false; }
void SenseVoice::set_threads(int) {}
bool SenseVoice::set_inverse_text_normalization(bool) { return false; }

bool SenseVoice::load(const std::string&) {
    utils::log_error("sensevoice: this build has no recognition engine compiled in");
    return false;
}

bool SenseVoice::transcribe(const float*, size_t, Transcript* out) {
    if (out) {
        *out = Transcript();
    }
    return false;
}

#endif  // YOAI_ASR

}  // namespace voice
}  // namespace yoai
