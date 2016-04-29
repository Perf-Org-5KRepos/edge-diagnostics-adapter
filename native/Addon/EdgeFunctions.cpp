//
// Copyright (C) Microsoft. All rights reserved.
//

#include "EdgeFunctions.h"
#include "MessageReceiver.h"
#include "Proxy_h.h"
#include <VersionHelpers.h>
#include <comdef.h>
#include <Strsafe.h>
#include <Shellapi.h>
#include <Shlobj.h>
#include <Aclapi.h>
#include <Sddl.h>

using v8::FunctionTemplate;

bool isInitialized = false;
bool isMessageReceiverCreated = false;
Nan::Persistent<Function> messageCallbackHandle;
Nan::Persistent<Function> logCallbackHandle;
CString m_rootPath;
HWND m_proxyHwnd;

NAN_MODULE_INIT(InitAll) 
{
    Nan::Set(target, Nan::New("initialize").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(initialize)).ToLocalChecked());
    Nan::Set(target, Nan::New("getEdgeInstances").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(getEdgeInstances)).ToLocalChecked());
    Nan::Set(target, Nan::New("setSecurityACLs").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(setSecurityACLs)).ToLocalChecked());        
    Nan::Set(target, Nan::New("connectTo").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(connectTo)).ToLocalChecked());
    Nan::Set(target, Nan::New("injectScriptTo").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(injectScriptTo)).ToLocalChecked());
    Nan::Set(target, Nan::New("forwardTo").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(forwardTo)).ToLocalChecked());
}

NODE_MODULE(Addon, InitAll)

inline void EnsureInitialized()
{
    if (!isInitialized)
    {
        Nan::ThrowTypeError("Not initialized - you must call initialize(...) before using the adapter."); 
        return; 
    }
}

void Log(_In_ const char* message)
{
    Isolate* isolate = Isolate::GetCurrent();
    
    Local<Value> log[1] = { Nan::New<String>(message).ToLocalChecked() };
    Local<Function>::New(isolate, logCallbackHandle)->Call(Nan::GetCurrentContext()->Global(), 1, log);
}

inline void EXIT_IF_NOT_S_OK(_In_ HRESULT hr)
{
    if (hr != S_OK)
    {
        _com_error err(hr);
        CString error;
        error.Format(L"ERROR: HRESULT 0x%08x : %s", hr, err.ErrorMessage());

        CStringA log(error);
        Log(log.GetString());

        return;
    }
}

void SendMessageToInstance(_In_ HWND instanceHwnd, _In_ CString& message)
{
    const size_t ucbParamsSize = sizeof(CopyDataPayload_StringMessage_Data);
	const size_t ucbStringSize = sizeof(WCHAR) * (::wcslen(message) + 1);
	const size_t ucbBufferSize = ucbParamsSize + ucbStringSize;
	std::unique_ptr<BYTE> pBuffer;
	pBuffer.reset(new BYTE[ucbBufferSize]);

	COPYDATASTRUCT copyData;
	copyData.dwData = CopyDataPayload_ProcSignature::StringMessage_Signature;
	copyData.cbData = static_cast<DWORD>(ucbBufferSize);
	copyData.lpData = pBuffer.get();

	CopyDataPayload_StringMessage_Data* pData = reinterpret_cast<CopyDataPayload_StringMessage_Data*>(pBuffer.get());
	pData->uMessageOffset = static_cast<UINT>(ucbParamsSize);

	HRESULT hr = ::StringCbCopyEx(reinterpret_cast<LPWSTR>(pBuffer.get() + pData->uMessageOffset), ucbStringSize, message, NULL, NULL, STRSAFE_IGNORE_NULLS);
	EXIT_IF_NOT_S_OK(hr);

	::SendMessage(instanceHwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(m_proxyHwnd), reinterpret_cast<LPARAM>(&copyData));
}

NAN_METHOD(initialize) 
{
    //::MessageBox(nullptr, L"Attach", L"Attach", 0);
    
    if (isInitialized)
    {
        Nan::ThrowTypeError("Already initialized - you cannot call initialize(...) more than once."); 
        return; 
    }
    
    if (info.Length() < 3 || !info[0]->IsString() || !info[1]->IsFunction() || !info[2]->IsFunction())
    {
        Nan::ThrowTypeError("Incorrect arguments - initialize(rootPath: string, onEdgeMessage: (msg: string) => void, onLogMessage: (msg: string) => void): boolean");
        return;
    }

    String::Utf8Value path(info[0]->ToString());
    m_rootPath = (char*)*path;
    messageCallbackHandle.Reset(info[1].As<Function>());
    logCallbackHandle.Reset(info[2].As<Function>());

    m_proxyHwnd = nullptr;

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    isInitialized = (hr == S_OK || hr == S_FALSE);
   
    info.GetReturnValue().Set(isInitialized);
}

NAN_METHOD(getEdgeInstances) 
{
    EnsureInitialized();
    if (info.Length() > 0)
    {
        Nan::ThrowTypeError("Incorrect arguments - getEdgeInstances(): { id: string, url: string, title: string, processName: string }[]");
        return;
    }
    
    struct Info {
        HWND hwnd;
        CString title;
        CString url;
        CString processName;
    };
    
    vector<Info> instances;
    
    Helpers::EnumWindowsHelper([&](HWND hwndTop) -> BOOL
    {
        Helpers::EnumChildWindowsHelper(hwndTop, [&](HWND hwnd) -> BOOL
        {
            if (Helpers::IsWindowClass(hwnd, L"Internet Explorer_Server"))
            {
                bool isEdgeContentProcess = false;
                
                DWORD processId;
                ::GetWindowThreadProcessId(hwnd, &processId);
                
                CString processName;
                CHandle handle(::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId));
                if (handle)
                {
                    DWORD length = ::GetModuleFileNameEx(handle, nullptr, processName.GetBufferSetLength(MAX_PATH), MAX_PATH);
                    processName.ReleaseBuffer(length);
                    isEdgeContentProcess = (processName.Find(L"MicrosoftEdgeCP.exe") == processName.GetLength() - 19);
                }

                if (isEdgeContentProcess)
                {            
                    CComPtr<IHTMLDocument2> spDocument;
                    HRESULT hr = Helpers::GetDocumentFromHwnd(hwnd, spDocument);
                    if (hr == S_OK)
                    {   
                        CComBSTR url;
                        hr = spDocument->get_URL(&url);
                        if (hr != S_OK)
                        {
                            url = L"unknown";
                        }

                        CComBSTR title;
                        hr = spDocument->get_title(&title);
                        if (hr != S_OK)
                        {
                            title = L"";
                        }
                        
                        Info i;
                        i.hwnd = hwnd;
                        i.url = url;
                        i.title = title;
                        i.processName = ::PathFindFileNameW(processName);
                        instances.push_back(i);
                    }
                }
            }

            return TRUE;
        });

        return TRUE;
    });
    
    int length = (int)instances.size();
    Local<Array> arr = Nan::New<Array>(length);
    for (int i = 0; i < length; i++)
    {
        CStringA id;
        id.Format("%p", instances[i].hwnd);
        
        Local<Object> obj = Nan::New<Object>(); 
        Nan::Set(obj, Nan::New("id").ToLocalChecked(), Nan::New<String>(id).ToLocalChecked()); 
        Nan::Set(obj, Nan::New("url").ToLocalChecked(), Nan::New<String>(CStringA(instances[i].url)).ToLocalChecked()); 
        Nan::Set(obj, Nan::New("title").ToLocalChecked(), Nan::New<String>(CStringA(instances[i].title)).ToLocalChecked());
        Nan::Set(obj, Nan::New("processName").ToLocalChecked(), Nan::New<String>(CStringA(instances[i].processName)).ToLocalChecked());
                       
        Nan::Set(arr, i, obj);
    }
    
    info.GetReturnValue().Set(arr);
}

NAN_METHOD(setSecurityACLs)
{
    EnsureInitialized();
    if (info.Length() < 1 || !info[0]->IsString())
    {
        Nan::ThrowTypeError("Incorrect arguments - setSecurityACLs(filePath: string): boolean");
        return;
    }

    info.GetReturnValue().Set(false);

    String::Utf8Value path(info[0]->ToString());
    CString fullPath((char*)*path);
    
	// Check to make sure that the dll has the ACLs to load in an appcontainer
	// We're doing this here as the adapter has no setup script and should be xcopy deployable/removeable
	PACL pOldDACL = NULL, pNewDACL = NULL;
	PSECURITY_DESCRIPTOR pSD = NULL;
	EXPLICIT_ACCESS ea;
	SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION;

	// The check is done on the folder and should be inherited to all objects
	DWORD dwRes = GetNamedSecurityInfo(fullPath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD);

	// Get the SID for "ALL APPLICATION PACAKGES" since it is localized
	PSID pAllAppPackagesSID = NULL;
	bool bResult = ConvertStringSidToSid(L"S-1-15-2-1", &pAllAppPackagesSID);

	if (bResult)
	{
		// Initialize an EXPLICIT_ACCESS structure for the new ACE. 
		ZeroMemory(&ea, sizeof(EXPLICIT_ACCESS));
		ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
		ea.grfAccessMode = SET_ACCESS;
		ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;;
		ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
		ea.Trustee.ptstrName = (LPTSTR)pAllAppPackagesSID;

		// Create a new ACL that merges the new ACE into the existing DACL.
		dwRes = SetEntriesInAcl(1, &ea, pOldDACL, &pNewDACL);
		if (dwRes == ERROR_SUCCESS)
		{
			dwRes = SetNamedSecurityInfo(fullPath.GetBuffer(), SE_FILE_OBJECT, si, NULL, NULL, pNewDACL, NULL);
			if (dwRes == ERROR_SUCCESS)
			{
                info.GetReturnValue().Set(true);
			}
			else
			{
				// The ACL was not set, this isn't fatal as it only impacts IE in EPM and Edge and the user can set it manually
                Log("ERROR: Could not set ACL to allow access to Edge.\nYou can set the ACL manually by adding Read & Execute permissions for 'All APPLICATION PACAKGES' to each dll.");
			}
		}
	}
	else
	{
        Log("ERROR: Failed to get the SID for ALL_APP_PACKAGES.");
        Log("ERROR: Win32 error code: " + GetLastError());
	}

	if (pAllAppPackagesSID != NULL)
	{
		::LocalFree(pAllAppPackagesSID);
	}

	if (pSD != NULL)
	{
		::LocalFree((HLOCAL)pSD);
	}
	if (pNewDACL != NULL)
	{
		::LocalFree((HLOCAL)pNewDACL);
	}
}

NAN_METHOD(connectTo)
{
    EnsureInitialized();
    if (info.Length() < 1 || !info[0]->IsString())
    {
        Nan::ThrowTypeError("Incorrect arguments - connectTo(id: string): string");
        return;
    }
    
    String::Utf8Value id(info[0]->ToString());
    #pragma warning(disable: 4312) // truncation to int
    HWND hwnd = (HWND)::strtol((const char*)(*id), NULL, 16);
    #pragma warning(default: 4312)

    info.GetReturnValue().Set(Nan::Null());
    
    CComPtr<IHTMLDocument2> spDocument;
    HRESULT hr = Helpers::GetDocumentFromHwnd(hwnd, spDocument);
    if (hr == S_OK)
    {
        SYSTEM_INFO sys;
        ::GetNativeSystemInfo(&sys);
        bool is64BitOS = PROCESSOR_ARCHITECTURE_AMD64 == sys.wProcessorArchitecture;
        BOOL isWoWTab = FALSE;
        ::IsWow64Process(GetCurrentProcess(), &isWoWTab);
        bool is64BitTab = is64BitOS && !isWoWTab;
    
        CString path(m_rootPath);
        path.Append(L"\\..\\..\\lib\\");
        if (is64BitTab)
		{
			path.Append(L"Proxy64.dll");
	    }
		else
		{
			path.Append(L"Proxy.dll");
		}
        
        CComPtr<IOleWindow> spSite;
		hr = Helpers::StartDiagnosticsMode(spDocument, __uuidof(ProxySite), path, __uuidof(spSite), reinterpret_cast<void**>(&spSite.p));
		if (hr == E_ACCESSDENIED && is64BitTab && ::IsWindows8Point1OrGreater())
		{
            Log("ERROR: Access denied while attempting to connect to a 64 bit tab. The most common solution to this problem is to open an Administrator command prompt, navigate to the folder containing this adapter, and type \"icacls proxy64.dll /grant \"ALL APPLICATION PACKAGES\":(RX)\"");		
        }
		else if (hr == ::HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) && is64BitTab) 
        {
			Log("ERROR: Module could not be found. Ensure Proxy64.dll exists in the out\\lib\\ folder");
		}
		else if (hr == ::HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) && !is64BitTab) 
        {
			Log("ERROR: Module could not be found. Ensure Proxy.dll exists in the out\\lib\\ folder");
		}
		else if (hr != S_OK)
		{
            EXIT_IF_NOT_S_OK(hr);
        }
        else
        {
            // Success, return the new hwnd as an id to this instance
            HWND hwnd;
            hr = spSite->GetWindow(&hwnd);
            EXIT_IF_NOT_S_OK(hr);

            CStringA newId;
            newId.Format("%p", hwnd);
            info.GetReturnValue().Set(Nan::New<String>(newId).ToLocalChecked());
            
            if (!isMessageReceiverCreated)
            {
                isMessageReceiverCreated = true;
                Isolate* isolate = Isolate::GetCurrent();
                Local<Function> progress = Local<Function>::New(isolate, messageCallbackHandle);
                
                MessageReceiver* pMR = new MessageReceiver(new Nan::Callback(progress), new Nan::Callback(progress), hwnd, &m_proxyHwnd);
                m_proxyHwnd = pMR->m_hWnd;
                AsyncQueueWorker(pMR);
            }
        }
    }
}

NAN_METHOD(injectScriptTo)
{
    EnsureInitialized();
    if (info.Length() < 4 || !info[0]->IsString() || !info[1]->IsString() || !info[2]->IsString() || !info[3]->IsString())
    {
        Nan::ThrowTypeError("Incorrect arguments - injectScriptTo(instanceId: string, engine: string, filename: string, script: string): void");
        return;
    }
    
    String::Utf8Value edgeInstanceId(info[0]->ToString());
    #pragma warning(disable: 4312) // truncation to int
    HWND instanceHwnd = (HWND)::strtol((const char*)(*edgeInstanceId), NULL, 16);
    #pragma warning(default: 4312)
    
    String::Utf8Value engine(info[1]->ToString());
    String::Utf8Value filename(info[2]->ToString());
    String::Utf8Value script(info[3]->ToString());
    
    CStringA command;
    command.Format("inject:%s:%s:%s", (const char*)(*engine), (const char*)(*filename), (const char*)(*script));
    CString message(command);
    SendMessageToInstance(instanceHwnd, message);
}

NAN_METHOD(forwardTo)
{
    EnsureInitialized();
    if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsString())
    {
        Nan::ThrowTypeError("Incorrect arguments - forwardTo(instanceId: string, message: string): void");
        return;
    }
    
    String::Utf8Value edgeInstanceId(info[0]->ToString());
    #pragma warning(disable: 4312) // truncation to int
    HWND instanceHwnd = (HWND)::strtol((const char*)(*edgeInstanceId), NULL, 16);
    #pragma warning(default: 4312)
    
    String::Utf8Value actualMessage(info[1]->ToString());
    
    CStringA convertedMessage((const char*)(*actualMessage));
    CString message(convertedMessage);
    SendMessageToInstance(instanceHwnd, message);
}