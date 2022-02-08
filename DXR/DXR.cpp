#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <DirectXMath.h>
#include <vector>
#include <iterator>
#include <dxcapi.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxcompiler.lib")

using namespace std;
using Microsoft::WRL::ComPtr;

namespace
{
	const int WINDOW_WIDTH = 640;
	const int WINDOW_HEIGHT = 360;
	const int BUFFER_COUNT = 3;
	const int MAX_BINDLESS_RESOURCE = 100;
	const int MAX_DEFINED_RESOURCE = 8;
};

void CHK(HRESULT hr)
{
	if (FAILED(hr)) {
		throw runtime_error("HRESULT is failed value.");
	}
}

class D3D
{
	ComPtr<IDXGIFactory2> mDxgiFactory;
	ComPtr<ID3D12Device> mDevice;
	uint32_t mRTVStride;
	uint32_t mDSVStride;
	uint32_t mResourceStride;
	uint32_t mSamplerStride;
	ComPtr<ID3D12CommandAllocator> mCmdAlloc[BUFFER_COUNT];
	ComPtr<ID3D12CommandQueue> mCmdQueue;
	ComPtr<IDXGISwapChain3> mSwapChain;
	ComPtr<ID3D12GraphicsCommandList> mCmdList;
	uint64_t mFrameCount = 0;
	ComPtr<ID3D12Fence> mFence;
	ComPtr<ID3D12Resource> mSwapChainTex[BUFFER_COUNT];
	ComPtr<ID3D12DescriptorHeap> mSwapChainRTVs;

	ComPtr<ID3D12CommandAllocator> mCmdAllocCopy;
	ComPtr<ID3D12CommandQueue> mCmdQueueCopy;
	ComPtr<ID3D12GraphicsCommandList> mCmdListCopy;
	ComPtr<ID3D12Resource> mCopyBuffer;

	enum class Constants {
		SceneMatrix,
		Max,
	};
	ComPtr<ID3D12Resource> mConstantBuffer[BUFFER_COUNT];

	enum class ShaderViews {
		// Base pass
		SceneCBVMatrix,
		SceneTexture,
		AccelerationStructure,
		SceneBindlessResource,
		Max = SceneBindlessResource + MAX_BINDLESS_RESOURCE,
	};
	ComPtr<ID3D12DescriptorHeap> mShaderView[BUFFER_COUNT];

	enum class Samplers {
		Default,
		Max,
	};
	ComPtr<ID3D12DescriptorHeap> mSampler;

	ComPtr<ID3D12Resource> mSceneTex;

	ComPtr<ID3D12Resource> mBindlessResource[MAX_BINDLESS_RESOURCE];

	struct VertexElement
	{
		float position[3];
		float normal[3];
	};

	ComPtr<ID3D12Resource> mVB;
	ComPtr<ID3D12Resource> mIB;
	const int SphereSlices = 12;
	const int SphereStacks = 12;

	ComPtr<ID3D12Resource> mVBPlane;
	ComPtr<ID3D12Resource> mIBPlane;

	ComPtr<ID3D12Resource> mBlas;
	ComPtr<ID3D12Resource> mBlasPlane;
	ComPtr<ID3D12Resource> mTlasInstance;
	ComPtr<ID3D12Resource> mTlas;

	ComPtr<ID3D12StateObject> mStateObject;
	ComPtr<ID3D12StateObjectProperties> mStateObjectProps;
	ComPtr<ID3D12RootSignature> mSceneRootSigGlobal;
	ComPtr<ID3D12RootSignature> mSceneRootSigLocal;
	ComPtr<ID3D12RootSignature> mSceneRootSigLocalNull;
	ComPtr<ID3D12Resource> mShaderBindingTable[BUFFER_COUNT];

	static int Align(int val, int align)
	{
		return ((val + align - 1) & ~(align - 1));
	}

	const uint32_t oRayGen = 0;
	const uint32_t oRayGenRootConstant = oRayGen + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	const uint32_t oHitGroup = Align(oRayGenRootConstant + 4, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
	const uint32_t oMiss = Align(oHitGroup + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
	const uint32_t oMax = Align(oMiss + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

	unique_ptr<uint8_t[]> mShaderBindingTableDefaultData;

public:
	~D3D()
	{
		mFrameCount++;

		// Wait GPU command completion
		mCmdQueue->Signal(mFence.Get(), mFrameCount);
		while (mFence->GetCompletedValue() < mFrameCount)
		{
			SwitchToThread();
		}
	}

	D3D(int width, int height, HWND hWnd)
	{
		UINT factoryFlags = 0;
#ifdef _DEBUG
		factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
		CHK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&mDxgiFactory)));

#if _DEBUG
		ComPtr<ID3D12Debug> debug = nullptr;
		D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
		if (debug)
		{
			debug->EnableDebugLayer();
			debug.Reset();
		}
#endif
		CHK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice)));
		mDSVStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		mRTVStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		mResourceStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		mSamplerStride = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAlloc[i])));
		}
		CHK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&mCmdAllocCopy)));

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CHK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCmdQueue)));
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		CHK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCmdQueueCopy)));

		DXGI_SWAP_CHAIN_DESC1 sc = {};
		sc.Width = WINDOW_WIDTH;
		sc.Height = WINDOW_HEIGHT;
		sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sc.SampleDesc.Count = 1;
		sc.BufferCount = BUFFER_COUNT;
		sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sc.BufferUsage = DXGI_USAGE_BACK_BUFFER | DXGI_USAGE_RENDER_TARGET_OUTPUT;
		ComPtr<IDXGISwapChain1> tempSwapChain;
		CHK(mDxgiFactory->CreateSwapChainForHwnd(mCmdQueue.Get(), hWnd, &sc, nullptr, nullptr, &tempSwapChain));
		CHK(tempSwapChain.As(&mSwapChain));
		CHK(mDxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

		CHK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
		CHK(mCmdList->Close());

		CHK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));

		CHK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, mCmdAllocCopy.Get(), nullptr, IID_PPV_ARGS(&mCmdListCopy)));

		D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc.NumDescriptors = 10;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mSwapChainRTVs)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE swapChainRTVHandle(mSwapChainRTVs->GetCPUDescriptorHandleForHeapStart());
		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainTex[i])));
			mDevice->CreateRenderTargetView(mSwapChainTex[i].Get(), nullptr, swapChainRTVHandle);
			swapChainRTVHandle.Offset(1, mRTVStride);
		}

		// Root Signature

		CD3DX12_DESCRIPTOR_RANGE descRange[10];
		CD3DX12_ROOT_PARAMETER rootParam[10];
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;

		ComPtr<ID3DBlob> rootSigBlob, rootSigError;

		// SceneView
		descRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		descRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		descRange[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		descRange[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_BINDLESS_RESOURCE, 0, 1);
		rootParam[0].InitAsDescriptorTable(4, descRange + 0);
		rootSigDesc.Init(1, rootParam, 0, nullptr);

		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mSceneRootSigGlobal)));

		rootParam[0].InitAsConstants(1, 0, 1); // b0, space1
		rootSigDesc.Init(1, rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);

		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mSceneRootSigLocal)));

		rootSigDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE); // none

		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mSceneRootSigLocalNull)));

		// Shader

		static const char shaderCodeSceneRayGen[] = R"#(
#define SHADER_INC_PER_BOTTOM_INSTANCE 0
struct Payload { float4 color; uint mode; };

cbuffer CMode : register(b0, space1) {
	uint Mode;
};
cbuffer CScene : register(b0) {
	float4x4 InvViewProj;
	float4 CameraPos;
};
RaytracingAccelerationStructure myAS : register(t0);
RWTexture2D<float4> myTarget : register(u0);
[shader("raygeneration")]
void main()
{
	float2 svpos = (0.5 + (float2)DispatchRaysIndex()) / (float2)DispatchRaysDimensions();
	float3 ndc = float3(svpos.x * 2 - 1, svpos.y * -2 + 1, 1);
	float4 farPos = mul(float4(ndc, 1), InvViewProj);
	farPos.xyz /= farPos.w;

	RayDesc ray;
	ray.Origin = CameraPos.xyz;
	ray.Direction = normalize(farPos.xyz - CameraPos.xyz);
	ray.TMin = 0.01;
	ray.TMax = 100.0;
	Payload payload = { (float4)0, Mode };
	TraceRay(myAS, RAY_FLAG_CULL_NON_OPAQUE,
		0x1, 0, SHADER_INC_PER_BOTTOM_INSTANCE, 0, ray, payload);

	myTarget[DispatchRaysIndex().xy] = payload.color;
}
)#";

		static const char shaderCodeSceneClosestHit[] = R"#(
struct Payload { float4 color; uint mode; };

Texture2D<float4> ColorMap[] : register(t0, space1);
[shader("closesthit")]
void main(inout Payload payload, BuiltInTriangleIntersectionAttributes attr)
{
	if (payload.mode == 0) {
		payload.color = float4(1, 1, 1, 1);
	}
	else if (payload.mode == 1) {
		float3 barycentrics = float3(1 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
		payload.color = float4(barycentrics, 1);
	}
	else if (payload.mode == 2) {
		uint id = PrimitiveIndex() % 8;
		payload.color = ColorMap[NonUniformResourceIndex(id)].Load(int3(0, 0, 0));
	}
	else if (payload.mode == 3) {
		uint id = InstanceIndex() % 8;
		payload.color = ColorMap[NonUniformResourceIndex(id)].Load(int3(0, 0, 0));
	}
}
)#";

		static const char shaderCodeSceneMiss[] = R"#(
struct Payload { float4 color; uint mode; };

[shader("miss")]
void main(inout Payload payload)
{
	payload.color = float4(0.3, 0.3, 0.3, 1);
}
)#";

		SetDllDirectory(L"../dll/");

		ComPtr<IDxcCompiler> dxc;
		CHK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)));
		ComPtr<IDxcLibrary> dxcLib;
		CHK(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxcLib)));

		ComPtr<IDxcBlobEncoding> dxcTxtSceneRayGen, dxcTxtSceneClosestHit, dxcTxtSceneMiss;
		CHK(dxcLib->CreateBlobWithEncodingFromPinned(shaderCodeSceneRayGen, _countof(shaderCodeSceneRayGen) - 1, CP_UTF8, &dxcTxtSceneRayGen));
		CHK(dxcLib->CreateBlobWithEncodingFromPinned(shaderCodeSceneClosestHit, _countof(shaderCodeSceneClosestHit) - 1, CP_UTF8, &dxcTxtSceneClosestHit));
		CHK(dxcLib->CreateBlobWithEncodingFromPinned(shaderCodeSceneMiss, _countof(shaderCodeSceneMiss) - 1, CP_UTF8, &dxcTxtSceneMiss));

		ComPtr<IDxcBlob> dxcBlobSceneRayGen, dxcBlobSceneClosestHit, dxcBlobSceneMiss;
		ComPtr<IDxcBlobEncoding> dxcError;
		ComPtr<IDxcOperationResult> dxcRes;
		const wchar_t* shaderArgs[] = { L"-Zi", L"-all_resources_bound", L"-Qembed_debug"};

		dxc->Compile(dxcTxtSceneRayGen.Get(), nullptr, L"main", L"lib_6_3", shaderArgs, _countof(shaderArgs), nullptr, 0, nullptr, &dxcRes);
		dxcRes->GetErrorBuffer(&dxcError);
		if (dxcError->GetBufferSize()) {
			OutputDebugStringA(reinterpret_cast<char*>(dxcError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		dxcRes->GetResult(&dxcBlobSceneRayGen);

		dxc->Compile(dxcTxtSceneClosestHit.Get(), nullptr, L"main", L"lib_6_3", shaderArgs, _countof(shaderArgs), nullptr, 0, nullptr, &dxcRes);
		dxcRes->GetErrorBuffer(&dxcError);
		if (dxcError->GetBufferSize()) {
			OutputDebugStringA(reinterpret_cast<char*>(dxcError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		dxcRes->GetResult(&dxcBlobSceneClosestHit);

		dxc->Compile(dxcTxtSceneMiss.Get(), nullptr, L"main", L"lib_6_3", shaderArgs, _countof(shaderArgs), nullptr, 0, nullptr, &dxcRes);
		dxcRes->GetErrorBuffer(&dxcError);
		if (dxcError->GetBufferSize()) {
			OutputDebugStringA(reinterpret_cast<char*>(dxcError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		dxcRes->GetResult(&dxcBlobSceneMiss);

		// Raytracing PSO

		ComPtr<ID3D12Device5> device5;
		CHK(mDevice.As(&device5));
		{
			CD3DX12_STATE_OBJECT_DESC rtDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

			// DXIL library
			auto libRG = rtDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			auto libCH = rtDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			auto libMiss = rtDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			CD3DX12_SHADER_BYTECODE libDxilRG(dxcBlobSceneRayGen->GetBufferPointer(), dxcBlobSceneRayGen->GetBufferSize());
			CD3DX12_SHADER_BYTECODE libDxilCH(dxcBlobSceneClosestHit->GetBufferPointer(), dxcBlobSceneClosestHit->GetBufferSize());
			CD3DX12_SHADER_BYTECODE libDxilMiss(dxcBlobSceneMiss->GetBufferPointer(), dxcBlobSceneMiss->GetBufferSize());
			libRG->SetDXILLibrary(&libDxilRG);
			libRG->DefineExport(L"MyRayGen", L"main");
			libCH->SetDXILLibrary(&libDxilCH);
			libCH->DefineExport(L"MyClosestHit", L"main");
			libMiss->SetDXILLibrary(&libDxilMiss);
			libMiss->DefineExport(L"MyMiss", L"main");

			auto hitGroup = rtDesc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
			hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
			hitGroup->SetHitGroupExport(L"MyHitGroup");
			hitGroup->SetClosestHitShaderImport(L"MyClosestHit");

			auto shaderConfig = rtDesc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
			UINT payloadSize = 4 * sizeof(float) + 1 * sizeof(unsigned);   // color + mode
			UINT attributeSize = 2 * sizeof(float); // barycentrics
			shaderConfig->Config(payloadSize, attributeSize);

			auto pipelineConfig = rtDesc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
			pipelineConfig->Config(1); // Max recursion depth

			// Local root signature is optional
			auto rootSigLocal = rtDesc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
			rootSigLocal->SetRootSignature(mSceneRootSigLocal.Get());
			// Bind local root signature to shader stage
			auto rootSigAssociation = rtDesc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
			rootSigAssociation->SetSubobjectToAssociate(*rootSigLocal);
			rootSigAssociation->AddExport(L"MyRayGen");
#if 1
			// On PIX unbind local signatures show warnings...
			auto rootSigLocalNull = rtDesc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
			rootSigLocalNull->SetRootSignature(mSceneRootSigLocalNull.Get());
			auto rootSigNullAssociation = rtDesc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
			rootSigNullAssociation->SetSubobjectToAssociate(*rootSigLocalNull);
			rootSigNullAssociation->AddExport(L"MyClosestHit");
			rootSigNullAssociation->AddExport(L"MyMiss");
#endif

			// Global root signature is also optional, but it seems nessesary in most cases
			auto rootSigGlobal = rtDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
			rootSigGlobal->SetRootSignature(mSceneRootSigGlobal.Get());

			CHK(device5->CreateStateObject(rtDesc, IID_PPV_ARGS(&mStateObject)));
		}

		// Shader Table

		{
			CHK(mStateObject.As(&mStateObjectProps));
			void* pRayGenID = mStateObjectProps->GetShaderIdentifier(L"MyRayGen");
			void* pMissID = mStateObjectProps->GetShaderIdentifier(L"MyMiss");
			void* pHitGroupID = mStateObjectProps->GetShaderIdentifier(L"MyHitGroup");

			mShaderBindingTableDefaultData = std::make_unique<uint8_t[]>(oMax);
			uint8_t* sbt = mShaderBindingTableDefaultData.get();
			memset(sbt, 0, oMax); // zero fill
			memcpy(sbt + oRayGen, pRayGenID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			memset(sbt + oRayGenRootConstant, 0, sizeof(uint32_t));
			memcpy(sbt + oHitGroup, pHitGroupID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
			memcpy(sbt + oMiss, pMissID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

			for (auto& sbtr : mShaderBindingTable)
			{
				auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(oMax);
				CHK(mDevice->CreateCommittedResource(
					&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sbtr)));
			}
		}

		// Resources

		for (auto& cb : mConstantBuffer)
		{
			auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(256 * (int)Constants::Max);
			CHK(mDevice->CreateCommittedResource(
				&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb)));
		}

		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WINDOW_WIDTH, WINDOW_HEIGHT, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mSceneTex)));

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(4 * 1024 * 1024);
		resDesc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mCopyBuffer)));
		vector<char> copyBuffer;
		copyBuffer.reserve(resDesc.Width);

		for (int i = 0; i < MAX_DEFINED_RESOURCE; ++i)
		{
			heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 1);
			CHK(mDevice->CreateCommittedResource(
				&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mBindlessResource[i])));

			static const float colors[MAX_DEFINED_RESOURCE][4] = {
				{1.0f, 0.0f, 0.0f, 1.0f},
				{0.7f, 0.7f, 0.0f, 1.0f},
				{0.0f, 1.0f, 0.0f, 1.0f},
				{0.0f, 0.7f, 0.7f, 1.0f},
				{0.0f, 0.0f, 1.0f, 1.0f},
				{0.7f, 0.0f, 0.7f, 1.0f},
				{0.5f, 0.5f, 0.5f, 1.0f},
				{1.0f, 1.0f, 1.0f, 1.0f},
			};
			D3D12_SUBRESOURCE_DATA sub = { colors[i], sizeof(colors[0]), sizeof(colors[0]) };
			UpdateSubresources(
				mCmdListCopy.Get(), mBindlessResource[i].Get(), mCopyBuffer.Get(),
				D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * i,
				0, 1, &sub);
		}
		D3D12_RESOURCE_BARRIER transitions[MAX_DEFINED_RESOURCE];
		for (int i = 0; i < MAX_DEFINED_RESOURCE; ++i)
		{
			transitions[i] = CD3DX12_RESOURCE_BARRIER::Transition(
				mBindlessResource[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		}
		mCmdListCopy->ResourceBarrier(MAX_DEFINED_RESOURCE, transitions);

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			descHeapDesc = {};
			descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			descHeapDesc.NumDescriptors = (int)ShaderViews::Max;
			descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mShaderView[i])));

			CD3DX12_CPU_DESCRIPTOR_HANDLE shaderViewHandle(mShaderView[i]->GetCPUDescriptorHandleForHeapStart());
			auto addrCB = mConstantBuffer[i]->GetGPUVirtualAddress();

			auto sv = CD3DX12_CPU_DESCRIPTOR_HANDLE(shaderViewHandle, (int)ShaderViews::SceneTexture, mResourceStride);
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uav.Format = mSceneTex->GetDesc().Format;
			mDevice->CreateUnorderedAccessView(mSceneTex.Get(), nullptr, &uav, sv);

			for (int i = 0; i < MAX_BINDLESS_RESOURCE; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
				srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				srv.Texture2D.PlaneSlice = 0; // Depth
				auto sv = CD3DX12_CPU_DESCRIPTOR_HANDLE(shaderViewHandle, (int)ShaderViews::SceneBindlessResource + i, mResourceStride);
				mDevice->CreateShaderResourceView(mBindlessResource[i].Get(), &srv, sv);
			}

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
			cbv.BufferLocation = addrCB + 256 * (int)Constants::SceneMatrix;
			cbv.SizeInBytes = 256;
			auto sv_c = CD3DX12_CPU_DESCRIPTOR_HANDLE(shaderViewHandle, (int)ShaderViews::SceneCBVMatrix, mResourceStride);
			mDevice->CreateConstantBufferView(&cbv, sv_c);
		}

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		descHeapDesc.NumDescriptors = (int)Samplers::Max;
		descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mSampler)));

		D3D12_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		samplerDesc.MaxLOD = FLT_MAX;

		CD3DX12_CPU_DESCRIPTOR_HANDLE samplerHandle(mSampler->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateSampler(&samplerDesc, samplerHandle);

		// Generate sphere triangles

		struct IndexList
		{
			short a[6];
		};
		vector<VertexElement> vertices;
		vector<IndexList> indices;
		vertices.reserve((SphereStacks + 1) * (SphereSlices + 1));
		for (int y = 0; y < SphereStacks + 1; ++y)
		{
			for (int x = 0; x < SphereSlices + 1; ++x)
			{
				// Generate a evenly tesselated plane
				float v[3] = { (float)x / (float)(SphereSlices), (float)y / (float)(SphereStacks), 0.0f };
				// Convert to spherical coordinate system
				float theta = 2 * 3.14159265f * v[0];
				float phi = 2 * 3.14159265f * v[1] / 2.0f;
				VertexElement ve = { sinf(phi) * sinf(theta), cosf(phi), sinf(phi) * cosf(theta), 0, 0, 0 };
				// Setup normal
				float r = 1.0f;
				ve.normal[0] = ve.position[0] / r;
				ve.normal[1] = ve.position[1] / r;
				ve.normal[2] = ve.position[2] / r;
				vertices.push_back(ve);
			}
		}
		indices.reserve(SphereStacks * SphereSlices);
		for (int y = 0; y < SphereStacks; ++y)
		{
			for (int x = 0; x < SphereSlices; ++x)
			{
				short b = static_cast<short>(y * (SphereSlices + 1) + x);
				short s = SphereSlices + 1;
				IndexList il = { b, b + s, b + 1, b + s, b + s + 1, b + 1 };
				indices.push_back(il);
			}
		}

		auto sizeVB = static_cast<uint32_t>(sizeof(vertices[0]) * vertices.size());
		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeVB, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mVB)));
		void* gpuMem;
		CHK(mVB->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, vertices.data(), sizeVB);

		auto sizeIB = static_cast<uint32_t>(sizeof(indices[0]) * indices.size());
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeIB, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mIB)));
		CHK(mIB->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, indices.data(), sizeIB);

		// Generate plane triangles
		vertices.clear();
		indices.clear();
		vertices.push_back({ -1, -1, +1,  0, +1,  0 });
		vertices.push_back({ +1, -1, +1,  0, +1,  0 });
		vertices.push_back({ -1, -1, -1,  0, +1,  0 });
		vertices.push_back({ +1, -1, -1,  0, +1,  0 });
		for (auto& v : vertices) { v.position[0] *= 3; v.position[1] *= 3; v.position[2] *= 3; }
		indices.push_back({ 0, 1, 2, 2, 1, 3 });

		sizeVB = static_cast<uint32_t>(sizeof(vertices[0]) * vertices.size());
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeVB, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mVBPlane)));
		CHK(mVBPlane->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, vertices.data(), sizeVB);

		sizeIB = static_cast<uint32_t>(sizeof(indices[0]) * indices.size());
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeIB, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mIBPlane)));
		CHK(mIBPlane->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, indices.data(), sizeIB);

		// DMA

		ComPtr<ID3D12Fence> fenceCopy;
		CHK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceCopy)));

		CHK(mCmdListCopy->Close());
		ID3D12GraphicsCommandList* cmdLists[] = { mCmdListCopy.Get() };
		mCmdQueueCopy->ExecuteCommandLists(1, CommandListCast(cmdLists));
		CHK(mCmdQueueCopy->Signal(fenceCopy.Get(), 1));
		BuildBVH(fenceCopy.Get()); // Include command wait
		CHK(mCmdAllocCopy->Reset());
	}

	void BuildBVH(ID3D12Fence* fenceCopy)
	{
		ComPtr<ID3D12Device5> device5;
		CHK(mDevice.As(&device5));

		ComPtr<ID3D12CommandQueue> cmdQueue;
		ComPtr<ID3D12GraphicsCommandList4> cmdList4;
		ComPtr<ID3D12CommandAllocator> cmdAlloc;
		ComPtr<ID3D12Fence> fence;
		ComPtr<ID3D12Resource> scratchBufBlas, scratchBufTlas;

		// Create compute queue

		D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_COMPUTE };
		CHK(device5->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue)));
		CHK(device5->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&cmdAlloc)));
		CHK(device5->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList4)));
		CHK(device5->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

		CHK(cmdQueue->Wait(fenceCopy, 1));

		// Prebuild BLAS

		D3D12_RAYTRACING_GEOMETRY_DESC blasGeomDescs[2] = {};
		// Sphere
		blasGeomDescs[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		blasGeomDescs[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		blasGeomDescs[0].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		blasGeomDescs[0].Triangles.VertexCount = (SphereStacks + 1) * (SphereSlices + 1);
		blasGeomDescs[0].Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
		blasGeomDescs[0].Triangles.IndexCount = (SphereStacks * SphereSlices) * 6;
		blasGeomDescs[0].Triangles.IndexBuffer = mIB->GetGPUVirtualAddress();
		blasGeomDescs[0].Triangles.VertexBuffer.StartAddress = mVB->GetGPUVirtualAddress();
		blasGeomDescs[0].Triangles.VertexBuffer.StrideInBytes = sizeof(VertexElement);
		// Plane
		blasGeomDescs[1] = blasGeomDescs[0];
		blasGeomDescs[1].Triangles.VertexCount = 4;
		blasGeomDescs[1].Triangles.IndexCount = 6;
		blasGeomDescs[1].Triangles.IndexBuffer = mIBPlane->GetGPUVirtualAddress();
		blasGeomDescs[1].Triangles.VertexBuffer.StartAddress = mVBPlane->GetGPUVirtualAddress();
		// Two geometries can merge together, but shaders cannot classify each other on SM6.3

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInput = {}, blasPlaneInput = {};
		// Sphere
		blasInput.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		blasInput.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
		blasInput.NumDescs = 1;
		blasInput.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		blasInput.pGeometryDescs = blasGeomDescs + 0;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuildInfo, blasPlanePrebuildInfo;
		device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInput, &blasPrebuildInfo);
		// Plane
		blasPlaneInput = blasInput;
		blasPlaneInput.pGeometryDescs = blasGeomDescs + 1;
		device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasPlaneInput, &blasPlanePrebuildInfo);

		// Scratch(Sphere + Plane)
		auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		UINT64 scratchSize =
			Align(blasPrebuildInfo.ScratchDataSizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
			+ blasPlanePrebuildInfo.ScratchDataSizeInBytes;
		auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratchBufBlas)));

		// Sphere
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(blasPrebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&mBlas)));
		// Plane
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(blasPlanePrebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&mBlasPlane)));

		// Build BLAS

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {}, blasPlaneDesc = {};
		// Sphere
		blasDesc.Inputs = blasInput;
		blasDesc.ScratchAccelerationStructureData = scratchBufBlas->GetGPUVirtualAddress();
		blasDesc.DestAccelerationStructureData = mBlas->GetGPUVirtualAddress();
		cmdList4->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
		// Plane
		blasPlaneDesc.Inputs = blasPlaneInput;
		blasPlaneDesc.ScratchAccelerationStructureData = scratchBufBlas->GetGPUVirtualAddress()
			+ Align(blasPrebuildInfo.ScratchDataSizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		blasPlaneDesc.DestAccelerationStructureData = mBlasPlane->GetGPUVirtualAddress();
		cmdList4->BuildRaytracingAccelerationStructure(&blasPlaneDesc, 0, nullptr);

		CD3DX12_RESOURCE_BARRIER uavBarriers[] = {
			CD3DX12_RESOURCE_BARRIER::UAV(mBlas.Get()),
			CD3DX12_RESOURCE_BARRIER::UAV(mBlasPlane.Get())
		};
		cmdList4->ResourceBarrier(_countof(uavBarriers), uavBarriers);

		// Setup TLAS

		D3D12_RAYTRACING_INSTANCE_DESC instanceDescs[2] = {};
		// Sphere
		instanceDescs[0].Transform[0][0] = instanceDescs[0].Transform[1][1] = instanceDescs[0].Transform[2][2] = 1.0f;
		instanceDescs[0].InstanceMask = 1;
		instanceDescs[0].AccelerationStructure = mBlas->GetGPUVirtualAddress();
		//instanceDesc[0].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
		// Plane
		instanceDescs[1] = instanceDescs[0];
		instanceDescs[1].AccelerationStructure = mBlasPlane->GetGPUVirtualAddress();

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(instanceDescs));
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mTlasInstance)));
		void* gpuMem;
		CHK(mTlasInstance->Map(0, nullptr, &gpuMem));
		memcpy(gpuMem, instanceDescs, sizeof(instanceDescs));

		// Prebuild TLAS

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInput = {};
		tlasInput.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasInput.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
		tlasInput.NumDescs = _countof(instanceDescs);
		tlasInput.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasInput.InstanceDescs = mTlasInstance->GetGPUVirtualAddress();
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuildInfo;
		device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInput, &tlasPrebuildInfo);

		heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		resDesc = CD3DX12_RESOURCE_DESC::Buffer(tlasPrebuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratchBufTlas)));

		resDesc = CD3DX12_RESOURCE_DESC::Buffer(tlasPrebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&mTlas)));

		// Build TLAS

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
		tlasDesc.Inputs = tlasInput;
		tlasDesc.ScratchAccelerationStructureData = scratchBufTlas->GetGPUVirtualAddress();
		tlasDesc.DestAccelerationStructureData = mTlas->GetGPUVirtualAddress();
		cmdList4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		// Submit building AS

		CHK(cmdList4->Close());
		ID3D12GraphicsCommandList* cmdLists[] = { cmdList4.Get() };
		cmdQueue->ExecuteCommandLists(1, CommandListCast(cmdLists));
		CHK(cmdQueue->Signal(fence.Get(), 1));
		CHK(mCmdQueue->Wait(fence.Get(), 1));
		while (fence->GetCompletedValue() != 1)
		{
			Sleep(1);
		};
		CHK(cmdAlloc->Reset());

		// Create views

		// AccelerationStructure
		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CD3DX12_CPU_DESCRIPTOR_HANDLE shaderViewHandle(mShaderView[i]->GetCPUDescriptorHandleForHeapStart());

			auto sv = CD3DX12_CPU_DESCRIPTOR_HANDLE(shaderViewHandle, (int)ShaderViews::AccelerationStructure, mResourceStride);
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			srv.RaytracingAccelerationStructure.Location = mTlas->GetGPUVirtualAddress();
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			mDevice->CreateShaderResourceView(nullptr, &srv, sv);
		}
	}

	void Draw()
	{
		mFrameCount++;
		auto frameIndex = mSwapChain->GetCurrentBackBufferIndex();

		ComPtr<ID3D12GraphicsCommandList4> cmdList4;
		CHK(mCmdList.As(&cmdList4));

		//-------------------------------

		float* pCB;
		CHK(mConstantBuffer[mFrameCount % BUFFER_COUNT]->Map(0, nullptr, reinterpret_cast<void**>(&pCB)));
		float* pCBSceneMatrix = pCB + 256 * (int)Constants::SceneMatrix / sizeof(*pCB);

		uint8_t* pSBT;
		CHK(mShaderBindingTable[mFrameCount % BUFFER_COUNT]->Map(0, nullptr, reinterpret_cast<void**>(&pSBT)));

		CD3DX12_GPU_DESCRIPTOR_HANDLE svBase(mShaderView[mFrameCount % BUFFER_COUNT]->GetGPUDescriptorHandleForHeapStart());
		auto svScene = svBase;

		auto samplerDefault = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSampler->GetGPUDescriptorHandleForHeapStart());

		// Make constant buffer

		auto fov = DirectX::XMConvertToRadians(45.0f);
		auto aspect = 1.0f * WINDOW_WIDTH / WINDOW_HEIGHT;
		auto nearClip = 0.01f;
		auto farClip = 100.0f;

		auto worldMat = DirectX::XMMatrixIdentity();
		auto viewMat = DirectX::XMMatrixLookAtLH(mCameraPos, mCameraTarget, mCameraUp);
		auto projMat = DirectX::XMMatrixPerspectiveFovLH(fov, aspect, nearClip, farClip);

		struct CBSceneMatrix {
			DirectX::XMMATRIX invViewProj;
			DirectX::XMVECTOR cameraPos;
		} sceneMatrix;
		static_assert(sizeof(sceneMatrix) <= 256, "");

		sceneMatrix.invViewProj = DirectX::XMMatrixTranspose(
			DirectX::XMMatrixInverse(nullptr, worldMat * viewMat * projMat));
		sceneMatrix.cameraPos = mCameraPos;
		memcpy(pCBSceneMatrix, &sceneMatrix, sizeof(sceneMatrix));

		// Make shader binding table

		memcpy(pSBT, mShaderBindingTableDefaultData.get(), oMax);
		_mm_sfence(); // overwrite data to write combining memory so insert a fence
		memcpy(pSBT + oRayGenRootConstant, &mRaytracingMode, sizeof(uint32_t));

		// Start recording commands

		CHK(mCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mCmdList->Reset(mCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		ID3D12DescriptorHeap* descHeap[] = { mShaderView[mFrameCount % BUFFER_COUNT].Get(), mSampler.Get() };
		mCmdList->SetDescriptorHeaps(_countof(descHeap), descHeap);

		// Draw scene

		CD3DX12_RESOURCE_BARRIER transitions[10];
		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSceneTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		mCmdList->ResourceBarrier(1, transitions);

		mCmdList->SetComputeRootSignature(mSceneRootSigGlobal.Get());
		mCmdList->SetComputeRootDescriptorTable(0, svScene);
		cmdList4->SetPipelineState1(mStateObject.Get());
		D3D12_DISPATCH_RAYS_DESC drDesc = {};
		drDesc.RayGenerationShaderRecord.StartAddress = mShaderBindingTable[mFrameCount % BUFFER_COUNT]->GetGPUVirtualAddress();
		drDesc.RayGenerationShaderRecord.SizeInBytes = oHitGroup;
		drDesc.HitGroupTable.StartAddress = drDesc.RayGenerationShaderRecord.StartAddress + oHitGroup;
		drDesc.HitGroupTable.SizeInBytes = oMiss - oHitGroup;
		drDesc.HitGroupTable.StrideInBytes = oMiss - oHitGroup;
		drDesc.MissShaderTable.StartAddress = drDesc.RayGenerationShaderRecord.StartAddress + oMiss;
		drDesc.MissShaderTable.SizeInBytes = oMax - oMiss;
		drDesc.MissShaderTable.StrideInBytes = oMax - oMiss;
		drDesc.Width = WINDOW_WIDTH;
		drDesc.Height = WINDOW_HEIGHT;
		drDesc.Depth = 1;
		cmdList4->DispatchRays(&drDesc);

		// Copy scene image to swap chain

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSceneTex.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
		transitions[1] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		mCmdList->ResourceBarrier(2, transitions);

		mCmdList->CopyResource(mSwapChainTex[frameIndex].Get(), mSceneTex.Get());

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainTex[frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		mCmdList->ResourceBarrier(1, transitions);

		// Finish recording commands
		CHK(mCmdList->Close());

		//-------------------------------

		// Execute recorded commands
		mCmdQueue->ExecuteCommandLists(1, CommandListCast(mCmdList.GetAddressOf()));
		CHK(mCmdQueue->Signal(mFence.Get(), mFrameCount));
	}

	void Present()
	{
		DXGI_PRESENT_PARAMETERS pp = {};
		CHK(mSwapChain->Present1(0, 0, &pp));
	}

	void Wait()
	{
		if (mFrameCount != 0)
		{
			while ((mFence->GetCompletedValue() + 1) < mFrameCount)
			{
				SwitchToThread();
			}
		}
	}

private:
	DirectX::XMVECTOR mCameraPos = DirectX::XMVectorSet(0.0f, 4.0f, -4.0f, 0);
	//DirectX::XMVECTOR mCameraPos = DirectX::XMVectorSet(0.0f, 0.0f, -4.0f, 0);
	DirectX::XMVECTOR mCameraTarget = DirectX::XMVectorSet(0, 0, 0, 0);
	DirectX::XMVECTOR mCameraUp = DirectX::XMVectorSet(0, 1, 0, 0);
	uint32_t mRaytracingMode = 1;

public:
	void MoveCamera(float forward, float trans, float rot)
	{
		if (fabs(forward) > std::numeric_limits<float>::epsilon())
		{
			auto dir = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(mCameraTarget, mCameraPos));
			auto vec = DirectX::XMVectorScale(dir, forward);
			mCameraPos = DirectX::XMVectorAdd(mCameraPos, vec);
			mCameraTarget = DirectX::XMVectorAdd(mCameraTarget, vec);
		}
		if (fabs(trans) > std::numeric_limits<float>::epsilon())
		{
			auto vec = DirectX::XMVectorScale(DirectX::XMVectorSet(1, 0, 0, 0), trans);
			mCameraPos = DirectX::XMVectorAdd(mCameraPos, vec);
			mCameraTarget = DirectX::XMVectorAdd(mCameraTarget, vec);
		}
		if (fabs(rot) > std::numeric_limits<float>::epsilon())
		{
			auto vec = DirectX::XMVectorSubtract(mCameraPos, mCameraTarget);
			float length = sqrtf(powf(DirectX::XMVectorGetX(vec), 2) + powf(DirectX::XMVectorGetZ(vec), 2));
			float x = DirectX::XMVectorGetX(vec) / length;
			float z = DirectX::XMVectorGetZ(vec) / length;
			float angle = atan2f(z, x);
			angle += rot;
			while (angle > DirectX::XM_PI)
				angle -= DirectX::XM_PI * 2;
			while (angle < -DirectX::XM_PI)
				angle += DirectX::XM_PI * 2;
			float newX = cosf(angle);
			float newZ = sinf(angle);
			auto newVec = DirectX::XMVectorSet(newX * length, DirectX::XMVectorGetY(vec), newZ * length, 0);
			mCameraPos = DirectX::XMVectorAdd(mCameraTarget, newVec);
		}
	}

	void ChangeMode(uint32_t mode)
	{
		mRaytracingMode = mode;
	}
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			PostMessage(hWnd, WM_DESTROY, 0, 0);
			return 0;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

static HWND setupWindow(int width, int height)
{
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = (HMODULE)GetModuleHandle(0);
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = _T("WindowClass");
	wcex.hIconSm = nullptr;
	if (!RegisterClassEx(&wcex)) {
		throw runtime_error("RegisterClassEx()");
	}

	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	const int windowWidth = (rect.right - rect.left);
	const int windowHeight = (rect.bottom - rect.top);

	HWND hWnd = CreateWindow(_T("WindowClass"), _T("Window"),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, windowWidth, windowHeight,
		nullptr, nullptr, nullptr, nullptr);
	if (!hWnd) {
		throw runtime_error("CreateWindow()");
	}

	return hWnd;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	MSG msg;
	ZeroMemory(&msg, sizeof msg);

	HWND mainWindowHandle = 0;
#ifdef NDEBUG
	try
#endif
	{
		mainWindowHandle = setupWindow(WINDOW_WIDTH, WINDOW_HEIGHT);
		ShowWindow(mainWindowHandle, SW_SHOW);
		UpdateWindow(mainWindowHandle);

		D3D d3d(WINDOW_WIDTH, WINDOW_HEIGHT, mainWindowHandle);

		while (msg.message != WM_QUIT) {
			BOOL r = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
			if (r == 0) {
				BYTE keyState[256] = {};
				if (GetKeyboardState(keyState)) {
					if (keyState['W'] & 0x80) {
						d3d.MoveCamera(0.05f, 0, 0);
					}
					if (keyState['S'] & 0x80) {
						d3d.MoveCamera(-0.05f, 0, 0);
					}
					if (keyState['Z'] & 0x80) {
						d3d.MoveCamera(0, -0.05f, 0);
					}
					if (keyState['C'] & 0x80) {
						d3d.MoveCamera(0, 0.05f, 0);
					}
					if (keyState['A'] & 0x80) {
						d3d.MoveCamera(0, 0, 0.04f);
					}
					if (keyState['D'] & 0x80) {
						d3d.MoveCamera(0, 0, -0.04f);
					}
					if (keyState['0'] & 0x80) {
						d3d.ChangeMode(0);
					}
					if (keyState['1'] & 0x80) {
						d3d.ChangeMode(1);
					}
					if (keyState['2'] & 0x80) {
						d3d.ChangeMode(2);
					}
					if (keyState['3'] & 0x80) {
						d3d.ChangeMode(3);
					}
				}
				d3d.Wait();
				d3d.Draw();
				d3d.Present();
			}
			else {
				DispatchMessage(&msg);
			}
		}
	}
#ifdef NDEBUG
	catch (std::exception& e) {
		MessageBoxA(mainWindowHandle, e.what(), "Exception occuured.", MB_ICONSTOP);
	}
#endif

	return static_cast<int>(msg.wParam);
}
