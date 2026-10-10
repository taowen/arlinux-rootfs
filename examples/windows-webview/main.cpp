#include <windows.h>
#include <cstdio>
#include <cwchar>
#undef __uuidof
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(id) struct __declspec(uuid(id))
#include "WebView2.h"

static FILE *logfile;
static HWND window;
static ICoreWebView2Controller *controller;
static ICoreWebView2 *webview;
static const wchar_t *flags=L"";
static void loghr(const char *step,HRESULT hr) { fprintf(logfile,"%s hr=%08lx\n",step,(unsigned long)hr); fflush(logfile); }
template<class I> class Handler: public I {
    ULONG refs=1;
public:
    virtual ~Handler() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
        *out=nullptr;
        if(id==IID_IUnknown||id==__uuidof(I)) { *out=static_cast<I*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n=--refs; if(!n) delete this; return n; }
};
class Options: public Handler<ICoreWebView2EnvironmentOptions> {
public:
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR *v) override { *v=(LPWSTR)CoTaskMemAlloc((wcslen(flags)+1)*sizeof(wchar_t)); if(!*v)return E_OUTOFMEMORY; wcscpy(*v,flags); return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *v) override { *v=nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *v) override { const wchar_t *s=L"140.0.3485.44"; *v=(LPWSTR)CoTaskMemAlloc((wcslen(s)+1)*sizeof(wchar_t)); if(!*v)return E_OUTOFMEMORY; wcscpy(*v,s); return S_OK; }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *v) override { *v=FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return E_NOTIMPL; }
};
class Navigation: public Handler<ICoreWebView2NavigationCompletedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs *args) override {
        BOOL ok=FALSE; COREWEBVIEW2_WEB_ERROR_STATUS status=COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN; args->get_IsSuccess(&ok); args->get_WebErrorStatus(&status);
        fprintf(logfile,"navigation success=%d status=%d\n",ok,status); fflush(logfile); return S_OK;
    }
};
class Message: public Handler<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs *args) override {
        LPWSTR message=nullptr; HRESULT hr=args->get_WebMessageAsJson(&message);
        if(SUCCEEDED(hr)){ fprintf(logfile,"message %ls\n",message); fflush(logfile); CoTaskMemFree(message); } return S_OK;
    }
};
class Failed: public Handler<ICoreWebView2ProcessFailedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs *args) override {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind; args->get_ProcessFailedKind(&kind);
        fprintf(logfile,"process-failed kind=%d\n",kind); fflush(logfile); return S_OK;
    }
};
class Controller: public Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,ICoreWebView2Controller *value) override {
        loghr("controller",hr); if(FAILED(hr)||!value)return S_OK;
        controller=value; controller->AddRef(); controller->get_CoreWebView2(&webview);
        RECT rect; GetClientRect(window,&rect); controller->put_Bounds(rect); controller->put_IsVisible(TRUE);
        EventRegistrationToken token;
        auto n=new Navigation; webview->add_NavigationCompleted(n,&token); n->Release();
        auto m=new Message; webview->add_WebMessageReceived(m,&token); m->Release();
        auto f=new Failed; webview->add_ProcessFailed(f,&token); f->Release();
        loghr("navigate",webview->NavigateToString(L"<!doctype html><html><body style='background:#173b52;color:white;font:32px sans-serif;padding:40px'><h1>ARLinux WebView2</h1><p>Local content: no Adobe account or network required.</p><input placeholder='Type here' style='font-size:30px' oninput='chrome.webview.postMessage({input:this.value})'><p id='tick'></p><script>let n=0,frames=0;function animate(){++frames;requestAnimationFrame(animate)}requestAnimationFrame(animate);setInterval(()=>{document.getElementById('tick').textContent='Tick '+(++n)+' / animation frames '+frames;document.body.style.background=n%2?'#173b52':'#415524';chrome.webview.postMessage({tick:n,frames});},1000);</script></body></html>"));
        return S_OK;
    }
};
class Environment: public Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,ICoreWebView2Environment *env) override {
        loghr("environment",hr); if(FAILED(hr)||!env)return S_OK;
        LPWSTR version=nullptr; env->get_BrowserVersionString(&version); fprintf(logfile,"runtime %ls\n",version); fflush(logfile); CoTaskMemFree(version);
        auto handler=new Controller; loghr("create-controller",env->CreateCoreWebView2Controller(window,handler)); handler->Release(); return S_OK;
    }
};
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_SIZE && controller){ RECT rect; GetClientRect(hwnd,&rect); controller->put_Bounds(rect); }
    if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
int wmain(int argc,wchar_t **argv) {
    if(argc>1)flags=argv[1];
    logfile=fopen("C:\\webview-probe.txt","w"); if(!logfile)return 1;
    loghr("COM",CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));
    WNDCLASSW wc={}; wc.lpfnWndProc=proc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"ARLinuxWebViewProbe"; RegisterClassW(&wc);
    window=CreateWindowW(wc.lpszClassName,L"ARLinux WebView2 standalone probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,50,50,1400,800,nullptr,nullptr,wc.hInstance,nullptr);
    HMODULE loader=LoadLibraryW(L"WebView2Loader.dll"); if(!loader){loghr("loader",HRESULT_FROM_WIN32(GetLastError()));return 2;}
    auto create=(HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*))GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions");
    if(!create)return 3;
    auto env=new Environment; auto options=new Options;
    loghr("create-environment",create(nullptr,L"C:\\webview-probe-data",options,env)); options->Release(); env->Release();
    MSG msg; while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(controller)controller->Close(); if(webview)webview->Release(); if(controller)controller->Release(); CoUninitialize(); fclose(logfile); return 0;
}
