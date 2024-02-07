// Copyright (c) 2023 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================
#pragma once

#include <fstream>
#include <string>
#include <tuple>
#include <vector>
#include <thread>
#include <mpi.h>

#include "INIReader.h"
#include "abstract_decoder.h"
#include "attention.h"
#include "debugger.h"
#include "decoder_layer.h"
#include "dist_linear.h"
#include "kvcache_manager.h"
#include "messenger.h"
#include "mlp_chatglm2.h"
#include "mlp_standard.h"
#include "timeline.h"
#include "transformer_ctx.h"
#include "transpose_util.h"
#include "weight_util.h"

using namespace xft;

struct QKPO_Dummy {
    QKPO_Dummy(int dim, int maxPos) {}
    void forward(float *query, float *key, int qStride, int kStride, const int *qk_shape, const int *position_ids) {}
};

// To get data types in MLP class
template <typename T>
struct MlpTypeExtractor;
template <template <typename...> class MLP_CLS, typename WeiT, typename InT, typename ImT, typename OutT>
struct MlpTypeExtractor<MLP_CLS<WeiT, InT, ImT, OutT>> {
    using Tin = InT;
    using Tim = ImT;
    using Tout = OutT;
};
template <typename WeiT, typename InT, typename ImT, typename OutT>
struct MlpTypeExtractor<MLP<WeiT, InT, ImT, OutT, true>> {
    using Tin = InT;
    using Tim = ImT;
    using Tout = OutT;
};
template <typename WeiT, typename InT, typename ImT, typename OutT, typename NORM_CLS>
struct MlpTypeExtractor<ChatGLM2MLP<WeiT, InT, ImT, OutT, NORM_CLS, true>> {
    using Tin = InT;
    using Tim = ImT;
    using Tout = OutT;
};

/*
Pipeline parallel and tensor parallel introduction:

  1) MPI_Instances = 16,XFT_PIPELINE_STAGES = 4  =>  pp_size = 4, tp_size = 4
  2) TP sync by oneCCL(row_comm) or shared_memory
  3) PP sync by MPI MPI_COMM_WORLD

  World Rank:      => Row Rank:       =>      tp0 tp1 tp2 tp3
  [ 0,  1,  2,  3,    [ 0, 1, 2, 3];     pp0 [  0,  1,  2,  3];
    4,  5,  6,  7,    [ 0, 1, 2, 3];     pp1 [  0,  1,  2,  3];
    8,  9, 10, 11,    [ 0, 1, 2, 3];     pp2 [  0,  1,  2,  3];
   12, 13, 14, 15];   [ 0, 1, 2, 3];     pp3 [  0,  1,  2,  3];

      Prompts
       │
       ▼
    Embedding(PP0) ◄────────────────────────────────────────────────────────────────────┐
	   │                                                                                │
  PP0  ▼                                                                                │
  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
  │ TP0                TP1                TP2                TP3                    │   │
  │ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐     │   │
  │ │ OMP            │ │ OMP            │ │ OMP            │ │ OMP            │     │   │
  │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │     │   │
  │ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│     │   │
  │ └──┬─────────────┘ └─┬──────────────┘ └─┬──────────────┘ └─┬──────────────┘ ... │   │
  │    │◄────────────────┘◄─────────────────┘◄─────────────────┘                    │   │
  │    ▼                                                                   layer0-7 │   │
  └────┬────────────────────────────────────────────────────────────────────────────┘   │
       │                                                                                │
  PP1  ▼                                                                                │
  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
  │ TP0                TP1                TP2                TP3                    │   │
  │ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐     │   │
  │ │ OMP            │ │ OMP            │ │ OMP            │ │ OMP            │     │   │
  │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │     │   │
  │ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│     │   │
  │ └──┬─────────────┘ └─┬──────────────┘ └─┬──────────────┘ └─┬──────────────┘ ... │   │
  │    │◄────────────────┘◄─────────────────┘◄─────────────────┘                    │   │
  │    ▼                                                                  layer8-15 │   │
  └────┬────────────────────────────────────────────────────────────────────────────┘   │
       │                                                                                │
  PP2  ▼                                                                                │
  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
  │ TP0                TP1                TP2                TP3                    │   │
  │ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐     │   │
  │ │ OMP            │ │ OMP            │ │ OMP            │ │ OMP            │     │   │
  │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │     │   │
  │ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│     │   │
  │ └──┬─────────────┘ └─┬──────────────┘ └─┬──────────────┘ └─┬──────────────┘ ... │   │
  │    │◄────────────────┘◄─────────────────┘◄─────────────────┘                    │   │
  │    ▼                                                                 layer16-23 │   │
  └────┬────────────────────────────────────────────────────────────────────────────┘   │
       │                                                                                │
  PP3  ▼                                                                                │
  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
  │ TP0                TP1                TP2                TP3                    │   │
  │ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐ ┌────────────────┐     │   │
  │ │ OMP            │ │ OMP            │ │ OMP            │ │ OMP            │     │   │
  │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │ │ │ │ │ │ │ │    │     │   │
  │ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│ │ ▼ ▼ ▼ ▼ ▼ ▼ ...│     │   │
  │ └──┬─────────────┘ └─┬──────────────┘ └─┬──────────────┘ └─┬──────────────┘ ... │   │
  │    │◄────────────────┘◄─────────────────┘◄─────────────────┘                    │   │
  │    ▼                                                                 layer24-31 │   │
  └────┬────────────────────────────────────────────────────────────────────────────┘   │
      ...                                                                              ...
       │                                                                                │
       ▼                                                                                │
	Predictor(PP3)                                                                      │
       │                                                                                │
       ▼                                                                                │
    Searchers                                                                           │
       │                                                                                │
       ▼ ───────────────────────────────────────────────────────────────────────────────┘
*/

// Template parameters:
// ATTN_CLS - class for attention impl.
// MLP_CLS - MLP implementation
// KVCacheT - data type of the cached keys/values
// ATTN_MLP_PARALLEL - true means attention and MLP are in parallel, using the same initial input
template <typename ATTN_CLS, typename MLP_CLS, typename KVCacheT = float16_t, bool ATTN_MLP_PARALLEL = false>
class CommonDecoder : public AbstractDecoder {
public:
    CommonDecoder(const std::string &modelPath, const std::string &modelType)
        : messenger(Messenger::getInstance())
#ifdef DEBUG
        , dbg("model_decoder.csv")
#endif
    {
        // Make sure Attention output can be feed to MLP
        static_assert(std::is_same_v<AttnOutT, MlpInT>, "Error: Attention Output and MLP Input are not the same type.");

        // Make sure MLP output can be feed to Attention
        static_assert(std::is_same_v<MlpOutT, AttnInT>, "Error: MLP Output and Attention Input are not the same type.");

        std::string configPath = modelPath + "/config.ini";
        INIReader reader = INIReader(configPath);
        wType = getWeightType(configPath, modelType);

        const int attHeadNum = reader.GetInteger(modelType, "head_num");
        // Use the same head number for the default multi-head attention
        const int kvHeadNum = reader.GetInteger(modelType, "kv_head_num", attHeadNum);
        const int size_per_head = reader.GetInteger(modelType, "size_per_head");
        const int imSize = reader.GetInteger(modelType, "inter_size");
        const int layers = reader.GetInteger(modelType, "num_layer");
        const int vocabSize = reader.GetInteger(modelType, "vocab_size");
        // Max Position Embedding for position embedding functions, with a default value set to 0
        const int maxPosEmbed = reader.GetInteger(modelType, "max_pos_seq_len", 0);
        // Max num of tokens that LLM can process. Also for allocating buffers. Default maxPosEmbed
        const int maxPositions = reader.GetInteger(modelType, "model_max_length", maxPosEmbed);
        // Seq length in Qwen model, if none, please ignore
        const int maxSeqLength = reader.GetInteger(modelType, "seq_length", -1);
        const int hiddenSize = attHeadNum * size_per_head;
        const int embeddingSize = hiddenSize;
        const int multi_query_group_num = reader.GetInteger(modelType, "multi_query_group_num", attHeadNum);
        const float epsilon = reader.GetFloat(modelType, "layernorm_eps", 1e-6);
        const std::string ropeType = reader.Get(modelType, "rope_scaling_type", "");
        const float ropeFactor = reader.GetFloat(modelType, "rope_scaling_factor", 1.0);
        const int ropeOrgMaxPosEmbed
                = reader.GetInteger(modelType, "rope_scaling_original_max_position_embeddings", 2048);
        const float ropeTheta = reader.GetFloat(modelType, "rope_theta", 10000.0);
        RopeParams *ropeParamsPtr = new RopeParams(ropeTheta, ropeType, ropeFactor, ropeOrgMaxPosEmbed);

        std::string act = reader.Get(modelType, "activation_type");
        std::transform(act.begin(), act.end(), act.begin(), ::tolower);

        this->startId = reader.GetInteger(modelType, "start_id", 0);
        this->endId = reader.GetInteger(modelType, "end_id", startId);

        this->initSeqLen = 0;
        this->accSeqLen = 0;

        this->prefixSeqLen = 0;
        this->prefixSharing = false;

        // Buffer related (not initialized)
        this->inputTokens = nullptr;
        this->maskSize = 0;
        this->attnMask = nullptr;
        actBuffers.reset(new hpj::Matrix<float>());

        // Context
        DecoderContext *ctx = getDecoderContext(layers, hiddenSize, attHeadNum, kvHeadNum, imSize, act, epsilon,
                vocabSize, embeddingSize, maxPositions, maxPosEmbed, maxSeqLength, ropeParamsPtr);

        // Decoder
        if (layers % pp_size != 0) {
            std::cerr << "Warning: layers cannot be evenly divided by pipeline parallel stage size(pp_size)." << std::endl;
        }

        int layers_per_pp_stage = layers / pp_size;
        int start_layer = pp_rank * layers_per_pp_stage;
        for (int i = start_layer; i < start_layer + layers_per_pp_stage; ++i) {
            auto pdec = new DECODER(ctx, i);
            this->setDecoderWeights(pdec, modelPath, i);
            this->decoders.push_back(pdec);
        }

        // Predictor
        int workers = messenger.getSize();
        int rank = messenger.getRank();
        int color = messenger.getColor();
        this->predictor = new DistLinear<LinearWeiT>(hiddenSize, vocabSize, rank, workers);
        this->setPredictorWeight(modelPath);

        // KVCache Manager
        this->kvCacheMgr.reset(new KVCacheManager<KVCacheT>(layers));
    }

    virtual ~CommonDecoder() {
        if (this->inputTokens) free(this->inputTokens);
        if (this->attnMask) free(this->attnMask);

        delete this->predictor;

        for (auto dec : this->decoders) {
            delete dec;
        }
    }

    std::tuple<float *, int, int> forward(int *ids, int64_t *dims, int step, bool logitsAll = false) {
        // Assume input has been synced with master in higher level.
        // Assume the 1st step input's shape is [userSideBS][1][seqLen].
        TimeLine t("Decoder.forward");
        TimeLine t1("Decoder.embedding");

        int userSideBS = dims[0];
        int beamSize = dims[1];
        int batchSize = (step == 0 ? userSideBS : userSideBS * beamSize); // as samples are duplicated at step 0
        int seqLen = dims[2];
        int pastSeqLen = step == 0 ? 0 : this->accSeqLen;
        int inputSeqLen = seqLen;

        // Prepare context
        DecoderContext *ctx = this->getContext();
        ctx->resize(batchSize, seqLen, pastSeqLen);

        if (step == 0) {
            // Reset initial and accumulated sequence length at the first step
            this->initSeqLen = seqLen;
            this->accSeqLen = 0;
            if (this->prefixSharing) {
                pastSeqLen = this->prefixSeqLen;
                inputSeqLen = seqLen - pastSeqLen;

                int *prefixIDs = (int *)malloc(userSideBS * pastSeqLen * sizeof(int));
                int *newIDs = (int *)malloc(userSideBS * inputSeqLen * sizeof(int));
                for (int bs = 0; bs < userSideBS; bs++) {
                    memcpy(prefixIDs + pastSeqLen * bs, ids + seqLen * bs, pastSeqLen * sizeof(int));
                    memcpy(newIDs + inputSeqLen * bs, ids + seqLen * bs + pastSeqLen, inputSeqLen * sizeof(int));
                }

                this->getPositionIds(prefixIDs, batchSize, pastSeqLen, 0);

                free(prefixIDs);
                ids = newIDs;
                ctx->resize(batchSize, inputSeqLen, pastSeqLen);
            }

            // Enlarge buffer if needed
            prepareBuffers(ctx, userSideBS, beamSize, logitsAll);
        }

        AttnInT *embBuf = (AttnInT *)actBuffers->Data();
        MlpOutT *outBuf = (MlpOutT *)(embBuf + batchSize * inputSeqLen * ctx->hiddenSize);

        // Embedding
        this->embeddingForward(ids, embBuf, batchSize, inputSeqLen);
        this->accSeqLen += seqLen;

#ifdef DEBUG
        dbg.debugPrint("---- embedding.forward ----\n");
        dbg.debugPrint("ids:\n");
        dbg.dumpMatrix(ids, batchSize, inputSeqLen, inputSeqLen);
        dbg.debugPrint("embBuf(rows: %d, cols: %d, stride: %d):\n", batchSize * inputSeqLen, ctx->hiddenSize,
                ctx->hiddenSize);
        dbg.dumpMatrix(embBuf, batchSize * inputSeqLen, ctx->hiddenSize, ctx->hiddenSize);
#endif

        // Prepare attention mask
        this->prepareAttnMask(ids, step + this->prefixSharing);

        // Token position ids, note: different models may have different impl.
        int *positionIds = this->getPositionIds(ids, batchSize, inputSeqLen, step + this->prefixSharing);
        t1.release();

        // if current pipeline parallel stage rank isn't the first stage, should receive previous stage data
        if (pp_rank > 0) {
            // [MPI] Recv data from world_rank 0
            MPI_Recv(embBuf, batchSize * inputSeqLen * ctx->hiddenSize, MPI_FLOAT, pp_rank - 1, 100 * (pp_rank - 1), MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        // Decoder: forward
        int hiddenSize = ctx->hiddenSize;
        int layers_per_pp_stage = this->decoders.size();
        for (int i = 0; i < layers_per_pp_stage; ++i) {
            int workers = this->messenger.getSize();
            if (step == 0 && this->prefixSharing) {
                // Expand the prefix KV cache for each batch
                this->kvCacheMgr->expandPrefixCache(i, userSideBS, this->prefixSeqLen);
            }
            KVCacheTensor<KVCacheT> &presentKey = this->kvCacheMgr->getKey(i);
            KVCacheTensor<KVCacheT> &presentValue = this->kvCacheMgr->getValue(i);

            // Pls be noted: in attention, 'outBuf' is used as imtermediate buffer, 'tmpBuf' is used as output
            AttnOutT *attnOut = (AttnOutT *)(this->getContext()->tmpBuf.Data());

            this->decoders[i]->forwardAttention(getContext(), embBuf, outBuf, attnOut, attnMask,
                    presentKey, // presentKey,
                    presentValue, // presentValue,
                    inputSeqLen, // inputSeqLen,
                    pastSeqLen, // pastSeqLen
                    step == 0, // useSelfAttn,
                    true, // doLnBefore,
                    positionIds);

            // Expand the KV cache as it only has values for beam 0
            if (step == 0 && beamSize > 1) { this->kvCacheMgr->expandCache(i, userSideBS, beamSize, seqLen); }

            // Merge the result of attention
            // When attention and FFN/MLP are in parallel, do not need to reduce after attention
            if constexpr (!ATTN_MLP_PARALLEL) {
                if (this->messenger.getSize() > 1) {
                    this->messenger.reduceAdd(attnOut, attnOut, batchSize * inputSeqLen * hiddenSize);
                }
            }

            // When attention and FFN/MLP are in parallel, use the initial embedding as input
            if constexpr (ATTN_MLP_PARALLEL) {
                if (this->messenger.getSize() > 1) {
                    this->decoders[i]->forwardFFN(getContext(), embBuf, outBuf, hiddenSize, hiddenSize, true);
                    this->messenger.reduceAdd(outBuf, embBuf, batchSize * inputSeqLen * hiddenSize);
                } else {
                    this->decoders[i]->forwardFFN(getContext(), embBuf, embBuf, hiddenSize, hiddenSize, true);
                }
            } else {
                // FFN (for multiple workers, output into outBuf and then reduce add to embBuf)
                if (this->messenger.getSize() > 1) {
                    this->decoders[i]->forwardFFN(getContext(), attnOut, outBuf, hiddenSize, hiddenSize, true);
                    this->messenger.reduceAdd(outBuf, embBuf, batchSize * inputSeqLen * hiddenSize);
                } else {
                    this->decoders[i]->forwardFFN(getContext(), attnOut, embBuf, hiddenSize, hiddenSize, true);
                }
            }
        }

        if (pp_rank < pp_size - 1) {
            // If current pipeline stage isn't the end of stage, return nullptr
            // [MPI] Send data to next pipeline stage
            MPI_Send(embBuf, batchSize * inputSeqLen * ctx->hiddenSize, MPI_FLOAT, pp_rank + 1, 100 * pp_rank, MPI_COMM_WORLD);
            return std::tuple<float *, int, int>(nullptr, 0, 0);
        }

        // Prepare input for final Layer Norm (only care about the last row of the result)
        // Shape of embBuf: (bs, seqLen, hiddenSize)
        MlpOutT *lnIn = embBuf;
        if (inputSeqLen > 1 && !logitsAll) { // copy is not needed when seqLen = 1 or logitsAll is true
            lnIn = outBuf;
#pragma omp parallel for
            for (int b = 0; b < batchSize; ++b) {
                memcpy(lnIn + b * hiddenSize, embBuf + ((b + 1) * inputSeqLen - 1) * hiddenSize,
                        hiddenSize * sizeof(MlpOutT));
            }
        }

#ifdef DEBUG
        dbg.debugPrint("LayerNorm In:\n");
        dbg.dumpMatrix(lnIn, batchSize, hiddenSize, hiddenSize);
#endif

        // LN, as it supports inplace computing, input and output can be the same
        MlpOutT *lnOut = embBuf;
        if (!logitsAll)
            lastLayerNormForward(lnIn, lnOut, batchSize);
        else
            lastLayerNormForward(lnIn, lnOut, batchSize * seqLen);

#ifdef DEBUG
        dbg.debugPrint("LayerNorm Out:\n");
        dbg.dumpMatrix(lnOut, batchSize, hiddenSize, hiddenSize);
#endif

        // Predictor
        float *finalOut = (float *)outBuf;
        if (!logitsAll)
            this->predictor->forward(lnOut, finalOut, batchSize);
        else
            this->predictor->forward(lnOut, finalOut, batchSize * seqLen);

#ifdef DEBUG
        auto splitSize = this->predictor->getSplitSize();
        dbg.debugPrint("finalOut:\n");
        dbg.dumpMatrix(finalOut, batchSize, splitSize, splitSize);
#endif

        // Expand the result to make it cover multiple beams
        if (step == 0 && beamSize > 1) {
            const int splitSize = this->predictor->getSplitSize();
            for (int b = userSideBS - 1; b >= 0; --b) {
                float *src = finalOut + b * splitSize;
#pragma omp parallel for
                for (int idx = b * beamSize; idx < (b + 1) * beamSize; ++idx) {
                    if (idx == b) { continue; }
                    float *dst = finalOut + idx * splitSize;
                    memcpy(dst, src, splitSize * sizeof(float));
                }
            }
        }

        // free temporary new ids for prefix sharing
        if (step == 0 && this->prefixSharing) { free(ids); }

        return std::tuple<float *, int, int>(
                finalOut, this->predictor->getSplitOffset(), this->predictor->getSplitSize());
    }

    void setPrefix(int *ids, int seqLen) {
        this->prefixSharing = true;
        this->prefixSeqLen = seqLen;
        prefixForward(ids, seqLen);
    }

    void unsetPrefix() { this->prefixSharing = false; }

    void prefixForward(int *ids, int seqLen) {
        // Assume input has been synced with master in higher level.
        // Assume the prefix token's shape is [1][1][seqLen].
        TimeLine t("Decoder.prefixForward");
        TimeLine t1("Decoder.prefixEmbedding");

        // Prepare context
        DecoderContext *ctx = this->getContext();
        ctx->resize(1, seqLen, 0);

        prepareBuffers(ctx, 1, 1, false, true);

        AttnInT *embBuf = (AttnInT *)actBuffers->Data();
        MlpOutT *outBuf = (MlpOutT *)(embBuf + 1 * seqLen * ctx->hiddenSize);

        // Embedding
        this->embeddingForward(ids, embBuf, 1, seqLen);

        // Prepare attention mask
        this->prepareAttnMask(ids, 0);

        // Token position ids, note: different models may have different impl.
        int *positionIds = this->getPositionIds(ids, 1, seqLen, 0);
        t1.release();

        // Decoder: forward
        int hiddenSize = ctx->hiddenSize;
        for (int i = 0; i < this->decoders.size(); ++i) {
            int workers = this->messenger.getSize();
            KVCacheTensor<KVCacheT> &presentKey = this->kvCacheMgr->getPrefixKey(i);
            KVCacheTensor<KVCacheT> &presentValue = this->kvCacheMgr->getPrefixValue(i);

            // Pls be noted: in attention, 'outBuf' is used as imtermediate buffer, 'tmpBuf' is used as output
            AttnOutT *attnOut = (AttnOutT *)(this->getContext()->tmpBuf.Data());
            this->decoders[i]->forwardAttention(getContext(), embBuf, outBuf, attnOut, attnMask,
                    presentKey, // presentKey,
                    presentValue, // presentValue,
                    seqLen, // inputSeqLen,
                    0, // pastSeqLen
                    true, // useSelfAttn,
                    true, // doLnBefore,
                    positionIds);

            // Merge the result of attention
            // When attention and FFN/MLP are in parallel, do not need to reduce after attention
            if constexpr (!ATTN_MLP_PARALLEL) {
                if (this->messenger.getSize() > 1) { this->messenger.reduceAdd(attnOut, attnOut, seqLen * hiddenSize); }
            }

            // When attention and FFN/MLP are in parallel, use the initial embedding as input
            if constexpr (ATTN_MLP_PARALLEL) {
                if (this->messenger.getSize() > 1) {
                    this->decoders[i]->forwardFFN(getContext(), embBuf, outBuf, hiddenSize, hiddenSize, true);
                    this->messenger.reduceAdd(outBuf, embBuf, seqLen * hiddenSize);
                } else {
                    this->decoders[i]->forwardFFN(getContext(), embBuf, embBuf, hiddenSize, hiddenSize, true);
                }
            } else {
                // FFN (for multiple workers, output into outBuf and then reduce add to embBuf)
                if (this->messenger.getSize() > 1) {
                    this->decoders[i]->forwardFFN(getContext(), attnOut, outBuf, hiddenSize, hiddenSize, true);
                    this->messenger.reduceAdd(outBuf, embBuf, seqLen * hiddenSize);
                } else {
                    this->decoders[i]->forwardFFN(getContext(), attnOut, embBuf, hiddenSize, hiddenSize, true);
                }
            }
        }
    }

    // Reorder cached keys and values, size=batchSize*beamSize
    void reorderCache(int *idx, int size) { kvCacheMgr->reorderCache(idx, size, initSeqLen, accSeqLen); }

    // Get decoder context
    DecoderContext *getContext() { return context.get(); }

    // How many layers
    int getLayers() { return decoders.size(); }

    Messenger &getMessenger() { return messenger; }

    int getRank() { return messenger.getRank(); }

    int getPPSize() { return this->pp_size; }

    int getTPSize() { return this->tp_size; }

    WDataType getDataType() { return wType; }

    int getEndId() { return endId; }

    int getInitSeqLen() { return initSeqLen; }

    std::tuple<std::shared_ptr<DecoderContext>, std::shared_ptr<KVCacheManager<KVCacheT>>,
            std::shared_ptr<hpj::Matrix<float>>>
    getSharedResources() {
        return std::make_tuple(context, kvCacheMgr, actBuffers);
    }

    void setSharedResources(const std::tuple<std::shared_ptr<DecoderContext>, std::shared_ptr<KVCacheManager<KVCacheT>>,
            std::shared_ptr<hpj::Matrix<float>>> &r) {
        this->context = std::get<0>(r);
        this->kvCacheMgr = std::get<1>(r);
        this->actBuffers = std::get<2>(r);
    }

    // When first step is skipped, call this function to make everything aligned
    void skipFirstStep(int initSeqLen) {
        // Reset initial and accumulated sequence length at the first step
        this->initSeqLen = initSeqLen;
        this->accSeqLen = initSeqLen;
    }

protected:
    using DECODER = Decoder<ATTN_CLS, MLP_CLS>;

    static bool fileExists(const std::string &filename) {
        std::ifstream file(filename);
        return file.good();
    }

    DecoderContext *getDecoderContext(int layers, const int hiddenSize, const int attHeadNum, const int kvHeadNum,
            const int imSize, const std::string &act, const float epsilon, int vocabSize, int embeddingSize,
            int maxPositions, int maxPosEmbed, int maxSeqLength, RopeParams *ropeParamsPtr) {
        pp_size = Env::getPipeline();
        pp_rank = messenger.getColor();
        int splits = messenger.getSize();
        int splitIdx = messenger.getRank();
        tp_size = splits;
        tp_rank = splitIdx;
        printf("pp_size: %d, pp_rank: %d, tp_size: %d, tp_rank: %d\n", pp_size, pp_rank, tp_size, tp_rank);

        if (context != nullptr) {
            if (context->hiddenSize == hiddenSize && context->attHeadNum == attHeadNum
                    && context->kvHeadNum == kvHeadNum && context->intermediateSize == imSize
                    && context->splitIdx == splitIdx) {
                return context.get();
            } else {
                printf("Different context size not unsupported!\n");
                exit(-1);
            }
        } else {
            this->context.reset(new DecoderContext(layers, hiddenSize, attHeadNum, kvHeadNum, imSize, act, epsilon,
                    vocabSize, embeddingSize, maxPositions, maxPosEmbed, maxSeqLength, splitIdx, splits, ropeParamsPtr));
        }

        return this->context.get();
    }

    void setDecoderWeights(DECODER *pdecoder, const std::string &modelPath, int layerIdx) {
        const int hiddenSize = getContext()->hiddenSize;
        const int imSize = getContext()->intermediateSize;
        const int kvHeadNum = getContext()->kvHeadNum;
        const int attHeadSize = getContext()->attHeadSize;
        const int mlpFactor = (getContext()->actType == DecoderContext::SWIGLU) ? 2 : 1;
        int qSize = hiddenSize;
        int kvSize = attHeadSize * kvHeadNum;
        int qkvSize = qSize + kvSize + kvSize;

#define ALLOC(size, alignment) aligned_alloc((alignment), (size))
        float *qkvWeight = (float *)ALLOC(hiddenSize * qkvSize * sizeof(float), 64);
        float *qkvBias = (float *)ALLOC(qkvSize * sizeof(float), 64);
        float *attnOutWeight = (float *)ALLOC(hiddenSize * hiddenSize * sizeof(float), 64);
        float *attnOutBias = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *fc1Weight = (float *)ALLOC(hiddenSize * imSize * mlpFactor * sizeof(float), 64);
        float *fc1Bias = (float *)ALLOC(imSize * sizeof(float), 64);
        float *fc2Weight = (float *)ALLOC(hiddenSize * imSize * sizeof(float), 64);
        float *fc2Bias = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *ln1Gamma = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *ln1Beta = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *ln2Gamma = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *ln2Beta = (float *)ALLOC(hiddenSize * sizeof(float), 64);
        float *fc3Weight = nullptr;

        // printf("hiddenSize=%d, qkvSize=%d\n", hiddenSize, qkvSize);
        loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".attention.query_key_value.weight.0.bin",
                qkvWeight, hiddenSize * qkvSize, getDataType());
        loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".attention.dense.weight.0.bin",
                attnOutWeight, hiddenSize * hiddenSize, getDataType());

        // Stardard 2 layer MLP
        if (fileExists(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.dense_h_to_4h.weight.0.bin")) {
            loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.dense_h_to_4h.weight.0.bin",
                    fc1Weight, hiddenSize * imSize * mlpFactor, getDataType());
            loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.dense_4h_to_h.weight.0.bin",
                    fc2Weight, hiddenSize * imSize, getDataType());
        }
        // gate, up, down weights for Llama like model
        else {
            fc3Weight = (float *)ALLOC(hiddenSize * imSize * sizeof(float), 64);
            loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.gate_proj.weight.0.bin",
                    fc1Weight, hiddenSize * imSize * mlpFactor, getDataType());
            loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.up_proj.weight.0.bin", fc2Weight,
                    hiddenSize * imSize, getDataType());
            loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.down_proj.weight.0.bin",
                    fc3Weight, hiddenSize * imSize, getDataType());
        }

        loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".input_layernorm.weight.bin", ln1Gamma,
                hiddenSize, getDataType());
        loadWeight(modelPath + "/model.layers." + std::to_string(layerIdx) + ".post_attention_layernorm.weight.bin",
                ln2Gamma, hiddenSize, getDataType());

#define READ_OPTIONAL(filename, addr, size, errmsg)                             \
    {                                                                           \
        int ret = loadWeight((filename), (addr), (size), getDataType(), false); \
        if (ret == 0) {                                                         \
            free(addr);                                                         \
            addr = nullptr;                                                     \
        } else {                                                                \
            if (ret != (size)) {                                                \
                printf("%s\n", (errmsg));                                       \
                exit(-1);                                                       \
            }                                                                   \
        }                                                                       \
    }

        // The bias is optional
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".attention.query_key_value.bias.0.bin",
                qkvBias, qkvSize, "read QKV bias error");
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".attention.dense.bias.bin",
                attnOutBias, hiddenSize, "read attn dense bias error");
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".input_layernorm.bias.bin", ln1Beta,
                hiddenSize, "read LN1 beta error");
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".post_attention_layernorm.bias.bin",
                ln2Beta, hiddenSize, "read LN2 beta error");
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.dense_h_to_4h.bias.0.bin",
                fc1Bias, imSize, "read FC1 bias error");
        READ_OPTIONAL(modelPath + "/model.layers." + std::to_string(layerIdx) + ".mlp.dense_4h_to_h.bias.bin", fc2Bias,
                hiddenSize, "read FC2 bias error");

#define FREE(x) \
    if ((x)) free((x))
        // Need the tranposed weights in our interface
        // ordering, trans, rows, cols, alpha, a, lda, b, ldb

        std::vector<float *> params = {qkvWeight, qkvBias, qkvWeight + qSize, qkvBias + qSize,
                qkvWeight + qSize + kvSize, qkvBias + qSize + kvSize, attnOutWeight, attnOutBias, ln1Gamma, ln1Beta,
                fc1Weight, fc1Bias, fc2Weight, fc2Bias, ln2Gamma, ln2Beta, fc3Weight};
        pdecoder->setWeights(getContext(), params, false);
        FREE(qkvWeight);
        FREE(attnOutWeight);
        FREE(fc1Weight);
        FREE(fc2Weight);
        FREE(fc3Weight);
        FREE(qkvBias);
        FREE(attnOutBias);
        FREE(fc1Bias);
        FREE(fc2Bias);
        FREE(ln1Gamma);
        FREE(ln1Beta);
        FREE(ln2Gamma);
        FREE(ln2Beta);
    }

    void setPredictorWeight(const std::string &modelPath) {
        int inputSize = predictor->getInputSize();
        int outputSize = predictor->getOutputSize();

        float *weight = (float *)malloc(inputSize * outputSize * sizeof(float));
        float *bias = nullptr;

        loadWeight(modelPath + "/model.lm_head.weight.bin", weight, inputSize * outputSize, this->getDataType());

        predictor->setWeight(weight, bias);

        free(weight);
    }

    virtual void prepareBuffers(
            DecoderContext *ctx, int userSideBS, int beamSize, bool logitsAll = false, bool prefix = false) {
        int batchSize = ctx->batchSize;
        int hiddenSize = ctx->hiddenSize;
        int seqLen = ctx->inputSeqLen;
        int vocabSize = ctx->vocabSize;
        int maxPositions = ctx->maxPositions;
        int layers = this->decoders.size();
        int workers = this->messenger.getSize();

        // Prepare buffers
        int logitsLen = logitsAll ? batchSize * seqLen : userSideBS * beamSize;
        int actRows = batchSize * seqLen; // rows for activation

        // Convert final output buffer size into rows in the units of hiddenSize
        int outRows = actRows;
        if (logitsLen * vocabSize > outRows * hiddenSize) { outRows = logitsLen * vocabSize / hiddenSize + 1; }

        this->actBuffers->Resize(actRows + outRows, hiddenSize);

        // Attention mask
        int sizeRequired = batchSize * seqLen * seqLen;
        getAttnMask(sizeRequired);

        // Cached keys/values
        // The maximum sequence length is to be the same as maxPositions, at most
        // And the cache always needs to account for beam size
        int headsPerSplit = (ctx->kvHeadNum + workers - 1) / workers;
        this->kvCacheMgr->resize(prefix ? this->prefixSeqLen : maxPositions, userSideBS * beamSize, headsPerSplit,
                ctx->attHeadSize, prefix);
    }

    float *getAttnMask(int sizeRequired) {
        if (this->maskSize < sizeRequired) {
            if (this->attnMask) free(this->attnMask);
            this->attnMask = (float *)aligned_alloc(64, sizeRequired * sizeof(float));
            this->maskSize = sizeRequired;
        }
        return this->attnMask;
    }

    int getStartId() { return startId; }

    virtual void embeddingForward(int *ids, float *output, int batchSize, int seqLen) {
        printf("embeddingForward(float) must be implemented.\n");
        exit(-1);
    }
    virtual void embeddingForward(int *ids, bfloat16_t *output, int batchSize, int seqLen) {
        printf("embeddingForward(bfloat16_t) must be implemented.\n");
        exit(-1);
    }

    virtual void lastLayerNormForward(float *input, float *output, int rows) {
        printf("lastLayerNormForward(float) must be implemented.\n");
        exit(-1);
    }
    virtual void lastLayerNormForward(bfloat16_t *input, bfloat16_t *output, int rows) {
        printf("lastLayerNormForward(bfloat16_t) must be implemented.\n");
        exit(-1);
    }

    virtual void prepareAttnMask(int *ids, int step) = 0;

public:
    virtual int *getPositionIds(int *ids, int batchSize, int seqLen, int step) { return nullptr; }

protected:
    // For communication
    Messenger &messenger;

    // For pipeline parallel and tensor parallel config
    int pp_size = 1; // pipeline parallel stage size
    int pp_rank = 0; // pipeline parallel stage rank
    int tp_size = 1; // tensor parallel size
    int tp_rank = 0; // tensor parallel rank

    // Execution context
    std::shared_ptr<DecoderContext> context;

    // The initial input sequence length, which is the prompt token size
    int initSeqLen;
    // Accumulated sequence length, = past_seq_len + current_seq_len
    int accSeqLen;
    // The prefix input  sequence length
    int prefixSeqLen;

    bool prefixSharing;

    // If not the master, need to receive token IDs from the master
    int *inputTokens;

    std::shared_ptr<KVCacheManager<KVCacheT>> kvCacheMgr;

    // Embedding output data type = input data type of Attention
    using AttnInT = typename AttnTypeExtractor<ATTN_CLS>::Tin;
    using AttnOutT = typename AttnTypeExtractor<ATTN_CLS>::Tout;
    using MlpInT = typename MlpTypeExtractor<MLP_CLS>::Tin;
    using MlpOutT = typename MlpTypeExtractor<MLP_CLS>::Tout;

    // Activation buffers (declared as float, but the actual data type may be different)
    std::shared_ptr<hpj::Matrix<float>> actBuffers;

protected:
    // Components most LLMs may use
    std::vector<DECODER *> decoders;

    using LinearWeiT = typename std::conditional<std::is_same_v<MlpOutT, bfloat16_t>, bfloat16_t, float16_t>::type;
    DistLinear<LinearWeiT> *predictor;

private:
    int maskSize; // size of allocated attnMask
    float *attnMask; // attention mask, set as private as may need to enlarge

    int startId;
    int endId;

    WDataType wType;

#ifdef DEBUG
    Debugger dbg;
#endif
};
