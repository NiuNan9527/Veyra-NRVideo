#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <functional>
#include "veyra/engine/EnhancementSettings.h"
#include "veyra/diagnostics/FrameMetrics.h"
#include "veyra/engine/PreviewView.h"
#include "veyra/sink/CaptureAudioSession.h"
#include "veyra/remoteplay/SessionInbox.h"
namespace veyra::source { struct RemotePlayConnectDesc; class RemotePlaySessionSource; }
namespace veyra::remoteplay { struct ControllerState;struct ControllerFeedback; }
namespace veyra::sink { struct RgbaImage; }
namespace veyra::gfx { class D3D12DeviceContext; class CommandSlotRing; }
namespace veyra::engine {
class FrameFlowWindow;
struct PlayerOptions { bool nr=false,sr=false,fg=false,realtime=true; uint32_t fgMultiplier=2; EnhancementSettings settings;
    bool captureReplayForTest=false; // file-backed live scheduler test; never enabled by UI
    bool captureReplayDisableFgAdmissionForTest=false; // controlled scheduler A/B only
    EnhancementSettings snapshot()const{auto s=settings;s.nr=nr;s.sr=sr;s.multiplier=fg?fgMultiplier:1;s.nrPolicy=realtime?(settings.nrPolicy==pipeline::NrSizePolicy::Native?pipeline::NrSizePolicy::Realtime:settings.nrPolicy):pipeline::NrSizePolicy::Native;return s;}
    static PlayerOptions from(EnhancementSettings s){PlayerOptions o;o.nr=s.nr;o.sr=s.sr;o.fg=s.multiplier>1;o.fgMultiplier=std::max(2u,s.multiplier);o.realtime=s.nrPolicy!=pipeline::NrSizePolicy::Native;o.settings=s;return o;}
};
enum class TransportState { Empty, Opening, Playing, Paused, Ended, Stopping, Failed };
struct PlayerSnapshot {
    TransportState transport=TransportState::Empty;
    uint64_t sessionId=0,rejectedRevision=0;float volume=1;bool muted=false,audioAvailable=false;
    std::wstring status=L"请打开视频或图片";
    EnhancementSettings desired,applied;bool applying=false;
    bool nrActive=false,srActive=false,fgActive=false;
    std::wstring backendWarning;
    std::wstring sourceNotice;
    std::wstring colorStatus;
    sink::CaptureAudioState captureAudio;
    unsigned audioInputChannels=0,audioOutputChannels=0;
    bool audioRebuffering=false;
    uint64_t audioVideoWaits=0;
    bool audioEndpointRecovering=false;HRESULT audioEndpointError=S_OK;uint64_t audioEndpointRecoveries=0;
    uint64_t seekRequested=0,seekPresented=0;double seekTarget=0;
    double position=0,duration=0,fps=0,lateMs=0,lateP95Ms=0;
    double nominalSourceFps=0;
    diagnostics::FrameMetrics metrics;
    std::optional<double> submissionFps;
    uint32_t flowPerf=0;int contentFps=0;
    uint64_t frames=0,generated=0;
    uint64_t captureReceived=0,captureDropped=0,nrEvaluated=0,nvofExecuted=0;
    uint64_t captureRateSkipped=0,processedCompleted=0;
    bool captureHalfRate=false;
    bool captureRecovering=false;unsigned captureReconnectAttempts=0;
    double captureFps=0,captureReadAgeMs=0,captureAgeMs=0,captureAgeP95Ms=0;
    double schedulingWaitP95Ms=0,processCpuP95Ms=0,presentCpuP95Ms=0;
    // Realtime file preview: dropped enhancement opportunities this metrics
    // window, and measured media-advance / wall-clock playback speed (1.0 =
    // normal speed). Zero previewSkipped means every decoded frame was enhanced.
    uint64_t previewSkipped=0;
    double playbackSpeed=0;
    bool fgBudgetLimited=false,xessGenerationSuppressed=false;
    bool running=false,failed=false,image=false,capture=false,remotePlay=false,lanStream=false;
    int remotePlayState=0; uint64_t remotePlaySkipped=0;
    bool remoteRecovering=false;unsigned remoteReconnectAttempts=0;std::wstring remoteRecoveryMessage;
    remoteplay::SessionInbox::Snapshot remoteStream;
    double remoteReceivedFps=0,remoteDecodedFps=0;bool remoteRatesReady=false;uint64_t remoteReceived=0,remoteDecoded=0,remoteIngressDropped=0;
};
class EngineController {
public:
    EngineController();
    ~EngineController();
    bool idle()const;
    bool requestSettings(EnhancementSettings);
    void open(HWND video,const std::wstring& path,PlayerOptions options);
#ifdef VEYRA_ENABLE_REMOTEPLAY
    void openRemotePlay(HWND, source::RemotePlayConnectDesc, PlayerOptions);
    remoteplay::ControllerFeedback remotePlayFeedback();
    void remotePlayController(const remoteplay::ControllerState&);
    void remotePlayLoginPin(std::string);
#endif
    void stop();
    void comparison(int mode,bool base,float split=.5f){comparisonMode_=mode;comparisonBase_=base;comparisonSplit_=std::clamp(split,0.0f,1.0f);}
    void pause(bool p);
    void previewView(PreviewView view){if(!std::isfinite(view.zoom)||!std::isfinite(view.centerX)||!std::isfinite(view.centerY))return;std::lock_guard lock(mutex_);view.zoom=std::clamp(view.zoom,.05f,64.0f);previewView_=view;}
    PreviewView previewView()const{std::lock_guard lock(mutex_);return previewView_;}
    void setVolume(float gain,bool mute);
    void seek(double seconds){if(!std::isfinite(seconds)||seconds<0)return;std::lock_guard lock(mutex_);snapshot_.seekTarget=seconds;++snapshot_.seekRequested;seekSeconds_.store(seconds);}
    void saveFrame(const std::wstring& path);
    void startExport(const std::wstring& input,const std::wstring& output,PlayerOptions,bool hevc);
    PlayerSnapshot snapshot()const;
private:
    void run(HWND,std::wstring,PlayerOptions,std::shared_ptr<source::RemotePlayConnectDesc> remoteRequest={});
    void runLargeImage(HWND,const sink::RgbaImage&,PlayerOptions,gfx::D3D12DeviceContext&,gfx::CommandSlotRing&);
    void post(std::function<void()>);
    void dispatch();
    void status(const std::wstring&,bool failed=false);
    mutable std::mutex mutex_;
    PlayerSnapshot snapshot_;
    std::shared_ptr<FrameFlowWindow> activeFlow_;
    std::shared_ptr<source::RemotePlaySessionSource> activeRemote_;
    PreviewView previewView_;
    std::wstring savePath_;
    std::thread worker_;
    std::condition_variable wake_;std::function<void()> pending_;bool shutdown_=false,busy_=false;
    EnhancementSettings desired_;uint64_t nextRevision_=1;
    std::atomic<bool> stop_{false},paused_{false},muted_{false};
    std::atomic<float> volume_{1};uint64_t sessionId_=0;
    std::atomic<double> seekSeconds_{-1};
    std::atomic<int> comparisonMode_{0};std::atomic<bool> comparisonBase_{false};std::atomic<float> comparisonSplit_{.5f};
};
}
