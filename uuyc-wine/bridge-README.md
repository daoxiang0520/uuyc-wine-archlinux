# The client's native bridge (evidence copy)

`uuycbridge.js` is taken verbatim from the installed client:

    bin/html/error/uuycbridge.js

It is the web-UI side of the bridge between the remote web app
(`https://uuyc.webapp.163.com/cloud-device`) and the native layer. It is shipped
in clear text, which is how the protocol below was recovered.

## Protocol

Windows host object (must exist, or the bridge reports
`{code:-1, msg:"Bridge environment loading failed."}`):

```js
window.chrome.webview.hostObjects.host.JSCallCppWithCallbackParameter(payload, {onResponse, onError})
```

Bridge methods the page can call:

| method | meaning |
| --- | --- |
| `getTicket` | obtain the session ticket |
| `getHeaderParams` | obtain the authenticated HTTP headers |
| `syncClientStatus` | push client status to native |
| `getOriginUIConfig` / `setOriginUIConfig` | UI configuration |
| `getSystemConfig` / `setSystemConfig` | system configuration |
| `setNavConfig` | navigation configuration |
| `onHandleUri` | handle a `uuremote:` URI |
| `onCopy` / `onClose` | clipboard / window |
| `sendRenderFinishEvent` | page finished rendering |

Environment is detected from the user agent: `navigator.userAgent.includes("remote-Window")`
selects the Windows branch.

## Why this matters for the Wine adaptation

The device list is fetched with headers that come from `getHeaderParams`, and the
ticket comes from `getTicket`. Both are answered by the native layer.

When the native ticket is empty — which is what `GameViewer.exe` reports at
startup (`uuToken: ""`, `gameId: ""`, `pcType: ""`) and what the WebView2 cookie
store shows (`nrd_access_token` created empty on every launch) — the page has no
credentials, the device list comes back empty, and the UI stays on the bundled
`云设备骨架屏` placeholder. There is then no device row to click and no session can
be started. That is the state this adaptation is currently blocked on.

Nothing here is a Wine defect: the fixable Wine bug (`window_prop_store_GetValue`,
see `../winepatch/`) was a separate crash that has been fixed and verified.

SPDX-License-Identifier: 0BSD
