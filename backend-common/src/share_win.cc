// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The share sheet on Windows (laufey_share.h): DataTransferManager for the
// window (IDataTransferManagerInterop), its DataRequested filling the data
// package, ShareCompleted / ShareCanceled reporting the outcome where Windows
// has them (else SHOWN).

#include "laufey_share.h"

#include <windows.h>
#include <roapi.h>
#include <shobjidl_core.h>
#include <winstring.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.foundation.h>
#include <windows.storage.h>
#include <wrl.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "laufey_backend_common.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace dt = ABI::Windows::ApplicationModel::DataTransfer;
namespace wf = ABI::Windows::Foundation;
namespace ws = ABI::Windows::Storage;

namespace laufey_common {

namespace {

HString ToHString(const std::string& s) {
  HString h;
  std::wstring w = Utf8ToWide(s);
  h.Set(w.c_str(), static_cast<unsigned>(w.size()));
  return h;
}

// One share: its callback fires once, whichever event comes first.
struct ShareState {
  laufey_share_fn callback = nullptr;
  void* user_data = nullptr;
  std::atomic<bool> done{false};
  ShareRequest request;
  ComPtr<dt::IDataTransferManager> manager;
  EventRegistrationToken requested_token{};

  void Finish(int status, const std::string& detail) {
    if (done.exchange(true))
      return;
    ShareReport(callback, user_data, status, detail);
  }
};

HRESULT FillPackage(ShareState* state, dt::IDataRequestedEventArgs* args) {
  ComPtr<dt::IDataRequest> request;
  HRESULT hr = args->get_Request(&request);
  if (FAILED(hr))
    return hr;
  ComPtr<dt::IDataPackage> package;
  hr = request->get_Data(&package);
  if (FAILED(hr))
    return hr;
  ComPtr<dt::IDataPackagePropertySet> props;
  if (SUCCEEDED(package->get_Properties(&props))) {
    // Windows requires a title.
    std::string title = state->request.title.empty()
                            ? (state->request.text.empty()
                                   ? std::string("Share")
                                   : state->request.text.substr(0, 80))
                            : state->request.title;
    HString t = ToHString(title);
    props->put_Title(t.Get());
  }
  if (!state->request.text.empty()) {
    HString text = ToHString(state->request.text);
    package->SetText(text.Get());
  }
  if (!state->request.url.empty()) {
    ComPtr<wf::IUriRuntimeClassFactory> uri_factory;
    if (SUCCEEDED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Foundation_Uri).Get(),
            IID_PPV_ARGS(&uri_factory)))) {
      ComPtr<wf::IUriRuntimeClass> uri;
      HString u = ToHString(state->request.url);
      ComPtr<dt::IDataPackage2> package2;
      if (SUCCEEDED(uri_factory->CreateUri(u.Get(), &uri)) &&
          SUCCEEDED(package.As(&package2)))
        package2->SetWebLink(uri.Get());
    }
  }
  // Outcome events where this Windows has them.
  ComPtr<dt::IDataPackage3> package3;
  if (SUCCEEDED(package.As(&package3))) {
    EventRegistrationToken token;
    package3->add_ShareCompleted(
        Callback<wf::ITypedEventHandler<dt::DataPackage*,
                                        dt::ShareCompletedEventArgs*>>(
            [state](dt::IDataPackage*,
                    dt::IShareCompletedEventArgs* e) -> HRESULT {
              std::string target;
              ComPtr<dt::IShareTargetInfo> info;
              if (e && SUCCEEDED(e->get_ShareTarget(&info)) && info) {
                HString name;
                if (SUCCEEDED(info->get_AppUserModelId(name.GetAddressOf())))
                  target = WideToUtf8(
                      WindowsGetStringRawBuffer(name.Get(), nullptr));
              }
              state->Finish(LAUFEY_SHARE_COMPLETED, target);
              return S_OK;
            })
            .Get(),
        &token);
  }
  ComPtr<dt::IDataPackage4> package4;
  if (SUCCEEDED(package.As(&package4))) {
    EventRegistrationToken token;
    package4->add_ShareCanceled(
        Callback<wf::ITypedEventHandler<dt::DataPackage*, IInspectable*>>(
            [state](dt::IDataPackage*, IInspectable*) -> HRESULT {
              state->Finish(LAUFEY_SHARE_CANCELLED, "");
              return S_OK;
            })
            .Get(),
        &token);
  } else {
    // No cancel event: the UI opening is all this Windows reports.
    state->Finish(LAUFEY_SHARE_SHOWN, "");
  }
  return S_OK;
}

}  // namespace

void ShareShowWin(void* hwnd_ptr, const ShareRequest& request,
                  laufey_share_fn callback, void* user_data) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    hwnd = GetForegroundWindow();
  if (!hwnd) {
    ShareReport(callback, user_data, LAUFEY_SHARE_FAILED,
                "no window to show the share sheet over");
    return;
  }
  ComPtr<IDataTransferManagerInterop> interop;
  HRESULT hr = RoGetActivationFactory(
      HStringReference(
          RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager)
          .Get(),
      IID_PPV_ARGS(&interop));
  if (FAILED(hr) || !interop) {
    ShareReport(callback, user_data, LAUFEY_SHARE_NOT_SUPPORTED,
                "DataTransferManager is not available");
    return;
  }
  // Leaked on purpose once the UI is up: the events may fire after this
  // returns, and Windows keeps the manager per window.
  auto* state = new ShareState();
  state->callback = callback;
  state->user_data = user_data;
  state->request = request;
  // Storage items need an async file lookup the data package can't wait for
  // here: files go as their paths in the text.
  if (!request.files.empty()) {
    std::string paths;
    for (const std::string& f : request.files)
      paths += (paths.empty() ? "" : "\n") + f;
    state->request.text =
        state->request.text.empty() ? paths : state->request.text + "\n" + paths;
  }
  hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&state->manager));
  if (FAILED(hr) || !state->manager) {
    state->Finish(LAUFEY_SHARE_FAILED, "no DataTransferManager for the window");
    return;
  }
  state->manager->add_DataRequested(
      Callback<wf::ITypedEventHandler<dt::DataTransferManager*,
                                      dt::DataRequestedEventArgs*>>(
          [state](dt::IDataTransferManager* manager,
                  dt::IDataRequestedEventArgs* args) -> HRESULT {
            HRESULT fill = FillPackage(state, args);
            // One request per share.
            manager->remove_DataRequested(state->requested_token);
            if (FAILED(fill))
              state->Finish(LAUFEY_SHARE_FAILED, "filling the data failed");
            return S_OK;
          })
          .Get(),
      &state->requested_token);
  hr = interop->ShowShareUIForWindow(hwnd);
  if (FAILED(hr)) {
    state->manager->remove_DataRequested(state->requested_token);
    state->Finish(LAUFEY_SHARE_FAILED, "ShowShareUIForWindow failed");
  }
}

}  // namespace laufey_common
