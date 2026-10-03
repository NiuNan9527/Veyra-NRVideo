#pragma once
#include "veyra/pipeline/LmxxfNrApi.h"
#include <d3d12.h>
#include <windows.h>
#include <filesystem>
#include <string>
namespace veyra::pipeline{class LmxxfNrBackend{public:LmxxfNrBackend()=default;~LmxxfNrBackend();LmxxfNrBackend(const LmxxfNrBackend&)=delete;LmxxfNrBackend&operator=(const LmxxfNrBackend&)=delete;bool initialize(ID3D12Device*,ID3D12CommandQueue*,const std::filesystem::path&);void shutdown();bool prepareAndRecordInput(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES,uint32_t,uint32_t,float,uint64_t,bool);bool recordOutput(ID3D12GraphicsCommandList*);bool enqueue();bool retire();ID3D12Resource*output()const{return static_cast<ID3D12Resource*>(job_.private_output);}const LmxxfNrCapabilities&capabilities()const{return capabilities_;}const std::string&lastError()const{return lastError_;}bool ready()const{return module_&&context_;}private:bool fail(const char*,int32_t);void refreshRuntimeError();bool cancelLiveJob();HMODULE module_=nullptr;LmxxfNrApi api_{};LmxxfNrCapabilities capabilities_{};void*context_=nullptr;ID3D12CommandQueue*queue_=nullptr;LmxxfNrJob job_{};uint64_t sessionId_=1,frameCounter_=0,listGeneration_=0;std::string lastError_;};}
