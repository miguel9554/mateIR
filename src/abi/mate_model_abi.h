#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MateModel MateModel;
typedef struct MateInstance MateInstance;

typedef enum MateStatusCode {
    MATE_STATUS_OK = 0,
    MATE_STATUS_ERROR = 1,
} MateStatusCode;

typedef struct MateStatus {
    MateStatusCode code;
    const char* message;
} MateStatus;

typedef enum MateEdge {
    MATE_EDGE_POSEDGE = 1,
    MATE_EDGE_NEGEDGE = 2,
} MateEdge;

typedef enum MateFlopsInitial {
    MATE_FLOPS_INITIAL_RANDOM = 0,
    MATE_FLOPS_INITIAL_ZERO = 1,
    MATE_FLOPS_INITIAL_ONE = 2,
} MateFlopsInitial;

typedef enum MateInputKind {
    MATE_INPUT_ASYNC = 0,
    MATE_INPUT_SYNC = 1,
    MATE_INPUT_CLOCK = 2,
    MATE_INPUT_RESET = 3,
} MateInputKind;

typedef struct MatePortInfo {
    int32_t id;
    int32_t width;
    int32_t nwords;
    int32_t is_signed;
    MateInputKind kind;
    int32_t clock_id;
    int32_t reset_id;
} MatePortInfo;

typedef struct MateInputUpdate {
    int32_t input_id;
    const uint64_t* words;
    int32_t nwords;
} MateInputUpdate;

/*
 * Model-specific symbol. Phase 1 generated model code compiles SV to an
 * interpreted model here; phase 2 generated model code returns a native model.
 */
MateStatusCode mate_model_create(const MateModel** out_model, MateStatus* status);

MateStatusCode mate_model_destroy(const MateModel* model, MateStatus* status);

MateStatusCode mate_instance_create(const MateModel* model,
                                    const char* instance_name,
                                    MateInstance** out_instance,
                                    MateStatus* status);
MateStatusCode mate_instance_destroy(MateInstance* instance, MateStatus* status);

MateStatusCode mate_instance_init(MateInstance* instance,
                                  MateFlopsInitial flops_initial,
                                  uint64_t seed,
                                  const MateInputUpdate* async_inputs,
                                  int32_t async_count,
                                  const MateInputUpdate* sync_inputs,
                                  int32_t sync_count,
                                  MateStatus* status);

int32_t mate_input_id(const MateModel* model, const char* leaf_name);
int32_t mate_output_id(const MateModel* model, const char* leaf_name);
int32_t mate_observable_id(const MateModel* model, const char* full_path);
int32_t mate_clock_id(const MateModel* model, const char* display_or_leaf_name);
int32_t mate_reset_id(const MateModel* model, const char* display_or_leaf_name);

MateStatusCode mate_input_info(const MateModel* model,
                               int32_t input_id,
                               MatePortInfo* out_info,
                               MateStatus* status);
MateStatusCode mate_output_info(const MateModel* model,
                                int32_t output_id,
                                MatePortInfo* out_info,
                                MateStatus* status);
MateStatusCode mate_observable_info(const MateModel* model,
                                    int32_t observable_id,
                                    MatePortInfo* out_info,
                                    MateStatus* status);

MateStatusCode mate_set_input(MateInstance* instance,
                              int32_t input_id,
                              const uint64_t* words,
                              int32_t nwords,
                              MateStatus* status);
/* Applies all updates, then evaluates combinational logic once. */
MateStatusCode mate_set_inputs(MateInstance* instance,
                               const MateInputUpdate* updates,
                               int32_t update_count,
                               MateStatus* status);
MateStatusCode mate_apply_clock(MateInstance* instance,
                                int32_t clock_id,
                                MateEdge edge,
                                const MateInputUpdate* updates_before_edge,
                                int32_t update_count,
                                MateStatus* status);
MateStatusCode mate_apply_reset(MateInstance* instance,
                                int32_t reset_id,
                                MateEdge edge,
                                MateStatus* status);
MateStatusCode mate_get_output(const MateInstance* instance,
                               int32_t output_id,
                               uint64_t* words,
                               int32_t nwords,
                               MateStatus* status);
MateStatusCode mate_get_observable(const MateInstance* instance,
                                   int32_t observable_id,
                                   uint64_t* words,
                                   int32_t nwords,
                                   MateStatus* status);

#ifdef __cplusplus
}
#endif
