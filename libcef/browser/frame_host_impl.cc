// Copyright (c) 2012 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "libcef/browser/frame_host_impl.h"

#include "include/cef_request.h"
#include "include/cef_stream.h"
#include "include/cef_v8.h"
#include "include/test/cef_test_helpers.h"
#include "libcef/browser/browser_host_base.h"
#include "libcef/browser/net_service/browser_urlrequest_impl.h"
#include "libcef/common/frame_util.h"
#include "libcef/common/net/url_util.h"
#include "libcef/common/process_message_impl.h"
#include "libcef/common/request_impl.h"
#include "libcef/common/string_util.h"
#include "libcef/common/task_runner_impl.h"

#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "services/service_manager/public/cpp/interface_provider.h"

namespace {

void StringVisitCallback(CefRefPtr<CefStringVisitor> visitor,
                         base::ReadOnlySharedMemoryRegion response) {
  string_util::ExecuteWithScopedCefString(
      std::move(response),
      base::BindOnce([](CefRefPtr<CefStringVisitor> visitor,
                        const CefString& str) { visitor->Visit(str); },
                     visitor));
}

void ViewTextCallback(CefRefPtr<CefFrameHostImpl> frame,
                      base::ReadOnlySharedMemoryRegion response) {
  if (auto browser = frame->GetBrowser()) {
    string_util::ExecuteWithScopedCefString(
        std::move(response),
        base::BindOnce(
            [](CefRefPtr<CefBrowser> browser, const CefString& str) {
              static_cast<CefBrowserHostBase*>(browser.get())->ViewText(str);
            },
            browser));
  }
}

}  // namespace

CefFrameHostImpl::CefFrameHostImpl(scoped_refptr<CefBrowserInfo> browser_info,
                                   bool is_main_frame,
                                   int64_t parent_frame_id)
    : is_main_frame_(is_main_frame),
      frame_id_(kInvalidFrameId),
      browser_info_(browser_info),
      is_focused_(is_main_frame_),  // The main frame always starts focused.
      parent_frame_id_(parent_frame_id) {
#if DCHECK_IS_ON()
  DCHECK(browser_info_);
  if (is_main_frame_) {
    DCHECK_EQ(parent_frame_id_, kInvalidFrameId);
  } else {
    DCHECK_GT(parent_frame_id_, 0);
  }
#endif
}

CefFrameHostImpl::CefFrameHostImpl(scoped_refptr<CefBrowserInfo> browser_info,
                                   content::RenderFrameHost* render_frame_host)
    : is_main_frame_(render_frame_host->GetParent() == nullptr),
      browser_info_(browser_info),
      is_focused_(is_main_frame_),  // The main frame always starts focused.
      parent_frame_id_(is_main_frame_
                           ? kInvalidFrameId
                           : MakeFrameId(render_frame_host->GetParent())) {
  DCHECK(browser_info_);
  SetRenderFrameHost(render_frame_host);
}

CefFrameHostImpl::~CefFrameHostImpl() {}

void CefFrameHostImpl::SetRenderFrameHost(content::RenderFrameHost* host) {
  CEF_REQUIRE_UIT();

  base::AutoLock lock_scope(state_lock_);

  // We should not be detached.
  CHECK(browser_info_);

  render_frame_.reset();

  render_frame_host_ = host;
  frame_id_ = MakeFrameId(host);
  url_ = host->GetLastCommittedURL().spec();
  name_ = host->GetFrameName();
}

bool CefFrameHostImpl::IsValid() {
  return !!GetBrowserHostBase();
}

void CefFrameHostImpl::Undo() {
  SendCommand("Undo");
}

void CefFrameHostImpl::Redo() {
  SendCommand("Redo");
}

void CefFrameHostImpl::Cut() {
  SendCommand("Cut");
}

void CefFrameHostImpl::Copy() {
  SendCommand("Copy");
}

void CefFrameHostImpl::Paste() {
  SendCommand("Paste");
}

void CefFrameHostImpl::Delete() {
  SendCommand("Delete");
}

void CefFrameHostImpl::SelectAll() {
  SendCommand("SelectAll");
}

void CefFrameHostImpl::ViewSource() {
  SendCommandWithResponse(
      "GetSource",
      base::BindOnce(&ViewTextCallback, CefRefPtr<CefFrameHostImpl>(this)));
}

void CefFrameHostImpl::GetSource(CefRefPtr<CefStringVisitor> visitor) {
  SendCommandWithResponse("GetSource",
                          base::BindOnce(&StringVisitCallback, visitor));
}

void CefFrameHostImpl::GetText(CefRefPtr<CefStringVisitor> visitor) {
  SendCommandWithResponse("GetText",
                          base::BindOnce(&StringVisitCallback, visitor));
}

void CefFrameHostImpl::LoadRequest(CefRefPtr<CefRequest> request) {
  auto params = cef::mojom::RequestParams::New();
  static_cast<CefRequestImpl*>(request.get())->Get(params);
  LoadRequest(std::move(params));
}

void CefFrameHostImpl::LoadURL(const CefString& url) {
  LoadURLWithExtras(url, content::Referrer(), kPageTransitionExplicit,
                    std::string());
}

void CefFrameHostImpl::ExecuteJavaScript(const CefString& jsCode,
                                         const CefString& scriptUrl,
                                         int startLine) {
  SendJavaScript(jsCode, scriptUrl, startLine);
}

bool CefFrameHostImpl::IsMain() {
  return is_main_frame_;
}

bool CefFrameHostImpl::IsFocused() {
  base::AutoLock lock_scope(state_lock_);
  return is_focused_;
}

CefString CefFrameHostImpl::GetName() {
  base::AutoLock lock_scope(state_lock_);
  return name_;
}

int64 CefFrameHostImpl::GetIdentifier() {
  base::AutoLock lock_scope(state_lock_);
  return frame_id_;
}

CefRefPtr<CefFrame> CefFrameHostImpl::GetParent() {
  int64 parent_frame_id;

  {
    base::AutoLock lock_scope(state_lock_);
    if (is_main_frame_ || parent_frame_id_ == kInvalidFrameId)
      return nullptr;
    parent_frame_id = parent_frame_id_;
  }

  auto browser = GetBrowserHostBase();
  if (browser)
    return browser->GetFrame(parent_frame_id);

  return nullptr;
}

CefString CefFrameHostImpl::GetURL() {
  base::AutoLock lock_scope(state_lock_);
  return url_;
}

CefRefPtr<CefBrowser> CefFrameHostImpl::GetBrowser() {
  return GetBrowserHostBase().get();
}

CefRefPtr<CefV8Context> CefFrameHostImpl::GetV8Context() {
  NOTREACHED() << "GetV8Context cannot be called from the browser process";
  return nullptr;
}

void CefFrameHostImpl::VisitDOM(CefRefPtr<CefDOMVisitor> visitor) {
  NOTREACHED() << "VisitDOM cannot be called from the browser process";
}

CefRefPtr<CefURLRequest> CefFrameHostImpl::CreateURLRequest(
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefURLRequestClient> client) {
  if (!request || !client)
    return nullptr;

  if (!CefTaskRunnerImpl::GetCurrentTaskRunner()) {
    NOTREACHED() << "called on invalid thread";
    return nullptr;
  }

  auto browser = GetBrowserHostBase();
  if (!browser)
    return nullptr;

  auto request_context = browser->request_context();

  CefRefPtr<CefBrowserURLRequest> impl =
      new CefBrowserURLRequest(this, request, client, request_context);
  if (impl->Start())
    return impl.get();
  return nullptr;
}

void CefFrameHostImpl::SendProcessMessage(
    CefProcessId target_process,
    CefRefPtr<CefProcessMessage> message) {
  DCHECK_EQ(PID_RENDERER, target_process);
  DCHECK(message && message->IsValid());
  if (!message || !message->IsValid())
    return;

  SendToRenderFrame(base::BindOnce(
      [](CefRefPtr<CefProcessMessage> message,
         const RenderFrameType& render_frame) {
        auto impl = static_cast<CefProcessMessageImpl*>(message.get());
        render_frame->SendMessage(impl->GetName(), impl->TakeArgumentList());
      },
      message));
}

void CefFrameHostImpl::SetFocused(bool focused) {
  base::AutoLock lock_scope(state_lock_);
  is_focused_ = focused;
}

void CefFrameHostImpl::RefreshAttributes() {
  CEF_REQUIRE_UIT();

  base::AutoLock lock_scope(state_lock_);
  if (!render_frame_host_)
    return;
  url_ = render_frame_host_->GetLastCommittedURL().spec();

  // Use the assigned name if it is non-empty. This represents the name property
  // on the frame DOM element. If the assigned name is empty, revert to the
  // internal unique name. This matches the logic in render_frame_util::GetName.
  name_ = render_frame_host_->GetFrameName();
  if (name_.empty()) {
    const auto node = content::FrameTreeNode::GloballyFindByID(
        render_frame_host_->GetFrameTreeNodeId());
    if (node) {
      name_ = node->unique_name();
    }
  }

  if (!is_main_frame_)
    parent_frame_id_ = MakeFrameId(render_frame_host_->GetParent());
}

void CefFrameHostImpl::NotifyMoveOrResizeStarted() {
  SendToRenderFrame(base::BindOnce([](const RenderFrameType& render_frame) {
    render_frame->MoveOrResizeStarted();
  }));
}

void CefFrameHostImpl::LoadRequest(cef::mojom::RequestParamsPtr params) {
  if (!url_util::FixupGURL(params->url))
    return;

  SendToRenderFrame(base::BindOnce(
      [](cef::mojom::RequestParamsPtr params,
         const RenderFrameType& render_frame) {
        render_frame->LoadRequest(std::move(params));
      },
      std::move(params)));

  auto browser = GetBrowserHostBase();
  if (browser)
    browser->OnSetFocus(FOCUS_SOURCE_NAVIGATION);
}

void CefFrameHostImpl::LoadURLWithExtras(const std::string& url,
                                         const content::Referrer& referrer,
                                         ui::PageTransition transition,
                                         const std::string& extra_headers) {
  // Only known frame ids or kMainFrameId are supported.
  const auto frame_id = GetFrameId();
  if (frame_id < CefFrameHostImpl::kMainFrameId)
    return;

  // Any necessary fixup will occur in LoadRequest.
  GURL gurl = url_util::MakeGURL(url, /*fixup=*/false);

  if (frame_id == CefFrameHostImpl::kMainFrameId) {
    // Load via the browser using NavigationController.
    auto browser = GetBrowserHostBase();
    if (browser) {
      content::OpenURLParams params(
          gurl, referrer, WindowOpenDisposition::CURRENT_TAB, transition,
          /*is_renderer_initiated=*/false);
      params.extra_headers = extra_headers;

      browser->LoadMainFrameURL(params);
    }
  } else {
    auto params = cef::mojom::RequestParams::New();
    params->url = gurl;
    params->referrer =
        blink::mojom::Referrer::New(referrer.url, referrer.policy);
    params->headers = extra_headers;
    LoadRequest(std::move(params));
  }
}

void CefFrameHostImpl::SendCommand(const std::string& command) {
  DCHECK(!command.empty());
  SendToRenderFrame(base::BindOnce(
      [](const std::string& command, const RenderFrameType& render_frame) {
        render_frame->SendCommand(command);
      },
      command));
}

void CefFrameHostImpl::SendCommandWithResponse(
    const std::string& command,
    cef::mojom::RenderFrame::SendCommandWithResponseCallback
        response_callback) {
  DCHECK(!command.empty());
  SendToRenderFrame(base::BindOnce(
      [](const std::string& command,
         cef::mojom::RenderFrame::SendCommandWithResponseCallback
             response_callback,
         const RenderFrameType& render_frame) {
        render_frame->SendCommandWithResponse(command,
                                              std::move(response_callback));
      },
      command, std::move(response_callback)));
}

void CefFrameHostImpl::SendJavaScript(const std::u16string& jsCode,
                                      const std::string& scriptUrl,
                                      int startLine) {
  if (jsCode.empty())
    return;
  if (startLine <= 0) {
    // A value of 0 is v8::Message::kNoLineNumberInfo in V8. There is code in
    // V8 that will assert on that value (e.g. V8StackTraceImpl::Frame::Frame
    // if a JS exception is thrown) so make sure |startLine| > 0.
    startLine = 1;
  }

  SendToRenderFrame(base::BindOnce(
      [](const std::u16string& jsCode, const std::string& scriptUrl,
         int startLine, const RenderFrameType& render_frame) {
        render_frame->SendJavaScript(jsCode, scriptUrl, startLine);
      },
      jsCode, scriptUrl, startLine));
}

void CefFrameHostImpl::MaybeSendDidStopLoading() {
  auto rfh = GetRenderFrameHost();
  if (!rfh)
    return;

  // We only want to notify for the highest-level LocalFrame in this frame's
  // renderer process subtree. If this frame has a parent in the same process
  // then the notification will be sent via the parent instead.
  auto rfh_parent = rfh->GetParent();
  if (rfh_parent && rfh_parent->GetProcess() == rfh->GetProcess()) {
    return;
  }

  SendToRenderFrame(base::BindOnce([](const RenderFrameType& render_frame) {
    render_frame->DidStopLoading();
  }));
}

void CefFrameHostImpl::ExecuteJavaScriptWithUserGestureForTests(
    const CefString& javascript) {
  if (!CEF_CURRENTLY_ON_UIT()) {
    CEF_POST_TASK(
        CEF_UIT,
        base::BindOnce(
            &CefFrameHostImpl::ExecuteJavaScriptWithUserGestureForTests, this,
            javascript));
    return;
  }

  content::RenderFrameHost* rfh = GetRenderFrameHost();
  if (rfh)
    rfh->ExecuteJavaScriptWithUserGestureForTests(javascript);
}

content::RenderFrameHost* CefFrameHostImpl::GetRenderFrameHost() const {
  CEF_REQUIRE_UIT();
  return render_frame_host_;
}

void CefFrameHostImpl::Detach() {
  CEF_REQUIRE_UIT();

  {
    base::AutoLock lock_scope(state_lock_);
    browser_info_ = nullptr;
  }

  // In case we never attached, clean up.
  while (!queued_actions_.empty()) {
    queued_actions_.pop();
  }

  render_frame_.reset();
  render_frame_host_ = nullptr;
}

// static
int64_t CefFrameHostImpl::MakeFrameId(const content::RenderFrameHost* host) {
  CEF_REQUIRE_UIT();
  auto host_nonconst = const_cast<content::RenderFrameHost*>(host);
  return MakeFrameId(host_nonconst->GetProcess()->GetID(),
                     host_nonconst->GetRoutingID());
}

// static
int64_t CefFrameHostImpl::MakeFrameId(int32_t render_process_id,
                                      int32_t render_routing_id) {
  return frame_util::MakeFrameId(render_process_id, render_routing_id);
}

// kMainFrameId must be -1 to align with renderer expectations.
const int64_t CefFrameHostImpl::kMainFrameId = -1;
const int64_t CefFrameHostImpl::kFocusedFrameId = -2;
const int64_t CefFrameHostImpl::kUnspecifiedFrameId = -3;
const int64_t CefFrameHostImpl::kInvalidFrameId = -4;

// This equates to (TT_EXPLICIT | TT_DIRECT_LOAD_FLAG).
const ui::PageTransition CefFrameHostImpl::kPageTransitionExplicit =
    static_cast<ui::PageTransition>(ui::PAGE_TRANSITION_TYPED |
                                    ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);

int64 CefFrameHostImpl::GetFrameId() const {
  base::AutoLock lock_scope(state_lock_);
  return is_main_frame_ ? kMainFrameId : frame_id_;
}

CefRefPtr<CefBrowserHostBase> CefFrameHostImpl::GetBrowserHostBase() const {
  base::AutoLock lock_scope(state_lock_);
  if (browser_info_)
    return browser_info_->browser();
  return nullptr;
}

const mojo::Remote<cef::mojom::RenderFrame>&
CefFrameHostImpl::GetRenderFrame() {
  CEF_REQUIRE_UIT();
  DCHECK(is_attached_);

  if (!render_frame_.is_bound() && render_frame_host_ &&
      render_frame_host_->GetRemoteInterfaces()) {
    // Connects to a CefFrameImpl that already exists in the renderer process.
    render_frame_host_->GetRemoteInterfaces()->GetInterface(
        render_frame_.BindNewPipeAndPassReceiver());
  }
  return render_frame_;
}

void CefFrameHostImpl::SendToRenderFrame(RenderFrameAction action) {
  if (!CEF_CURRENTLY_ON_UIT()) {
    CEF_POST_TASK(CEF_UIT, base::BindOnce(&CefFrameHostImpl::SendToRenderFrame,
                                          this, std::move(action)));
    return;
  }

  if (!render_frame_host_) {
    // Either we're a placeholder frame without a renderer representation, or
    // we've been detached.
    return;
  }

  if (!is_attached_) {
    // Queue actions until we're notified by the renderer that it's ready to
    // handle them.
    queued_actions_.push(std::move(action));
    return;
  }

  auto& render_frame = GetRenderFrame();
  if (!render_frame)
    return;

  std::move(action).Run(render_frame);
}

void CefFrameHostImpl::SendMessage(const std::string& name,
                                   base::Value arguments) {
  if (auto browser = GetBrowserHostBase()) {
    if (auto client = browser->GetClient()) {
      auto& list_value = base::Value::AsListValue(arguments);
      CefRefPtr<CefProcessMessageImpl> message(new CefProcessMessageImpl(
          name, const_cast<base::ListValue*>(&list_value), /*read_only=*/true));
      browser->GetClient()->OnProcessMessageReceived(
          browser.get(), this, PID_RENDERER, message.get());
      message->Detach();
    }
  }
}

void CefFrameHostImpl::FrameAttached() {
  if (!is_attached_) {
    is_attached_ = true;

    auto& render_frame = GetRenderFrame();
    while (!queued_actions_.empty()) {
      if (render_frame) {
        std::move(queued_actions_.front()).Run(render_frame);
      }
      queued_actions_.pop();
    }
  }
}

void CefFrameHostImpl::DidFinishFrameLoad(const GURL& validated_url,
                                          int http_status_code) {
  auto browser = GetBrowserHostBase();
  if (browser)
    browser->OnDidFinishLoad(this, validated_url, http_status_code);
}

void CefFrameHostImpl::UpdateDraggableRegions(
    base::Optional<std::vector<cef::mojom::DraggableRegionEntryPtr>> regions) {
  auto browser = GetBrowserHostBase();
  if (!browser)
    return;

  CefRefPtr<CefDragHandler> handler;
  auto client = browser->GetClient();
  if (client)
    handler = client->GetDragHandler();
  if (!handler)
    return;

  std::vector<CefDraggableRegion> draggable_regions;
  if (regions) {
    draggable_regions.reserve(regions->size());

    for (const auto& region : *regions) {
      const auto& rect = region->bounds;
      const CefRect bounds(rect.x(), rect.y(), rect.width(), rect.height());
      draggable_regions.push_back(
          CefDraggableRegion(bounds, region->draggable));
    }
  }

  handler->OnDraggableRegionsChanged(browser.get(), this, draggable_regions);
}

void CefExecuteJavaScriptWithUserGestureForTests(CefRefPtr<CefFrame> frame,
                                                 const CefString& javascript) {
  CefFrameHostImpl* impl = static_cast<CefFrameHostImpl*>(frame.get());
  if (impl)
    impl->ExecuteJavaScriptWithUserGestureForTests(javascript);
}
