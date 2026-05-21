#define GMOD_USE_SOURCESDK
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <vector>
#include <semaphore>
#include <thread>
#include <functional>
#include <format>

#include <GarrysMod/Lua/Interface.h>

#include <ModuleLoader.cpp>
#include <Garrysmod/FactoryLoader.hpp>
#include <GarrysMod/Symbol.hpp>
#include <symbolfinder.cpp>

#include <hde32.c>
#include <hde64.c>
#include <buffer.c>
#include <trampoline.c>
#include <hook.c>
#include <hook.cpp>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
	#include <shellapi.h>
	#include <d3d9.h>
	#include <d3d11_4.h>

	#define PATH_MAX MAX_PATH
	#define XR_USE_PLATFORM_WIN32
	#define XR_USE_GRAPHICS_API_D3D11

	#ifdef _WIN64
		#define COMPILER_MSVC64
	#endif
#else
	#include <GL/gl.h>
	#include <sys/mman.h>
	#include <dlfcn.h>
	#include <unistd.h>
	#include <time.h>

	#define XR_USE_TIMESPEC
	#define XR_USE_GRAPHICS_API_OPENGL
#endif

#ifdef DEBUG
	#define XR_EXTENSION_PROTOTYPES
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <mathlib/mathlib.h>
#include <mathlib/vmatrix.h>
#include <color.h>
#include <materialsystem/imaterialsystem.h>

#define XRMOD_MODULE_VERSION 1

#define MAX_STR_LEN     256
#define MAX_ACTIONS     64
#define MAX_ACTIONSETS  16
#define PI				3.141592653589793116

#define RAD2DEG			(180.0/PI)
#define DEG2RAD 		(PI/180.0)

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

typedef struct QueuedPose {
	bool valid = false;
	XrPosef left;
	XrPosef right;
} QueuedPose;

typedef ITexture* (*CreateNamedRenderTargetTextureEx)(IMaterialSystem*, const char *,
		int, 
		int, 
		RenderTargetSizeMode_t,	// Controls how size is generated (and regenerated on video mode change).
		ImageFormat, 
		MaterialRenderTargetDepth_t, 
		unsigned int,
		unsigned int);

//#define PushMatrix(mtx) LUA->PushUserType(&(mtx.m), GarrysMod::Lua::Type::MATRIX);

const XrPosef XR_IDENTITYPOSE = { {0,0,0,1}, {0,0,0} };

XrInstance				g_Instance = XR_NULL_HANDLE;
bool					g_bUse1_0 = false;
XrSession				g_Session = XR_NULL_HANDLE;
bool					g_SessionStarted = false;
XrSystemId				g_SystemId = XR_NULL_SYSTEM_ID;

XrSpace					g_SpaceStage = XR_NULL_HANDLE;
XrSpace					g_SpaceView = XR_NULL_HANDLE;

actionSet               g_actionSets[MAX_ACTIONSETS];
int                     g_actionSetCount = 0;
XrActiveActionSet       g_activeActionSets[MAX_ACTIONSETS];
int                     g_activeActionSetCount = 0;
action                  g_actions[MAX_ACTIONS];
int                     g_actionCount = 0;

uint32_t		g_TextureWidth = 0;
uint32_t		g_TextureHeight = 0;
XrSwapchain		g_Swapchain = XR_NULL_HANDLE;
XrFrameState	g_FrameState{XR_TYPE_FRAME_STATE,nullptr};
XrView g_Views[2] = { {XR_TYPE_VIEW,nullptr},{XR_TYPE_VIEW,nullptr} };
XrFovf g_FOV = {0.f,0.f,0.f,0.f};

std::vector<QueuedPose>	g_FramePoses = {};
uint8_t frameSimulate = 0;
uint8_t frameRender = 0;

std::thread				g_tWaitThread;
bool					g_bUseWaitThread = true;
std::binary_semaphore	semaphoreRender{1};
std::binary_semaphore	semaphoreSignal{0};

// Swapchain image lists have to be defined after platform-specific info
#ifdef _WIN32
	std::vector<XrSwapchainImageD3D11KHR> g_SwapchainImages = {};

	const Symbol sym_CreateRenderTarget = Symbol::FromSignature("\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x50\x48\x8b\x0d\x92\xdf\x0c\x02\x41\x8b");
#else
	std::vector<XrSwapchainImageOpenGLKHR> g_SwapchainImages = {};
#endif

XrTime	g_lastPredictedFrameTime = 0;
int		predictionScale = 0; // Percentage of original prediction amount to use

int	g_luaRefs[LuaRefIndex_Max];
int	g_luaRefCount = 0;


/// Extension variables

// Determines if the relevant extensions are enabled
bool g_supportsHandTracking = false;
bool g_supportsBodyTracking = false;
// Determines if we have sufficient hand/body tracking
bool g_hasHandTracking = false;
bool g_hasBodyTracking = false;

XrHandTrackerEXT g_handTrackerLeft = XR_NULL_HANDLE;
XrHandTrackerEXT g_handTrackerRight = XR_NULL_HANDLE;
XrBodyTrackerFB g_bodyTracker = XR_NULL_HANDLE;

// Pointer functions
PFN_xrCreateHandTrackerEXT	xrCreateHandTrackerEXT = nullptr;
PFN_xrDestroyHandTrackerEXT	xrDestroyHandTrackerEXT = nullptr;
PFN_xrLocateHandJointsEXT	xrLocateHandJointsEXT = nullptr;

PFN_xrCreateBodyTrackerFB	xrCreateBodyTrackerFB = nullptr;
PFN_xrDestroyBodyTrackerFB	xrDestroyBodyTrackerFB = nullptr;
PFN_xrLocateBodyJointsFB	xrLocateBodyJointsFB = nullptr;
PFN_xrGetBodySkeletonFB		xrGetBodySkeletonFB = nullptr;

#ifdef _WIN32
	PFN_xrConvertWin32PerformanceCounterToTimeKHR ConvertSysTimeToXrTime = nullptr;
#else
	PFN_xrConvertTimespecTimeToTimeKHR ConvertSysTimeToXrTime = nullptr;
#endif


char                    g_createTextureOrigBytes[14];
IMaterialSystem*		g_pMatSys = nullptr;
CreateNamedRenderTargetTextureEx fn_CreateNamedRenderTargetTextureEx = nullptr;

#ifdef _WIN32

	typedef HRESULT         (APIENTRY* CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	//STDMETHOD(Present)(THIS_ CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion) PURE;
	typedef HRESULT			(APIENTRY* Present)(IDirect3DDevice9*, CONST RECT*,CONST RECT*,HWND,CONST RGNDATA*);
	CreateTexture           g_createTexture = NULL;
	Present					g_Present = NULL;

	ID3D11Device5*          g_d3d11Device = NULL;
	ID3D11DeviceContext4*	g_d3d11DeviceContext = NULL;
	ID3D11Texture2D*        g_d3d11Texture = NULL;
	ID3D11Fence*			g_d3d11Fence = NULL;
	UINT64					g_d3d11FenceValue = 0Ui64;
	HANDLE					g_fenceEvent = NULL;
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

	Detouring::Hook hook_Present;

#else

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

	if(g_supportsHandTracking)
	{
		xrCreateHandTrackerEXT =	(PFN_xrCreateHandTrackerEXT) getXRFunction("xrCreateHandTrackerEXT");
		xrDestroyHandTrackerEXT =	(PFN_xrDestroyHandTrackerEXT) getXRFunction("xrDestroyHandTrackerEXT");
		xrLocateHandJointsEXT =		(PFN_xrLocateHandJointsEXT) getXRFunction("xrLocateHandJointsEXT");
	}

	if(g_supportsBodyTracking)
	{
		xrCreateBodyTrackerFB =		(PFN_xrCreateBodyTrackerFB) getXRFunction("xrCreateBodyTrackerFB");
		xrDestroyBodyTrackerFB =	(PFN_xrDestroyBodyTrackerFB) getXRFunction("xrDestroyBodyTrackerFB");
		xrLocateBodyJointsFB =		(PFN_xrLocateBodyJointsFB) getXRFunction("xrLocateBodyJointsFB");
		xrGetBodySkeletonFB =		(PFN_xrGetBodySkeletonFB) getXRFunction("xrGetBodySkeletonFB");
	}
}

void ClearPrototypeFunctions()
{
	ConvertSysTimeToXrTime = nullptr;

	if(g_supportsHandTracking)
	{
		xrCreateHandTrackerEXT =	nullptr;
		xrDestroyHandTrackerEXT =	nullptr;
		xrLocateHandJointsEXT =		nullptr;
	}

	if(g_supportsBodyTracking)
	{
		xrCreateBodyTrackerFB =		nullptr;
		xrDestroyBodyTrackerFB =	nullptr;
		xrLocateBodyJointsFB =		nullptr;
		xrGetBodySkeletonFB =		nullptr;
	}
}

void EndFrameFail() {
	XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO,nullptr};
	frameEndInfo.displayTime = g_FrameState.predictedDisplayTime;
	frameEndInfo.layers = nullptr;
	frameEndInfo.layerCount = 0;
	frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	xrEndFrame(g_Session,&frameEndInfo);
	semaphoreRender.release();
}

XrResult BeginFrame()
{
	semaphoreRender.acquire();

	// If we somehow haven't finished copying the last frame we wait on that here
	#ifdef _WIN32
		if(g_d3d11Fence->GetCompletedValue() < g_d3d11FenceValue)
		{
			g_d3d11Fence->SetEventOnCompletion(g_d3d11FenceValue,g_fenceEvent);
			WaitForSingleObject(g_fenceEvent,INFINITE);
		}
	#else
	#endif

	XrResult result = xrBeginFrame(g_Session, nullptr);

	return result;
}

const XrCompositionLayerProjection CreateLayer(XrView* viewInfo) {
	std::vector<XrCompositionLayerProjectionView> projectViews;
	projectViews.resize(2);

	QueuedPose poses = g_FramePoses[frameRender];

	for(int i = 0; i < 2; i++)
	{
		XrCompositionLayerProjectionView* projectView = &projectViews[i];
		projectView->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projectView->next = nullptr;
		projectView->fov = g_Views[i].fov;
		projectView->pose = i == 0 ? poses.left : poses.right;

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

IDirect3DQuery9* pEventQuery = nullptr;
void EndFrame()
{
	if(!g_FramePoses[frameRender].valid)
	{
		EndFrameFail();
		return;
	}
	g_FramePoses[frameRender].valid = false;

	// Acquire swapchain image

	uint32_t imageIndex;
	XrResult result = xrAcquireSwapchainImage(g_Swapchain,nullptr,&imageIndex);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		return;
		//LUA->ThrowError(GetResultString("XRMod: Failed to acquire swapchain image (%s)",result));
	}

	XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr};
	//waitInfo.timeout = 17000000; // 17ms (~58.82 fps)
	waitInfo.timeout = XR_INFINITE_DURATION;
	result = xrWaitSwapchainImage(g_Swapchain,&waitInfo);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		return;
		//LUA->ThrowError(GetResultString("XRMod: Failed to wait for swapchain image (%s)",result));
	}

	// Flush to ensure texture results are immediately available
	#ifdef _WIN32
		if (pEventQuery != nullptr)
		{
			while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
				pEventQuery->Release();
		}
	#endif

	// Update swapchain with rendered image
	#ifdef _WIN32
		g_d3d11DeviceContext->CopyResource(g_SwapchainImages[imageIndex].texture,g_d3d11Texture);
		g_d3d11DeviceContext->Flush();

		g_d3d11FenceValue++;
		g_d3d11DeviceContext->Signal(g_d3d11Fence, g_d3d11FenceValue);
	#else
		// TODO: Do this for opengl too
	#endif

	result = xrReleaseSwapchainImage(g_Swapchain,nullptr);
	if(XR_FAILED(result))
	{
		EndFrameFail();
		return;
		//LUA->ThrowError(GetResultString("XRMod: Failed to release swapchain image (%s)",result));
	}

	// Set up composition layers
	const XrCompositionLayerProjection project = CreateLayer((XrView*) &g_Views);
	std::vector<const XrCompositionLayerBaseHeader*> layers = { (const XrCompositionLayerBaseHeader*) &project };

	XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO,nullptr};
	frameEndInfo.displayTime = g_FrameState.predictedDisplayTime;
	frameEndInfo.layers = layers.data();
	frameEndInfo.layerCount = layers.size();
	frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	result = xrEndFrame(g_Session,&frameEndInfo);
	if(XR_FAILED(result))
	{
		//LUA->ThrowError(GetResultString("XRMod Error: xrEndFrame failed (%s)",result));
	}

	semaphoreRender.release();
}

void AdvanceRenderFrame()
{
	frameRender++;
	if(frameRender >= g_FramePoses.size())
		frameRender = 0;
}

void WaitThreadLoop()
{
	while (true)
	{
		semaphoreSignal.acquire();
		
		// End thread when session is destroyed
		if(g_Session == XR_NULL_HANDLE)
			break;

		EndFrame();
		AdvanceRenderFrame();
	}
}

#ifdef _WIN32
	HRESULT APIENTRY PresentHook(IDirect3DDevice9* pDevice, CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion)
	{
		if(g_SessionStarted)
			BeginFrame();

		HRESULT result = hook_Present.GetTrampoline<Present>()(pDevice,pSourceRect,pDestRect,hDestWindowOverride,pDirtyRegion);

		if(g_SessionStarted)
		{
			// Create query right away to ensure it's directly after this frame's draw calls
			#ifdef _WIN32
				g_pD3D9Device->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
				if(pEventQuery != nullptr)
					pEventQuery->Issue(D3DISSUE_END);
			#else
				// TODO: OpenGL equivalent of queries or fences
			#endif

			if(g_bUseWaitThread)
				semaphoreSignal.release();
			else {
				EndFrame();
				AdvanceRenderFrame();
			}
		}

		return result;
	};
#endif

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

char strBuffer[MAX_STR_LEN];
char* GetResultString(const char* form, XrResult result) {
	char resStr[MAX_STR_LEN];
	if(g_Instance)
		xrResultToString(g_Instance,result,resStr);
	else
		snprintf(resStr, MAX_STR_LEN, "Error Code %d", result);

	snprintf(strBuffer, MAX_STR_LEN, form, resStr);
	return strBuffer;
}

// TODO: Optimize this
Color textCol(211,81,255,255);
void PrintConsoleText(char* str, GarrysMod::Lua::ILuaBase *LUA) {
	LUA->GetField(GarrysMod::Lua::INDEX_GLOBAL, "MsgC");

	LUA->CreateTable();

	LUA->PushNumber(textCol[0]);
	LUA->SetField(-2,"r");
	LUA->PushNumber(textCol[1]);
	LUA->SetField(-2,"g");
	LUA->PushNumber(textCol[2]);
	LUA->SetField(-2,"b");
	LUA->PushNumber(textCol[3]);
	LUA->SetField(-2,"a");

	/*char newStr[MAX_STR_LEN];
	strncpy(newStr,str,MAX_STR_LEN);
	strncat(newStr,"\n",MAX_STR_LEN - strlen(str));*/
	LUA->PushString(str);
	LUA->PushString("\n");

	LUA->PCall(3, 0, 0);
}


bool ContainsString(const char** array, int count, const char* str)
{
	for(int i = 0; i < count; i++)
	{
		if(strcmp(array[i],str) == 0)
			return true;
	}

	return false;
}

XrResult AttemptCreateInstance(std::vector<const char*>* extensions, XrVersion apiVersion)
{
	XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO,nullptr};

	createInfo.enabledExtensionCount = (uint32_t) extensions->size();
	createInfo.enabledExtensionNames = extensions->data();

	std::vector<const char*> apiLayers = {};
	#ifdef DEBUG
		//apiLayers.push_back("XR_APILAYER_LUNARG_core_validation");
	#endif
	createInfo.enabledApiLayerCount = apiLayers.size();
	createInfo.enabledApiLayerNames = apiLayers.data();

	XrApplicationInfo appInfo;
	appInfo.apiVersion = apiVersion;
	strncpy(appInfo.applicationName, "Garry's Mod", XR_MAX_APPLICATION_NAME_SIZE);
	appInfo.applicationVersion = 1;
	strncpy(appInfo.engineName, "", XR_MAX_ENGINE_NAME_SIZE);
	appInfo.engineVersion = 0;

	createInfo.applicationInfo = appInfo;
	createInfo.createFlags = 0;

	return xrCreateInstance(&createInfo,&g_Instance);
}

const char* requiredExtensions[] = {
	#ifdef _WIN32
		XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
		XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
	#else
		XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
		XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
	#endif
	#ifdef DEBUG
		XR_EXT_DEBUG_UTILS_EXTENSION_NAME
	#endif
};

const char* optionalExtensions[] = {
	XR_KHR_GENERIC_CONTROLLER_EXTENSION_NAME,
	XR_EXT_HAND_TRACKING_EXTENSION_NAME,
	XR_FB_BODY_TRACKING_EXTENSION_NAME,
	XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME
};

XrResult RefreshInstance(GarrysMod::Lua::ILuaBase *LUA = nullptr) {
	uint32_t extensionCount;
	XrResult result = xrEnumerateInstanceExtensionProperties(nullptr,0,&extensionCount,nullptr);
	if(XR_FAILED(result))
		return result;

	std::vector<XrExtensionProperties> extensionProperties(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES,nullptr});
	result = xrEnumerateInstanceExtensionProperties(nullptr,extensionCount,&extensionCount,extensionProperties.data());
	if(XR_FAILED(result))
		return result;

	std::vector<const char*> extensions = {};
	int numRequiredExts = sizeof(requiredExtensions)/sizeof(requiredExtensions[0]);
	int numOptionalExts = sizeof(optionalExtensions)/sizeof(optionalExtensions[0]);

	int requiredCount = 0;
	int optionalCount = 0;
	for(int i = 0; i < extensionCount; i++)
	{
		bool checkRequired = requiredCount < numRequiredExts;
		bool checkOptional = optionalCount < numOptionalExts;
		if(!checkRequired && !checkOptional)
			break;

		XrExtensionProperties* ext = &extensionProperties[i];

		if(checkRequired)
		{
			bool found = false;
			for(int e = 0; e < numRequiredExts; e++)
			{
				if(strcmp(ext->extensionName,requiredExtensions[e]) == 0)
				{
					extensions.push_back(requiredExtensions[e]);
					requiredCount++;
					found = true;
					break;
				}
			}

			if(found) continue;
		}

		if(checkOptional)
		{
			for(int e = 0; e < numOptionalExts; e++)
			{
				if(strcmp(ext->extensionName,optionalExtensions[e]) == 0)
				{
					extensions.push_back(optionalExtensions[e]);
					optionalCount++;
					break;
				}
			}
		}
	}

	if(requiredCount < numRequiredExts)
		return XR_ERROR_EXTENSION_NOT_PRESENT;

	g_bUse1_0 = false;
	result = AttemptCreateInstance(&extensions,XR_CURRENT_API_VERSION);
	if(XR_FAILED(result))
	{
		// Revert to 1.0 if the runtime doesn't support the current version
		if(result == XR_ERROR_API_VERSION_UNSUPPORTED)
		{
			// 1.0 Doesn't have the grip_surface binding so we need the palm extension
			extensions.push_back(XR_EXT_PALM_POSE_EXTENSION_NAME);
			result = AttemptCreateInstance(&extensions,XR_API_VERSION_1_0);
		}

		if(XR_FAILED(result))
			return result;
		else
			g_bUse1_0 = true;
	}

	const char** extArray = extensions.data();
	int count = extensions.size();
	g_supportsHandTracking = ContainsString(extArray, count, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
	g_supportsBodyTracking = ContainsString(extArray, count, XR_FB_BODY_TRACKING_EXTENSION_NAME)
							&& ContainsString(extArray, count, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);

	char str[256];
	snprintf(str,256,"Supports hands: %i",g_supportsHandTracking ? 1 : 0);
	PrintConsoleText(str,LUA);
	
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
	semaphoreRender.acquire();

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


	if(g_hasHandTracking)
	{
		xrDestroyHandTrackerEXT(g_handTrackerLeft);
		g_handTrackerLeft = XR_NULL_HANDLE;

		xrDestroyHandTrackerEXT(g_handTrackerRight);
		g_handTrackerRight = XR_NULL_HANDLE;

		g_hasHandTracking = false;
	}

	if(g_hasBodyTracking)
	{
		xrDestroyBodyTrackerFB(g_bodyTracker);
		g_bodyTracker = XR_NULL_HANDLE;
		g_hasBodyTracking = false;
	}

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
	g_FramePoses.clear();
	g_FOV = {0,0,0,0};

	g_FrameState.shouldRender = XR_FALSE;
	g_FrameState.predictedDisplayPeriod = 0;
	g_FrameState.predictedDisplayTime = 0;

	g_lastPredictedFrameTime = 0;


	for(int i = 0; i < g_luaRefCount; i++)
		LUA->ReferenceFree(g_luaRefs[i]);
	g_luaRefCount = 0;

	#ifdef _WIN32
		if (g_d3d11Device != NULL) {
			g_d3d11Fence->Release();
			g_d3d11Fence = NULL;
			g_d3d11DeviceContext->Release();
			g_d3d11DeviceContext = NULL;
			g_d3d11Device->Release();
			g_d3d11Device = NULL;

			CloseHandle(g_fenceEvent);
			g_fenceEvent = NULL;
		}

		g_d3d11Texture = NULL;
		g_pD3D9Device = NULL;
		g_sharedTexture = NULL;

		if(hook_Present.IsValid())
			hook_Present.Destroy();
	#else
		g_sharedTexture = GL_INVALID_VALUE;
	#endif

	semaphoreSignal.release();
	if(g_tWaitThread.joinable())
		g_tWaitThread.join();
	
	semaphoreSignal.try_acquire();

	frameSimulate = 0;
	frameRender = 0;

	g_SystemId = XR_NULL_SYSTEM_ID;

	semaphoreRender.release();
}

// Doesn't need changing
LUA_FUNCTION(GetVersion) {
	LUA->PushNumber(XRMOD_MODULE_VERSION);

	return 1;
}

// Done
LUA_FUNCTION(IsHMDPresent) {
	if(g_Instance == XR_NULL_HANDLE) {
		XrResult result = RefreshInstance(LUA);
		if(XR_FAILED(result))
		{
			PrintConsoleText(GetResultString("XRMod: Failed to create instance (%s)",result),LUA);
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
		ID3D11Device* baseDevice;
		ID3D11DeviceContext* baseContext;
		if (D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &baseDevice, NULL, &baseContext) != S_OK)
			LUA->ThrowError("D3D11CreateDevice failed");
		
		if(baseDevice->QueryInterface(__uuidof(ID3D11Device5), (void**) &g_d3d11Device) != S_OK)
			LUA->ThrowError("ID3D11Device5 QueryInterface failed");
		baseDevice->Release();

		if(baseContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**) &g_d3d11DeviceContext) != S_OK)
			LUA->ThrowError("ID3D11DeviceContext4 QueryInterface failed");
		baseContext->Release();

		if(g_d3d11Device->CreateFence(0Ui64,D3D11_FENCE_FLAG_NONE,__uuidof(ID3D11Fence),(void**) &g_d3d11Fence) != S_OK)
			LUA->ThrowError("ID3D11Device5::CreateFence failed");
		g_d3d11FenceValue = 0Ui64;

		g_fenceEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
		if(g_fenceEvent == NULL)
			LUA->ThrowError("CreateEvent failed");
	#else
		// TODO: Is this needed for OpenGL?
	#endif
}

void InitExtensions(GarrysMod::Lua::ILuaBase *LUA)
{
	XrSystemProperties properties{
		XR_TYPE_SYSTEM_PROPERTIES,
		nullptr,
		XR_NULL_SYSTEM_ID,
		0,
		"",
		{0,0,0},
		{XR_FALSE,XR_FALSE}};
	// Helper object to keep track of previous structure in the chain
	XrBaseInStructure** nextptr = (XrBaseInStructure**) &properties.next;

	XrSystemHandTrackingPropertiesEXT propertiesHand{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT,nullptr,XR_FALSE};
	if(g_supportsHandTracking)
	{
		*nextptr = (XrBaseInStructure*) &propertiesHand;
		nextptr = (XrBaseInStructure**) &propertiesHand.next;
	}

	XrResult result = xrGetSystemProperties(g_Instance, g_SystemId, &properties);
	if(XR_FAILED(xrGetSystemProperties))
		LUA->ThrowError("XRMod Error: Failed to get system properties");
	
	XrBaseInStructure* next = (XrBaseInStructure*) properties.next;

	bool hasUpperBody = false;
	bool hasLowerBody = false;

	char str[256];
	snprintf(str,256,"Hand tracking: %i",propertiesHand.supportsHandTracking);
	PrintConsoleText(str,LUA);
	g_hasHandTracking = propertiesHand.supportsHandTracking == XR_TRUE;

	/*while(next != nullptr)
	{
		switch (next->type)
		{
			case XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT:
			{
				XrSystemHandTrackingPropertiesEXT* handProperties = (XrSystemHandTrackingPropertiesEXT*) next;
				g_hasHandTracking = handProperties->supportsHandTracking == XR_TRUE;
				break;
			}
			// TODO: We might just want to remove this one
			case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
			{
				XrSystemBodyTrackingPropertiesFB* bodyProperties = (XrSystemBodyTrackingPropertiesFB*) next;
				hasUpperBody = bodyProperties->supportsBodyTracking == XR_TRUE;
				break;
			}
			case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META:
			{
				XrSystemPropertiesBodyTrackingFullBodyMETA* bodyProperties = (XrSystemPropertiesBodyTrackingFullBodyMETA*) next;
				hasLowerBody = bodyProperties->supportsFullBodyTracking == XR_TRUE;
				break;
			}
		}

		next = (XrBaseInStructure*) next->next;
	}*/

	g_hasBodyTracking = hasUpperBody && hasLowerBody;

	if(g_hasHandTracking)
	{
		XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,nullptr};
		createInfo.hand = XR_HAND_LEFT_EXT;
		createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

		result = xrCreateHandTrackerEXT(g_Session,&createInfo,&g_handTrackerLeft);
		if(XR_FAILED(result))
			LUA->ThrowError(GetResultString("XRMod Error: Failed to create hand tracker (%s)",result));

		createInfo.hand = XR_HAND_RIGHT_EXT;
		result = xrCreateHandTrackerEXT(g_Session,&createInfo,&g_handTrackerRight);
		if(XR_FAILED(result))
			LUA->ThrowError(GetResultString("XRMod Error: Failed to create hand tracker (%s)",result));
	}

	if(g_hasBodyTracking)
	{
		XrBodyTrackerCreateInfoFB createInfo{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB,nullptr};
		createInfo.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;

		result = xrCreateBodyTrackerFB(g_Session,&createInfo,&g_bodyTracker);
		if(XR_FAILED(result))
			LUA->ThrowError(GetResultString("XRMod Error: Failed to create body tracker (%s)",result));
		
		XrBodySkeletonFB skeleton;
		result = xrGetBodySkeletonFB(g_bodyTracker,&skeleton);
		if(XR_FAILED(result))
			LUA->ThrowError(GetResultString("XRMod Error: Failed to get body skeleton (%s)",result));

		bool waist = false;
		bool footLeft = false;
		bool footRight = false;
		for(int i = 0; i < skeleton.jointCount; i++)
		{
			int32_t joint = skeleton.joints[i].joint;
			switch (joint)
			{
				case XR_FULL_BODY_JOINT_SPINE_LOWER_META:
				{
					waist = true;
					break;
				}
				case XR_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META:
				{
					footLeft = true;
					break;
				}
				case XR_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META:
				{
					footRight = true;
					break;
				}
			}

			if(waist && footLeft && footRight)
				break;
		}

		if(!waist || !footLeft || !footRight)
		{
			g_hasBodyTracking = false;
			result = xrDestroyBodyTrackerFB(g_bodyTracker);
			g_bodyTracker = XR_NULL_HANDLE;

			PrintConsoleText("XRMod: Insufficient body trackers for FBT",LUA);
		}
	}
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

	InitExtensions(LUA);

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
	
	if(g_bUseWaitThread)
		g_tWaitThread = std::thread(&WaitThreadLoop);

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
		g_Present = ((Present**)g_pD3D9Device)[0][17];

		if(hook_Present.Create(g_Present,&PresentHook))
			hook_Present.Enable();
		else
			LUA->ThrowError("XRMod Error: Failed to hook IDirect3DDevice9::Present");
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
	LUA->CheckType(-3,GarrysMod::Lua::Type::String); // setName
	LUA->CheckType(-2,GarrysMod::Lua::Type::String); // localizedSetName
	LUA->CheckType(-1,GarrysMod::Lua::Type::Table); // actions

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

		if(LUA->GetType(-2) == GarrysMod::Lua::Type::String) // Key of this table (action name)
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
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::String)
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
			else if(strcmp(strType, "vibration") == 0)
				createInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
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
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::String)
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
	LUA->CheckType(-2,GarrysMod::Lua::Type::String);
	LUA->CheckType(-1,GarrysMod::Lua::Type::Table);

	XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,nullptr};
	suggestedBindings.countSuggestedBindings = 0;
	suggestedBindings.interactionProfile = CreateXrPath(LUA->GetString(-2));

	std::vector<XrActionSuggestedBinding> bindings = {};

	LUA->PushNil();
	while(LUA->Next(-2) != 0) {
		if(LUA->GetType(-1) == GarrysMod::Lua::Type::String && LUA->GetType(-2) == GarrysMod::Lua::Type::String)
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

			char bind[XR_MAX_PATH_LENGTH];
			strncpy(bind,LUA->GetString(-1),XR_MAX_PATH_LENGTH);

			// 1.1 has grip_surface built-in, 1.0 has to get it from the palm pose extension
			if(g_bUse1_0 && act->type == XR_ACTION_TYPE_POSE_INPUT)
			{
				if(strcmp(bind,"/user/hand/left/input/grip_surface/pose") == 0)
					strncpy(bind,"/user/hand/left/input/palm_ext/pose",MAX_STR_LEN);
				else if(strcmp(bind,"/user/hand/right/input/grip_surface/pose") == 0)
					strncpy(bind,"/user/hand/right/input/palm_ext/pose",MAX_STR_LEN);
			}
			binding.binding = CreateXrPath(bind);

			bindings.push_back(binding);
			suggestedBindings.countSuggestedBindings++;
		}

		LUA->Pop();
	}
	suggestedBindings.suggestedBindings = bindings.data();

	XrResult result = xrSuggestInteractionProfileBindings(g_Instance,&suggestedBindings);
	if(XR_FAILED(result))
	{
		if(result != XR_ERROR_PATH_UNSUPPORTED)
			PrintConsoleText(GetResultString("XRMod: Failed to suggest interaction profile bindings (%s)",result),LUA);
	}

	return 0;
}

LUA_FUNCTION(SetActiveActionSets) {
	if(g_Instance == XR_NULL_HANDLE)
		LUA->ThrowError("XRMod Error: Invalid Instance");

	g_activeActionSetCount = 0;

	// Loops through all action sets and sets them to the provided arguments (if provided)
	for (int i = 0; i < MAX_ACTIONSETS; i++) {
		if (LUA->GetType(i + 1) == GarrysMod::Lua::Type::String) {
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
	return LUA->GetUserType<VMatrix>(-1, GarrysMod::Lua::Type::Matrix);
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
	uint32_t viewCount = 2;

	XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO,nullptr};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.space = g_SpaceView;
	viewLocateInfo.displayTime = g_FrameState.predictedDisplayTime;

	XrViewState viewState{XR_TYPE_VIEW_STATE,nullptr,0};

	uint32_t viewCountOutput = 0;
	XrView views[2] = { {XR_TYPE_VIEW,nullptr},{XR_TYPE_VIEW,nullptr} };
	XrResult result = xrLocateViews(g_Session,&viewLocateInfo,&viewState,viewCount,&viewCountOutput,(XrView *) &views);
	if(XR_FAILED(result))
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

	hFov = max(abs(views[0].fov.angleRight), abs(views[0].fov.angleLeft),
				abs(views[1].fov.angleRight), abs(views[1].fov.angleLeft));
	vFov = max(abs(views[0].fov.angleUp), abs(views[0].fov.angleDown),
				abs(views[1].fov.angleUp), abs(views[1].fov.angleDown));
	aspect = tan(hFov) / tan(vFov);

	g_FOV = {
		(float) -hFov,
		(float) hFov,
		(float) vFov,
		(float) -vFov
	};

	LUA->PushNumber(aspect);
	LUA->SetField(-2, "AspectSymmetric");

	LUA->PushNumber(hFov * 2 * RAD2DEG);
	LUA->SetField(-2, "FovSymmetric");

	// Asymmetric FOV offsets
	double fLeftSym = tan(g_FOV.angleLeft);
	double fRightSym = tan(g_FOV.angleRight);
	double fTopSym = tan(g_FOV.angleUp);
	double fBottomSym = tan(g_FOV.angleDown);

	double fLeft = tan(views[0].fov.angleLeft);
	double fRight = tan(views[0].fov.angleRight);
	double fTop = tan(views[0].fov.angleUp);
	double fBottom = tan(views[0].fov.angleDown);

	/*double offset = (fLeft + fRight) / (fRight - fLeft);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetXLeft");

	offset = (fBottom + fTop) / (fTop - fBottom);
	LUA->PushNumber(offset);
	LUA->SetField(-2, "OffsetYLeft");*/

	LUA->PushNumber((1.0 - fLeft / fLeftSym) / 2);
	LUA->SetField(-2, "U0Left");
	LUA->PushNumber(0.5 + (fRight / fRightSym) / 2);
	LUA->SetField(-2, "U1Left");
	LUA->PushNumber((1.0 - fTop / fTopSym) / 2);
	LUA->SetField(-2, "V0Left");
	LUA->PushNumber(0.5 + (fBottom / fBottomSym) / 2);
	LUA->SetField(-2, "V1Left");

	fLeft = tan(views[1].fov.angleLeft);
	fRight = tan(views[1].fov.angleRight);
	fTop = tan(views[1].fov.angleUp);
	fBottom = tan(views[1].fov.angleDown);

	LUA->PushNumber((1.0 - fLeft / fLeftSym) / 2);
	LUA->SetField(-2, "U0Right");
	LUA->PushNumber(0.5 + (fRight / fRightSym) / 2);
	LUA->SetField(-2, "U1Right");
	LUA->PushNumber((1.0 - fTop / fTopSym) / 2);
	LUA->SetField(-2, "V0Right");
	LUA->PushNumber(0.5 + (fBottom / fBottomSym) / 2);
	LUA->SetField(-2, "V1Right");

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

void QuatMul(XrQuaternionf& q0, XrQuaternionf& q1, XrQuaternionf& out)
{
	out.x =  q0.x * q1.w + q0.y * q1.z - q0.z * q1.y + q0.w * q1.x;
	out.y = -q0.x * q1.z + q0.y * q1.w + q0.z * q1.x + q0.w * q1.y;
	out.z =  q0.x * q1.y - q0.y * q1.x + q0.z * q1.w + q0.w * q1.z;
	out.w = -q0.x * q1.x - q0.y * q1.y - q0.z * q1.z + q0.w * q1.w;
}

XrQuaternionf QuatMul(XrQuaternionf& q0, XrQuaternionf& q1)
{
	XrQuaternionf out;
	QuatMul(q0,q1,out);
	return out;
}

void QuatInvert(const XrQuaternionf& p, XrQuaternionf& q)
{
	q.x = -p.x;
	q.y = -p.y;
	q.z = -p.z;
	q.w = p.w;

	float magnitudeSqr = p.x * p.x + p.y * p.y + p.z * p.z + p.w * p.w;
	if ( magnitudeSqr )
	{
		float inv = 1.0f / magnitudeSqr;
		q.x *= inv;
		q.y *= inv;
		q.z *= inv;
		q.w *= inv;
	}
}

// 180 degrees
const double minPitch = 0;
const double maxPitch = 180.0 * DEG2RAD;
const double pitchRange = maxPitch - minPitch;
LUA_FUNCTION(GetFingercurls) {
	if(!g_SessionStarted || !g_hasHandTracking)
		return 0;

	XrHandJointsLocateInfoEXT locateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,nullptr};
	locateInfo.baseSpace = g_SpaceStage;
	locateInfo.time = g_FrameState.predictedDisplayTime;

	XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];

	XrHandJointLocationsEXT locations{
		XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
		nullptr,
		XR_FALSE,
		XR_HAND_JOINT_COUNT_EXT,
		joints
	};

	Quaternion inverse, offset;
	for(int hand = 0; hand < 2; hand++)
	{
		XrHandTrackerEXT tracker = hand == 0 ? g_handTrackerLeft : g_handTrackerRight;
		XrResult result = xrLocateHandJointsEXT(tracker,&locateInfo,&locations);
		if(XR_FAILED(result))
			LUA->ThrowError(GetResultString("XRMod Error: Failed to locate hand joints (%s)",result));

		if(!locations.isActive)
		{
			LUA->PushNumber(0.0);
			LUA->PushNumber(0.0);
			LUA->PushNumber(0.0);
			LUA->PushNumber(0.0);
			LUA->PushNumber(0.0);
			continue;
		}

		// Proximal joint
		XrHandJointLocationEXT* j1;
		// Tip joint
		XrHandJointLocationEXT* j2;
		for(int i = 0; i < 5; i++)
		{
			if(i == 0)
			{
				j1 = &joints[XR_HAND_JOINT_THUMB_PROXIMAL_EXT];
				j2 = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
			}
			else
			{
				j1 = &joints[XR_HAND_JOINT_THUMB_METACARPAL_EXT + 5 * i];
				j2 = &joints[XR_HAND_JOINT_THUMB_TIP_EXT + 5 * i];
			}
			if(j1->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT == 0 ||
				j2->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT == 0)
			{
				LUA->PushNumber(0.0);
				continue;
			}

			/*QuatInvert(j1->pose.orientation,inverse);
			QuatMul(inverse,j2->pose.orientation,offset);*/
			Quaternion q1 = {
				j1->pose.orientation.x,
				j1->pose.orientation.y,
				j1->pose.orientation.z,
				j1->pose.orientation.w
			};
			QuaternionInvert(q1,inverse);
			q1 = {
				j2->pose.orientation.x,
				j2->pose.orientation.y,
				j2->pose.orientation.z,
				j2->pose.orientation.w
			};
			QuaternionMult(q1,inverse,offset);

			double pitch = asin(-(( 2.0f * offset.x * offset.z ) - ( 2.0f * offset.w * offset.y ) ));
			if(i == 1)
			{
				char str[MAX_STR_LEN];
				snprintf(str,MAX_STR_LEN,"Pitch: %d",pitch);
				PrintConsoleText(str,LUA);
			}

			double curl = (pitch-minPitch)/pitchRange;
			LUA->PushNumber(curl);
		}
	}

	return 10;
}

LUA_FUNCTION(GetBodyTrackers) {
	if(!g_hasBodyTracking)
		LUA->ThrowError("XRMod Error: Body tracking not available");

	XrBodyJointsLocateInfoFB locateInfo{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB,nullptr};
	locateInfo.baseSpace = g_SpaceStage;
	locateInfo.time = g_FrameState.predictedDisplayTime;

	XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB,nullptr};
	XrResult result = xrLocateBodyJointsFB(g_bodyTracker,&locateInfo,&locations);
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to locate body joints (%s)",result));

	XrPosef pose;
	VMatrix* mtx;
	for(int i = 0; i < 3; i++)
	{
		int joint;
		switch(i)
		{
			case 0:
			{
				joint = XR_FULL_BODY_JOINT_SPINE_LOWER_META;
				break;
			}
			case 1:
			{
				joint = XR_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META;
				break;
			}
			case 2:
			{
				joint = XR_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META;
				break;
			}
		}

		pose = locations.jointLocations[joint].pose;
		mtx = PushNewMatrix(LUA);
		ComposeTransform(pose,mtx);
	}

	return 3;
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

	std::vector<XrViewConfigurationView> viewConfigs;
	viewConfigs.resize(2);
	viewConfigs[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW,nullptr};
	viewConfigs[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW,nullptr};
	// We'll get an XR validation error if we don't initialize these

	uint32_t viewCount = 2;

	XrResult result = xrEnumerateViewConfigurationViews(g_Instance,g_SystemId,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,viewCount,&viewCount,viewConfigs.data());
	if(XR_FAILED(result))
		LUA->ThrowError(GetResultString("XRMod Error: Failed to enumerate view configurations (%s)",result));
	
	if(viewCount != 2 /*|| viewConfigs[0].recommendedImageRectWidth != viewConfigs[1].recommendedImageRectWidth || viewConfigs[0].recommendedImageRectHeight != viewConfigs[1].recommendedImageRectHeight*/)
		LUA->ThrowError("XRMod Error: View count is not 2");

	g_TextureWidth = viewConfigs[0].recommendedImageRectWidth;
	g_TextureHeight = viewConfigs[0].recommendedImageRectHeight;

	if(LUA->IsType(-1,GarrysMod::Lua::Type::Number))
	{
		uint32_t scale = LUA->GetNumber(-1);
		if(scale != 100)
		{
			g_TextureWidth = min(g_TextureWidth * scale / 100, viewConfigs[0].maxImageRectWidth);
			g_TextureHeight = min(g_TextureHeight * scale / 100, viewConfigs[0].maxImageRectHeight);
		}
	}

	LUA->PushNumber(g_TextureWidth);
	LUA->PushNumber(g_TextureHeight);
	return 2;
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

	if(LUA->IsType(-1,GarrysMod::Lua::Type::Number) && LUA->IsType(-2,GarrysMod::Lua::Type::Number))
	{
		g_TextureWidth = LUA->GetNumber(-2);
		g_TextureHeight = LUA->GetNumber(-1);
	}

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
		LUA->ThrowError(GetResultString("XRMod Error: Failed to retrieve number of required swapchain images (%s)",result));

	g_SwapchainImages.resize(imageCount);
	g_FramePoses.resize(imageCount);
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
		g_FramePoses[i].valid = false;
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
				snprintf(str, MAX_STR_LEN, "XRMod: New session state %d", event->state);
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
				PrintConsoleText("XRMod: Instance loss pending",LUA);

				return 2; // Signal to call shutdown
			}
		}

		eventData.type = XR_TYPE_EVENT_DATA_BUFFER;
		eventData.next = nullptr;
		result = xrPollEvent(g_Instance,&eventData);
	}
	if(result != XR_EVENT_UNAVAILABLE)
	{
		PrintConsoleText(GetResultString("XRMod Error: Failed to poll events (%s)",result),LUA);
		return 1; // Actual failure result
	}

	// Successfully finished the event queue
	return 0;
}

XrTime lastFrameTime = 0;
void DampenPrediction(GarrysMod::Lua::ILuaBase *LUA)
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

	if(predictionScale != 100)
	{
		XrTime predictionAmount = g_FrameState.predictedDisplayTime - timeXr;
		if (predictionAmount > 0) {
			g_FrameState.predictedDisplayTime = timeXr + (predictionScale * predictionAmount) / 100;
		}
	}

	XrTime delta = timeXr - lastFrameTime;
	/*char str[MAX_STR_LEN];
	snprintf(str,MAX_STR_LEN,"Delta: %f ms",roundf(delta/1000000.f));
	PrintConsoleText(str,LUA);*/
	
	g_FrameState.predictedDisplayTime = max(g_FrameState.predictedDisplayTime, g_lastPredictedFrameTime + 1);
	g_lastPredictedFrameTime = g_FrameState.predictedDisplayTime;
	lastFrameTime = timeXr;
}

LUA_FUNCTION(SetPredictionScale) {
	LUA->CheckType(-1,GarrysMod::Lua::Type::Number);
	predictionScale = LUA->GetNumber();
	return 0;
}

bool CallInGameRenderFunc(XrPosef eyeLeft, XrPosef eyeRight, GarrysMod::Lua::ILuaBase *LUA) {
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "ErrorNoHaltWithStack");
	LUA->GetField(-2, "VRUtilClientRender");
	LUA->Remove(-3);

	if(!LUA->IsType(-1,GarrysMod::Lua::Type::Function)) {
		LUA->Pop(2);
		return false;
	}

	VMatrix* mtxLeft = PushNewMatrix(LUA);
	ComposeTransform(eyeLeft, mtxLeft);

	VMatrix* mtxRight = PushNewMatrix(LUA);
	ComposeTransform(eyeRight, mtxRight);

	int errcode = LUA->PCall(2,0,-4);
	if(errcode != 0)
	{
		//PrintError(LUA);
	} else
	{
		g_FramePoses[frameSimulate].valid = true;
		g_FramePoses[frameSimulate].left = g_Views[0].pose;
		g_FramePoses[frameSimulate].right = g_Views[1].pose;
	}

	LUA->Pop(); // Pops the error message

	return true;
}

void AdvanceSimulateFrame()
{
	frameSimulate++;
	if(frameSimulate >= g_FramePoses.size())
		frameSimulate = 0;
}

LUA_FUNCTION(DoRenderLoop) {
	uint8_t pollResult = PollEvents(LUA);
	if(pollResult > 0) {
		if(pollResult == 2) {
			LUA->PushBool(false); // Tell lua we need to shut down
		} else LUA->PushBool(true);

		if(g_SessionStarted)
			AdvanceSimulateFrame();

		return 1;
	}

	LUA->PushBool(true);

	if(!g_SessionStarted) {
		//AdvanceSimulateFrame();
		return 1;
	}

	/*#ifdef _WIN32
		if (g_d3d11Texture == NULL) {
			LUA->PushBool(true);
			return 1;
		}
	#endif*/

	// xrWaitFrame -> xrBeginFrame -> xrEndFrame
	//XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO,nullptr};
	XrResult result = xrWaitFrame(g_Session, nullptr, &g_FrameState);
	if(XR_FAILED(result))
	{
		//EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod Error: xrWaitFrame failed (%s)",result));
	}

	DampenPrediction(LUA);

	// Layer section
	XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO,nullptr};
	viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	viewLocateInfo.space = g_SpaceStage;
	viewLocateInfo.displayTime = g_FrameState.predictedDisplayTime;

	XrViewState viewState{XR_TYPE_VIEW_STATE,nullptr};

	uint32_t viewCountOutput = 0;
	result = xrLocateViews(g_Session,&viewLocateInfo,&viewState,2,&viewCountOutput,(XrView *) &g_Views);
	if(XR_FAILED(result))
	{
		//EndFrameFail();
		LUA->ThrowError(GetResultString("XRMod: Failed to locate views (%s)",result));
	}

	// Render here
	if(g_FrameState.shouldRender == XR_TRUE)
		bool bSuccess = CallInGameRenderFunc(g_Views[0].pose, g_Views[1].pose, LUA);
	
	AdvanceSimulateFrame();
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
	if(LUA->GetType(-1) == GarrysMod::Lua::Type::String)
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


#ifdef DEBUG_SYMBOLS
#define rangeStart 87
#define rangeEnd 91
#define RANGE 1+rangeEnd-rangeStart
#define MATSYS_CREATERENDERTARGET 89
int index;
int hits;

std::vector<std::string> names;
Detouring::Hook hook;
ITexture* DetourHook(IMaterialSystem* thisTemp, char *pRTName,				// Pass in NULL here for an unnamed render target.
				int w, 
				int h, 
				RenderTargetSizeMode_t sizeMode,	// Controls how size is generated (and regenerated on video mode change).
				ImageFormat format, 
				MaterialRenderTargetDepth_t depth = MATERIAL_RT_DEPTH_SHARED, 
				unsigned int textureFlags = TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
				unsigned int renderTargetFlags = 0)
{
	//index = i;
	if(thisTemp == g_pMatSys && w == 64 && h == 64)
	{
		hits++;
		if(pRTName != nullptr)
			names.push_back(std::string(pRTName));
	}
	return hook.GetTrampoline<CreateNamedRenderTargetTextureEx>()(thisTemp,pRTName,w,h,sizeMode,format,depth,textureFlags,renderTargetFlags);
}

void FrameHook(float frameTime)
{
	//Sleep(1);
	return hook.GetTrampoline<BeginFrameFunc>()(frameTime);
}

void ScanForFunction(GarrysMod::Lua::ILuaBase *LUA)
{
	SourceSDK::FactoryLoader loader("materialsystem");
	g_pMatSys = loader.GetInterface<IMaterialSystem>(MATERIAL_SYSTEM_INTERFACE_VERSION);

	if(!loader.IsValid())
		PrintConsoleText("XRMod: Failed to acquire factory",LUA);

	if(g_pMatSys == nullptr)
		PrintConsoleText("XRMod: Failed to acquire g_pMaterialSystem",LUA);
	else
	{
		index = -1;
		hits = 0;
		int attempts = 0;
		int errcode;
		names.clear();
		for(int i = rangeStart; i <= rangeEnd; i++)
		{
			CreateNamedRenderTargetTextureEx fn = ((CreateNamedRenderTargetTextureEx**) g_pMatSys)[0][i];

			hook.Create(fn, &DetourHook);
			if(hook.IsValid())
			{
				hook.Enable();
				attempts++;

				char name[MAX_STR_LEN];
				snprintf(name,MAX_STR_LEN,"jeff_%i",i);

				LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
				LUA->GetField(-1, "GetRenderTargetEx");
				LUA->Remove(-2);
				LUA->PushString(name);
				LUA->PushNumber(64.0);
				LUA->PushNumber(64.0);
				LUA->PushNumber(0.0);
				LUA->PushNumber(0.0);
				LUA->PushNumber(0.0);
				LUA->PushNumber(0.0);
				LUA->PushNumber(-1.0);
				errcode = LUA->PCall(8, 1, 0);

				hook.Destroy();
				
				if(hits > 0)
				{
					index = i;
					uint64_t size = (uint64_t)((uint8_t**) g_pMatSys)[0][i+1] - (uint64_t)((uint8**) g_pMatSys)[0][i];
					snprintf(name,MAX_STR_LEN,"%i hits on index %i, size: %i",hits,i,(int)size);
					hits = 0;
					PrintConsoleText(name,LUA);

					for(int n = 0; n < names.size(); n++)
					{
						PrintConsoleText(names[n].data(),LUA);
					}
					names.clear();

					uint8_t* p = (uint8_t*) fn;
					strncpy(name,"Bytes: ",MAX_STR_LEN);
					for(int b = 0; b < 24; b++)
					{
						std::string byte = std::format("\\x{:x}",p[b]);
						strncat(name,byte.data(),MAX_STR_LEN);
					}
					PrintConsoleText(name,LUA);
					break;
				}
			}
		}


		/*for(int i = rangeEnd; i >= rangeStart; i--)
		{
			if(hooks[i].IsValid())
				hooks[i].Destroy();
		}*/

		char txt[MAX_STR_LEN];
		snprintf(txt,MAX_STR_LEN,"Error: %i Attempts: %i Index: %i",errcode,attempts,index);
		PrintConsoleText(txt,LUA);

		BeginFrameFunc fn = ((BeginFrameFunc**) g_pMatSys)[0][MATSYS_CREATERENDERTARGET-48];
		hook.Create(fn,&FrameHook);
		if(hook.IsValid())
			hook.Enable();

		//g_pMatSys->SetThreadMode(MATERIAL_SINGLE_THREADED);
	}
}
#endif // DEBUG_SYMBOLS

void AcquireFunctionPointers(GarrysMod::Lua::ILuaBase *LUA)
{
	SourceSDK::FactoryLoader loader("materialsystem");
	g_pMatSys = loader.GetInterface<IMaterialSystem>(MATERIAL_SYSTEM_INTERFACE_VERSION);

	if(!loader.IsValid())
		PrintConsoleText("XRMod: Failed to acquire factory",LUA);

	if(g_pMatSys == nullptr)
		PrintConsoleText("XRMod: Failed to acquire g_pMaterialSystem",LUA);
	else
	{
		SymbolFinder symfinder;
		auto pointer = reinterpret_cast<uint8_t *>( symfinder.Resolve(loader.GetModule(),
			sym_CreateRenderTarget.name.c_str(), sym_CreateRenderTarget.length ) );
		
		if (pointer != nullptr)
			fn_CreateNamedRenderTargetTextureEx = (CreateNamedRenderTargetTextureEx) pointer;
		
		ITexture* tex = fn_CreateNamedRenderTargetTextureEx(g_pMatSys,"duh",
		64,64,RT_SIZE_DEFAULT,IMAGE_FORMAT_RGBA8888,MATERIAL_RT_DEPTH_SHARED,0,0);
		if(tex != nullptr)
			PrintConsoleText("It lives",LUA);
	}
}

GMOD_MODULE_OPEN(){
	#ifdef DEBUG_SYMBOLS
		ScanForFunction(LUA);
	#endif
	AcquireFunctionPointers(LUA);
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->GetField(-1, "vrmod");

	if (!LUA->IsType(-1, GarrysMod::Lua::Type::Table)) {
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

		LUA->PushCFunction(GetFingercurls);
		LUA->SetField(-2, "GetFingercurls");

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

	g_pMatSys = nullptr;
	#ifdef DEBUG_SYMBOLS
	if(hook.IsValid())
		hook.Destroy();
	#endif

	#ifdef DEBUG
	debugLuaHandle = nullptr;
	#endif

	return 0;
}

// ?/16 functions done