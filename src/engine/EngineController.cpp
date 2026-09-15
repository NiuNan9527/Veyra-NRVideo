#include "veyra/engine/EngineController.h"
#include "veyra/engine/VideoPresenter.h"
#include "veyra/engine/VideoExportJob.h"
#include "veyra/engine/LivePresentationTiming.h"
#include "veyra/engine/PresentationScheduler.h"
#include "veyra/engine/DeadlineWait.h"
#include "veyra/engine/EnhancementDelayEstimate.h"
#include "veyra/engine/LiveGpuScheduler.h"
#include "veyra/engine/LivePresentationResetPolicy.h"
#include "veyra/engine/TimingWindow.h"
#include "veyra/engine/FrameFlowWindow.h"
#include "veyra/engine/LiveFgAdmission.h"
#include "veyra/engine/FgRecoveryBudget.h"
#include "veyra/engine/RealtimePreviewScheduling.h"
#include "veyra/engine/CaptureHalfRate.h"
#include "veyra/source/MediaFileSource.h"
#include "veyra/source/LanStreamSource.h"
#include "veyra/source/CaptureCardSource.h"
#include "veyra/media/InputUrl.h"
#ifdef VEYRA_ENABLE_REMOTEPLAY
#include "veyra/source/RemotePlaySessionSource.h"
#endif
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/ResetCoordinator.h"
#include "veyra/diagnostics/ResetCause.h"
#include "veyra/sink/WasapiAudioSink.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/RuntimePaths.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <cmath>
#include <deque>
#include <algorithm>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
namespace veyra::engine {
using Clock=std::chrono::steady_clock;
namespace {
double elapsedMs(Clock::time_point from){return std::chrono::duration<double,std::milli>(Clock::now()-from).count();}
int64_t monotonic100ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;}
struct OnExit {std::function<void()> action;~OnExit(){action();}};
void logFrameFlow(const diagnostics::FrameFlowMetrics& m,const char* state){
    const auto& c=m.counters;const auto& id=m.latest;
    const auto& r=m.reset; const auto ms=[&](diagnostics::ResetStage s){return r.stageMs[size_t(s)].value_or(-1.0);};
    veyra::log::info("frame-flow",std::format("state={} session={} revision={} epoch={} source={} batch={} readyFence={} consumerFence={} captureReceived={} mailboxOverwritten={} sourceAccepted={} sourceSkippedBeforeGraph={} realSubmitted={} fgCandidate={} fgSkippedBeforeEval={} fgEvaluated={} fgWarmup={} fgReadyValid={} fgInvalid={} realPresented={} generatedPresented={} generatedExpiredAfterEval={} cancelledBeforePresent={} commandSlots={} commandHighWater={} presentationHighWater={} slotWaitCount={} slotWaitMs={:.3f} generatedFps={:.2f} presentSubmitFps={:.2f} resetRevision={} resetEpoch={} resetReason={} resetOutcome={} resetDrainMs={:.3f} resetDestroyMs={:.3f} resetCreateMs={:.3f} resetWarmupMs={:.3f} resetFirstValidMs={:.3f} resetTotalMs={:.3f}",state,id.sessionId,id.frame.settingsRevision,id.frame.epoch,id.frame.sourceFrameId,id.batchId,id.readyFence,id.consumerFence,c.captureReceived,c.mailboxOverwritten,c.sourceAccepted,c.sourceSkippedBeforeGraph,c.realSubmitted,c.fgCandidate,c.fgSkippedBeforeEval,c.fgEvaluated,c.fgWarmup,c.fgReadyValid,c.fgInvalid,c.realPresented,c.generatedPresented,c.generatedExpiredAfterEval,c.cancelledBeforePresent,c.commandSlotsInFlight,c.commandSlotHighWater,c.presentationBatchHighWater,m.slotReuseWaitCount,m.slotReuseWaitMs.value_or(0),m.validGeneratedFps,m.presentSubmitFps,r.settingsRevision,r.epoch,unsigned(r.reason),unsigned(r.outcome),ms(diagnostics::ResetStage::Drain),ms(diagnostics::ResetStage::Destroy),ms(diagnostics::ResetStage::Create),ms(diagnostics::ResetStage::Warmup),ms(diagnostics::ResetStage::FirstValid),r.totalMs.value_or(-1.0)));
}
}
EngineController::EngineController(){worker_=std::thread(&EngineController::dispatch,this);}
EngineController::~EngineController(){ {std::lock_guard lock(mutex_);shutdown_=true;pending_={};stop_=true;}wake_.notify_one();if(worker_.joinable())worker_.join(); }
void EngineController::dispatch(){
    for(;;){std::function<void()> task;{std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return shutdown_||bool(pending_);});if(shutdown_)break;task=std::move(pending_);pending_={};busy_=true;stop_=false;}
        try{task();}catch(const std::exception&){status(L"任务异常，已停止；请查看诊断",true);}
        {std::lock_guard lock(mutex_);busy_=false;snapshot_.running=false;if(snapshot_.transport==TransportState::Stopping)snapshot_.transport=TransportState::Empty;}
    }
}
void EngineController::post(std::function<void()> task){ {std::lock_guard lock(mutex_);stop_=true;pending_=std::move(task);}wake_.notify_one(); }
bool EngineController::idle()const{std::lock_guard lock(mutex_);return !busy_&&!pending_;}
void EngineController::open(HWND video,const std::wstring& path,PlayerOptions opts){
    const bool captureReplay=opts.captureReplayForTest;
    const bool disableAdmission=opts.captureReplayDisableFgAdmissionForTest;
    {std::lock_guard lock(mutex_);snapshot_={};activeFlow_.reset();previewView_={};snapshot_.sessionId=++sessionId_;snapshot_.transport=TransportState::Opening;savePath_.clear();desired_=opts.snapshot();desired_.revision=++nextRevision_;snapshot_.desired=desired_;opts=PlayerOptions::from(desired_);}
    opts.captureReplayForTest=captureReplay;
    opts.captureReplayDisableFgAdmissionForTest=disableAdmission;
    post([this,video,path,opts]{paused_=false;seekSeconds_=-1;run(video,path,opts);});
}
#ifdef VEYRA_ENABLE_REMOTEPLAY
void EngineController::openRemotePlay(HWND window,source::RemotePlayConnectDesc desc,PlayerOptions opts){
    auto request=std::make_shared<source::RemotePlayConnectDesc>(std::move(desc));
    {std::lock_guard lock(mutex_);snapshot_={};activeFlow_.reset();previewView_={};snapshot_.sessionId=++sessionId_;snapshot_.transport=TransportState::Opening;snapshot_.remotePlay=true;snapshot_.capture=true;savePath_.clear();desired_=opts.snapshot();desired_.revision=++nextRevision_;snapshot_.desired=desired_;opts=PlayerOptions::from(desired_);}
    post([this,window,request,opts]{paused_=false;seekSeconds_=-1;run(window,L"remoteplay:",opts,request);});
}
remoteplay::ControllerFeedback EngineController::remotePlayFeedback(){std::lock_guard lock(mutex_);return activeRemote_?activeRemote_->takeFeedback():remoteplay::ControllerFeedback{};}
void EngineController::remotePlayController(const remoteplay::ControllerState& state){std::lock_guard lock(mutex_);if(activeRemote_)activeRemote_->controller(state);}
void EngineController::remotePlayLoginPin(std::string pin){std::lock_guard lock(mutex_);if(activeRemote_)activeRemote_->loginPin(std::move(pin));}
#endif
void EngineController::stop(){std::lock_guard lock(mutex_);stop_=true;pending_={};snapshot_.transport=busy_?TransportState::Stopping:TransportState::Empty;}
void EngineController::pause(bool p){paused_=p;std::lock_guard lock(mutex_);if(snapshot_.running)snapshot_.transport=p?TransportState::Paused:TransportState::Playing;}
void EngineController::setVolume(float gain,bool mute){if(!std::isfinite(gain))return;volume_=std::clamp(gain,0.0f,1.0f);muted_=mute;}
bool EngineController::requestSettings(EnhancementSettings s){
    if(!s.validate().empty()){veyra::log::warn("settings","invalid whole settings transaction rejected");status(L"整套设置无效，未应用任何字段",false);return false;}
    std::lock_guard lock(mutex_);if(snapshot_.image)s.multiplier=1;
    // Revision partitions GPU history and measurements. Audio-only edits must
    // not invalidate in-flight video, and identical notifications are no-ops.
    s.revision=desired_.revision;if(s==desired_)return true;
    if(!s.sameVideoConfiguration(desired_))s.revision=++nextRevision_;
    desired_=s;snapshot_.desired=s;snapshot_.applying=snapshot_.applied!=desired_;return true;
}
void EngineController::saveFrame(const std::wstring& path){std::lock_guard lock(mutex_);savePath_=path;}
void EngineController::startExport(const std::wstring& input,const std::wstring& output,PlayerOptions opts,bool hevc){
    const auto frozen=opts.snapshot();post([this,input,output,frozen,hevc]{CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool ok=exportVideo(input,output,PlayerOptions::from(frozen),hevc,stop_,[this](double p,const std::wstring& s){std::lock_guard lock(mutex_);snapshot_.status=s;snapshot_.position=p;snapshot_.duration=1;snapshot_.running=true;});{std::lock_guard lock(mutex_);snapshot_.running=false;snapshot_.failed=!ok&&!stop_;}CoUninitialize();});
}
PlayerSnapshot EngineController::snapshot()const{
    std::lock_guard lock(mutex_);auto copy=snapshot_;copy.volume=volume_;copy.muted=muted_;
    const bool playing=copy.running&&!copy.image&&copy.transport==TransportState::Playing;
    if(activeFlow_)copy.metrics.flow=activeFlow_->snapshot(monotonic100ns());
    copy.fgBudgetLimited=playing&&copy.metrics.flow.lastFgRejected100ns>0&&monotonic100ns()-copy.metrics.flow.lastFgRejected100ns<10000000;
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if(copy.remotePlay&&activeRemote_){
        const auto s=activeRemote_->sessionSnapshot();copy.remoteStream=s;copy.remotePlayState=int(s.state);
        const auto recovery=activeRemote_->recoveryStatus();copy.remoteRecovering=recovery.active;copy.remoteReconnectAttempts=recovery.attempts;copy.remoteRecoveryMessage=recovery.message;
        copy.remoteReceivedFps=s.receivedFps;copy.remoteDecodedFps=s.decodedFps;copy.remoteRatesReady=s.ratesReady;
        copy.remoteReceived=s.video.accessUnits;copy.remoteDecoded=s.decodedFrames;copy.remoteIngressDropped=s.video.dropped;
        copy.remotePlaySkipped=activeRemote_->skipped();copy.captureAudio=activeRemote_->audioState();copy.audioAvailable=copy.captureAudio.available;
    }
#endif
    auto& flow=copy.metrics.flow;
    if(!playing){flow.sourceCompletedFps=flow.outputCompletedFps=flow.validGeneratedFps=flow.presentSubmitFps=flow.xessSdkSubmitFps=0;}
    copy.fps=flow.sourceCompletedFps;copy.submissionFps=flow.presentSubmitFps;
    return copy;
}
void EngineController::status(const std::wstring& s,bool failed){std::lock_guard lock(mutex_);snapshot_.status=s;snapshot_.failed=failed;if(failed)snapshot_.transport=TransportState::Failed;}
void EngineController::run(HWND window,std::wstring path,PlayerOptions options,std::shared_ptr<source::RemotePlayConnectDesc> remoteRequest){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    status(L"正在初始化GPU与本地运行时…");
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;source::MediaFileSource source;
    sink::AudioPipeline audioPipe;sink::AudioRenderer audio;VideoPresenter presenter;
    pipeline::EnhanceGraph graph(ctx,ring);AVFrame* imageFrame=nullptr;AVFrame* cachedFrame=nullptr;pipeline::FramePacket cachedPacket;
    source::CaptureCardSource captureSource;source::LanStreamSource lanSource;
    const bool physicalCapture=path.starts_with(L"capture:")||path.starts_with(L"capture2:");
    const bool isLan=media::isRealtimeNetworkUrl(path);
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto remote=remoteRequest?std::make_shared<source::RemotePlaySessionSource>():nullptr;
    const bool isRemote=bool(remote);
    {std::lock_guard lock(mutex_);activeRemote_=remote;}
#else
    (void)remoteRequest;const bool isRemote=false;
#endif
    const bool isCapture=physicalCapture||options.captureReplayForTest||isRemote||isLan;
    const bool pairAnchoredLive=physicalCapture||options.captureReplayForTest||isRemote||isLan;
    const bool useLiveFgAdmission=!(options.captureReplayForTest&&options.captureReplayDisableFgAdmissionForTest);
    source::IFrameSource* activeSource=physicalCapture?static_cast<source::IFrameSource*>(&captureSource):isLan?static_cast<source::IFrameSource*>(&lanSource):static_cast<source::IFrameSource*>(&source);
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if(remote)activeSource=remote.get();
#endif
    if(options.captureReplayForTest)veyra::log::info("capture-test","file replay exercises live scheduler; no physical capture device or latency measurement");
    bool audioStarted=false;bool failed=false;
    try {
        do {
            Status st=Status::Ok;gfx::DeviceContextDesc dd;dd.commandSlotCount=6;
            if(!ctx.initialize(dd,st)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),6,st)){status(L"D3D12初始化失败，请查看诊断",true);break;}
            auto ext=std::filesystem::path(path).extension().wstring();for(auto& c:ext)c=towlower(c);
            const bool isImage=ext==L".png"||ext==L".jpg"||ext==L".jpeg";
            sink::RgbaImage image;
            uint32_t width=0,height=0;double duration=0;
            if(isImage){
                if(!sink::loadImage(path,image)){status(L"无法解码PNG/JPEG",true);break;}
                if(!pipeline::Extent{image.width,image.height}.valid()||uint64_t(image.width)*image.height>16777216){
                    runLargeImage(window,image,options,ctx,ring);break;
                }
                imageFrame=av_frame_alloc();imageFrame->format=AV_PIX_FMT_RGBA;imageFrame->width=image.width;imageFrame->height=image.height;imageFrame->pts=0;imageFrame->color_range=AVCOL_RANGE_JPEG;imageFrame->colorspace=AVCOL_SPC_RGB;imageFrame->color_trc=AVCOL_TRC_IEC61966_2_1;
                if(av_frame_get_buffer(imageFrame,32)<0){status(L"图片资源分配失败",true);break;}
                for(unsigned y=0;y<image.height;++y)memcpy(imageFrame->data[0]+size_t(y)*imageFrame->linesize[0],image.pixels.data()+size_t(y)*image.width*4,size_t(image.width)*4);
                width=image.width;height=image.height;options.fg=false;
                {std::lock_guard lock(mutex_);desired_.multiplier=1;snapshot_.image=true;snapshot_.desired=desired_;}
            }else{
                source::SourceOpenDesc od;od.path=path;
                // Files use the same shared D3D12 device as the graph. The
                // source performs a capability check and falls back to
                // software before returning its first frame; capture/PS5
                // retain their own decode contracts.
                od.preferHardwareDecode=!isCapture||isLan;
                if(od.preferHardwareDecode){od.d3d12Device=ctx.device();od.d3d12Queue=ctx.directQueue();}
                // Diagnostic uses the production graph/presenter to validate
                // D3D12VA imports without needing a paired PS5 or credentials.
                if(!isCapture&&GetEnvironmentVariableW(L"VEYRA_TEST_FILE_HW_DECODE",nullptr,0)){
                    od.preferHardwareDecode=true;od.d3d12Device=ctx.device();od.d3d12Queue=ctx.directQueue();
                }
#ifdef VEYRA_ENABLE_REMOTEPLAY
                if(remote){
                    status(L"正在连接 PS5…");
                    ctx.device()->AddRef();remoteRequest->decodeDevice=std::shared_ptr<ID3D12Device>(ctx.device(),[](ID3D12Device* device){device->Release();});
                    if(!remote->connect(std::move(*remoteRequest))){status(L"PS5 连接失败，请查看诊断",true);break;}
                    remoteRequest.reset();
                    while(!stop_){
                        const AVFrame* first=nullptr;const auto result=remote->read(cachedPacket,&first);
                        const auto state=remote->sessionSnapshot().state;
                        {std::lock_guard lock(mutex_);snapshot_.remotePlayState=int(state);}
                        if(result==source::SourceReadStatus::Frame){cachedFrame=av_frame_clone(first);break;}
                        if(result==source::SourceReadStatus::Error){const auto recovery=remote->recoveryStatus();status(recovery.message.empty()?L"PS5 未返回可解码画面，请检查主机及网络后重新连接。":recovery.message,true);break;}
                        if(state==remoteplay::SessionState::LoginPinRequired)status(L"PS5 要求登录 PIN，请在串流面板输入");
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    if(stop_||!cachedFrame)break;
                }else
#endif
                if(!(physicalCapture?captureSource.configure(od):activeSource->open(od))){status(!isCapture&&!source.errorMessage().empty()?source.errorMessage():L"无法打开视频，请查看诊断",true);break;}
                // A file's actual decoded pixel format and HDR VUI are only
                // reliable on AVFrame. Prime one frame before constructing
                // the graph so AV1/MOV 10-bit and HDR inputs do not get an
                // SDR graph by mistake. The cloned frame is consumed by the
                // normal owner-thread loop below, preserving its PTS.
                if(!isCapture){
                    pipeline::FramePacket firstPacket;const AVFrame* firstFrame=nullptr;
                    const auto firstStatus=activeSource->read(firstPacket,&firstFrame);
                    if(firstStatus!=source::SourceReadStatus::Frame||firstFrame==nullptr||firstPacket.pts.isUnknown()){
                        status(activeSource->info().kind==pipeline::SourceKind::File&&source.errorMessage().empty()
                            ?L"视频首帧无法解码或没有有效时间戳，请查看诊断":source.errorMessage().empty()?L"视频首帧读取失败":source.errorMessage(),true);
                        break;
                    }
                    cachedFrame=av_frame_clone(firstFrame);
                    if(!cachedFrame){status(L"视频首帧缓存失败",true);break;}
                    cachedPacket=firstPacket;
                }
                width=activeSource->info().width;height=activeSource->info().height;duration=isCapture?0:activeSource->info().duration.toDouble();
                {std::lock_guard lock(mutex_);snapshot_.sourceNotice=activeSource->info().dolbyVision.description();}
            }
            if(!pipeline::Extent{width,height}.valid()){status(L"图像尺寸超出单张GPU纹理能力，需要分块处理",true);break;}
            pipeline::EnhanceGraphDesc gd;gd.sourceWidth=width;gd.sourceHeight=height;gd.hdrInput=!isImage&&activeSource->info().color.isHdrPath();
            gd.highQualityPresentation=isRemote&&activeSource->info().color.reconstructChroma;
            gd.rgbInput=isImage||(isCapture&&activeSource->info().color.pixelFormat==pipeline::SourcePixelFormat::Bgra8);
            gd.captureBitDepth=activeSource->info().color.pixelFormat==pipeline::SourcePixelFormat::P010?10:activeSource->info().color.pixelFormat==pipeline::SourcePixelFormat::P016?16:8;
            gd.yuy2Input=isCapture&&activeSource->info().color.pixelFormat==pipeline::SourcePixelFormat::Yuy2;gd.stillImage=isImage;
            const auto resolution=pipeline::ResolutionPlan::make({width,height},options.sr,options.snapshot().nrPolicy,isImage,options.settings.revision,options.settings.srTarget,options.settings.lowLatency&&options.nr);
            gd.workWidth=resolution.base.width;gd.workHeight=resolution.base.height;gd.nrWidth=resolution.nr.width;gd.nrHeight=resolution.nr.height;gd.flowWidth=resolution.flow.width;gd.flowHeight=resolution.flow.height;
            const bool nvidiaAdapter=ctx.adapter().isNvidia;
            const bool xessFg=options.settings.frameGenerationBackend==FrameGenerationBackend::XeSS;
            gd.nrBeforeSr=!isImage&&options.settings.lowLatency&&options.nr&&resolution.srApplied;gd.enableSr=resolution.srApplied&&nvidiaAdapter;gd.videoSrQuality=options.settings.videoSrQuality;gd.enableNr=options.nr&&nvidiaAdapter;gd.nrRuntime=options.settings.nrRuntime;gd.enableFg=options.fg&&(nvidiaAdapter||xessFg);gd.fgMultiplier=options.fgMultiplier;gd.frameGenerationBackend=options.settings.frameGenerationBackend;gd.enableNvofStandalone=gd.enableNr;
            gd.noFeatures=false;gd.model=options.settings.model;gd.residual=options.settings.residual;gd.protection=options.settings.protection;gd.settingsRevision=options.settings.revision;gd.flowQuality=options.settings.flow;gd.contentRate=options.settings.content;
            gd.opticalFlowBackend=options.settings.opticalFlowBackend;gd.amdFlowHalfResolution=options.settings.amdFlowHalfResolution;
            gd.hdrOutput=options.settings.useHdrPreview(gd.hdrInput,gfx::PresentSink::hdrDisplayActive(window));
            veyra::log::info("display-color",std::format("hdrInput={} hdrOutput={} forceSdrPreview={}",gd.hdrInput,gd.hdrOutput,options.settings.forceSdrPreview));
            gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
            std::wstring backendRecoveryWarning;
            auto initializePreview=[&](pipeline::EnhanceGraphDesc& desc,PlayerOptions& selected){
                backendRecoveryWarning.clear();
                auto supported=selected.snapshot();
                if(disableUnsupportedNvidiaEffects(supported,nvidiaAdapter)){
                    selected=PlayerOptions::from(supported);
                    const auto plan=pipeline::ResolutionPlan::make({width,height},false,supported.nrPolicy,isImage,supported.revision,supported.srTarget,false);
                    desc.workWidth=plan.base.width;desc.workHeight=plan.base.height;desc.nrWidth=plan.nr.width;desc.nrHeight=plan.nr.height;desc.flowWidth=plan.flow.width;desc.flowHeight=plan.flow.height;
                    desc.enableNr=desc.enableSr=desc.enableNvofStandalone=desc.nrBeforeSr=false;desc.enableFg=selected.fg;desc.fgMultiplier=selected.fgMultiplier;
                    backendRecoveryWarning=L"当前 GPU 不支持 NVIDIA 增强；NR、超分及 DLSS 已关闭，XeSS选择保留";
                    veyra::log::warn("capability","normalized requested NVIDIA effects before graph creation; applied settings reflect actual disabled stages");
                }
                for(unsigned attempt=0;attempt<5;++attempt){
                    bool opened=graph.initialize(desc);
                    auto failure=graph.failedBackend();
                    if(opened){
                        opened=presenter.open(ctx,window,graph,selected.settings.captureCompatible)&&graph.createViews();
                        failure=FailedBackend::Infrastructure;
                        if(opened&&graph.xessEnabled()&&!presenter.xessActive()){opened=false;failure=FailedBackend::Fg;}
                    }
                    if(opened)return true;
                    auto reduced=selected.snapshot();
                    if(FAILED(ctx.device()->GetDeviceRemovedReason())||!disableFailedBackend(reduced,failure))return false;
                    veyra::log::warn("backend-recovery",std::format("initialization failed component={} attempt={} revision={} -> nr={} sr={} multiplier={}; original SDK error above",unsigned(failure),attempt+1,reduced.revision,reduced.nr,reduced.sr,reduced.multiplier));
                    if(!backendRecoveryWarning.empty())backendRecoveryWarning+=L"；";
                    backendRecoveryWarning+=std::wstring(backendFailureName(failure))+L"初始化失败，已关闭依赖效果（错误码见日志）";
                    if(!ring.drainQueue()||!ring.discardRecording())return false;
                    presenter.close();graph.shutdown();selected=PlayerOptions::from(reduced);
                    const auto plan=pipeline::ResolutionPlan::make({width,height},selected.sr,reduced.nrPolicy,isImage,reduced.revision,reduced.srTarget,reduced.lowLatency&&selected.nr);
                    desc.workWidth=plan.base.width;desc.workHeight=plan.base.height;desc.nrWidth=plan.nr.width;desc.nrHeight=plan.nr.height;desc.flowWidth=plan.flow.width;desc.flowHeight=plan.flow.height;
                    desc.enableNr=selected.nr&&nvidiaAdapter;desc.enableSr=plan.srApplied&&nvidiaAdapter;desc.enableFg=selected.fg&&(nvidiaAdapter||reduced.frameGenerationBackend==FrameGenerationBackend::XeSS);desc.fgMultiplier=selected.fgMultiplier;
                    desc.enableNvofStandalone=desc.enableNr;desc.nrBeforeSr=!isImage&&reduced.lowLatency&&selected.nr&&desc.enableSr;
                }
                return false;
            };
            const auto initialRequested=options.snapshot();
            if(!initializePreview(gd,options)){status(L"视频初始化失败，请查看对应组件的诊断日志",true);break;}
            if(!backendRecoveryWarning.empty()){
                std::lock_guard lock(mutex_);desired_.rejectVideoRequest(initialRequested,options.snapshot());snapshot_.desired=desired_;snapshot_.backendWarning=backendRecoveryWarning;
            }
            if(!nvidiaAdapter&&(options.nr||options.sr||(options.fg&&!xessFg))){
                veyra::log::warn("capability",std::format("non-NVIDIA adapter disabled requested features: nr={} sr={} fgBackend={} flowBackend={}",options.nr,options.sr,frameGenerationBackendName(options.settings.frameGenerationBackend),opticalFlowBackendName(options.settings.opticalFlowBackend)));
                status(L"当前非 NVIDIA 适配器：NR、NVIDIA 超分与 DLSS 已禁用；可使用 XeSS 预览和 AMD 光流",false);
            }
            if(gd.hdrInput)status(gd.hdrOutput?L"HDR输入 · HDR保留增强/显示":L"HDR输入 · SDR色调映射后增强/显示（1000nit参考峰值）",false);
            if(graph.xessEnabled()&&!presenter.xessActive())status(L"XeSS 未启用：运行时或设备不兼容；当前为普通呈现",false);
            {
                std::lock_guard lock(mutex_);
                if(!nvidiaAdapter&&(options.nr||options.sr||(options.fg&&!xessFg)))snapshot_.backendWarning=L"当前 GPU 不支持所选 NVIDIA 增强";
                if(graph.xessEnabled()&&!presenter.xessActive())snapshot_.backendWarning=L"XeSS 初始化失败；当前为普通呈现";
            }
            captureSource.setAudioSync(unsigned(options.settings.audioSync),options.settings.audioOffsetMs);
            if(physicalCapture&&!captureSource.start()){status(L"无法启动采集，请查看诊断",true);break;}
            {std::lock_guard lock(mutex_);snapshot_.duration=duration;snapshot_.nominalSourceFps=isImage?0:activeSource->info().averageFps;snapshot_.running=true;snapshot_.transport=TransportState::Playing;snapshot_.image=isImage;snapshot_.capture=isCapture;snapshot_.applied=options.snapshot();snapshot_.desired=desired_;}
            status(isImage?L"图片已增强，可保存PNG/JPEG":std::format(L"{} | 输入 {}×{} / 底图 {}×{} / NR {}×{} / 光流 {}×{} / FG与输出 {}×{} | {}",isRemote?L"PS5 串流":isLan?L"PC 局域网串流":isCapture?L"实时采集":L"播放",width,height,gd.workWidth,gd.workHeight,gd.nrWidth,gd.nrHeight,gd.flowWidth,gd.flowHeight,gd.workWidth,gd.workHeight,gd.nrBeforeSr?L"低延迟 · NR先行后超分":gd.nrWidth<gd.workWidth?L"实时内部处理并回填":L"原生NR（性能成本较高）"));
            pipeline::EnhanceGraph::FrameOutputs out;bool reset=true,hasOutput=false,audioRebuffering=false,seekPreviewPending=false;
            bool initialRemoteFramePending=isRemote;
            bool initialFileFramePending=!isImage&&!isCapture;
            uint64_t activeSeekId=0;
            Clock::time_point seekStarted{};
            uint64_t seekDecoded=0;
            bool fileAwaitingVideo=true,fileAudioAlignPending=true,fileInputEnded=false;double lastFilePresentedMs=0,lastFilePresentLateness=0;std::deque<double> latenessSamples;
            if(!isImage&&!isCapture&&audioPipe.open(path)){
                audioPipe.holdForVideo();audioPipe.startThread(&audio,true);audioStarted=true;
                std::lock_guard lock(mutex_);snapshot_.audioAvailable=true;
            }
            auto holdFileAudio=[&]{if(audioStarted)audioPipe.holdForVideo();fileAwaitingVideo=true;fileInputEnded=false;};
            TimingWindow captureAges,scheduleWaits,processTimes,presentTimes,decodeTimes,gpuReadyTimes;
            std::array<TimingWindow,size_t(diagnostics::GpuStage::Count)> gpuStageTimes;
            std::array<uint64_t,size_t(diagnostics::GpuStage::Count)> lastGpuSampleEnd{};
            auto nextTimingLog=Clock::now()+std::chrono::seconds(1);
            auto anchor=Clock::now(),statsStart=anchor;double anchorMs=0;uint64_t frames=0,sourceFrames=0;bool wasPaused=false;double discardBefore=0;
            double lastAudioClockMs=0;bool audioClockExhausted=false;auto audioTailAnchor=anchor;
            bool publishedAudioRecovery=false;HRESULT publishedAudioError=S_OK;uint64_t publishedAudioRecoveries=0;
            const bool injectFileEndpointLoss=GetEnvironmentVariableW(L"VEYRA_TEST_FILE_ENDPOINT_LOSS",nullptr,0)>0;
            bool fileEndpointLossInjected=false;
            auto publishAudioStatus=[&]{
                const bool recovering=audioPipe.endpointRecovering();const auto error=audioPipe.endpointError();const auto count=audioPipe.endpointRecoveries();
                {std::lock_guard lock(mutex_);snapshot_.audioRebuffering=audioPipe.waitingForVideo();snapshot_.audioVideoWaits=audioPipe.videoWaitCount();}
                if(recovering!=publishedAudioRecovery||error!=publishedAudioError||count!=publishedAudioRecoveries){
                    publishedAudioRecovery=recovering;publishedAudioError=error;publishedAudioRecoveries=count;
                    std::lock_guard lock(mutex_);snapshot_.audioInputChannels=audioPipe.pcmFormat().channels;snapshot_.audioOutputChannels=audio.outputFormat().channels;snapshot_.audioEndpointRecovering=recovering;snapshot_.audioEndpointError=error;snapshot_.audioEndpointRecoveries=count;
                }
            };
            PresentationScheduler liveTimeline;uint64_t submitted=0,expired=0;std::deque<int64_t> submissionTimes;
            DeadlineWait deadlineWait;
            // Realtime file preview scheduling (P2/P3): expired decoded
            // candidates lose their enhancement opportunity against the audio
            // master clock; FG admission predicts pair completion; XeSS-FG
            // generation is suppressed over stable intervals when sustained
            // late. Export/images/paused frames never enter these paths.
            uint64_t previewSkippedTotal=0,previewSkippedSinceSubmit=0;
            bool previewSkipSinceProcess=false;
            XessGenerationGate xessGenerationGate;
            double playbackSpeedLastPts=0;Clock::time_point playbackSpeedLastWall{};
            CaptureHalfRate captureSampler;
            FrameLineageTracker lineageTracker;
            auto completedProcessing=[&](pipeline::FrameIdentity id){std::lock_guard lock(mutex_);if(snapshot_.applied.revision==id.settingsRevision)++snapshot_.processedCompleted;};
            auto host100ns=[](){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()/100;};
            // Live ingress stays latest-frame; file preview keeps the audio clock
            // and may skip expired enhancement opportunities, never PCM.
            // Graph, query collection and presentation remain on this owner.
            graph.recordGpuTimings();presenter.recordGpuTimings();
            struct LiveStats {pipeline::FrameIdentity identity;uint64_t submitted=0,expired=0;double waitMs=0,presentMs=0,readyMs=0,ageMs=0,fps=0,ageP95=0,waitP95=0,presentP95=0,readyP95=0;diagnostics::GpuSample blit;};
            LiveStats liveStats;std::deque<int64_t> liveSubmissions;
            TimingWindow liveAges,liveWaits,livePresent,liveReady;
            FgRecoveryBudget fgBudget;uint64_t fgBudgetRevision=options.settings.revision;
            int64_t nextFgAdmissionLog=0;
            std::atomic<uint64_t> historyResets=0,presentationDrains=0,presentationCompletedReal=0,presentationSkippedGenerated=0,presentationCancelledJobs=0;
            std::atomic<uint64_t> presentationGeneration{0};
            std::unique_ptr<LiveGpuScheduler> liveScheduler;
            struct CompletionWatch {
                pipeline::EnhanceGraph::FrameOutputs output;
                std::shared_ptr<FrameFlowWindow> flow;
                Clock::time_point processStart;
                std::optional<double> gpuExecutionMs,fgExecutionMs;
                bool real=false;
                std::atomic<bool> ready=false;
                Clock::time_point readyObserved{};
                std::array<int64_t,4> frameReadyObserved{};
            };
            int64_t nextFgDeadlineLog=0;
            std::vector<std::shared_ptr<CompletionWatch>> pendingCompletions;
            std::shared_ptr<FrameFlowWindow> frameFlow;
            const uint64_t runSessionId=[&]{std::lock_guard lock(mutex_);return snapshot_.sessionId;}();
            pipeline::ResetReason pendingResetCause=pipeline::ResetReason::Open;
            auto traceFrame=[&](diagnostics::TraceKind kind,pipeline::FrameIdentity identity,uint64_t batch,
                uint64_t fence,int64_t pts,uint32_t detail,uint32_t count,double ms){
                Logger::instance().recordFrame({host100ns(),runSessionId,identity,batch,fence,pts,kind,detail,count,ms});
            };
            std::optional<diagnostics::FrameFlowMetrics::ResetRecord> resetRecord;
            std::optional<diagnostics::FrameFlowMetrics::ResetRecord> lastResetRecord;
            Clock::time_point resetStart{},resetStageStart{};
            auto markResetStage=[&](diagnostics::ResetStage stage){
                if(!resetRecord)return;
                const auto now=Clock::now();
                resetRecord->stageMs[size_t(stage)]=std::chrono::duration<double,std::milli>(now-resetStageStart).count();
                resetStageStart=now;
                if(frameFlow)frameFlow->resetLifecycle(*resetRecord);
            };
            auto finishReset=[&](diagnostics::ResetOutcome outcome){
                if(!resetRecord)return;
                resetRecord->outcome=outcome;resetRecord->totalMs=elapsedMs(resetStart);
                lastResetRecord=resetRecord;
                if(frameFlow)frameFlow->resetLifecycle(*resetRecord);
                const auto& r=*resetRecord;const auto ms=[&](diagnostics::ResetStage s){return r.stageMs[size_t(s)].value_or(-1.0);};
                traceFrame(diagnostics::TraceKind::Reset,{r.epoch,r.settingsRevision,r.sourceFrameId},0,0,0,r.reason,unsigned(outcome),*r.totalMs);
                veyra::log::info("reset-lifecycle",std::format("session={} revision={} epoch={} source={} reason={} outcome={} drainMs={:.3f} destroyMs={:.3f} createMs={:.3f} warmupSubmitMs={:.3f} firstValidObserveMs={:.3f} totalMs={:.3f} (CPU observed; -1=not measured)",r.sessionId,r.settingsRevision,r.epoch,r.sourceFrameId,unsigned(r.reason),unsigned(r.outcome),ms(diagnostics::ResetStage::Drain),ms(diagnostics::ResetStage::Destroy),ms(diagnostics::ResetStage::Create),ms(diagnostics::ResetStage::Warmup),ms(diagnostics::ResetStage::FirstValid),*r.totalMs));
                resetRecord.reset();
            };
            auto completeReset=[&](pipeline::FrameIdentity identity){
                if(!resetRecord||resetRecord->settingsRevision!=identity.settingsRevision||resetRecord->epoch!=identity.epoch||resetRecord->sourceFrameId!=identity.sourceFrameId)return;
                markResetStage(diagnostics::ResetStage::FirstValid);
                finishReset(diagnostics::ResetOutcome::Completed);
            };
            OnExit resetExit{[&]{finishReset(stop_?diagnostics::ResetOutcome::Cancelled:diagnostics::ResetOutcome::Failed);}};
            auto collectTimings=[&]{
                auto record=[&](const diagnostics::GpuFrameTiming& sample){
                    const auto& fgCost=sample.gpu[size_t(diagnostics::GpuStage::FgBatch)];
                    for(auto& watch:pendingCompletions){
                        const auto& id=watch->output.batch.identity;
                        if(id.epoch==sample.identity.epoch&&id.settingsRevision==sample.identity.settingsRevision&&id.sourceFrameId==sample.identity.sourceFrameId){
                            if(const auto span=diagnostics::graphExecutionSpanMs(sample))watch->gpuExecutionMs=span;
                            if(fgCost.state==diagnostics::SampleState::Measured)watch->fgExecutionMs=fgCost.milliseconds;
                        }
                    }
                    if(sample.identity.settingsRevision==fgBudgetRevision&&fgCost.state==diagnostics::SampleState::Measured&&fgCost.milliseconds)fgBudget.fgCost(*fgCost.milliseconds,host100ns());
                    if(frameFlow)frameFlow->gpuFrame(sample,host100ns());
                    for(size_t i=0;i<sample.gpu.size();++i){const auto& gpu=sample.gpu[i];
                        if(gpu.state==diagnostics::SampleState::Measured&&gpu.milliseconds)
                            traceFrame(diagnostics::TraceKind::Gpu,sample.identity,0,0,0,unsigned(i),1,*gpu.milliseconds);
                    }
                };
                for(const auto& sample:graph.takeGpuTimings())record(sample);
                for(const auto& sample:presenter.takeGpuTimings(ctx.fence()))record(sample);
            };
            // Shared with the presenter: resolve each batch on this single
            // object so SDK status and generated counts are consumed once.
            auto pollCompletions=[&]{
                collectTimings();
                for(auto it=pendingCompletions.begin();it!=pendingCompletions.end();){
                    auto& watch=**it;auto& batch=watch.output;
                    const auto completedFence=ctx.fence()->GetCompletedValue();
                    for(unsigned i=0;i<batch.batch.count;++i){const auto& item=batch.batch.frames[i];
                        if(!watch.frameReadyObserved[i]&&item.lease&&completedFence>=item.lease->readyFence)watch.frameReadyObserved[i]=host100ns();
                    }
                    if(!graph.resolveGeneration(batch)){++it;continue;}
                    // The GPU may finish between the first poll and resolve.
                    // Collect its timestamps before removing this watch.
                    if(!watch.gpuExecutionMs)collectTimings();
                    unsigned valid=0,invalid=0;
                    for(unsigned i=0;i<batch.batch.count;++i)if(batch.batch.frames[i].kind==pipeline::FrameKind::Generated){
                        if(batch.batch.frames[i].validity==pipeline::GenerationValidity::Valid)++valid;else ++invalid;
                    }
                    watch.flow->ready(batch.batch.batchId,watch.real,valid,invalid,host100ns());
                    traceFrame(diagnostics::TraceKind::Ready,batch.batch.identity,batch.batch.batchId,std::max(batch.videoFenceValue,batch.genFenceValue),batch.batch.b100ns,invalid,valid,elapsedMs(watch.processStart));
                    if(watch.real)completedProcessing(batch.batch.identity);
                    if(batch.batch.identity.settingsRevision==fgBudgetRevision)fgBudget.complete(watch.gpuExecutionMs,batch.fgEvaluated>0,batch.historyReset||batch.fgRecovery,host100ns(),watch.fgExecutionMs);
                    completeReset(batch.batch.identity);
                    watch.readyObserved=Clock::now();watch.ready=true;it=pendingCompletions.erase(it);
                }
                collectTimings();
            };
            if(!isImage){liveScheduler=std::make_unique<LiveGpuScheduler>();veyra::log::info("scheduler",std::format("single GPU owner thread={} capacity=2 source={} (bounded enhanced lookahead)",GetCurrentThreadId(),isCapture?"live":"file"));}
            auto drainLivePresentation=[&]{if(liveScheduler){++presentationDrains;liveScheduler->cancel();pollCompletions();pendingCompletions.clear();}};
            auto advanceLive=[&]{if(liveScheduler){pollCompletions();if(stop_||(paused_&&!seekPreviewPending)||seekSeconds_>=0)liveScheduler->cancel();else liveScheduler->advance(host100ns());}};
            auto waitLive=[&]{const auto due=liveScheduler?liveScheduler->wakeAt():0;deadlineWait.slice(due>host100ns()?std::min(1.0,double(due-host100ns())/10000):1.0);};
            uint64_t metricsRevision=options.settings.revision,metricsEpoch=0,metricsWindowEpoch=0,statsSourceBase=0;
            uint64_t slotWaitBase=ring.cpuWaitCount(),submitBase=ring.submitCount();double slotWaitMsBase=ring.cpuWaitMilliseconds();
            std::shared_ptr<FrameCompletionRates> completionRates;
            source::CaptureMetrics captureFlowBase{},captureFlowLast{};
            uint64_t rateSkippedBase=0;
            // Queued steps capture these locals; cancel before their destruction.
            OnExit stopPresentation{[&]{liveScheduler.reset();finishReset(stop_?diagnostics::ResetOutcome::Cancelled:diagnostics::ResetOutcome::Failed);std::lock_guard lock(mutex_);if(snapshot_.sessionId==runSessionId){if(frameFlow)snapshot_.metrics.flow=frameFlow->snapshot(host100ns());activeFlow_.reset();}}};
            const bool injectSourceGap=options.captureReplayForTest&&GetEnvironmentVariableW(L"VEYRA_TEST_REPLAY_SOURCE_GAP",nullptr,0)>0;
            std::optional<Clock::time_point> sourceGapUntil;
            // Master media clock for file playback: the audio device clock when
            // mapped, a monotonic anchor without audio, and its frozen value
            // while an endpoint recovers. Steady-state video scheduling reads
            // this clock; it never waits on audio coverage.
            auto nowMs=[&](){
                if(!audioStarted)return anchorMs+std::chrono::duration<double,std::milli>(Clock::now()-anchor).count();
                publishAudioStatus();const double a=audio.mediaTimeMs();
                if(std::isfinite(a)){lastAudioClockMs=a;audioClockExhausted=false;return a;}
                // A disconnected endpoint freezes the shared media clock. Only a
                // fully exhausted audio stream may hand its tail to the wall clock.
                if(audioPipe.clockExhausted()){
                    if(!audioClockExhausted){audioClockExhausted=true;audioTailAnchor=Clock::now();}
                    return lastAudioClockMs+std::chrono::duration<double,std::milli>(Clock::now()-audioTailAnchor).count();
                }
                return lastAudioClockMs;
            };
            uint64_t hdrDisplayCheck=0;
            bool captureRecovering=false;unsigned captureRetries=0;
            Clock::time_point captureRetryAt{};
            bool xessPresentationRecovery=false;
            auto recoverXessPresentation=[&]{
                if(!presenter.xessFailed()||FAILED(ctx.device()->GetDeviceRemovedReason()))return false;
                auto reduced=options.snapshot();
                if(!disableFailedBackend(reduced,FailedBackend::Fg))return false;
                holdFileAudio();fileAudioAlignPending=true;
                drainLivePresentation();
                if(!ring.drainQueue()||!ring.discardRecording())return false;
                out={};hasOutput=false;
                // A failed scheduler stays failed after cancel. Recreate it on
                // its owner, then rebuild the SDK swapchain via settings below.
                if(liveScheduler)liveScheduler=std::make_unique<LiveGpuScheduler>();
                {
                    std::lock_guard lock(mutex_);
                    desired_.rejectVideoRequest(options.snapshot(),reduced);
                    desired_.revision=++nextRevision_;snapshot_.desired=desired_;snapshot_.applying=true;
                    snapshot_.backendWarning=L"XeSS 呈现失败，正在恢复基础播放；错误码见日志";
                }
                veyra::log::warn("backend-recovery","XeSS presentation failed; drained commands, requesting swapchain rebuild without FG");
                xessPresentationRecovery=true;
                reset=true;pendingResetCause=pipeline::ResetReason::Settings;
                return true;
            };
            while(!stop_){
                collectTimings();
                if(captureRecovering){
                    if(Clock::now()<captureRetryAt){std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
                    ++captureRetries;
                    {std::lock_guard lock(mutex_);snapshot_.captureReconnectAttempts=captureRetries;}
                    status(std::format(L"采集信号中断，正在重连原设备（第 {} 次）",captureRetries));
                    if(captureSource.reconnect(muted_?0.0f:volume_.load(),unsigned(options.settings.audioSync),options.settings.audioOffsetMs)){
                        captureRecovering=false;reset=true;pendingResetCause=pipeline::ResetReason::DeviceLost;captureSampler.reset();
                        {std::lock_guard lock(mutex_);snapshot_.captureRecovering=false;}
                        status(L"采集设备已重新连接，等待新画面");
                    }else{
                        captureRetryAt=Clock::now()+std::chrono::seconds(std::min(5u,captureRetries));
                        continue;
                    }
                }
                if(gd.hdrInput&&GetTickCount64()-hdrDisplayCheck>2000){
                    hdrDisplayCheck=GetTickCount64();
                    const bool native=options.settings.useHdrPreview(gd.hdrInput,gfx::PresentSink::hdrDisplayActive(window));
                    if(native!=gd.hdrOutput){std::lock_guard lock(mutex_);if(desired_.revision==options.settings.revision){desired_.revision=++nextRevision_;snapshot_.desired=desired_;snapshot_.applying=true;}}
                }
                if(audioStarted)publishAudioStatus();
                if(audioStarted&&injectFileEndpointLoss&&!fileEndpointLossInjected&&frames>=20){audioPipe.requestEndpointLossForTest();fileEndpointLossInjected=true;}
                advanceLive();
                if(liveScheduler&&liveScheduler->failed()){if(recoverXessPresentation())continue;status(L"视频呈现失败，请查看诊断",true);break;}
                const float gain=muted_?0.0f:volume_.load();audio.setGain(gain);
                if(physicalCapture)captureSource.recoverAudio(gain,unsigned(options.settings.audioSync),options.settings.audioOffsetMs);
                captureSource.setAudioSync(unsigned(options.settings.audioSync),options.settings.audioOffsetMs);
                if(physicalCapture){const bool available=captureSource.setAudioGain(gain);const auto audioState=captureSource.audioState();std::lock_guard lock(mutex_);snapshot_.audioAvailable=available;snapshot_.captureAudio=audioState;snapshot_.audioInputChannels=audioState.inputChannels;snapshot_.audioOutputChannels=audioState.outputChannels;}
#ifdef VEYRA_ENABLE_REMOTEPLAY
                if(remote){remote->setAudioGain(gain);remote->setAudioSync(unsigned(options.settings.audioSync),options.settings.audioOffsetMs);
                    const auto state=remote->audioState();const auto session=remote->sessionSnapshot();const auto rates=remote->rates();
                    std::lock_guard lock(mutex_);snapshot_.audioAvailable=state.available;snapshot_.captureAudio=state;snapshot_.remotePlayState=int(session.state);snapshot_.remotePlaySkipped=remote->skipped();snapshot_.remoteReceivedFps=rates.receivedFps;snapshot_.remoteDecodedFps=rates.decodedFps;snapshot_.remoteRatesReady=rates.ready;snapshot_.remoteReceived=rates.received;snapshot_.remoteDecoded=rates.decoded;snapshot_.remoteIngressDropped=rates.ingressDropped;}
#endif
                // Save the latest processed real frame before a settings transaction
                // invalidates it (live rendering can be one batch behind processing).
                std::wstring save;EnhancementSettings requested;{std::lock_guard lock(mutex_);requested=desired_;if(hasOutput)save.swap(savePath_);}
                if(!save.empty()){try{sink::RgbaImage result;const auto e=std::filesystem::path(save).extension().wstring();
                    drainLivePresentation();
                    if(graph.hdrOutput()){auto hdrPath=std::filesystem::path(save);hdrPath.replace_extension(L".jxr");if(sink::saveHdrScreenshot(hdrPath.wstring(),ctx,ring,graph.videoFrameResource(out.videoSlot)))status(L"HDR截图已保存："+hdrPath.wstring());else status(L"HDR截图保存失败，请查看日志",false);}else if(!sink::readRgba8(ctx,ring,graph.videoFrameResource(out.videoSlot),result)||!sink::saveImage(save,result,e==L".jpg"||e==L".jpeg"))status(L"保存失败（目标文件可能已存在），播放已保留",false);else{status(L"图片已保存："+save);veyra::log::info("image-save",std::format("saved extent={}x{} revision={}",gd.workWidth,gd.workHeight,options.settings.revision));}}
                    catch(const std::exception& e){veyra::log::warn("image-save",std::format("save exception; retaining session: {}",e.what()));status(L"保存异常，播放已保留；可再次保存",false);}}
                if(isImage)requested.multiplier=1;
                if(requested.revision==options.settings.revision&&requested!=options.settings){
                    options.settings.audioSync=requested.audioSync;options.settings.audioOffsetMs=requested.audioOffsetMs;
                    captureSource.setAudioSync(unsigned(requested.audioSync),requested.audioOffsetMs);
                    std::lock_guard lock(mutex_);snapshot_.applied=options.snapshot();snapshot_.applying=desired_!=snapshot_.applied;
                    veyra::log::info("settings",std::format("Audio applied videoRevision={} mode={} offsetMs={} (video history retained)",requested.revision,unsigned(requested.audioSync),requested.audioOffsetMs));
                }
                const auto previous=options.snapshot();const auto previousDesc=gd;bool transaction=false;
                if(requested.revision!=previous.revision){
                    holdFileAudio();
                    finishReset(diagnostics::ResetOutcome::Cancelled);
                    resetStart=resetStageStart=Clock::now();
                    resetRecord=diagnostics::FrameFlowMetrics::ResetRecord{};
                    resetRecord->sessionId=runSessionId;resetRecord->settingsRevision=requested.revision;
                    resetRecord->reason=static_cast<uint8_t>(pipeline::ResetReason::Settings);
                    if(frameFlow)frameFlow->resetLifecycle(*resetRecord);
                    drainLivePresentation();
                    // Rebuilding the video graph must not tear down a healthy
                    // capture audio endpoint; only the new video mapping is
                    // invalidated here. Audio transport reset is reserved for
                    // an actual pause/stop or input discontinuity.
                    if(physicalCapture)captureSource.videoReset(false);
#ifdef VEYRA_ENABLE_REMOTEPLAY
                    if(remote)remote->videoReset(false);
#endif

                    // A drain may outlive several slider notifications. Build
                    // only the latest pending configuration at this boundary.
                    {std::lock_guard lock(mutex_);requested=desired_;}
                    if(requested.revision==previous.revision){finishReset(diagnostics::ResetOutcome::Cancelled);continue;}
                    resetRecord->settingsRevision=requested.revision;
                    auto next=PlayerOptions::from(requested);auto nextDesc=gd;
                    const auto plan=pipeline::ResolutionPlan::make({width,height},next.sr,requested.nrPolicy,isImage,requested.revision,requested.srTarget,requested.lowLatency&&requested.nr);
                    nextDesc.workWidth=plan.base.width;nextDesc.workHeight=plan.base.height;nextDesc.nrWidth=plan.nr.width;nextDesc.nrHeight=plan.nr.height;nextDesc.flowWidth=plan.flow.width;nextDesc.flowHeight=plan.flow.height;
                    const bool nvidiaAdapter=ctx.adapter().isNvidia;
                    const bool xessFg=next.settings.frameGenerationBackend==FrameGenerationBackend::XeSS;
                    nextDesc.nrBeforeSr=!isImage&&requested.lowLatency&&requested.nr&&plan.srApplied;nextDesc.enableSr=plan.srApplied&&nvidiaAdapter;nextDesc.videoSrQuality=next.settings.videoSrQuality;nextDesc.enableNr=next.nr&&nvidiaAdapter;nextDesc.nrRuntime=next.settings.nrRuntime;nextDesc.enableFg=next.fg&&(nvidiaAdapter||xessFg);nextDesc.fgMultiplier=next.fgMultiplier;nextDesc.frameGenerationBackend=next.settings.frameGenerationBackend;nextDesc.enableNvofStandalone=nextDesc.enableNr;
                    nextDesc.model=requested.model;nextDesc.residual=requested.residual;nextDesc.protection=requested.protection;nextDesc.settingsRevision=requested.revision;nextDesc.flowQuality=requested.flow;nextDesc.contentRate=requested.content;
                    nextDesc.opticalFlowBackend=requested.opticalFlowBackend;nextDesc.amdFlowHalfResolution=requested.amdFlowHalfResolution;
                    nextDesc.hdrOutput=requested.useHdrPreview(nextDesc.hdrInput,gfx::PresentSink::hdrDisplayActive(window));
                    const bool rebuild=(!nvidiaAdapter&&(next.nr||next.sr||(next.fg&&!xessFg)))||gd.hdrOutput!=nextDesc.hdrOutput||previous.captureCompatible!=requested.captureCompatible||gd.nrRuntime!=nextDesc.nrRuntime||gd.opticalFlowBackend!=nextDesc.opticalFlowBackend||gd.amdFlowHalfResolution!=nextDesc.amdFlowHalfResolution||gd.enableNr!=nextDesc.enableNr||gd.enableFg!=nextDesc.enableFg||gd.frameGenerationBackend!=nextDesc.frameGenerationBackend||gd.fgMultiplier!=nextDesc.fgMultiplier||gd.videoSrQuality!=nextDesc.videoSrQuality||gd.flowQuality!=nextDesc.flowQuality||gd.nrBeforeSr!=nextDesc.nrBeforeSr||gd.workWidth!=nextDesc.workWidth||gd.workHeight!=nextDesc.workHeight||gd.nrWidth!=nextDesc.nrWidth||gd.nrHeight!=nextDesc.nrHeight||gd.flowWidth!=nextDesc.flowWidth||gd.flowHeight!=nextDesc.flowHeight;
                    bool accepted=ring.drainQueue();out={};hasOutput=false;
                    resetRecord->rebuilt=rebuild;
                    markResetStage(diagnostics::ResetStage::Drain);
                    if(!accepted){finishReset(diagnostics::ResetOutcome::Failed);status(L"设置切换排空失败，已停止",true);break;}
                    backendRecoveryWarning.clear();
                    if(accepted&&rebuild){presenter.close();graph.shutdown();markResetStage(diagnostics::ResetStage::Destroy);accepted=initializePreview(nextDesc,next);
                        markResetStage(diagnostics::ResetStage::Create);
                        if(!accepted){presenter.close();graph.shutdown();if(!graph.initialize(gd)||!presenter.open(ctx,window,graph,options.settings.captureCompatible)||!graph.createViews()){status(L"设置失败且旧资源恢复失败，已停止",true);break;}}
                    }else if(accepted)accepted=graph.applySettings(requested);
                    if(accepted){
                        if(xessPresentationRecovery){backendRecoveryWarning=L"XeSS 呈现失败，已关闭补帧并恢复播放；错误码见日志";xessPresentationRecovery=false;}
                        if(!backendRecoveryWarning.empty()){
                            std::lock_guard lock(mutex_);desired_.rejectVideoRequest(requested,next.snapshot());snapshot_.desired=desired_;snapshot_.backendWarning=backendRecoveryWarning;
                        }
                        options=next;gd=nextDesc;transaction=true;reset=true;
                        resetRecord->epoch=0;
                        if(!nvidiaAdapter&&(next.nr||next.sr||(next.fg&&!xessFg)))veyra::log::warn("capability",std::format("non-NVIDIA adapter disabled requested settings revision={} nr={} sr={} fgBackend={} flowBackend={}",requested.revision,next.nr,next.sr,frameGenerationBackendName(requested.frameGenerationBackend),opticalFlowBackendName(requested.opticalFlowBackend)));
                    }
                    else {
                        finishReset(diagnostics::ResetOutcome::RolledBack);
                        std::lock_guard lock(mutex_);desired_.rejectVideoRequest(requested,previous);snapshot_.desired=desired_;snapshot_.rejectedRevision=requested.revision;snapshot_.applying=desired_!=previous;snapshot_.status=L"设置应用失败，已恢复上一套参数";snapshot_.backendWarning=requested.nrRuntime!=previous.nrRuntime?L"NR运行版本切换失败，已恢复上一套参数":requested.frameGenerationBackend==FrameGenerationBackend::XeSS?L"XeSS 未能启用，已恢复上一套参数":L"后端切换失败，已恢复上一套参数";}
                }
                if(stop_)break;
                double seek;{std::lock_guard lock(mutex_);seek=seekSeconds_.exchange(-1);if(seek>=0)activeSeekId=snapshot_.seekRequested;}
                if(seek>=0&&!isImage&&!isCapture){
                    seekStarted=Clock::now();seekDecoded=0;
                    holdFileAudio();fileAudioAlignPending=true;
                    drainLivePresentation();
                    if(!ring.drainQueue()||!activeSource->seek({static_cast<int64_t>(seek*1000000),1000000})){status(L"跳转失败",true);break;}
                    veyra::log::info("seek-latency",std::format("request={} targetSeconds={:.3f} drainAndDemuxMs={:.3f}",activeSeekId,seek,elapsedMs(seekStarted)));
                    initialFileFramePending=false;av_frame_free(&cachedFrame);out={};hasOutput=false;
                    discardBefore=seek*1000;seekPreviewPending=true;reset=true;pendingResetCause=pipeline::ResetReason::Seek;anchorMs=discardBefore;anchor=Clock::now();lastAudioClockMs=discardBefore;audioClockExhausted=false;audioRebuffering=false;if(audioStarted){audioPipe.setPaused(paused_);audioPipe.requestSeek(discardBefore);}
                }
                if(!transaction&&((paused_&&!seekPreviewPending)||(isImage&&hasOutput))){
                    drainLivePresentation();
                    if(!wasPaused){holdFileAudio();if(physicalCapture)captureSource.videoReset();
#ifdef VEYRA_ENABLE_REMOTEPLAY
                    if(remote)remote->videoReset();
#endif
}
                    if(audioStarted)audioPipe.setPaused(true);wasPaused=true;
                    const bool referencesValid=hasOutput&&out.batch.count&&out.batch.frames[out.batch.count-1].lease&&out.batch.frames[out.batch.count-1].lease->referencesValid;
                    if(hasOutput&&!presenter.present(ctx,ring,graph,out.videoSlot,false,referencesValid,comparisonMode_,comparisonBase_,comparisonSplit_,out.batch.identity,previewView())){if(recoverXessPresentation())continue;status(L"画面呈现失败",true);break;}
                    if(hasOutput&&graph.resolveGeneration(out))completeReset(out.batch.identity);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));continue;
                }
                if(wasPaused&&!paused_){holdFileAudio();audioRebuffering=false;if(audioStarted)audioPipe.setPaused(false);anchor=Clock::now();anchorMs=out.ptsMs;reset=true;pendingResetCause=pipeline::ResetReason::PauseResume;wasPaused=false;}
                // Backpressure before reading the capacity-one source mailbox:
                // when a lease frees we consume the newest available sample.
                if(liveScheduler&&!transaction){
                    const auto leaseWaitStart=Clock::now();
                    while((liveScheduler->occupancy()>=2||!graph.nextFrameSlotAvailable())&&!stop_&&(!paused_||seekPreviewPending)&&!liveScheduler->failed()){
                        advanceLive();if(seekSeconds_>=0)break;
                        if(liveScheduler->occupancy()==0&&!graph.nextFrameSlotAvailable()&&elapsedMs(leaseWaitStart)>2000){status(L"等待输出纹理释放超时",true);stop_=true;break;}
                        if(liveScheduler->occupancy()>=2||!graph.nextFrameSlotAvailable())waitLive();
                        std::lock_guard lock(mutex_);if(desired_.revision!=options.settings.revision)break;
                    }
                    if(stop_||seekSeconds_>=0||(paused_&&!seekPreviewPending)||liveScheduler->failed()||liveScheduler->occupancy()>=2||!graph.nextFrameSlotAvailable())continue;
                }
                const auto decodeStart=Clock::now();
                if(injectSourceGap&&frames>=12){
                    if(!sourceGapUntil){sourceGapUntil=Clock::now()+std::chrono::milliseconds(400);veyra::log::info("capture-test","inject 400ms Waiting before next source read");}
                    if(Clock::now()<*sourceGapUntil){advanceLive();waitLive();continue;}
                }
                pipeline::FramePacket pkt;const AVFrame* frame=imageFrame;
                if(cachedFrame&&((initialRemoteFramePending||initialFileFramePending)||(transaction&&(!isCapture||paused_)))){frame=cachedFrame;pkt=cachedPacket;initialRemoteFramePending=false;initialFileFramePending=false;}
                else if(!isImage){
                    auto rs=physicalCapture?captureSource.tryRead(pkt,&frame):activeSource->read(pkt,&frame);
                    // Keep the accepted transaction and rollback state alive until
                    // the next real capture sample arrives. Never bind old PTS to now.
                    while(transaction&&isCapture&&rs==source::SourceReadStatus::Waiting&&!stop_){
                        if(paused_&&cachedFrame){frame=cachedFrame;pkt=cachedPacket;rs=source::SourceReadStatus::Frame;break;}
                        advanceLive();waitLive();rs=physicalCapture?captureSource.tryRead(pkt,&frame):activeSource->read(pkt,&frame);
                    }
                    if(stop_)break;
                    if(rs==source::SourceReadStatus::Waiting){advanceLive();waitLive();continue;}
                    if(rs==source::SourceReadStatus::Eos){
                        fileInputEnded=true;
                        while(liveScheduler&&liveScheduler->occupancy()&&!stop_&&(!paused_||seekPreviewPending)&&!liveScheduler->failed()){advanceLive();if(liveScheduler->occupancy())waitLive();}
                        if(stop_)break;
                        if(liveScheduler&&liveScheduler->failed()){status(L"视频尾帧呈现失败，请查看诊断",true);break;}
                        if(paused_)continue;
                        collectTimings();seekPreviewPending=false;status(L"视频已播放完毕");paused_=true;{std::lock_guard lock(mutex_);snapshot_.transport=TransportState::Ended;}continue;
                    }
                    if(rs!=source::SourceReadStatus::Frame||pkt.pts.isUnknown()){
                        if(physicalCapture){
                            drainLivePresentation();if(!ring.drainQueue()){status(L"采集恢复时GPU排空失败",true);break;}
                            captureSource.videoReset();av_frame_free(&cachedFrame);hasOutput=false;out={};
                            captureRecovering=true;captureRetries=0;captureRetryAt=Clock::now()+std::chrono::milliseconds(500);
                            {std::lock_guard lock(mutex_);snapshot_.captureRecovering=true;snapshot_.captureFps=0;}
                            status(L"采集信号中断，等待原设备恢复");continue;
                        }
#ifdef VEYRA_ENABLE_REMOTEPLAY
                        if(remote){const auto recovery=remote->recoveryStatus();status(recovery.message.empty()?L"PS5 串流异常，请检查主机状态后重新连接。":recovery.message,true);break;}
#endif
                        status(isCapture?L"采集信号中断，请检查设备连接或格式":source.errorMessage().empty()?L"视频解码或时间戳错误":source.errorMessage(),true);break;}
                }
                if(isRemote&&frame&&(uint32_t(frame->width)!=width||uint32_t(frame->height)!=height)){
                    drainLivePresentation();if(!ring.drainQueue()){status(L"串流尺寸切换排空失败",true);break;}
                    width=uint32_t(frame->width);height=uint32_t(frame->height);
                    auto plan=pipeline::ResolutionPlan::make({width,height},options.sr,options.settings.nrPolicy,false,options.settings.revision,options.settings.srTarget,options.settings.lowLatency&&options.nr);
                    gd.nrBeforeSr=options.settings.lowLatency&&options.nr&&plan.srApplied;gd.enableSr=plan.srApplied&&ctx.adapter().isNvidia;
                    gd.sourceWidth=width;gd.sourceHeight=height;gd.hdrInput=!isImage&&activeSource->info().color.isHdrPath();gd.hdrOutput=options.settings.useHdrPreview(gd.hdrInput,gfx::PresentSink::hdrDisplayActive(window));gd.workWidth=plan.base.width;gd.workHeight=plan.base.height;gd.nrWidth=plan.nr.width;gd.nrHeight=plan.nr.height;gd.flowWidth=plan.flow.width;gd.flowHeight=plan.flow.height;
                    presenter.close();graph.shutdown();out={};hasOutput=false;
                    if(!graph.initialize(gd)||!presenter.open(ctx,window,graph,options.settings.captureCompatible)||!graph.createViews()){status(L"串流尺寸切换失败",true);break;}
                    reset=true;pendingResetCause=pipeline::ResetReason::Resize;
                }
                const bool rereadCached=transaction&&frame==cachedFrame;
                if(!isImage&&!isCapture&&seekSeconds_>=0)continue;
                const bool halfRate=isCapture&&options.settings.content==ContentRate::Capture60To30&&
                    CaptureHalfRate::supported(activeSource->info().averageFps);
                if(reset||pipeline::breaksHistory(pkt.flags))captureSampler.reset();
                if(halfRate&&!rereadCached){
                    const auto interval=liveSourceInterval100ns(pipeline::Rational::unknown(),activeSource->info().averageFps);
                    if(!pkt.pts.isUnknown()&&!captureSampler.accept(pkt.pts.to100ns(),interval,false)){
                        std::lock_guard lock(mutex_);++snapshot_.captureRateSkipped;continue;
                    }
                    pkt.duration={interval*2,10000000};
                }
                if(!halfRate)captureSampler.reset();
                if(isImage)pkt.sequence=1;
                if(!rereadCached)++sourceFrames;
                const double decodeMs=elapsedMs(decodeStart);decodeTimes.add(decodeMs);
                double pts=isImage?0:pkt.pts.toDouble()*1000;
                if(isCapture&&frames==0){anchor=Clock::now();anchorMs=pts;}
                if(seekPreviewPending)++seekDecoded;
                if(pts+0.1<discardBefore)continue;
                auto retainCandidate=[&](){
                    if(frame==cachedFrame)return true;
                    AVFrame* owned=av_frame_clone(frame);
                    if(!owned)return false;
                    av_frame_free(&cachedFrame);cachedFrame=owned;cachedPacket=pkt;frame=cachedFrame;
                    return true;
                };
                // Realtime preview candidates: decode stays ordered and
                // reference-lossless; an expired candidate loses only its
                // enhancement/display opportunity (plan §4.3). The open/seek
                // anchor, paused seek preview and settings replay never drop.
                if(!isCapture&&!isImage&&!transaction&&!fileAwaitingVideo&&!seekPreviewPending){
                    const double previewIntervalMs=double(liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps))/10000.0;
                    bool previewSourceFailed=false;
                    while(!stop_&&previewCandidateExpired(nowMs(),pts,previewIntervalMs)){
                        // read() can clear the decoder's borrowed AVFrame even
                        // on EOS/EAGAIN. Own this candidate before looking ahead.
                        if(!retainCandidate()){previewSourceFailed=true;break;}
                        pipeline::FramePacket next;const AVFrame* raw=nullptr;
                        const auto rs=activeSource->read(next,&raw);
                        if(rs==source::SourceReadStatus::Eos)break; // final decoded candidate is kept
                        if(rs!=source::SourceReadStatus::Frame||next.pts.isUnknown()){previewSourceFailed=true;break;}
                        const double nextPts=next.pts.toDouble()*1000;
                        if(nextPts+0.1<discardBefore)continue; // post-seek decode residue keeps dropping
                        pkt=next;frame=raw;
                        pts=nextPts;
                        ++previewSkippedTotal;++previewSkippedSinceSubmit;previewSkipSinceProcess=true;
                    }
                    if(previewSourceFailed){status(source.errorMessage().empty()?L"视频读取或候选帧保存失败":source.errorMessage(),true);break;}
                }
                if(!retainCandidate()){status(L"无法保存解码帧，已停止播放",true);break;}
                const auto processStart=Clock::now();
                const auto captureArrival=pkt.arrivalHost100ns?pkt.arrivalHost100ns:host100ns();
                // A PS5 callback is a compressed AU, not a decoded video frame.
                // Its decode baseline must not consume the entire FG lookahead
                // budget. Still measure full ingress latency from captureArrival.
                const auto liveInputReady=isRemote&&pkt.decodedHost100ns>0?pkt.decodedHost100ns:captureArrival;
                const auto sourceArrival=pkt.arrivalHost100ns?pkt.arrivalHost100ns:std::chrono::duration_cast<std::chrono::nanoseconds>(decodeStart.time_since_epoch()).count()/100;
                // A skipped preview candidate breaks the temporal span the
                // motion/NR/FG history was built on: the next processed frame
                // is an explicit history reset (plan §4.4). It is a SOFT
                // break: metrics windows, completion predictions and reset
                // lifecycle records continue (only hard resets — open/seek/
                // pause/discontinuity/settings — reopen them).
                const bool hardFrameReset=reset||pipeline::breaksHistory(pkt.flags&~static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Drop));
                const bool temporalBreak=pipeline::breaksHistory(pkt.flags)||previewSkipSinceProcess;
                const bool previewOnlyReset=!hardFrameReset&&temporalBreak;
                if(hardFrameReset)++metricsWindowEpoch;
                const bool historyReset=hardFrameReset||temporalBreak;
                if(historyReset){++historyResets;++presentationGeneration;}
                // A mailbox Drop must reset SR/NR/flow/FG history, but draining
                // the two-batch presenter here discarded completed real frames.
                if(liveScheduler&&presentationDrainRequired(reset,pkt.flags))drainLivePresentation();
                const bool injectedReject=transaction&&!options.nr&&GetEnvironmentVariableW(L"VEYRA_TEST_REJECT_NR_DISABLE",nullptr,0)>0;
                if(injectedReject)veyra::log::error("settings-test","test-only reject NR-disable transaction before graph process; no driver failure");
                pipeline::EnhanceGraph::FgAdmission admitFg;
                if(fgBudgetRevision!=options.settings.revision){fgBudget.reset();fgBudgetRevision=options.settings.revision;}
                // DLSS can reseed after a skipped pair. XeSS
                // owns generation inside its presenter and has no graph admission.
                if(isCapture&&!rereadCached&&useLiveFgAdmission&&options.settings.frameGenerationBackend!=FrameGenerationBackend::XeSS){
                    const auto presentP95=livePresent.p95();
                    admitFg=[&,presentP95](const pipeline::FrameBatch& batch){
                        // Physical capture and PS5 delivery clocks are not
                        // guaranteed to match the host clock. Anchor each new
                        // input pair before enhancement, so a 59.94-vs-60Hz
                        // mismatch cannot accumulate into a stale deadline.
                        // Never extend it based on processing/ready completion.
                        if(pairAnchoredLive)liveTimeline.resetPair(batch.identity.epoch,batch.b100ns,liveInputReady,liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps));
                        else if(!liveTimeline.anchored(batch.identity.epoch))liveTimeline.reset(batch.identity.epoch,batch.b100ns,liveInputReady,liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps));
                        const auto interval=liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps);
                        const auto a=(historyReset||batch.b100ns<=batch.a100ns||batch.b100ns-batch.a100ns>10000000)?batch.b100ns-interval:batch.a100ns;
                        const auto lastGenerated=pipeline::FrameBatch::interpolate(a,batch.b100ns,options.fgMultiplier-1,options.fgMultiplier);
                        const auto now=host100ns(),deadline=liveTimeline.deadline(lastGenerated);
                        const double elapsed=elapsedMs(processStart);
                        const bool admitted=fgBudget.admit(now,deadline,elapsed,presentP95);
                        if(now>=nextFgAdmissionLog){nextFgAdmissionLog=now+10000000;veyra::log::info("live-fg-admission",std::format("timeline={} admitted={} remainingDeadlineMs={:.3f} predictedMs={:.3f} elapsedMs={:.3f} decodedAgeMs={:.3f} callbackAgeMs={:.3f} presentP95Ms={:.3f}",pairAnchoredLive?(isRemote?"decoded-pair":"capture-pair"):"continuous",admitted,double(deadline-now)/10000,fgBudget.predicted(now).value_or(-1),elapsed,double(now-liveInputReady)/10000,double(now-captureArrival)/10000,presentP95));}
                        return admitted;
                    };
                }
                // File playback: same pair admission, but deadlines are PTS
                // deadlines on the audio master clock. A pair whose last
                // generated timestamp can no longer be reached spends no FG
                // Evaluate; the real frame keeps its normal cadence (plan §4.1).
                if(!isCapture&&!isImage&&!rereadCached&&!transaction&&options.fg&&options.settings.frameGenerationBackend!=FrameGenerationBackend::XeSS){
                    admitFg=[&,historyReset](const pipeline::FrameBatch& batch){
                        if(fileAwaitingVideo)return true; // Bounded startup lookahead; audio has not started.
                        const auto interval=liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps);
                        const auto a=(historyReset||batch.b100ns<=batch.a100ns||batch.b100ns-batch.a100ns>10000000)?batch.b100ns-interval:batch.a100ns;
                        const auto lastGenerated=pipeline::FrameBatch::interpolate(a,batch.b100ns,options.fgMultiplier-1,options.fgMultiplier);
                        const auto now=host100ns();
                        return fgBudget.admit(now,now+lastGenerated-int64_t(nowMs()*10000.0),elapsedMs(processStart),livePresent.p95());
                    };
                }
                uint64_t processWaitBase=0,processSubmitBase=0;double processWaitMsBase=0,processSlotWaitMs=0;
                if(resetRecord&&resetRecord->epoch==0)resetStageStart=Clock::now();
                // Bounded integration-test delay on the owner thread. It
                // exercises missed deadlines and recovery without changing
                // settings/history or inventing GPU execution timestamps.
                wchar_t testWork[16]{};
                if(!isImage&&GetEnvironmentVariableW(L"VEYRA_TEST_VIDEO_WORK_MS",testWork,16))std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(_wtoi(testWork),0,150)));
                bool processed=false;{processWaitBase=ring.cpuWaitCount();processWaitMsBase=ring.cpuWaitMilliseconds();processSubmitBase=ring.submitCount();processed=!injectedReject&&graph.process(frame,pts,historyReset,out,pkt.sequence,&pkt.colorInfo,comparisonMode_!=0,admitFg);processSlotWaitMs=ring.cpuWaitMilliseconds()-processWaitMsBase;}
                previewSkipSinceProcess=false;
                if(!processed){
                    const auto failedComponent=graph.failedBackend();
                    if(!transaction){
                        auto reduced=options.snapshot();
                        if(SUCCEEDED(ctx.device()->GetDeviceRemovedReason())&&disableFailedBackend(reduced,failedComponent)){
                            const auto attempted=options.snapshot();
                            drainLivePresentation();
                            if(!ring.drainQueue()||!ring.discardRecording()){status(L"增强故障恢复排空失败",true);break;}
                            out={};hasOutput=false;presenter.close();graph.shutdown();
                            {std::lock_guard lock(mutex_);reduced.revision=++nextRevision_;}
                            options=PlayerOptions::from(reduced);
                            const auto plan=pipeline::ResolutionPlan::make({width,height},options.sr,reduced.nrPolicy,isImage,reduced.revision,reduced.srTarget,reduced.lowLatency&&reduced.nr);
                            gd.workWidth=plan.base.width;gd.workHeight=plan.base.height;gd.nrWidth=plan.nr.width;gd.nrHeight=plan.nr.height;gd.flowWidth=plan.flow.width;gd.flowHeight=plan.flow.height;
                            gd.enableNr=options.nr&&nvidiaAdapter;gd.enableSr=plan.srApplied&&nvidiaAdapter;gd.enableFg=options.fg&&(nvidiaAdapter||reduced.frameGenerationBackend==FrameGenerationBackend::XeSS);gd.fgMultiplier=options.fgMultiplier;gd.enableNvofStandalone=gd.enableNr;gd.settingsRevision=reduced.revision;gd.nrBeforeSr=!isImage&&reduced.lowLatency&&gd.enableNr&&gd.enableSr;
                            if(!initializePreview(gd,options)){status(L"增强故障后的基础图重建失败",true);break;}
                            backendRecoveryWarning=std::wstring(backendFailureName(failedComponent))+L"运行失败，已关闭对应效果；错误码见日志"+(backendRecoveryWarning.empty()?L"":L"；"+backendRecoveryWarning);
                            {std::lock_guard lock(mutex_);desired_.rejectVideoRequest(attempted,options.snapshot());snapshot_.desired=desired_;snapshot_.applied=options.snapshot();snapshot_.applying=desired_!=snapshot_.applied;snapshot_.backendWarning=backendRecoveryWarning;}
                            veyra::log::warn("backend-recovery",std::format("runtime component={} disabled; rebuilt revision={} nr={} sr={} multiplier={}; retry next source frame",unsigned(failedComponent),options.settings.revision,options.nr,options.sr,options.snapshot().multiplier));
                            finishReset(diagnostics::ResetOutcome::Failed);reset=true;pendingResetCause=pipeline::ResetReason::Settings;
                            holdFileAudio();fileAudioAlignPending=true;
                            continue;
                        }
                    }
                    if(transaction){
                        ring.drainQueue();out={};presenter.close();graph.shutdown();options=PlayerOptions::from(previous);
                        gd=previousDesc;
                        if(graph.initialize(gd)&&presenter.open(ctx,window,graph,options.settings.captureCompatible)&&graph.createViews()&&graph.process(frame,pts,true,out,pkt.sequence,&pkt.colorInfo,comparisonMode_!=0)){
                            finishReset(diagnostics::ResetOutcome::RolledBack);
                            std::lock_guard lock(mutex_);desired_.rejectVideoRequest(requested,previous);snapshot_.desired=desired_;snapshot_.rejectedRevision=requested.revision;snapshot_.applying=desired_!=previous;snapshot_.status=L"参数执行失败，已整套回滚";transaction=false;
                        }else{status(L"参数回滚失败，已停止",true);break;}
                    }else{status(std::wstring(backendFailureName(failedComponent))+L"执行失败；请查看日志",true);break;}
                }
                if(transaction){std::lock_guard lock(mutex_);snapshot_.applied=options.snapshot();snapshot_.applying=desired_!=snapshot_.applied;snapshot_.status=std::format(L"输入 {}×{} / 底图 {}×{} / NR {}×{} / 光流 {}×{} / FG与输出 {}×{} | {}",width,height,gd.workWidth,gd.workHeight,gd.nrWidth,gd.nrHeight,gd.flowWidth,gd.flowHeight,gd.workWidth,gd.workHeight,gd.nrBeforeSr?L"低延迟 · NR先行后超分":gd.nrWidth<gd.workWidth?L"实时内部处理并回填":L"原生NR（性能成本较高）");veyra::log::info("display-color",std::format("hdrInput={} hdrOutput={} forceSdrPreview={}",gd.hdrInput,gd.hdrOutput,options.settings.forceSdrPreview));veyra::log::info("settings",std::format("Applied revision={} sourcePtsMs={} fgBackend={} flowBackend={} multiplier={} (source kept open)",options.settings.revision,pts,frameGenerationBackendName(options.settings.frameGenerationBackend),opticalFlowBackendName(options.settings.opticalFlowBackend),options.snapshot().multiplier));}
                if(veyra::log::verboseFrameLogs())veyra::log::info("source-identity",std::format("source={} totalRead={} graphProcessed={} cached={} revision={} nvofStandalone={}",pkt.sequence,sourceFrames,frames+1,rereadCached,options.settings.revision,gd.enableNvofStandalone));
                reset=false;hasOutput=true;
                const auto processDone=Clock::now();
                if(!resetRecord&&out.historyReset&&!previewOnlyReset){
                    resetStart=processStart;resetStageStart=processStart;
                    resetRecord=diagnostics::FrameFlowMetrics::ResetRecord{};resetRecord->sessionId=runSessionId;resetRecord->settingsRevision=out.batch.identity.settingsRevision;resetRecord->epoch=out.batch.identity.epoch;resetRecord->sourceFrameId=out.batch.identity.sourceFrameId;resetRecord->reason=static_cast<uint8_t>(diagnostics::resetCause(pkt.flags,pendingResetCause,out.detectedReset));
                    resetRecord->stageMs[size_t(diagnostics::ResetStage::Drain)]=0.0;resetRecord->stageMs[size_t(diagnostics::ResetStage::Destroy)]=0.0;resetRecord->stageMs[size_t(diagnostics::ResetStage::Create)]=0.0;
                    resetRecord->rebuilt=false;markResetStage(diagnostics::ResetStage::Warmup);
                    if(frameFlow)frameFlow->resetLifecycle(*resetRecord);
                }
                if(resetRecord&&resetRecord->epoch==0){
                    resetRecord->epoch=out.batch.identity.epoch;resetRecord->sourceFrameId=out.batch.identity.sourceFrameId;
                    markResetStage(diagnostics::ResetStage::Warmup);
                }
                pendingResetCause=pipeline::ResetReason::None;
                if(!frameFlow||metricsRevision!=options.settings.revision||metricsEpoch!=metricsWindowEpoch){
                    const bool settingsChanged=metricsRevision!=options.settings.revision;
                    metricsRevision=options.settings.revision;metricsEpoch=metricsWindowEpoch;statsStart=Clock::now();statsSourceBase=sourceFrames-uint64_t(!rereadCached);
                    slotWaitBase=processWaitBase;slotWaitMsBase=processWaitMsBase;submitBase=processSubmitBase;
                    captureAges.clear();scheduleWaits.clear();processTimes.clear();presentTimes.clear();decodeTimes.clear();gpuReadyTimes.clear();for(auto& window:gpuStageTimes)window.clear();lastGpuSampleEnd={};latenessSamples.clear();submissionTimes.clear();submitted=expired=0;
                    // Admission predictions and the XeSS gate must not carry
                    // samples across a settings revision (new backend/dimensions);
                    // soft preview-skip history breaks do NOT reopen them.
                    if(settingsChanged){xessGenerationGate.reset();presenter.setXessGenerationSuppressed(false);}
                    playbackSpeedLastWall={};
                    if(liveScheduler){liveStats={};liveStats.identity=out.batch.identity;liveSubmissions.clear();liveAges.clear();liveWaits.clear();livePresent.clear();liveReady.clear();}
                    if(settingsChanged||!completionRates)completionRates=std::make_shared<FrameCompletionRates>(host100ns());
                    frameFlow=std::shared_ptr<FrameFlowWindow>(new FrameFlowWindow(runSessionId,out.batch.identity,host100ns(),completionRates),[](FrameFlowWindow* window){logFrameFlow(window->snapshot(monotonic100ns()),"closed");delete window;});
                    if(resetRecord)frameFlow->resetLifecycle(*resetRecord);else if(lastResetRecord)frameFlow->resetLifecycle(*lastResetRecord);
                    frameFlow->update([&](auto& m){m.counters.settingsResets+=uint64_t(settingsChanged);});
                    captureFlowBase=captureFlowLast;
                    {std::lock_guard lock(mutex_);rateSkippedBase=snapshot_.captureRateSkipped;activeFlow_=frameFlow;}
                    veyra::log::info("metrics",std::format("reset session={} appliedRevision={} epoch={} sourceBase={}",runSessionId,metricsRevision,metricsEpoch,statsSourceBase));
                }
                const double processMs=std::chrono::duration<double,std::milli>(processDone-processStart).count();
                frameFlow->advanceHistory(out.batch.identity);
                frameFlow->update([&](auto& m){m.counters.historyResets+=uint64_t(out.historyReset);m.counters.captureDropResets+=uint64_t(pipeline::hasFrameFlag(pkt.flags,pipeline::FrameFlagBits::Drop));});
                const auto lineage=lineageTracker.observe(out.batch,sourceArrival,pkt.arrivalHost100ns>0,rereadCached||isImage);
                if(lineage&&out.hasGenerated){
                    frameFlow->pairArrived(*lineage);
                    if(veyra::log::verboseFrameLogs())veyra::log::info("frame-lineage",std::format("batch={} epoch={} revision={} sourceA={} sourceB={} ptsA={} ptsB={} arrivalA={} arrivalB={} captureCallbacks={}",out.batch.batchId,out.batch.identity.epoch,out.batch.identity.settingsRevision,lineage->a.identity.sourceFrameId,lineage->b.identity.sourceFrameId,lineage->a.pts100ns,lineage->b.pts100ns,lineage->a.host100ns,lineage->b.host100ns,lineage->a.captureCallback&&lineage->b.captureCallback));
                }
                processTimes.add(processMs);
                traceFrame(diagnostics::TraceKind::Submitted,out.batch.identity,out.batch.batchId,std::max(out.videoFenceValue,out.genFenceValue),out.batch.b100ns,out.fgSkippedBeforeEval,out.fgEvaluated,processMs);
                frameFlow->cpu(diagnostics::CpuStage::Decode,decodeMs,host100ns());
                if(isRemote&&pkt.decodedHost100ns>0&&!rereadCached)frameFlow->cpu(diagnostics::CpuStage::DecodedQueue,std::max(0.0,double(std::chrono::duration_cast<std::chrono::nanoseconds>(decodeStart.time_since_epoch()).count()/100-pkt.decodedHost100ns)/10000),host100ns());
                frameFlow->update([&](auto& m){m.lastSubmit100ns=host100ns();});
                frameFlow->cpu(diagnostics::CpuStage::Submit,processMs,host100ns());
                frameFlow->cpu(diagnostics::CpuStage::SlotWait,processSlotWaitMs,host100ns());
                frameFlow->update([&](auto& m){m.latest.frame=out.batch.identity;m.latest.batchId=out.batch.batchId;m.latest.readyFence=std::max(out.videoFenceValue,out.genFenceValue);m.counters.sourceAccepted+=!rereadCached;++m.counters.realSubmitted;m.counters.previewSkippedBeforeGraph+=previewSkippedSinceSubmit;m.counters.fgCandidate+=out.fgCandidates;m.counters.fgEvaluated+=out.fgEvaluated;m.counters.fgSkippedBeforeEval+=out.fgSkippedBeforeEval;if(out.fgSkippedBeforeEval)m.lastFgRejected100ns=host100ns();if(!out.hasGenerated)m.counters.fgWarmup+=out.fgEvaluated;});
                previewSkippedSinceSubmit=0;
                if(transaction||frames==0){
                    std::lock_guard lock(mutex_);snapshot_.captureHalfRate=halfRate;
                    veyra::log::info("capture-rate",std::format("revision={} requested60To30={} active={} transportFps={} originalPtsPreserved=true",options.settings.revision,options.settings.content==ContentRate::Capture60To30,halfRate,isCapture?activeSource->info().averageFps:0));
                }
                const bool pairPacing=pairAnchoredLive&&options.fg;
                if(isCapture&&!rereadCached&&(isRemote||pairPacing||!liveTimeline.anchored(out.batch.identity.epoch))){
                    const auto duration100ns=liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps);
                    // File replay has no device pacing and still needs its PTS
                    // clock. Physical capture without FG presents as soon as ready.
                    const bool paceSourcePts=options.fg||(!physicalCapture&&!isLan);
                    if(!liveTimeline.anchored(out.batch.identity.epoch)||(pairPacing&&frames==0))veyra::log::info("capture-timeline",std::format("interval100ns={} packetDurationKnown={} packetDurationPositive={} nominalFps={} FG={} pacing={}",duration100ns,!pkt.duration.isUnknown(),pkt.duration.num>0,activeSource->info().averageFps,options.fg,pairAnchoredLive?(isRemote?"decoded-pair":"capture-pair"):paceSourcePts?"source-pts":"capture-ready"));
                    if(pairPacing||isRemote)liveTimeline.resetPair(out.batch.identity.epoch,out.batch.b100ns,liveInputReady,options.fg?duration100ns:0,paceSourcePts);
                    else if(!liveTimeline.anchored(out.batch.identity.epoch))liveTimeline.reset(out.batch.identity.epoch,out.batch.b100ns,liveInputReady,options.fg?duration100ns:0,paceSourcePts);
                }
                if(!isImage&&!isCapture&&frames==0){anchor=Clock::now();anchorMs=lastAudioClockMs=pts;}
                double frameWaitMs=0,framePresentMs=0;
                double gpuWaitMs=0;
                if(liveScheduler){
                    const auto timeline=liveTimeline;
                    const uint64_t jobGeneration=presentationGeneration.load();
                    auto watch=std::make_shared<CompletionWatch>();watch->output=out;watch->flow=frameFlow;watch->processStart=processStart;watch->real=!rereadCached;
                    pendingCompletions.push_back(watch);
                    struct LiveStepState {
                        unsigned next=0,handled=0,remaining=0;bool readyReported=false;
                        Clock::time_point readyStart=Clock::now();std::optional<Clock::time_point> deadlineStart;
                        double readyMs=0,waitMs=0,presentMs=0,ageMs=0;uint64_t count=0,dropped=0;
                        diagnostics::GpuSample blit;
                    };
                    auto step=std::make_shared<LiveStepState>();
                    for(unsigned i=0;i<out.batch.count;++i)if(out.batch.frames[i].kind!=pipeline::FrameKind::Generated||out.batch.frames[i].validity==pipeline::GenerationValidity::Valid)++step->remaining;
                    const double sourceIntervalMs=double(liveSourceInterval100ns(pkt.duration,activeSource->info().averageFps))/10000;
                    const auto baselineReady=pkt.decodedHost100ns>0?pkt.decodedHost100ns:captureArrival;
                    const bool delayEnhanced=options.nr||options.sr||options.fg;
                    if(!liveScheduler->push([&,watch,step,timeline,captureArrival,baselineReady,delayEnhanced,activeSeekId,lineage,jobGeneration,rereadCached,sourceIntervalMs,flow=frameFlow](int64_t now)->LiveGpuScheduler::Step{
                        using State=LiveGpuScheduler::State;auto& batch=watch->output;auto& s=*step;
                        auto updatePending=[&](LiveStepState*){
                            unsigned left=0;for(unsigned i=s.next;i<batch.batch.count;++i)if(batch.batch.frames[i].kind!=pipeline::FrameKind::Generated||batch.batch.frames[i].validity==pipeline::GenerationValidity::Valid)++left;
                            flow->update([&](auto& m){if(left>s.remaining)m.pendingOutputFrames+=left-s.remaining;else m.pendingOutputFrames-=std::min(m.pendingOutputFrames,s.remaining-left);});s.remaining=left;
                        };std::unique_ptr<LiveStepState,decltype(updatePending)> pendingUpdate(&s,updatePending);
                        if(!isCapture&&!watch->ready){
                            if(elapsedMs(s.readyStart)>2000){veyra::log::error("capture-present","GPU ready timeout");return {State::Failed};}
                            return {State::Pending,now+2000};
                        }
                        if(watch->ready&&!s.readyReported){s.readyMs=std::max(0.0,std::chrono::duration<double,std::milli>(watch->readyObserved-s.readyStart).count());s.readyReported=true;flow->cpu(diagnostics::CpuStage::ReadyWait,s.readyMs,host100ns());}
                        if(!isCapture&&fileAwaitingVideo&&!seekPreviewPending&&!fileInputEnded&&
                           (liveScheduler->occupancy()<2||!pendingCompletions.empty()))return {State::Pending,now+2000};
                        if(!isCapture&&audioStarted&&fileAudioAlignPending){
                            if(audioPipe.endpointRecovering())return {State::Pending,now+10000};
                            const double ptsMs=double(batch.batch.b100ns)/10000,media=audio.mediaTimeMs();
                            if(std::isfinite(media)&&media+1<ptsMs&&!std::isfinite(audioPipe.requestSeek(ptsMs)))return {State::Failed};
                            fileAudioAlignPending=false;
                        }
                        while(s.next<batch.batch.count){auto& item=batch.batch.frames[s.next];const bool generated=item.kind==pipeline::FrameKind::Generated;
                            if(stop_||(paused_&&!seekPreviewPending)||seekSeconds_>=0)return {State::Complete};
                            if(isCapture&&!graph.resolveFrame(batch,s.next)){
                                if(elapsedMs(s.readyStart)>2000){veyra::log::error("capture-present","GPU frame ready timeout");return {State::Failed};}
                                return {State::Pending,host100ns()+2000};
                            }
                            if(isCapture&&!s.readyReported){s.readyMs=elapsedMs(s.readyStart);s.readyReported=true;flow->cpu(diagnostics::CpuStage::ReadyWait,s.readyMs,host100ns());}
                            if(generated&&item.validity!=pipeline::GenerationValidity::Valid){++s.handled;++s.next;continue;}
                            if(generated&&(comparisonMode_!=0||!generatedPresentationCurrent(jobGeneration,presentationGeneration.load()))){++presentationSkippedGenerated;++s.next;continue;}
                            const double itemPtsMs=double(item.pts100ns)/10000;
                            const auto decisionTime=host100ns();
                            const bool generationExpired=generated&&(isCapture?timeline.expired(item.pts100ns,decisionTime,100000):!fileAwaitingVideo&&previewGeneratedExpired(nowMs(),itemPtsMs));
                            if(generated&&isCapture&&decisionTime>=nextFgDeadlineLog){
                                nextFgDeadlineLog=decisionTime+10000000;
                                const auto ready=watch->frameReadyObserved[s.next];
                                veyra::log::info("fg-deadline",std::format("revision={} batch={} subframe={} expired={} readyObservedLateMs={:.3f} decisionLateMs={:.3f} observedReadyToDecisionMs={:.3f} batchReady={} (CPU fence observations, not scanout)",item.identity.settingsRevision,batch.batch.batchId,item.subframe,generationExpired,double(ready-timeline.deadline(item.pts100ns))/10000,double(decisionTime-timeline.deadline(item.pts100ns))/10000,ready?double(decisionTime-ready)/10000:-1,watch->ready.load()));
                            }
                            if(generationExpired){++s.dropped;++s.handled;++s.next;s.deadlineStart.reset();continue;}
                            if(!s.deadlineStart)s.deadlineStart=Clock::now();
                            if(isCapture){if(host100ns()<timeline.deadline(item.pts100ns))return {State::Pending,timeline.deadline(item.pts100ns)};}
                            else if(!fileAwaitingVideo){
                                if(audioStarted)audioPipe.videoReady(itemPtsMs);
                                if(nowMs()+.25<itemPtsMs)return {State::Pending,now+std::min<int64_t>(10000,int64_t((itemPtsMs-nowMs())*10000))};
                            }
                            const auto waited=elapsedMs(*s.deadlineStart);s.deadlineStart.reset();s.waitMs+=waited;flow->cpu(diagnostics::CpuStage::DeadlineWait,waited,host100ns());
                            const auto begin=Clock::now();const auto before=presenter.submittedCount();
                            const auto beforeXess=presenter.xessGeneratedCount(),beforeXessPresented=presenter.xessPresentedCount();
                            if(!presenter.present(ctx,ring,graph,item.lease->slot,generated,item.lease->referencesValid,comparisonMode_,comparisonBase_,comparisonSplit_,item.identity,previewView()))return {State::Failed};
                            const auto xessGenerated=presenter.xessGeneratedCount()-beforeXess,xessPresented=presenter.xessPresentedCount()-beforeXessPresented;
                            item.lease->consumerFence=ring.lastSignaledValue();const bool didPresent=presenter.submittedCount()>before;s.blit=presenter.blitTiming(ctx.fence());
                            const auto elapsed=elapsedMs(begin);s.presentMs+=elapsed;flow->cpu(diagnostics::CpuStage::Present,elapsed,host100ns());
                            if(didPresent){
                                traceFrame(diagnostics::TraceKind::Present,item.identity,batch.batch.batchId,item.lease->consumerFence,item.pts100ns,item.subframe,1,elapsed);
                                if(veyra::log::verboseFrameLogs())veyra::log::info("submit",std::format("batch={} epoch={} revision={} subframe={} pts100ns={} host100ns={} fence={} (submission, display unmeasured)",batch.batch.batchId,item.identity.epoch,item.identity.settingsRevision,item.subframe,item.pts100ns,host100ns(),item.lease->consumerFence));
                            }
                            if(xessPresented)flow->xessSubmitted(xessPresented,xessGenerated,host100ns());
                            if(didPresent&&!isCapture){
                                lastFilePresentedMs=itemPtsMs;lastFilePresentLateness=fileAwaitingVideo?0:nowMs()-itemPtsMs;
                                {std::lock_guard lock(mutex_);if(snapshot_.sessionId==runSessionId&&snapshot_.applied.revision==item.identity.settingsRevision){
                                    if(activeSeekId&&snapshot_.seekPresented!=activeSeekId)veyra::log::info("seek-latency",std::format("request={} firstPresentMs={:.3f} decodedToTarget={} ptsMs={:.3f}",activeSeekId,elapsedMs(seekStarted),seekDecoded,itemPtsMs));
                                    snapshot_.position=itemPtsMs/1000;snapshot_.lateMs=lastFilePresentLateness;snapshot_.seekPresented=activeSeekId;}}
                                double nextPts=itemPtsMs+sourceIntervalMs;
                                for(unsigned next=s.next+1;next<batch.batch.count;++next){const auto& candidate=batch.batch.frames[next];if(candidate.kind!=pipeline::FrameKind::Generated||(candidate.validity==pipeline::GenerationValidity::Valid&&comparisonMode_==0)){nextPts=double(candidate.pts100ns)/10000;break;}}
                                if(audioStarted){audioPipe.videoPresented(nextPts);audioPipe.setPaused(paused_);}
                                if(fileAwaitingVideo)veyra::log::info("audio-sync",std::format("file lookahead anchor ptsMs={:.3f} queued={} (continuous audio after bounded prefill)",itemPtsMs,liveScheduler->occupancy()));
                                fileAwaitingVideo=false;anchor=Clock::now();anchorMs=itemPtsMs;seekPreviewPending=false;
                                if(graph.xessEnabled()&&presenter.xessActive()&&xessGenerationGate.observe(nowMs()-itemPtsMs,sourceIntervalMs)){
                                    presenter.setXessGenerationSuppressed(xessGenerationGate.suppressed());
                                    veyra::log::info("xess-fg-gate",std::format("event={} revision={} lateMs={:.3f}",xessGenerationGate.suppressed()?"suppress":"resume",item.identity.settingsRevision,nowMs()-itemPtsMs));
                                }
                            }
                            // Real B owns captureArrival. Generated A/B frames and
                            // paused cached frames must not invent input anchors.
                            if(didPresent&&physicalCapture&&!generated&&!rereadCached)captureSource.videoPresented(double(item.pts100ns)/10000,host100ns(),captureArrival);
#ifdef VEYRA_ENABLE_REMOTEPLAY
                            if(didPresent&&remote)remote->videoPresented(double(item.pts100ns)/10000,host100ns());
#endif
                            if(didPresent&&!generated&&!rereadCached)flow->latency(captureArrival,host100ns());
                            if(didPresent&&!generated&&!rereadCached&&!paused_){
                                const auto stamp=host100ns();const auto stats=flow->snapshot(stamp);
                                const auto color=stats.gpuTiming[size_t(diagnostics::GpuStage::Color)].mean;
                                const auto blit=stats.gpuTiming[size_t(diagnostics::GpuStage::Blit)].mean;
                                std::optional<double> basic;
                                if(color&&blit)basic=*color+*blit+elapsed;
                                const auto estimate=enhancementDelayEstimate(delayEnhanced,isCapture,
                                    isCapture?double(stamp-baselineReady)/10000:lastFilePresentLateness,basic);
                                if(estimate)flow->cpu(diagnostics::CpuStage::EnhancementDelayEstimate,*estimate,stamp);
                            }
                            if(didPresent&&generated&&lineage)flow->generatedLatency(*lineage,host100ns());
                            if(didPresent){++s.handled;++s.count;flow->presented(generated,item.lease->consumerFence,host100ns());if(!generated)++presentationCompletedReal;s.ageMs=double(host100ns()-captureArrival)/10000;
                                if(liveStats.identity.epoch==batch.batch.identity.epoch&&liveStats.identity.settingsRevision==batch.batch.identity.settingsRevision){const auto time=host100ns();liveSubmissions.push_back(time);while(liveSubmissions.size()>1&&time-liveSubmissions.front()>10000000)liveSubmissions.pop_front();}}
                            ++s.next;
                        }
                        return {State::Complete};
                    },[&,watch,step,flow=frameFlow]{
                        const auto& batch=watch->output;const auto& s=*step;
                        flow->update([&](auto& m){m.pendingOutputFrames-=std::min(m.pendingOutputFrames,s.remaining);});
                        if(s.handled<batch.batch.count)++presentationCancelledJobs;
                        if(s.handled<batch.batch.count)traceFrame(diagnostics::TraceKind::Cancelled,batch.batch.identity,batch.batch.batchId,0,batch.batch.b100ns,0,batch.batch.count-s.handled,0);
                        flow->update([&](auto& m){m.counters.cancelledBeforePresent+=batch.batch.count-s.handled;m.gpuReadyWaitMs=s.readyMs;m.deadlineWaitMs=s.waitMs;if(s.count)m.captureArrivalToPresentReturnMs=s.ageMs;m.counters.generatedExpiredAfterEval+=s.dropped;});
                        if(liveStats.identity.epoch!=batch.batch.identity.epoch||liveStats.identity.settingsRevision!=batch.batch.identity.settingsRevision)return;
                        liveStats.submitted+=s.count;liveStats.expired+=s.dropped;
                        if(s.count){liveStats.waitMs=s.waitMs;liveStats.presentMs=s.presentMs;liveStats.readyMs=s.readyMs;liveStats.ageMs=s.ageMs;liveStats.blit=s.blit;
                            liveAges.add(s.ageMs);liveWaits.add(s.waitMs);livePresent.add(s.presentMs);liveReady.add(s.readyMs);liveStats.ageP95=liveAges.p95();liveStats.waitP95=liveWaits.p95();liveStats.presentP95=livePresent.p95();liveStats.readyP95=liveReady.p95();}
                        liveStats.fps=liveSubmissions.size()>1?double(liveSubmissions.size()-1)*1e7/(liveSubmissions.back()-liveSubmissions.front()):0;
                    })){status(L"视频呈现队列失败",true);break;}
                    frameFlow->update([&](auto& m){m.pendingOutputFrames+=step->remaining;});
                    advanceLive();
                    const auto occupancy=liveScheduler->occupancy();frameFlow->update([&](auto& m){m.counters.presentationBatchHighWater=std::max(m.counters.presentationBatchHighWater,occupancy);});
                    const auto completed=liveStats;
                    frameWaitMs=completed.waitMs;framePresentMs=completed.presentMs;gpuWaitMs=completed.readyMs;submitted=completed.submitted;expired=completed.expired;
                }else{
                    // Images have no media clock, audio or temporal batches.
                    const auto readyStart=Clock::now();
                    while(!stop_&&!graph.resolveGeneration(out)){
                        if(elapsedMs(readyStart)>2000){status(L"图片 GPU 就绪超时",true);stop_=true;break;}
                        deadlineWait.slice(.2);
                    }
                    if(stop_)break;
                    gpuWaitMs=elapsedMs(readyStart);gpuReadyTimes.add(gpuWaitMs);
                    frameFlow->cpu(diagnostics::CpuStage::ReadyWait,gpuWaitMs,host100ns());
                    frameFlow->ready(out.batch.batchId,false,0,0,host100ns());completeReset(out.batch.identity);
                    bool presentFailed=false;
                    for(auto& item:out.batch.frames){
                        if(!item.lease)continue;
                        const auto begin=Clock::now();const auto before=presenter.submittedCount();
                        if(!presenter.present(ctx,ring,graph,item.lease->slot,false,item.lease->referencesValid,comparisonMode_,comparisonBase_,comparisonSplit_,item.identity,previewView())){presentFailed=true;break;}
                        item.lease->consumerFence=ring.lastSignaledValue();framePresentMs+=elapsedMs(begin);
                        frameFlow->cpu(diagnostics::CpuStage::Present,elapsedMs(begin),host100ns());
                        if(presenter.submittedCount()>before){++submitted;frameFlow->presented(false,item.lease->consumerFence,host100ns());}
                    }
                    if(presentFailed){status(L"图片呈现失败",true);break;}
                }
                if(!liveScheduler){scheduleWaits.add(frameWaitMs);presentTimes.add(framePresentMs);}
                audioRebuffering=audioStarted&&audioPipe.waitingForVideo();
                const double lateness=isCapture?0:isImage?nowMs()-pts:lastFilePresentLateness;
                if(!paused_&&!isCapture){latenessSamples.push_back(std::abs(lateness));if(latenessSamples.size()>1200)latenessSamples.pop_front();}
                std::vector<double> sorted(latenessSamples.begin(),latenessSamples.end());std::sort(sorted.begin(),sorted.end());
                auto captureStats=physicalCapture?captureSource.metrics():source::CaptureMetrics{};
                if(isLan){const auto lan=lanSource.metrics();captureStats.received=lan.received;captureStats.dropped=lan.dropped;captureStats.delivered=lan.delivered;captureStats.callbackFps=lan.inputFps;captureStats.readAgeMs=lan.readAgeMs;}
#ifdef VEYRA_ENABLE_REMOTEPLAY
                if(remote){const auto rp=remote->sessionSnapshot();captureStats.received=rp.video.accessUnits;captureStats.dropped=remote->skipped();captureStats.delivered=sourceFrames;
                    captureStats.readAgeMs=double(std::max<int64_t>(0,host100ns()-pkt.arrivalHost100ns))/10000;
                }
#endif
                diagnostics::FrameMetrics measured;pipeline::EnhanceGraph::Metrics graphStats;uint64_t slotWaitCount=0,commandSubmits=0;double slotWaitMilliseconds=0;uint32_t slotsInFlight=0;
                {measured=graph.gpuMetrics();graphStats=graph.metrics();measured.gpu[size_t(diagnostics::GpuStage::Blit)]=presenter.blitTiming(ctx.fence(),options.settings.revision,out.batch.identity.epoch);slotWaitCount=ring.cpuWaitCount()-slotWaitBase;slotWaitMilliseconds=ring.cpuWaitMilliseconds()-slotWaitMsBase;commandSubmits=ring.submitCount()-submitBase;slotsInFlight=ring.inFlightCount();
                    collectTimings();
                }
                if(measured.identity.settingsRevision!=options.settings.revision||measured.identity.epoch!=out.batch.identity.epoch){measured={};measured.identity=out.batch.identity;for(auto& sample:measured.gpu)sample.state=diagnostics::SampleState::Pending;}
                if(graph.xessEnabled()){
                    graphStats.fgGeneratedFrames=presenter.xessGeneratedCount();
                }
                for(size_t stage=0;stage<measured.gpu.size();++stage){const auto& sample=measured.gpu[stage];if(sample.state==diagnostics::SampleState::Measured&&sample.milliseconds&&sample.end&&sample.end!=lastGpuSampleEnd[stage]){gpuStageTimes[stage].add(*sample.milliseconds);lastGpuSampleEnd[stage]=sample.end;}}
                measured.resolution=pipeline::ResolutionPlan::make({width,height},options.sr,options.snapshot().nrPolicy,isImage,options.settings.revision,options.settings.srTarget,options.settings.lowLatency&&options.nr);measured.decodeCpuMs=decodeMs;measured.submitCpuMs=std::chrono::duration<double,std::milli>(processDone-processStart).count();measured.gpuWaitCpuMs=gpuWaitMs;measured.deadlineWaitCpuMs=frameWaitMs;measured.presentCpuMs=framePresentMs;measured.queueWatermark=out.batch.count;
                LiveStats completed;if(liveScheduler)completed=liveStats;
                uint64_t skipped=0;{std::lock_guard lock(mutex_);skipped=snapshot_.captureRateSkipped-rateSkippedBase;}
                frameFlow->update([&](auto& m){m.counters.captureReceived=captureStats.received-captureFlowBase.received;m.counters.mailboxOverwritten=captureStats.dropped-captureFlowBase.dropped;m.counters.sourceSkippedBeforeGraph=skipped;m.slotReuseWaitCount=slotWaitCount;m.slotReuseWaitMs=slotWaitMilliseconds;m.counters.commandSlotsInFlight=slotsInFlight;m.counters.commandSlotHighWater=std::max(m.counters.commandSlotHighWater,slotsInFlight);});
                frameFlow->update([&](auto& m){m.slotWaitPerFrameMs=processSlotWaitMs;});
                captureFlowLast=captureStats;
                measured.flow=frameFlow->snapshot(host100ns());
                measured.sourceFrames=measured.flow.counters.sourceAccepted;measured.validGenerated=measured.flow.counters.fgReadyValid;measured.submitted=measured.flow.counters.realPresented+measured.flow.counters.generatedPresented;measured.expired=measured.flow.counters.generatedExpiredAfterEval;
                const double ageP95=liveScheduler?completed.ageP95:captureAges.p95(),waitP95=liveScheduler?completed.waitP95:scheduleWaits.p95(),presentP95=liveScheduler?completed.presentP95:presentTimes.p95();
                ++frames;{std::lock_guard lock(mutex_);snapshot_.metrics=measured;snapshot_.colorStatus=gd.hdrOutput?(graph.hdr10Output()?L"HDR → HDR10 / PQ":L"HDR → scRGB / 浮点"):gd.hdrInput?L"HDR → SDR色调映射":L"SDR → SDR";snapshot_.position=(isCapture||isImage?pts:lastFilePresentedMs)/1000;snapshot_.frames=sourceFrames;snapshot_.generated=graphStats.fgGeneratedFrames;snapshot_.lateMs=lateness;snapshot_.lateP95Ms=sorted.empty()?0:sorted[size_t((sorted.size()-1)*0.95)];
                    // Measured playback speed: media-PTS advance per wall time
                    // over ~1s windows (1.0 = normal speed), resampled on seek.
                    const auto speedNow=Clock::now();
                    if(playbackSpeedLastWall==Clock::time_point()||pts+0.5<playbackSpeedLastPts){playbackSpeedLastPts=pts;playbackSpeedLastWall=speedNow;}
                    else if(std::chrono::duration<double>(speedNow-playbackSpeedLastWall).count()>=0.9){
                        const double wallS=std::chrono::duration<double>(speedNow-playbackSpeedLastWall).count();
                        const double mediaS=(pts-playbackSpeedLastPts)/1000.0;
                        if(mediaS>=0)snapshot_.playbackSpeed=mediaS/wallS;
                        playbackSpeedLastPts=pts;playbackSpeedLastWall=speedNow;
                    }
                    snapshot_.previewSkipped=measured.flow.counters.previewSkippedBeforeGraph;
                    snapshot_.fgBudgetLimited=measured.flow.lastFgRejected100ns>0&&host100ns()-measured.flow.lastFgRejected100ns<10000000;
                    snapshot_.xessGenerationSuppressed=graph.xessEnabled()&&presenter.xessGenerationSuppressed();
                    snapshot_.nrActive=graph.nrEnabled()&&graphStats.nrEvaluateCount>0;
                    snapshot_.audioRebuffering=audioRebuffering;
                    snapshot_.audioVideoWaits=audioPipe.videoWaitCount();
                    snapshot_.srActive=gd.enableSr&&graphStats.srEvaluateCount>0;
                    snapshot_.fgActive=options.fg&&(graph.xessEnabled()?presenter.xessActive()&&graphStats.fgGeneratedFrames>0:graph.fgEnabled()&&measured.flow.counters.fgReadyValid>0);
                    if(transaction&&backendRecoveryWarning.empty()&&nvidiaAdapter&&(!graph.xessEnabled()||presenter.xessActive()))snapshot_.backendWarning.clear();
                    snapshot_.flowPerf=graph.actualFlowPerf();snapshot_.contentFps=out.measuredContentRate;
                    snapshot_.nrEvaluated=graphStats.nrEvaluateCount;snapshot_.nvofExecuted=graphStats.nvofExecuteCount;
                    snapshot_.captureReceived=captureStats.received;snapshot_.captureDropped=captureStats.dropped;snapshot_.captureFps=captureStats.callbackFps;snapshot_.captureReadAgeMs=captureStats.readAgeMs;snapshot_.captureAgeMs=liveScheduler?completed.ageMs:captureStats.frameAgeMs;snapshot_.captureAgeP95Ms=ageP95;
                    snapshot_.schedulingWaitP95Ms=waitP95;snapshot_.processCpuP95Ms=processTimes.p95();snapshot_.presentCpuP95Ms=presentP95;
                }
                const auto gpuP95=[&](diagnostics::GpuStage stage){return gpuStageTimes[size_t(stage)].p95();};
                if(Clock::now()>=nextTimingLog){const auto [retained,overwritten]=Logger::instance().frameTraceSize();
                    veyra::log::info("frame-trace",std::format("retained={} capacity=8192 overwritten={} (records available in diagnostic preview)",retained,overwritten));}
                if(!isCapture&&Clock::now()>=nextTimingLog){const double playbackSpeedNow=[&]{std::lock_guard lock(mutex_);return snapshot_.playbackSpeed;}();
                {const auto dashboard=frameFlow->snapshot(host100ns());veyra::log::info("output-queue",std::format("pendingFrames={} enhancementProcessingMs={:.3f} extraDelayEstimateMs={:.3f}",dashboard.pendingOutputFrames,dashboard.enhancementProcessing.mean.value_or(-1),dashboard.cpuTiming[size_t(diagnostics::CpuStage::EnhancementDelayEstimate)].mean.value_or(-1)));}
                veyra::log::info("player-timing",std::format("revision={} decodeP95Ms={:.3f} graphSubmitP95Ms={:.3f} gpuReadyP95Ms={:.3f} presentP95Ms={:.3f} gpuColorP95Ms={:.3f} gpuSrP95Ms={:.3f} gpuFlowP95Ms={:.3f} gpuNrP95Ms={:.3f} gpuResidualP95Ms={:.3f} gpuFgBatchP95Ms={:.3f} gpuBlitP95Ms={:.3f} slotWaits={} slotWaitMs={:.3f} commandSubmits={} displaySubmits={} expiredGenerated={} previewSkipped={} playbackSpeed={:.3f} processed={}",options.settings.revision,decodeTimes.p95(),processTimes.p95(),liveScheduler?completed.readyP95:gpuReadyTimes.p95(),presentP95,gpuP95(diagnostics::GpuStage::Color),gpuP95(diagnostics::GpuStage::Sr),gpuP95(diagnostics::GpuStage::Flow),gpuP95(diagnostics::GpuStage::Nr),gpuP95(diagnostics::GpuStage::Residual),gpuP95(diagnostics::GpuStage::FgBatch),gpuP95(diagnostics::GpuStage::Blit),slotWaitCount,slotWaitMilliseconds,commandSubmits,submitted,expired,measured.flow.counters.previewSkippedBeforeGraph,playbackSpeedNow,sourceFrames-statsSourceBase));nextTimingLog=Clock::now()+std::chrono::seconds(1);}
                if(isCapture&&Clock::now()>=nextTimingLog){
                    logFrameFlow(measured.flow,"active");
                    const auto rates=snapshot();
                    veyra::log::info("frame-rate",std::format("revision={} gpuCompletedFps={:.2f} completedReal={} capture60To30={} rateSkipped={} presentSubmitFps={:.2f} windowMs=1000 (not scanout FPS)",options.settings.revision,rates.fps,rates.processedCompleted,rates.captureHalfRate,rates.captureRateSkipped,rates.submissionFps.value_or(0)));
                    veyra::log::info("capture-timing",std::format("revision={} received={} processed={} dropped={} callbackFps={:.2f} readAgeMs={:.3f} callbackToPresentReturnP95Ms={:.3f} processCpuP95Ms={:.3f} gpuReadyP95Ms={:.3f} schedulingWaitP95Ms={:.3f} presentCpuP95Ms={:.3f} gpuColorP95Ms={:.3f} gpuSrP95Ms={:.3f} gpuFlowP95Ms={:.3f} gpuNrP95Ms={:.3f} gpuResidualP95Ms={:.3f} gpuFgBatchP95Ms={:.3f} gpuBlitP95Ms={:.3f} slotWaits={} slotWaitMs={:.3f} commandSubmits={} displaySubmits={} expiredGenerated={} nr={} nvof={} generated={} historyResets={} presentationDrains={} presentationCompletedReal={} presentationSkippedGenerated={} presentationCancelledJobs={} singleGpuOwner=1 batchCapacity=2 (not HDMI-to-display latency)",options.settings.revision,captureStats.received,sourceFrames-statsSourceBase,captureStats.dropped,captureStats.callbackFps,captureStats.readAgeMs,ageP95,processTimes.p95(),liveScheduler?completed.readyP95:gpuReadyTimes.p95(),waitP95,presentP95,gpuP95(diagnostics::GpuStage::Color),gpuP95(diagnostics::GpuStage::Sr),gpuP95(diagnostics::GpuStage::Flow),gpuP95(diagnostics::GpuStage::Nr),gpuP95(diagnostics::GpuStage::Residual),gpuP95(diagnostics::GpuStage::FgBatch),gpuP95(diagnostics::GpuStage::Blit),slotWaitCount,slotWaitMilliseconds,commandSubmits,submitted,expired,graphStats.nrEvaluateCount,graphStats.nvofExecuteCount,graphStats.fgGeneratedFrames,historyResets.load(),presentationDrains.load(),presentationCompletedReal.load(),presentationSkippedGenerated.load(),presentationCancelledJobs.load()));
                    nextTimingLog=Clock::now()+std::chrono::seconds(1);
                }
            }
        }while(false);
    }catch(const std::exception& e){veyra::log::error("engine",e.what());status(L"引擎异常，请查看诊断",true);failed=true;}
    (void)failed;
#ifdef VEYRA_ENABLE_REMOTEPLAY
    {std::lock_guard lock(mutex_);activeRemote_.reset();}
    if(remote)remote->close();
#endif
    audioPipe.stopThread();audio.shutdown();ring.drainQueue();presenter.close();graph.shutdown();captureSource.close();lanSource.close();source.close();av_frame_free(&cachedFrame);av_frame_free(&imageFrame);ring.shutdown();ctx.shutdown();
    {std::lock_guard lock(mutex_);snapshot_.running=false;snapshot_.audioEndpointRecovering=false;snapshot_.audioRebuffering=false;}
    CoUninitialize();
}
}
