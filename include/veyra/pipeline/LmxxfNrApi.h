#pragma once
#include <cstdint>
#define LMXXF_NR_ABI_VERSION 1u
#define LMXXF_NR_API_V1_SIZE 136u
#define LMXXF_NR_FRAME_FLAG_STRENGTH (1u<<0)
enum LmxxfNrStatus{LMXXF_NR_OK=0,LMXXF_NR_UNSUPPORTED_ABI=1,LMXXF_NR_INVALID_ARGUMENT=2,LMXXF_NR_NOT_IMPLEMENTED=3,LMXXF_NR_UNAVAILABLE=4,LMXXF_NR_FAILED=5};
struct LmxxfNrCapabilities{uint32_t struct_size,abi_version,max_input_width,max_input_height,history_supported,overlap_supported,graph_supported,hip_ready,gfx1201_target;};
struct LmxxfNrCreateInfo{uint32_t struct_size;void*device;void*queue;const wchar_t*assets_directory;uint32_t flags;};
struct LmxxfNrFrameInfo{uint32_t struct_size;uint64_t session_id,frame_id,list_generation;void*command_list;uint32_t color_width,color_height;void*color;uint32_t color_state,flags;float transfer_strength,color_strength;uint32_t debug_view;float model_scale;void*exposure;uint32_t exposure_state;float pre_exposure,exposure_scale;};
struct LmxxfNrJob{uint32_t struct_size;void*handle;void*private_output;};
struct LmxxfNrTimings{uint32_t struct_size,valid;float network_ms;uint32_t reserved;uint64_t frame_id;};
struct LmxxfNrApi{uint32_t struct_size,abi_version;int32_t(*QueryCapabilities)(LmxxfNrCapabilities*);int32_t(*Create)(const LmxxfNrCreateInfo*,void**);int32_t(*Destroy)(void*);int32_t(*PrepareSession)(void*);int32_t(*PrepareFrame)(void*,const LmxxfNrFrameInfo*,LmxxfNrJob*);int32_t(*RecordInputs)(void*,void*,void*);int32_t(*EnqueueHip)(void*,void*,void*);int32_t(*RecordOutputs)(void*,void*,void*);int32_t(*ExecuteAfterProducer)(void*,void*,void*);int32_t(*CancelUnsubmitted)(void*,void*);int32_t(*Poll)(void*,void*,uint32_t*);int32_t(*Retire)(void*,void*);int32_t(*ResetHistory)(void*);int32_t(*Drain)(void*);int32_t(*GetStatus)(void*,char*,uint32_t);int32_t(*GetLastError)(char*,uint32_t);int32_t(*GetTimings)(void*,LmxxfNrTimings*);};
using LmxxfNrGetApiFn=int32_t(*)(uint32_t,LmxxfNrApi*);
