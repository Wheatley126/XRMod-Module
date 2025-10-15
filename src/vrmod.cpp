#include <gmod/Interface.h>
#include <openxr/openxr.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <vector>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
	#include <shellapi.h>
	#include <d3d9.h>
	#include <d3d11.h>
	#define PATH_MAX MAX_PATH
	#define XR_USE_PLATFORM_WIN32
#else
	#include <GL/gl.h>
	#include <sys/mman.h>
	#include <dlfcn.h>
	#include <unistd.h>
	#include <time.h>
	#define XR_USE_TIMESPEC
#endif

#define XRMOD_MODULE_VERSION 1

#define MAX_STR_LEN     256
#define MAX_ACTIONS     64
#define MAX_ACTIONSETS  16
#define PI            3.141592653589793116

const double RAD2DEG = 180.0/PI;

enum ELuaRefIndex{
	LuaRefIndex_EmptyTable,
	LuaRefIndex_PoseTable,
	LuaRefIndex_HmdPose,
	LuaRefIndex_ActionTable,
	LuaRefIndex_Max,
};

typedef struct action {
	XrAction handle = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	//char fullname[MAX_STR_LEN];
	int luaRefs[2];
	char name[XR_MAX_ACTION_NAME_SIZE];
	XrActionType type;
} action;

typedef struct actionSet {
	XrActionSet handle = XR_NULL_HANDLE;
	char name[XR_MAX_ACTION_SET_NAME_SIZE];
} actionSet;

typedef struct {
	void* handle;
	int eType;
} vrTexture;

//#define PushMatrix(mtx) LUA->PushUserType(&(mtx.m), GarrysMod::Lua::Type::MATRIX);

// Placeholder for VMatrix in Source SDK
class VMatrix {
	public:
		float m[4][4];
};

const XrPosef XR_IDENTITYPOSE = { {0,0,0,1}, {0,0,0} };

XrInstance             g_Instance = XR_NULL_HANDLE;
XrSession              g_Session = XR_NULL_HANDLE;
bool					g_SessionStarted = false;
XrSystemId              g_SystemId = XR_NULL_SYSTEM_ID;

XrSpace					g_SpaceStage = XR_NULL_HANDLE;
XrSpace					g_SpaceView = XR_NULL_HANDLE;

actionSet               g_actionSets[MAX_ACTIONSETS];
int                     g_actionSetCount = 0;
XrActiveActionSet       g_activeActionSets[MAX_ACTIONSETS];
int                     g_activeActionSetCount = 0;
action                  g_actions[MAX_ACTIONS];
int                     g_actionCount = 0;

uint32_t g_TextureWidth = 0;
uint32_t g_TextureHeight = 0;
XrSwapchain				g_Swapchain = XR_NULL_HANDLE;
XrFrameState g_FrameState{XR_TYPE_FRAME_STATE,nullptr};

XrTime g_lastPredictedFrameTime = 0;
int predictionScale = 0; // Percentage of original prediction amount to use

int                     g_luaRefs[LuaRefIndex_Max];
int                     g_luaRefCount = 0;

char                    g_createTextureOrigBytes[14];

#ifdef _WIN32

	#define XR_USE_GRAPHICS_API_D3D11

	typedef HRESULT         (APIENTRY* CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	CreateTexture           g_createTexture = NULL;
	ID3D11Device*           g_d3d11Device = NULL;
	ID3D11DeviceContext*	g_d3d11DeviceContext = NULL;
	ID3D11Texture2D*        g_d3d11Texture = NULL;
	HANDLE                  g_sharedTexture = NULL;
	IDirect3DDevice9*       g_pD3D9Device = NULL;

	typedef void*           (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
	HRESULT APIENTRY CreateTextureHook(IDirect3DDevice9* pDevice, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** tex, HANDLE* shared_handle) {
		if (WriteProcessMemory(GetCurrentProcess(), g_createTexture, g_createTextureOrigBytes, 14, NULL) == 0)
			MessageBoxA(NULL, "WriteProcessMemory from hook failed", "", NULL);

		if (g_sharedTexture == NULL) {
			shared_handle = &g_sharedTexture;
			pool = D3DPOOL_DEFAULT;
		}

		return g_createTexture(pDevice, w, h, levels, usage, format, pool, tex, shared_handle);
	};

#else

	#define XR_USE_GRAPHICS_API_OPENGL

	typedef struct{
		void ClearEntryPoints();
		uint64_t m_nTotalGLCycles, m_nTotalGLCalls;
		int unknown1;
		int unknown2; 
		int m_nOpenGLVersionMajor; 
		int m_nOpenGLVersionMinor;  
		int m_nOpenGLVersionPatch;
		bool m_bHave_OpenGL;
		char *m_pGLDriverStrings[4];
		int m_nDriverProvider;        
		void *firstFunc;
	}COpenGLEntryPoints;

	typedef void *(*GL_GetProcAddressCallbackFunc_t)(const char *, bool &, const bool, void *);
	typedef COpenGLEntryPoints*(*GetOpenGLEntryPoints_t)(GL_GetProcAddressCallbackFunc_t callback);
	typedef void            (*glGenTextures_t)(GLsizei n, GLuint *textures);
	void*                   g_createTexture = NULL;
	GLuint                  g_sharedTexture = GL_INVALID_VALUE;
	COpenGLEntryPoints*     g_GL = NULL;

	void CreateTextureHook(GLsizei n, GLuint *textures) {
		memcpy((void*)g_createTexture, (void*)g_createTextureOrigBytes, 14);
		((glGenTextures_t)g_createTexture)(n, textures);
		g_sharedTexture = textures[0];

		return;
	}

#endif

#ifdef DEBUG
	#define XR_EXTENSION_PROTOTYPES
#endif
#include <openxr/openxr_platform.h>

#ifdef _WIN32
	PFN_xrConvertWin32PerformanceCounterToTimeKHR ConvertSysTimeToXrTime = nullptr;
#else
	PFN_xrConvertTimespecTimeToTimeKHR ConvertSysTimeToXrTime = nullptr;
#endif

// Swapchain image lists have to be defined after platform-specific info
#ifdef _WIN32
	std::vector<XrSwapchainImageD3D11KHR> g_SwapchainImages = {};
#else
	std::vector<XrSwapchainImageOpenGLKHR> g_SwapchainImages = {};
#endif

XrPath CreateXrPath(const char* pathString) {
	XrPath path;
	xrStringToPath(g_Instance,pathString,&path);
	return path;
}

PFN_xrVoidFunction getXRFunction(const char* name)
{
	PFN_xrVoidFunction func;

	if(XR_FAILED(xrGetInstanceProcAddr(g_Instance, name, &func)))
		return XR_NULL_HANDLE;
	
	return func;
}

void SetupPrototypeFunctions()
{
	#ifdef _WIN32
		ConvertSysTimeToXrTime = (PFN_xrConvertWin32PerformanceCounterToTimeKHR) getXRFunction("xrConvertWin32PerformanceCounterToTimeKHR");
	#else
		ConvertSysTimeToXrTime = (PFN_xrConvertTimespecTimeToTimeKHR) getXRFunction("xrConvertTimespecTimeToTimeKHR");
	#endif
}

void ClearPrototypeFunctions()
{
	ConvertSysTimeToXrTime = nullptr;
}

QAngle QuatToAngle(XrQuaternionf q) {
	float q0 = q.x;
    float q1 = q.y;
    float q2 = q.z;
    float q3 = q.w;

    float t2 = 2.0*(q0*q2 - q1*q3);
	if(t2 > 1.0)
		t2 = 1.0;
    else if(t2 < -1.0)
		t2 = -1.0;

	QAngle out;

    if(t2 == 1)
	{
        out.x = asin(t2);
        out.y = 0;
        out.z = -atan2(q0, q1);
	} else if(t2 == -1) {
        out.x = asin(t2);
        out.y = 0;
        out.z = atan2(q0, q1);
	} else {
        out.x = asin(t2);
        out.y = atan2(2.0*(q0*q1 + q2*q3), q0*q0 - q1*q1 - q2*q2 + q3*q3);
        out.z = atan2(2.0*(q0*q3 + q1*q2), q0*q0 + q1*q1 - q2*q2 - q3*q3);
	}

	// x -> z
	// y -> x
	// z -> y
	q0 = out.x;
	out.x = out.z * RAD2DEG;
	out.z = out.y * RAD2DEG;
	out.y = q0 * RAD2DEG;

    return out;
}

char* GetResultString(const char* form, XrResult result) {
	char resStr[MAX_STR_LEN];
	xrResultToString(g_Instance,result,resStr);

	char str[MAX_STR_LEN];
	snprintf(str, MAX_STR_LEN, form, resStr);
	return str;
}

void PrintConsoleText(char* str, GarrysMod::Lua::ILuaBase *LUA) {
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "print");
	LUA->PushString(str);
	LUA->PCall(1, 0, 0);
	LUA->Pop();
}



XrResult RefreshInstance() {
	XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO,nullptr};

	std::vector<const char*> extensions = {};

	#ifdef _WIN32
		extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
		extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	#else
		extensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
		extensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
	#endif
	#ifdef DEBUG
		extensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
	#endif

	createInfo.enabledExtensionCount = (uint32_t) extensions.size();
	createInfo.enabledExtensionNames = extensions.data();

	std::vector<const char*> apiLayers = {};
	#ifdef DEBUG
		//apiLayers.push_back("XR_APILAYER_LUNARG_core_validation");
	#endif
	createInfo.enabledApiLayerCount = apiLayers.size();
	createInfo.enabledApiLayerNames = apiLayers.data();

	XrApplicationInfo appInfo;
	appInfo.apiVersion = XR_CURRENT_API_VERSION;
	strcpy(appInfo.applicationName, "Garry's Mod");
	appInfo.applicationVersion = 1;
	strcpy(appInfo.engineName, "");
	appInfo.engineVersion = 0;

	createInfo.applicationInfo = appInfo;
	createInfo.createFlags = 0;

	XrResult result = xrCreateInstance(&createInfo,&g_Instance);
	if(XR_FAILED(result))
		return result;
	
	SetupPrototypeFunctions();
	return result;
}

#ifdef DEBUG
GarrysMod::Lua::ILuaBase* debugLuaHandle = nullptr;
XrDebugUtilsMessengerEXT debugGlobalMessenger = XR_NULL_HANDLE;
XrBool32 handleXRError(
	XrDebugUtilsMessageSeverityFlagsEXT severity,
	XrDebugUtilsMessageTypeFlagsEXT type,
	const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
	void* userData
)
{
	char output[MAX_STR_LEN] = "XRMod DEBUG: ";

	switch (type)
	{
		case XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT :
		{
			strcat(output,"General ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT :
		{
			strcat(output,"Validation ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT :
		{
			strcat(output,"Performance ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT :
		{
			strcat(output,"Conformance ");
			break;
		}
	}

	switch (severity)
	{
		case XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT :
		{
			strcat(output,"(Verbose) ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT :
		{
			strcat(output,"(Info) ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT :
		{
			strcat(output,"(Warning) ");
			break;
		}
		case XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT :
		{
			strcat(output,"(Error) ");
			break;
		}
	}

	strcat(output,callbackData->message);
	if(debugLuaHandle != nullptr)
		PrintConsoleText(output,debugLuaHandle);

	return XR_FALSE;
}

bool CreateDebugMessenger(GarrysMod::Lua::ILuaBase *LUA)
{
	XrDebugUtilsMessengerEXT debugMessenger;

    XrDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo{XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,nullptr};
    debugMessengerCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugMessengerCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
    debugMessengerCreateInfo.userCallback = (PFN_xrDebugUtilsMessengerCallbackEXT) handleXRError;
    debugMessengerCreateInfo.userData = nullptr;

	PFN_xrCreateDebugUtilsMessengerEXT xrCreateDebugUtilsMessengerEXT = (PFN_xrCreateDebugUtilsMessengerEXT) getXRFunction("xrCreateDebugUtilsMessengerEXT");

	if(XR_FAILED(xrCreateDebugUtilsMessengerEXT(g_Instance, &debugMessengerCreateInfo, &debugMessenger)))
	{
		PrintConsoleText("XRMod Error: Failed to create debug messenger",LUA);
		return false;
	}

	debugGlobalMessenger = debugMessenger;
	return true;
}
#endif

void ClearSession(GarrysMod::Lua::ILuaBase *LUA) {
	if(g_Session != XR_NULL_HANDLE)
	{
		xrEndSession(g_Session);
		xrDestroySession(g_Session);
		g_Session = XR_NULL_HANDLE;
		g_SessionStarted = false;
	}

	for(int i = 0; i < g_actionCount; i++)
	{
		action* act = &g_actions[i];

		xrDestroyAction(act->handle);
		act->handle = XR_NULL_HANDLE;
		strcpy(act->name,"");

		if(act->space != XR_NULL_HANDLE)
		{
			xrDestroySpace(act->space);
			act->space = XR_NULL_HANDLE;
		}

		for(int j = 0; j < 2; j++)
			LUA->ReferenceFree(act->luaRefs[j]);
	}
	g_actionCount = 0;

	for(int i = 0; i < g_actionSetCount; i++)
	{
		actionSet* set = &g_actionSets[i];

		xrDestroyActionSet(set->handle);
		set->handle = XR_NULL_HANDLE;
		strcpy(set->name,"");
	}
	g_actionSetCount = 0;

	for(int i = 0; i < g_activeActionSetCount; i++)
	{
		g_activeActionSets[i].actionSet = XR_NULL_HANDLE;
		g_activeActionSets[i].subactionPath = XR_NULL_PATH;
	}
	g_activeActionSetCount = 0;


	if(g_SpaceStage != XR_NULL_HANDLE)
	{
		xrDestroySpace(g_SpaceStage);
		g_SpaceStage = XR_NULL_HANDLE;
	}

	if(g_SpaceView != XR_NULL_HANDLE)
	{
		xrDestroySpace(g_SpaceView);
		g_SpaceView = XR_NULL_HANDLE;
	}

	if(g_Swapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(g_Swapchain);
		g_Swapchain = XR_NULL_HANDLE;
	}
	
	g_SwapchainImages.clear();

	g_FrameState.shouldRender = XR_FALSE;
	g_FrameState.predictedDisplayPeriod = 0;
	g_FrameState.predictedDisplayTime = 0;

	g_lastPredictedFrameTime = 0;


	for(int i = 0; i < g_luaRefCount; i++)
		LUA->ReferenceFree(g_luaRefs[i]);
	g_luaRefCount = 0;

	#ifdef _WIN32
		if (g_d3d11Device != NULL) {
			g_d3d11DeviceContext->Release();
			g_d3d11DeviceContext = NULL;
			g_d3d11Device->Release();
			g_d3d11Device = NULL;
		}

		g_d3d11Texture = NULL;
		g_pD3D9Device = NULL;
		g_sharedTexture = NULL;
	#else
		g_sharedTexture = GL_INVALID_VALUE;
	#endif

	g_SystemId = XR_NULL_SYSTEM_ID;
}

// Doesn't need changing
LUA_FUNCTION(GetVersion) {
	LUA->PushNumber(XRMOD_MODULE_VERSION);

	return 1;
}

// Done
LUA_FUNCTION(IsHMDPresent) {
	if(g_Instance == XR_NULL_HANDLE) {
		if(XR_FAILED(RefreshInstance()))
		{
			LUA->PushBool(false);
			return 1;
		}

		#ifdef DEBUG
			CreateDebugMessenger(LUA);
		#endif
	}

	XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO,nullptr,XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};

	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	LUA->PushBool(xrGetSystem(g_Instance,&getInfo,&systemId) == XR_SUCCESS);

	return 1;
}

XrResult GetGraphicsRequirements()
{
	#ifdef _WIN32
		PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
		XrResult result = xrGetInstanceProcAddr(g_Instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&xrGetD3D11GraphicsRequirementsKHR);

		if (result != XR_SUCCESS)
			return result;

		XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR,nullptr};
		return xrGetD3D11GraphicsRequirementsKHR(g_Instance, g_SystemId, &requirements);
	#else
		PFN_xrGetOpenGLGraphicsRequirementsKHR xrGetOpenGLGraphicsRequirementsKHR = nullptr;
		XrResult result = xrGetInstanceProcAddr(g_Instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&xrGetOpenGLGraphicsRequirementsKHR);

		if (result != XR_SUCCESS)
			return result;

		XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR,nullptr};
		return xrGetOpenGLGraphicsRequirementsKHR(g_Instance, g_SystemId, &requirements);
	#endif
}

void AcquireRenderDevice(GarrysMod::Lua::ILuaBase *LUA) {
	#ifdef _WIN32
		if (D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &g_d3d11Device, NULL, &g_d3d11DeviceContext) != S_OK)
			LUA->ThrowError("D3D11CreateDevice failed");
	#else
		// TODO: Is this needed for OpenGL?
	#endif
}

void InitializeSession(GarrysMod::Lua::ILuaBase *LUA) {
	if (g_Session != XR_NULL_HANDLE)
		LUA->ThrowError("XRMod Error: Session already active");
	if (g_SystemId == XR_NULL_SYSTEM_ID)
		LUA->ThrowError("XRMod Error: SystemID is invalid");

	XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
	createInfo.createFlags = 0;

	XrResult result = GetGraphicsRequirements();
	if(result != XR_SUCCESS)
		LUA->ThrowError(GetResultString("XRMod Error: Failed to retrieve graphics requirements (%s)",result));

	#ifdef _WIN32
		XrGraphicsBindingD3D11KHR graphics{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR,nullptr};
		graphics.device = g_d3d11Device;

		createInfo.next = &graphics;
	#else
		// TODO: Figure out what to use in place of this (perhaps g_createTextureOrigBytes?)
		//createInfo.next = nullptr;
	#endif
	createInfo.systemId = g_SystemId;

	result = xrCreateSession(g_Instance, &createInfo, &g_Session);
	if (result != XR_SUCCESS)
		LUA->ThrowError(GetResultString("XRMod Error: Failed to create session (%s)",result));

	XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,nullptr};
	spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	spaceCreateInfo.poseInReferenceSpace = {
		{-0.5f, 0.5f, 0.5f, 0.5f}, // Rotates coordinates to match source orientation
		{0.f, 0.f, 0.f}
	};

	result = xrCreateReferenceSpace(g_Session,&spaceCreateInfo,&g_SpaceStage);
	if (result != XR_SUCCESS) {
		xrDestroySession(g_Session);
		g_Session = XR_NULL_HANDLE;
		LUA->ThrowError(GetResultString("XRMod Error: Failed to create stage reference space (%s)",result));
	}

	spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	spaceCreateInfo.poseInReferenceSpace = XR_IDENTITYPOSE;

	result = xrCreateReferenceSpace(g_Session,&spaceCreateInfo,&g_SpaceView);
	if (result != XR_SUCCESS) {
		xrDestroySession(g_Session);
		g_Session = XR_NULL_HANDLE;
		LUA->ThrowError(GetResultString("XRMod Error: Failed to create view reference space (%s)",result));
	}

	for(int i = 0; i < LuaRefIndex_Max; i++){
		LUA->CreateTable();
		g_luaRefs[i] = LUA->ReferenceCreate();
		g_luaRefCount++;
	}
}

// Hopefully this works
LUA_FUNCTION(Init) {
	if (g_Instance == XR_NULL_HANDLE)
	{
		XrResult result = RefreshInstance();
		if(result != XR_SUCCESS)
		{
			char str[MAX_STR_LEN];
			snprintf(str, MAX_STR_LEN, "XRMod Error: Instance creation failed (Error Code %d)", (int) result);
			LUA->ThrowError(str);
		}

		#ifdef DEBUG
			CreateDebugMessenger(LUA);
		#endif
	}

	XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO,nullptr};
	getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	if(xrGetSystem(g_Instance,&getInfo,&g_SystemId) != XR_SUCCESS)
		LUA->ThrowError("XRMod Error: Failed to get system info. Is your HMD connected?");

	#ifdef _WIN32
		HMODULE hMod = GetModuleHandleA("shaderapidx9.dll");
		if (hMod == NULL) LUA->ThrowError("GetModuleHandleA failed");
		CreateInterfaceFn CreateInterface = (CreateInterfaceFn)GetProcAddress(hMod, "CreateInterface");
		if (CreateInterface == NULL) LUA->ThrowError("GetProcAddress failed");

		# ifdef _WIN64
			DWORD_PTR fnAddr = ((DWORD_PTR**)CreateInterface("ShaderDevice001", NULL))[0][5];
			g_pD3D9Device = *(IDirect3DDevice9**)(fnAddr + 8 + (*(DWORD_PTR*)(fnAddr + 3) & 0xFFFFFFFF));
		# else
			g_pD3D9Device = **(IDirect3DDevice9***)(((DWORD_PTR**)CreateInterface("ShaderDevice001", NULL))[0][5] + 2);
		# endif

		g_createTexture = ((CreateTexture**)g_pD3D9Device)[0][23];
	#else
		# ifdef __x86_64__
			void *lib = dlopen("libtogl_client.so", RTLD_NOW | RTLD_NOLOAD);
		# else
			void *lib = dlopen("libtogl.so", RTLD_NOW | RTLD_NOLOAD);
		# endif

		if(lib==NULL)
			LUA->ThrowError("dlopen fail");

		GetOpenGLEntryPoints_t GetOpenGLEntryPoints = (GetOpenGLEntryPoints_t)dlsym(lib, "GetOpenGLEntryPoints");

		if(GetOpenGLEntryPoints==NULL)
			LUA->ThrowError("dlsym fail");

		g_GL = GetOpenGLEntryPoints(NULL);
		dlclose(lib);

		# ifdef __x86_64__
			g_createTexture = *((void**)&g_GL->firstFunc+50);
		# else
			g_createTexture = *((void**)&g_GL->firstFunc+48);
		# endif
	#endif

	AcquireRenderDevice(LUA);
	InitializeSession(LUA);

	return 0;
}

LUA_FUNCTION(CreateActionSet) {
	LUA->CheckType(-3,GarrysMod::Lua::Type::STRING); // setName
	LUA->CheckType(-2,GarrysMod::Lua::Type::STRING); // localizedSetName
	LUA->CheckType(-1,GarrysMod::Lua::Type::TABLE); // actions

	const char* setName = LUA->GetString(-3);
	actionSet* set = nullptr;
	for(int i = 0; i < g_actionSetCount; i++)
	{
		if(strcmp(g_actionSets[i].name, setName) == 0)
		{
			set = &g_actionSets[i];
			break;
		}
	}

	// Action set does not exist
	if(set == nullptr)
	{
		if(g_actionSetCount >= MAX_ACTIONSETS)
			LUA->ThrowError("XRMod Error: Failed to create action set (Max has been reached)");

		set = &g_actionSets[g_actionSetCount];

		XrActionSetCreateInfo createInfo{XR_TYPE_ACTION_SET_CREATE_INFO,nullptr};
		strncpy(createInfo.actionSetName,setName,XR_MAX_ACTION_SET_NAME_SIZE);
		strncpy(createInfo.localizedActionSetName,LUA->GetString(-2),XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
		createInfo.priority = 0;

		XrResult result = xrCreateActionSet(g_Instance,&createInfo,&(set->handle));
		if(result != XR_SUCCESS)
			LUA->ThrowError(GetResultString("XRMod Error: Failed to create action set (%s)",result));
		
		strncpy(set->name,setName,XR_MAX_ACTION_SET_NAME_SIZE);
		g_actionSetCount++;
	}

	// -1: ActionsTable
	// -2: SetName

	LUA->PushNil(); // First Key in table
	// -1: nil
	// -2: ActionsTable
	// -3: SetName
	while(LUA->Next(-2) != 0) {
		// -1: Value (subtable)
		// -2: Key (replaces nil)
		// -3: ActionsTable
		// -4: SetName
		bool failed = false;
		if(g_actionCount >= MAX_ACTIONS)
		{
			failed = true;
			PrintConsoleText("XRMod: Cannot create any more actions (limit has been reached)",LUA);
		}

		XrActionCreateInfo createInfo{XR_TYPE_ACTION_CREATE_INFO,nullptr};
		createInfo.countSubactionPaths = 0;
		createInfo.subactionPaths = nullptr;

		if(LUA->GetType(-2) == GarrysMod::Lua::Type::STRING) // Key of this table (action name)
			strncpy(createInfo.actionName, LUA->GetString(-2), XR_MAX_ACTION_NAME_SIZE);
		else
		{
			failed = true;
			PrintConsoleText("Failed to create action: Name is not a string", LUA);
		}

		LUA->GetField(-1,"type");
		// -1: Value2 (value of "type")
		// -2: Value (subtable)
		// -3: Key (replaces nil)
		// -4: ActionsTable
		// -5: SetName
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::STRING)
		{
			const char* strType = LUA->GetString(-1);

			if(strcmp(strType, "boolean") == 0)
				createInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
			else if(strcmp(strType, "float") == 0)
				createInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
			else if(strcmp(strType, "vector2f") == 0)
				createInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
			else if(strcmp(strType, "pose") == 0)
				createInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
			else
			{
				failed = true;
				PrintConsoleText("XRMod: Failed to create action: Invalid action type", LUA);
			}
		} else
		{
			failed = true;
			PrintConsoleText("XRMod: Failed to create action: Type is not a string", LUA);
		}
		LUA->Pop();
		// -1: Value (subtable)
		// -2: Key (replaces nil)
		// -3: ActionsTable
		// -4: SetName

		LUA->GetField(-1,"localizedActionName");
		// Same order as previous
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::STRING)
			strncpy(createInfo.localizedActionName, LUA->GetString(-1), XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
		else
		{
			failed = true;
			PrintConsoleText("XRMod: Failed to create action: Localized name is not a string", LUA);
		}
		LUA->Pop();

		LUA->Pop(); // Pop this subtable to prepare for next iteration
		// -1: Key (replaces nil)
		// -2: ActionsTable
		// -3: SetName

		if(!failed)
		{
			action* act = &g_actions[g_actionCount];
			XrResult result = xrCreateAction(set->handle,&createInfo,&(act->handle));
			if(XR_FAILED(result))
				LUA->ThrowError(GetResultString("XRMod Error: Failed to create action (%s)",result));

			act->type = createInfo.actionType;
			strncpy(act->name, createInfo.actionName, XR_MAX_ACTION_NAME_SIZE);

			if(createInfo.actionType == XR_ACTION_TYPE_POSE_INPUT)
			{
				XrActionSpaceCreateInfo spaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO,nullptr};
				spaceCreateInfo.action = act->handle;
				spaceCreateInfo.poseInActionSpace = XR_IDENTITYPOSE;
				spaceCreateInfo.subactionPath = XR_NULL_PATH;

				result = xrCreateActionSpace(g_Session,&spaceCreateInfo,&(act->space));
				if(XR_FAILED(result))
					PrintConsoleText(GetResultString("XRMod Error: Failed to create action space (%s)",result),LUA);
			} else act->space = XR_NULL_HANDLE;

			for(int i = 0; i < 2; i++) {
				LUA->CreateTable();
				act->luaRefs[i] = LUA->ReferenceCreate();
			}

			g_actionCount++;
		}
	}

	return 0;
}

action* GetActionFromName(const char* actionName) {
	for(int i = 0; i < g_actionCount; i++) {
		if(strcmp(g_actions[i].name,actionName) == 0)
			return &g_actions[i];
	}

	return nullptr;
}

LUA_FUNCTION(SuggestBindings) {
	LUA->CheckType(-2,GarrysMod::Lua::Type::STRING);
	LUA->CheckType(-1,GarrysMod::Lua::Type::TABLE);

	XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,nullptr};
	suggestedBindings.countSuggestedBindings = 0;
	suggestedBindings.interactionProfile = CreateXrPath(LUA->GetString(-2));

	std::vector<XrActionSuggestedBinding> bindings = {};

	LUA->PushNil();
	while(LUA->Next(-2) != 0) {
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::STRING && LUA->GetType(-2) == GarrysMod::Lua::Type::STRING)
		{
			action* act = GetActionFromName(LUA->GetString(-2));
			if(act == nullptr)
			{
				char str[MAX_STR_LEN];
				snprintf(str, MAX_STR_LEN, "XRMod: Failed to find action to bind '%s'", LUA->GetString(-2));
				PrintConsoleText(str, LUA);
				LUA->Pop();
				continue;
			}

			XrActionSuggestedBinding binding;
			binding.action = act->handle;
			binding.binding = CreateXrPath(LUA->GetString(-1));
			bindings.push_back(binding);

			suggestedBindings.countSuggestedBindings++;
		}

		LUA->Pop();
	}
	suggestedBindings.suggestedBindings = bindings.data();

	XrResult result = xrSuggestInteractionProfileBindings(g_Instance,&suggestedBindings);
	if(XR_FAILED(result))
		PrintConsoleText(GetResultString("XRMod: Failed to suggest interaction profile bindings (%s)",result),LUA);

	return 0;
}

LUA_FUNCTION(SetActiveActionSets) {
	if(g_Instance == XR_NULL_HANDLE)
		LUA->ThrowError("XRMod Error: Invalid Instance");

	g_activeActionSetCount = 0;

	// Loops through all action sets and sets them to the provided arguments (if provided)
	for (int i = 0; i < MAX_ACTIONSETS; i++) {
		if (LUA->GetType(i + 1) == GarrysMod::Lua::Type::STRING) {
			const char* actionSetName = LUA->CheckString(i + 1);

			// Find the action set's index in g_actionSets
			int actionSetIndex = -1;
			for (int j = 0; j < g_actionSetCount; j++) {
				if (strcmp(actionSetName, g_actionSets[j].name) == 0) {
					g_activeActionSets[g_activeActionSetCount].actionSet = g_actionSets[j].handle;
					g_activeActionSets[g_activeActionSetCount].subactionPath = XR_NULL_PATH;
					g_activeActionSetCount++;
					break;
				}
			}
		}
		else {
			break;
		}
	}

	return 0;
}

void PushMatrixAsTable(GarrysMod::Lua::ILuaBase* LUA, VMatrix* mtx) {
	LUA->CreateTable();

	for (unsigned int row = 0; row < 4; row++) {
		LUA->PushNumber(row + 1);
		LUA->CreateTable();

		for (unsigned int col = 0; col < 4; col++) {
			LUA->PushNumber(col+1);
			LUA->PushNumber(mtx->m[row][col]);
			LUA->SetTable(-3);
		}
		LUA->SetTable(-3);
	}
}

VMatrix* PushNewMatrix(GarrysMod::Lua::ILuaBase* LUA)
{
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "Matrix");
	LUA->Remove(-2);

	LUA->PCall(0, 1, 0);
	return LUA->GetUserType<VMatrix>(-1, GarrysMod::Lua::Type::MATRIX);
}

void ComposeProjection(XrFovf* fov, float zNear, float zFar, VMatrix *mtx)
{
	float fLeft = tan(fov->angleLeft);
	float fRight = tan(fov->angleRight);
	float fTop = tan(fov->angleUp);
	float fBottom = tan(fov->angleDown);

	float idx = 1.0f / (fRight - fLeft);
	float idy = 1.0f / (fBottom - fTop);
	float idz = 1.0f / (zFar - zNear);
	float sx = fRight + fLeft;
	float sy = fBottom + fTop;

	mtx->m[0][0] = 2*idx; mtx->m[0][1] = 0;     mtx->m[0][2] = sx*idx;    mtx->m[0][3] = 0;
	mtx->m[1][0] = 0;     mtx->m[1][1] = 2*idy; mtx->m[1][2] = sy*idy;    mtx->m[1][3] = 0;
	mtx->m[2][0] = 0;     mtx->m[2][1] = 0;     mtx->m[2][2] = -zFar*idz; mtx->m[2][3] = -zFar*zNear*idz;
	mtx->m[3][0] = 0;     mtx->m[3][1] = 0;     mtx->m[3][2] = -1.0f;     mtx->m[3][3] = 0;
}

float dot(Vector* a, Vector* b)
{
	return a->x*b->x + a->y*b->y + a->z*b->z;
}

Vector cross(Vector* a, Vector* b)
{
	Vector out;
	out.x = a->y * b->z - a->z * b->y;
	out.y = a->z * b->x - a->x * b->z;
	out.z = a->x * b->y - a->y * b->x;
	return out;
}

Vector operator *(float f, Vector v)
{
	v.x *= f;
	v.y *= f;
	v.z *= f;
	return v;
}

Vector operator +(Vector a, Vector b)
{
	a.x += b.x;
	a.y += b.y;
	a.z += b.z;
	return a;
}

void RotateVector(Vector* v, XrQuaternionf q, Vector* out)
{
	// Extract the vector part of the quaternion
    Vector qv;
	qv.x = q.x;
	qv.y = q.y;
	qv.z = q.z;
	Vector* u = &qv;

    // Extract the scalar part of the quaternion
    float s = q.w;

    // Do the math
    *out = 2.0f * dot(u, v) * qv
           + (s*s - dot(u, u)) * *v
           + 2.0f * s * cross(u, v);
}


Vector GetForwardVec()
{
	Vector v;
	v.x = 0;
	v.y = 0;
	v.z = -1;
	return v;
}

Vector GetLeftVec()
{
	Vector v;
	v.x = -1;
	v.y = 0;
	v.z = 0;
	return v;
}

Vector GetUpVec()
{
	Vector v;
	v.x = 0;
	v.y = 1;
	v.z = 0;
	return v;
}

const float XrToSource = 1/0.01905;
const float SourceToXr = 0.01905;

void ComposeTransform(XrPosef pose, VMatrix *mtx)
{
	// Translation Component
	mtx->m[0][3] = pose.position.x;
	mtx->m[1][3] = pose.position.y;
	mtx->m[2][3] = pose.position.z;

	// Rotation Component
	// Extract the values from Q
    XrQuaternionf q = pose.orientation;

	Vector out;

	Vector vec = GetForwardVec();
	RotateVector(&vec, q, &out);
	mtx->m[0][0] = out.x;
	mtx->m[1][0] = out.y;
	mtx->m[2][0] = out.z;

	vec = GetLeftVec();
	RotateVector(&vec, q, &out);
	mtx->m[0][1] = out.x;
	mtx->m[1][1] = out.y;
	mtx->m[2][1] = out.z;

	vec = GetUpVec();
	RotateVector(&vec, q, &out);
	mtx->m[0][2] = out.x;
	mtx->m[1][2] = out.y;
	mtx->m[2][2] = out.z;

	// Bottom row
	mtx->m[3][0] = 0;
	mtx->m[3][1] = 0;
	mtx->m[3][2] = 0;
	mtx->m[3][3] = 1;
}

// Done
LUA_FUNCTION(GetDisplayInfo) {
	std::vector<XrViewConfigurationView> viewConfigs;
	viewConfigs.resize(2);
	// We'll get an XR validation error if we don't initialize these
	viewConfigs[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW,nullptr};
	viewConfigs[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW,nullptr};

	uint32_t viewCount = 2;

	XrResult result = xrEnumerateViewConfigurationViews(g_Instance,g_SystemId,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,viewCount,&viewCount,viewConfigs.data());
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to enumerate view configurations (%s)",result));
	
	if(viewCount != 2 /*|| viewConfigs[0].recommendedImageRectWidth != viewConfigs[1].recommendedImageRectWidth || viewConfigs[0].recommendedImageRectHeight != viewConfigs[1].recommendedImageRectHeight*/)
		LUA->ThrowError("XRMod Error: View count is not 2");

	g_TextureWidth = viewConfigs[0].recommendedImageRectWidth;
	g_TextureHeight = viewConfigs[0].recommendedImageRectHeight;

	float fNearZ = (float)LUA->CheckNumber(1);
	float fFarZ = (float)LUA->CheckNumber(2);

	XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO,nullptr};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.space = g_SpaceView;
	viewLocateInfo.displayTime = 11000000; // 11ms in nanoseconds (~90 fps)

	XrViewState viewState{XR_TYPE_VIEW_STATE,nullptr,0};

	uint32_t viewCountOutput = 0;
	XrView views[2] = { {XR_TYPE_VIEW,nullptr},{XR_TYPE_VIEW,nullptr} };
	result = xrLocateViews(g_Session,&viewLocateInfo,&viewState,viewCount,&viewCountOutput,(XrView *) &views);
	if(result != XR_SUCCESS)
		LUA->ThrowError(GetResultString("XRMod Error: Failed to locate views (%s)",result));

	LUA->CreateTable();

	LUA->PushNumber(g_TextureWidth);
	LUA->SetField(-2, "RecommendedWidth");
	LUA->PushNumber(g_TextureHeight);
	LUA->SetField(-2, "RecommendedHeight");

	VMatrix* transformLeft = PushNewMatrix(LUA);
	ComposeTransform(views[0].pose, transformLeft);
	LUA->SetField(-2, "TransformLeft");

	VMatrix* transformRight = PushNewMatrix(LUA);
	ComposeTransform(views[1].pose, transformRight);
	LUA->SetField(-2, "TransformRight");

	double hFov = abs(views[0].fov.angleRight - views[0].fov.angleLeft);
	double vFov = abs(views[0].fov.angleUp - views[0].fov.angleDown);
	double aspect = tan(hFov / 2) / tan(vFov / 2);

	LUA->PushNumber(aspect);
	LUA->SetField(-2, "AspectLeft");

	LUA->PushNumber(hFov * RAD2DEG);
	LUA->SetField(-2, "FovLeft");

	// TODO: Try using the max angle instead
	hFov = abs(views[1].fov.angleRight - views[1].fov.angleLeft);
	vFov = abs(views[1].fov.angleUp - views[1].fov.angleDown);
	aspect = tan(hFov / 2) / tan(vFov / 2);

	LUA->PushNumber(aspect);
	LUA->SetField(-2, "AspectRight");

	LUA->PushNumber(hFov * RAD2DEG);
	LUA->SetField(-2, "FovRight");

	// Asymmetric FOV offsets
	double fLeft = tan(views[0].fov.angleLeft);
	double fRight = tan(views[0].fov.angleRight);
	double fTop = tan(views[0].fov.angleUp);
	double fBottom = tan(views[0].fov.angleDown);

	double offset = (fLeft + fRight) / (fRight - fLeft);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetXLeft");

	offset = (fBottom + fTop) / (fTop - fBottom);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetYLeft");

	fLeft = tan(views[1].fov.angleLeft);
	fRight = tan(views[1].fov.angleRight);
	fTop = tan(views[1].fov.angleUp);
	fBottom = tan(views[1].fov.angleDown);

	offset = (fLeft + fRight) / (fRight - fLeft);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetXRight");

	offset = (fBottom + fTop) / (fTop - fBottom);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetYRight");

	return 1;
}

// Done
LUA_FUNCTION(UpdatePosesAndActions) {
	if(g_Session == XR_NULL_HANDLE)
		return 0;

	XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO,nullptr};
	syncInfo.activeActionSets = (XrActiveActionSet*) &g_activeActionSets;
	syncInfo.countActiveActionSets = g_activeActionSetCount;

	XrResult result = xrSyncActions(g_Session,&syncInfo);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to sync actions (%s)",result));

	return 0;
}

// Done
LUA_FUNCTION(GetPoses) {
	LUA->ReferencePush(g_luaRefs[LuaRefIndex_PoseTable]);

	for(int i = -1; i < g_actionCount; i++)
	{
		if(i != -1 && g_actions[i].type != XR_ACTION_TYPE_POSE_INPUT) continue;

		XrPosef pose;
		char name[MAX_STR_LEN];

		if(i == -1) // HMD Pose
		{
			XrSpaceLocation location{XR_TYPE_SPACE_LOCATION,nullptr};
			XrResult result = xrLocateSpace(g_SpaceView,g_SpaceStage,g_FrameState.predictedDisplayTime,&location);
			if(XR_FAILED(result))
				LUA->ThrowError(GetResultString("XRMod Error: Failed to locate view space (%s)",result));
			pose = location.pose;

			strcpy(name,"hmd");
			LUA->ReferencePush(g_luaRefs[LuaRefIndex_HmdPose]);

		} else {
			action* act = &g_actions[i];

			XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO,nullptr};
			getInfo.action = act->handle;

			XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE,nullptr};
			XrResult result = xrGetActionStatePose(g_Session,&getInfo,&state);
			if(XR_FAILED(result))
				LUA->ThrowError(GetResultString("XRMod Error: Failed to retrieve pose state (%s)",result));
			
			if(state.isActive)
			{
				XrSpaceLocation location{XR_TYPE_SPACE_LOCATION,nullptr};	
				result = xrLocateSpace(act->space,g_SpaceStage,g_FrameState.predictedDisplayTime,&location);
				if(XR_FAILED(result))
					LUA->ThrowError(GetResultString("XRMod Error: Failed to locate action space (%s)",result));
				pose = location.pose;

				strcpy(name,act->name);
				LUA->ReferencePush(act->luaRefs[0]);
			} else continue;
		}

		// TODO: Implement velocities later
		VMatrix* mtx = PushNewMatrix(LUA);
		ComposeTransform(pose, mtx);
		LUA->SetField(-2, "pose");

		Vector vec;
		vec.x = vec.y = vec.z = 0.f;
		LUA->PushVector(vec);
		LUA->SetField(-2, "vel");

		QAngle ang;
		ang.x = ang.y = ang.z = 0.f;
		LUA->PushAngle(ang);
		LUA->SetField(-2, "ang");
		LUA->PushAngle(ang);
		LUA->SetField(-2, "angvel");

		LUA->SetField(-2, name);
	}

	return 1;
}

// Mostly done
LUA_FUNCTION(GetActions) {
	char* changedActionNames[MAX_ACTIONS];
	bool changedActionStates[MAX_ACTIONS];
	int changedActionCount = 0;

	LUA->ReferencePush(g_luaRefs[LuaRefIndex_ActionTable]);

	for (int i = 0; i < g_actionCount; i++) {
		action* act = &g_actions[i];

		XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO,nullptr};
		getInfo.action = act->handle;

		switch(act->type) {
			case XR_ACTION_TYPE_BOOLEAN_INPUT:
			{
				XrActionStateBoolean stateBool{XR_TYPE_ACTION_STATE_BOOLEAN,nullptr};
				xrGetActionStateBoolean(g_Session,&getInfo,&stateBool);
				LUA->PushBool(stateBool.currentState);
				LUA->SetField(-2, act->name);

				if(stateBool.changedSinceLastSync){
					changedActionNames[changedActionCount] = act->name;
					changedActionStates[changedActionCount] = stateBool.currentState;
					changedActionCount++;
				}
				break;
			}
			case XR_ACTION_TYPE_FLOAT_INPUT:
			{
				XrActionStateFloat stateFloat{XR_TYPE_ACTION_STATE_FLOAT,nullptr};
				xrGetActionStateFloat(g_Session,&getInfo,&stateFloat);

				LUA->PushNumber(stateFloat.currentState);
				LUA->SetField(-2, act->name);
				break;
			}
			case XR_ACTION_TYPE_VECTOR2F_INPUT:
			{
				XrActionStateVector2f stateVector2{XR_TYPE_ACTION_STATE_VECTOR2F,nullptr};
				xrGetActionStateVector2f(g_Session,&getInfo,&stateVector2);
				LUA->ReferencePush(act->luaRefs[0]);

				LUA->PushNumber(stateVector2.currentState.x);
				LUA->SetField(-2, "x");
				LUA->PushNumber(stateVector2.currentState.y);
				LUA->SetField(-2, "y");
				LUA->SetField(-2, act->name);
				break;
			}
			// TODO: Implement finger poses
			/*case ActionType_Skeleton:
			{
				// TODO: Implement hand tracking
				g_pInput->GetSkeletalSummaryData(g_actions[i].handle, &skeletalSummaryData);

				LUA->ReferencePush(g_actions[i].luaRefs[0]);
				LUA->ReferencePush(g_actions[i].luaRefs[1]);

				for (int j = 0; j < 5; j++) {
					LUA->PushNumber(j + 1);
					LUA->PushNumber(skeletalSummaryData.flFingerCurl[j]);
					LUA->SetTable(-3);
				}

				LUA->SetField(-2, "fingerCurls");
				LUA->SetField(-2, g_actions[i].name);
				break;
			}*/
		}
	}

	if (changedActionCount == 0){
		LUA->ReferencePush(g_luaRefs[LuaRefIndex_EmptyTable]);
	}else{
		LUA->CreateTable();

		for(int i = 0; i < changedActionCount; i++){
			LUA->PushBool(changedActionStates[i]);
			LUA->SetField(-2,changedActionNames[i]);
		}
	}

	return 2;
}

// Done
LUA_FUNCTION(ShareTextureBegin) {
	char patch[] = "\x68\x0\x0\x0\x0\xC3\x44\x24\x04\x0\x0\x0\x0\xC3";
	*(uint32_t*)(patch + 1) = (uint32_t)((uintptr_t)CreateTextureHook);

	#if defined _WIN64 || defined __x86_64__
		patch[5] = '\xC7';
		*(uint32_t*)(patch + 9) = (uint32_t)((uintptr_t)CreateTextureHook >> 32);
	#endif

	#ifdef _WIN32
		if (ReadProcessMemory(GetCurrentProcess(), g_createTexture, g_createTextureOrigBytes, 14, NULL) == 0)
			LUA->ThrowError("XRMod Error: ReadProcessMemory failed");

		if (WriteProcessMemory(GetCurrentProcess(), g_createTexture, patch, 14, NULL) == 0)
			LUA->ThrowError("XRMod Error: WriteProcessMemory failed");
	#else
		uintptr_t alignedAddr = (uintptr_t)g_createTexture & ~(getpagesize()-1);

		if(mprotect((void*)alignedAddr, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC) == -1)
			LUA->ThrowError("XRMod Error: mprotect fail");

		memcpy((void*)g_createTextureOrigBytes, (void*)g_createTexture, 14);
		memcpy((void*)g_createTexture, (void*)patch, 14);
	#endif

	return 0;
}

// TODO
LUA_FUNCTION(ShareTextureFinish) {
	#ifdef _WIN32
		if (g_sharedTexture == NULL)
			LUA->ThrowError("XRMod Error: g_sharedTexture is null");

		ID3D11Resource* res;
		if (FAILED(g_d3d11Device->OpenSharedResource(g_sharedTexture, __uuidof(ID3D11Resource), (void**)&res)))
			LUA->ThrowError("XRMod Error: OpenSharedResource failed");

		if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&g_d3d11Texture)))
			LUA->ThrowError("XRMod Error: QueryInterface failed");
	#else
		if (g_sharedTexture == GL_INVALID_VALUE)
			LUA->ThrowError("XRMod Error: g_sharedTexture is invalid");

		//g_vrTexture.handle = (void*)(uintptr_t)g_sharedTexture;
	#endif

	// Create swapchain
	XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO,nullptr};
	createInfo.arraySize = 1;
	createInfo.createFlags = 0;
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	createInfo.faceCount = 1;
	createInfo.mipCount = 1;
	createInfo.sampleCount = 1;
	createInfo.width = g_TextureWidth*2;
	createInfo.height = g_TextureHeight;

	#ifdef _WIN32
		createInfo.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	#else
		createInfo.format = GL_SRGB8_ALPHA8;
	#endif

	XrResult result = xrCreateSwapchain(g_Session,&createInfo,&g_Swapchain);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to create swapchain (%s)",result));

	uint32_t imageCount;

	result = xrEnumerateSwapchainImages(g_Swapchain,0,&imageCount,nullptr);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to retreive number of required swapchain images (%s)",result));

	g_SwapchainImages.resize(imageCount);
	for(int i = 0; i < imageCount; i++)
	{
		#ifdef _WIN32
			g_SwapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
			g_SwapchainImages[i].texture = NULL;
		#else
			g_SwapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
			g_SwapchainImages[i].image = nullptr;
		#endif

		g_SwapchainImages[i].next = nullptr;
	}

	result = xrEnumerateSwapchainImages(g_Swapchain,imageCount,&imageCount,(XrSwapchainImageBaseHeader*) g_SwapchainImages.data());
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to enumerate swapchain images (%s)",result));

	return 0;
}

void StartSession(GarrysMod::Lua::ILuaBase *LUA) {
	XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO,nullptr,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};

	XrResult result = xrBeginSession(g_Session,&beginInfo);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to start session (%s)",result));
	
	g_SessionStarted = true;
}

uint8_t PollEvents(GarrysMod::Lua::ILuaBase *LUA) {
	XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER,nullptr};

	XrResult result = xrPollEvent(g_Instance,&eventData);
	while(result == XR_SUCCESS) {
		// Iterates through all queued events until it runs out
		switch(eventData.type) {
			case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
				XrEventDataEventsLost *event = (XrEventDataEventsLost *) &eventData;

				char str[MAX_STR_LEN];
				snprintf(str,MAX_STR_LEN,"XRMod: %d events in queue were lost",event->lostEventCount);
				PrintConsoleText(str,LUA);
				break;
			}

			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				XrEventDataSessionStateChanged* event = (XrEventDataSessionStateChanged*) &eventData;

				char str[MAX_STR_LEN];
				snprintf(str, MAX_STR_LEN, "New XR State: %d", event->state);
				PrintConsoleText(str,LUA);

				switch(event->state) {
					case XR_SESSION_STATE_READY: {
						StartSession(LUA);
						break;
					}
				}

				break;
			}

			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
				break;
			}

			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				break;
			}

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				return 2; // Signal to call shutdown
			}
		}

		result = xrPollEvent(g_Instance,&eventData);
	}
	if(result != XR_EVENT_UNAVAILABLE)
		return 1; // Actual failure result

	// Successfully finished the event queue
	return 0;
}

const XrCompositionLayerProjection CreateLayer(XrView* viewInfo) {
	std::vector<XrCompositionLayerProjectionView> projectViews;
	projectViews.resize(2);

	for(int i = 0; i < 2; i++)
	{
		XrCompositionLayerProjectionView* projectView = &projectViews[i];
		projectView->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projectView->next = nullptr;
		projectView->fov = viewInfo[i].fov;
		projectView->pose = viewInfo[i].pose;

		projectView->subImage.swapchain = g_Swapchain;
		projectView->subImage.imageArrayIndex = 0;
		projectView->subImage.imageRect.extent.width = g_TextureWidth;
		projectView->subImage.imageRect.extent.height = g_TextureHeight;

		if(i == 0)
			projectView->subImage.imageRect.offset.x = 0;
		else
			projectView->subImage.imageRect.offset.x = g_TextureWidth;
		projectView->subImage.imageRect.offset.y = 0;
	}

	const XrCompositionLayerProjection project{
		XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		nullptr,
		0,
		g_SpaceStage,
		projectViews.size(),
		projectViews.data()
	};
	/*project.views = projectViews;
	project.viewCount = 2;
	project.layerFlags = 0;
	project.space = g_SpaceStage;*/

	return project;
}

void EndFrameFail() {
	XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO,nullptr};
	frameEndInfo.displayTime = g_FrameState.predictedDisplayTime;
	frameEndInfo.layers = nullptr;
	frameEndInfo.layerCount = 0;
	frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	xrEndFrame(g_Session,&frameEndInfo);
}

XrTime DampenPrediction(XrTime predictedTime)
{
	// Dampen frame time predictions
	#ifdef _WIN32
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
	#else
		timespec time;
		clock_gettime(CLOCK_MONOTONIC,&time);
	#endif

	XrTime timeXr;
	ConvertSysTimeToXrTime(g_Instance,&time,&timeXr);

	XrTime predictionAmount = predictedTime - timeXr;
	if (predictionAmount > 0) {
		predictedTime = timeXr + (predictionScale * predictionAmount) / 100;
	}
	predictedTime = max(predictedTime, g_lastPredictedFrameTime + 1);
	g_lastPredictedFrameTime = predictedTime;

	return predictedTime;
}

LUA_FUNCTION(SetPredictionScale) {
	LUA->CheckType(-1,GarrysMod::Lua::Type::NUMBER);
	predictionScale = LUA->GetNumber();
	return 0;
}

bool CallInGameRenderFunc(XrPosef eyeLeft, XrPosef eyeRight, GarrysMod::Lua::ILuaBase *LUA) {
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "ErrorNoHaltWithStack");
	LUA->GetField(-2, "VRUtilClientRender");
	LUA->Remove(-3);

	if(!LUA->IsType(-1,GarrysMod::Lua::Type::FUNCTION)) {
		LUA->Pop(2);
		return false;
	}

	VMatrix* mtxLeft = PushNewMatrix(LUA);
	ComposeTransform(eyeLeft, mtxLeft);

	VMatrix* mtxRight = PushNewMatrix(LUA);
	ComposeTransform(eyeRight, mtxRight);

	LUA->PCall(2,0,-4);
	/*if(errcode != 0)
		PrintError(LUA);*/

	LUA->Pop(); // Pops the error message

	// Flush to ensure texture results are immediately available
	#ifdef _WIN32
		IDirect3DQuery9* pEventQuery = nullptr;
		g_pD3D9Device->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
		if (pEventQuery != nullptr)
		{
			pEventQuery->Issue(D3DISSUE_END);
			while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
				pEventQuery->Release();
		}
	#endif

	return true;
}

LUA_FUNCTION(DoRenderLoop) {
	uint8_t pollResult = PollEvents(LUA);
	if(pollResult > 0) {
		if(pollResult == 2) {
			LUA->PushBool(false); // Tell lua we need to shut down
		} else LUA->PushBool(true);

		return 1;
	}

	if(!g_SessionStarted) {
		LUA->PushBool(true);
		return 1;
	}

	/*#ifdef _WIN32
		if (g_d3d11Texture == NULL) {
			LUA->PushBool(true);
			return 1;
		}
	#endif*/

	XrFrameState frameState{XR_TYPE_FRAME_STATE,nullptr};

	// xrWaitFrame -> xrBeginFrame -> xrEndFrame
	//XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO,nullptr};
	XrResult result = xrWaitFrame(g_Session, nullptr, &frameState);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod Error: xrWaitFrame failed (%s)",result));
	}
	g_FrameState = frameState;

	XrTime dampenedDisplayTime = DampenPrediction(frameState.predictedDisplayTime);

	//XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO,nullptr};
	result = xrBeginFrame(g_Session, nullptr);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod Error: xrBeginFrame failed (%s)",result));
	}

	// Layer section
	XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO,nullptr};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.space = g_SpaceStage;
	viewLocateInfo.displayTime = dampenedDisplayTime;

	XrViewState viewState{XR_TYPE_VIEW_STATE,nullptr};

	uint32_t viewCountOutput = 0;
	XrView views[2] = { {XR_TYPE_VIEW,nullptr},{XR_TYPE_VIEW,nullptr} };
	result = xrLocateViews(g_Session,&viewLocateInfo,&viewState,2,&viewCountOutput,(XrView *) &views);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod: Failed to locate views (%s)",result));
	}

	// Render here
	if(frameState.shouldRender == XR_TRUE)
	{
		bool bSuccess = CallInGameRenderFunc(views[0].pose, views[1].pose, LUA);
		if(!bSuccess) {
			EndFrameFail();
			LUA->PushBool(true);
			return 1;
		}
	} else {
		EndFrameFail();
		LUA->PushBool(true);
		return 1;
	}

	// Acquire swapchain image

	uint32_t imageIndex = 0;
	result = xrAcquireSwapchainImage(g_Swapchain,nullptr,&imageIndex);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod: Failed to acquire swapchain image (%s)",result));
	}

	XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr};
	//waitInfo.timeout = 17000000; // 17ms (~58.82 fps)
	waitInfo.timeout = XR_INFINITE_DURATION;
	result = xrWaitSwapchainImage(g_Swapchain,&waitInfo);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod: Failed to wait for swapchain image (%s)",result));
	}

	// Update swapchain with rendered image
	#ifdef _WIN32
		g_d3d11DeviceContext->CopyResource(g_SwapchainImages[imageIndex].texture,g_d3d11Texture);
		g_d3d11DeviceContext->Flush();
	#else
		// TODO: Do this for opengl too
	#endif

	result = xrReleaseSwapchainImage(g_Swapchain,nullptr);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod: Failed to release swapchain image (%s)",result));
	}

	// Set up composition layers
	const XrCompositionLayerProjection project = CreateLayer((XrView*) &views);
	std::vector<const XrCompositionLayerBaseHeader*> layers = { (const XrCompositionLayerBaseHeader*) &project };

	XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO,nullptr};
	frameEndInfo.displayTime = frameState.predictedDisplayTime;
	frameEndInfo.layers = layers.data();
	frameEndInfo.layerCount = layers.size();
	frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	result = xrEndFrame(g_Session,&frameEndInfo);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: xrEndFrame failed (%s)",result));

	LUA->PushBool(true);
	return 1;
}

LUA_FUNCTION(AttachActionSets) {
	// Despite what the name implies, you actually have to attach these individually
	// TODO: Chat was she wrong about that?
	for(int i = 0; i < g_actionSetCount; i++)
	{
		XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,nullptr};
		attachInfo.countActionSets = 1;
		attachInfo.actionSets = &g_actionSets[i].handle;

		XrResult result = xrAttachSessionActionSets(g_Session,&attachInfo);
		if(XR_FAILED(result))
			PrintConsoleText(GetResultString("XRMod: Failed to attach action set (%s)",result),LUA);
	}

	return 0;
}

// Done
LUA_FUNCTION(Shutdown) {
	if (g_Session == XR_NULL_HANDLE)
		return 0;
	
	ClearSession(LUA);

	return 0;
}

// Done
LUA_FUNCTION(TriggerHaptic) {
	if(g_Session == XR_NULL_HANDLE)
		return 0;
		//LUA->ThrowError("XRMod Error: Session is invalid");

	const char* actionName = LUA->CheckString(1);

	for (int i = 0; i < g_actionCount; i++) {
		if (strcmp(g_actions[i].name, actionName) == 0) {
			// Currently unused
			//float _delay = (float)LUA->CheckNumber(2);

			XrHapticActionInfo actionInfo{XR_TYPE_HAPTIC_ACTION_INFO,nullptr};
			actionInfo.action = g_actions[i].handle;

			XrHapticVibration feedback{XR_TYPE_HAPTIC_VIBRATION,nullptr};
			feedback.duration = (XrDuration)LUA->CheckNumber(3)*1000000; // Convert to nanoseconds
			feedback.frequency = (float)LUA->CheckNumber(4);
			feedback.amplitude = (float)LUA->CheckNumber(5);

			xrApplyHapticFeedback(g_Session,&actionInfo,(const XrHapticBaseHeader *)&feedback);
			break;
		}
	}

	return 0;
}

LUA_FUNCTION(GetTrackedDeviceNames) {
	XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES,nullptr};
	if(xrGetSystemProperties(g_Instance,g_SystemId,&properties) != XR_SUCCESS)
		LUA->ThrowError("XRMod Error: Failed to retreive system properties");

	LUA->CreateTable();
	LUA->PushNumber(1);
	LUA->PushString(properties.systemName);
	LUA->SetTable(-3);

	return 1;
}

LUA_FUNCTION(GetInteractionProfile) {
	if(g_Session == XR_NULL_HANDLE)
		LUA->ThrowError("XRMod Error: Tried to retrieve interaction profile without a valid session");

	XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE,nullptr};

	const char* userPath;
	if(LUA->GetType(-1) == GarrysMod::Lua::Type::STRING)
		userPath = LUA->GetString(-1);
	else
		userPath = "/user/hand/left";

	XrPath controllerPath = CreateXrPath(userPath);
	XrResult result = xrGetCurrentInteractionProfile(g_Session,controllerPath,&state);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to retrieve interaction profile (%s)",result));

	char profilePath[MAX_STR_LEN];
	uint32_t length;
	xrPathToString(g_Instance,state.interactionProfile,MAX_STR_LEN,&length,profilePath);
	LUA->PushString(profilePath,length);

	return 1;
}

GMOD_MODULE_OPEN(){
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "vrmod");

	if (!LUA->IsType(-1, GarrysMod::Lua::Type::TABLE)) {
		LUA->Pop();
		LUA->CreateTable();
	}

		LUA->PushCFunction(GetVersion);
		LUA->SetField(-2, "GetVersion");

		LUA->PushCFunction(IsHMDPresent);
		LUA->SetField(-2, "IsHMDPresent");

		LUA->PushCFunction(Init);
		LUA->SetField(-2, "Init");

		LUA->PushCFunction(CreateActionSet);
		LUA->SetField(-2, "CreateActionSet");

		LUA->PushCFunction(SuggestBindings);
		LUA->SetField(-2, "SuggestBindings");

		LUA->PushCFunction(SetActiveActionSets);
		LUA->SetField(-2, "SetActiveActionSets");

		LUA->PushCFunction(GetDisplayInfo);
		LUA->SetField(-2, "GetDisplayInfo");

		LUA->PushCFunction(UpdatePosesAndActions);
		LUA->SetField(-2, "UpdatePosesAndActions");

		LUA->PushCFunction(GetPoses);
		LUA->SetField(-2, "GetPoses");

		LUA->PushCFunction(GetActions);
		LUA->SetField(-2, "GetActions");

		LUA->PushCFunction(ShareTextureBegin);
		LUA->SetField(-2, "ShareTextureBegin");

		LUA->PushCFunction(ShareTextureFinish);
		LUA->SetField(-2, "ShareTextureFinish");

		LUA->PushCFunction(SetPredictionScale);
		LUA->SetField(-2, "SetPredictionScale");

		LUA->PushCFunction(DoRenderLoop);
		LUA->SetField(-2, "DoRenderLoop");

		LUA->PushCFunction(AttachActionSets);
		LUA->SetField(-2, "AttachActionSets");

		LUA->PushCFunction(Shutdown);
		LUA->SetField(-2, "Shutdown");

		LUA->PushCFunction(TriggerHaptic);
		LUA->SetField(-2, "TriggerHaptic");

		LUA->PushCFunction(GetTrackedDeviceNames);
		LUA->SetField(-2, "GetTrackedDeviceNames");

		LUA->PushCFunction(GetInteractionProfile);
		LUA->SetField(-2, "GetInteractionProfile");

		LUA->SetField(-2, "vrmod");

	LUA->Pop();

	#ifdef DEBUG
	debugLuaHandle = LUA;
	#endif

	return 0;
}

GMOD_MODULE_CLOSE(){
	ClearSession(LUA);

	if(g_Instance != XR_NULL_HANDLE)
	{
		#ifdef DEBUG
			if(debugGlobalMessenger != XR_NULL_HANDLE)
			{
				PFN_xrDestroyDebugUtilsMessengerEXT xrDestroyDebugUtilsMessengerEXT = (PFN_xrDestroyDebugUtilsMessengerEXT) getXRFunction("xrDestroyDebugUtilsMessengerEXT");
				if(xrDestroyDebugUtilsMessengerEXT != XR_NULL_HANDLE)
					xrDestroyDebugUtilsMessengerEXT(debugGlobalMessenger);
			}
		#endif

		xrDestroyInstance(g_Instance);
		ClearPrototypeFunctions();
		g_Instance = XR_NULL_HANDLE;
	}

	#ifdef DEBUG
	debugLuaHandle = nullptr;
	#endif

	return 0;
}

// ?/16 functions done