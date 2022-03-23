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
#pragma comment(lib, "dxguid.lib")
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
	HWND g_mainWindowHandle = 0;
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
	uint64_t mFrameCount = 300;
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

	enum class RTVs {
		Scene,
		Max,
	};
	ComPtr<ID3D12DescriptorHeap> mRTV;

	enum class DSVs {
		Scene,
		Max,
	};
	ComPtr<ID3D12DescriptorHeap> mDSV;

	enum class ShaderViews {
		// Base pass
		SceneCBVMatrix,
		SceneBindlessResource,
		Max = SceneBindlessResource + MAX_BINDLESS_RESOURCE,
	};
	ComPtr<ID3D12DescriptorHeap> mShaderView[BUFFER_COUNT];

	enum class Samplers {
		Default,
		Max,
	};
	ComPtr<ID3D12DescriptorHeap> mSampler;

	ComPtr<ID3D12RootSignature> mSceneRootSig;
	ComPtr<ID3D12PipelineState> mScenePSO;
	ComPtr<ID3D12Resource> mSceneTex;
	ComPtr<ID3D12Resource> mSceneZ;

	ComPtr<ID3D12Resource> mBindlessResource[MAX_BINDLESS_RESOURCE];

	struct VertexElement
	{
		float position[3];
		float normal[3];
	};

	ComPtr<ID3D12Resource> mVB;
	ComPtr<ID3D12Resource> mIB;
	D3D12_VERTEX_BUFFER_VIEW mVBView = {};
	D3D12_INDEX_BUFFER_VIEW mIBView = {};
	const int SphereSlices = 12;
	const int SphereStacks = 12;

	ComPtr<ID3D12Resource> mVBPlane;
	ComPtr<ID3D12Resource> mIBPlane;
	D3D12_VERTEX_BUFFER_VIEW mVBPlaneView = {};
	D3D12_INDEX_BUFFER_VIEW mIBPlaneView = {};

	const float kDefaultDSClearColor[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	const float kDefaultRTClearColor[4] = { 0.1f, 0.2f, 0.4f, 0.0f };

	int Align(int val, int align)
	{
		return ((val + align - 1) & ~(align - 1));
	}

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
		mCmdList->Close();

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

		// Shader

		CD3DX12_DESCRIPTOR_RANGE descRange[10];
		descRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		CD3DX12_ROOT_PARAMETER rootParam[10];
		rootParam[0].InitAsDescriptorTable(1, descRange); // CBV_SRV_UAV
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
		rootSigDesc.Init(1, rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> rootSigBlob, rootSigError;

		descRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0); // VS
		//descRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); // PS
		descRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_BINDLESS_RESOURCE, 0, 1); // PS
		rootParam[0].InitAsDescriptorTable(1, descRange + 0, D3D12_SHADER_VISIBILITY_VERTEX); // CBV_SRV_UAV
		//rootParam[1].InitAsDescriptorTable(1, descRange + 1, D3D12_SHADER_VISIBILITY_PIXEL); // CBV_SRV_UAV
		rootParam[1].InitAsDescriptorTable(1, descRange + 1, D3D12_SHADER_VISIBILITY_PIXEL); // CBV_SRV_UAV
		rootParam[2].InitAsConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
		rootSigDesc.Init(3, rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootSigBlob, &rootSigError));
		CHK(mDevice->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&mSceneRootSig)));

		static const char shaderCodeSceneVS[] = R"#(
cbuffer CScene {
	float4x4 ViewProj;
};
struct Output {
	float4 position : SV_Position;
	float3 world : WorldPosition;
	float3 normal : Normal;
};
Output main(float3 position : Position, float3 normal : Normal) {
	Output output;
	output.position = mul(float4(position, 1), ViewProj);
	output.world = position;
	output.normal = normalize(normal);
	return output;
}
)#";

		static const char shaderCodeScenePS[] = R"#(
cbuffer CRootParam : register(b0) {
	uint RootParamOffset;
};
Texture2D<float4> ColorMap[] : register(t0, space1);
struct Input {
	float4 position : SV_Position;
	float3 world : WorldPosition;
	float3 normal : Normal;
};
float4 main(Input input) : SV_Target {
	float4 color;
	if (RootParamOffset < 8) {
		color = ColorMap[RootParamOffset].Load(int3(0, 0, 0));
	} else {
		uint index = ((uint)(input.position.x) + (uint)(input.position.y)) % 8;
		color = ColorMap[ NonUniformResourceIndex(index) ].Load(int3(0, 0, 0));
	}
	float intensity = input.normal.y * 0.5 + 0.5;
	color.xyz *= intensity;
	return color;
}
)#";

		SetDllDirectory(L"../dll/");

		ComPtr<IDxcCompiler> dxc;
		CHK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)));
		ComPtr<IDxcLibrary> dxcLib;
		CHK(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxcLib)));

		ComPtr<IDxcBlobEncoding> dxcTxtSceneVS, dxcTxtScenePS;
		CHK(dxcLib->CreateBlobWithEncodingFromPinned(shaderCodeSceneVS, _countof(shaderCodeSceneVS) - 1, CP_UTF8, &dxcTxtSceneVS));
		CHK(dxcLib->CreateBlobWithEncodingFromPinned(shaderCodeScenePS, _countof(shaderCodeScenePS) - 1, CP_UTF8, &dxcTxtScenePS));

		ComPtr<IDxcBlob> dxcBlobShadowVS, dxcBlobSceneVS, dxcBlobScenePS;
		ComPtr<IDxcBlobEncoding> dxcError;
		ComPtr<IDxcOperationResult> dxcRes;
		const wchar_t* shaderArgs[] = { L"-Zi", L"-all_resources_bound", L"-Qembed_debug" };

		dxc->Compile(dxcTxtSceneVS.Get(), nullptr, L"main", L"vs_6_0", shaderArgs, _countof(shaderArgs), nullptr, 0, nullptr, &dxcRes);
		dxcRes->GetErrorBuffer(&dxcError);
		if (dxcError->GetBufferSize()) {
			OutputDebugStringA(reinterpret_cast<char*>(dxcError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		dxcRes->GetResult(&dxcBlobSceneVS);
		dxc->Compile(dxcTxtScenePS.Get(), nullptr, L"main", L"ps_6_0", shaderArgs, _countof(shaderArgs), nullptr, 0, nullptr, &dxcRes);
		dxcRes->GetErrorBuffer(&dxcError);
		if (dxcError->GetBufferSize()) {
			OutputDebugStringA(reinterpret_cast<char*>(dxcError->GetBufferPointer()));
			throw runtime_error("Shader compile error.");
		}
		dxcRes->GetResult(&dxcBlobScenePS);

		D3D12_INPUT_ELEMENT_DESC ieDesc[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		auto dsDesc = CD3DX12_DEPTH_STENCIL_DESC(CD3DX12_DEFAULT());
		//dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
		auto rsDesc = CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT());
		//rsDesc.CullMode = D3D12_CULL_MODE_NONE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = mSceneRootSig.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(dxcBlobSceneVS->GetBufferPointer(), dxcBlobSceneVS->GetBufferSize());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(dxcBlobScenePS->GetBufferPointer(), dxcBlobScenePS->GetBufferSize());
		psoDesc.InputLayout = { ieDesc, _countof(ieDesc) };
		psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.RasterizerState = rsDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.BlendState = CD3DX12_BLEND_DESC(CD3DX12_DEFAULT());
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleDesc.Count = 1;
		CHK(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mScenePSO)));

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
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		auto clearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, kDefaultRTClearColor);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, &clearValue, IID_PPV_ARGS(&mSceneTex)));

		resDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, WINDOW_WIDTH, WINDOW_HEIGHT, 1, 1);
		resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
		clearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_D32_FLOAT, kDefaultDSClearColor);
		CHK(mDevice->CreateCommittedResource(
			&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&mSceneZ)));

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
				{0.5f, 0.5f, 0.0f, 1.0f},
				{0.0f, 1.0f, 0.0f, 1.0f},
				{0.0f, 0.5f, 0.5f, 1.0f},
				{0.0f, 0.0f, 1.0f, 1.0f},
				{0.5f, 0.0f, 0.5f, 1.0f},
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

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc.NumDescriptors = (int)RTVs::Max;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mRTV)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTV->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateRenderTargetView(mSceneTex.Get(), nullptr, rtvHandle);

		descHeapDesc = {};
		descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		descHeapDesc.NumDescriptors = (int)DSVs::Max;
		CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mDSV)));

		CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mDSV->GetCPUDescriptorHandleForHeapStart());
		mDevice->CreateDepthStencilView(mSceneZ.Get(), nullptr, dsvHandle);

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			descHeapDesc = {};
			descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			descHeapDesc.NumDescriptors = (int)ShaderViews::Max;
			descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			CHK(mDevice->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&mShaderView[i])));

			CD3DX12_CPU_DESCRIPTOR_HANDLE shaderViewHandle(mShaderView[i]->GetCPUDescriptorHandleForHeapStart());
			auto addrCB = mConstantBuffer[i]->GetGPUVirtualAddress();

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
			auto sv = CD3DX12_CPU_DESCRIPTOR_HANDLE(shaderViewHandle, (int)ShaderViews::SceneCBVMatrix, mResourceStride);
			mDevice->CreateConstantBufferView(&cbv, sv);
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

		mVBView.BufferLocation = mVB->GetGPUVirtualAddress();
		mVBView.StrideInBytes = sizeof(VertexElement);
		mVBView.SizeInBytes = sizeVB;
		mIBView.BufferLocation = mIB->GetGPUVirtualAddress();
		mIBView.Format = DXGI_FORMAT_R16_UINT;
		mIBView.SizeInBytes = sizeIB;

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

		mVBPlaneView.BufferLocation = mVBPlane->GetGPUVirtualAddress();
		mVBPlaneView.StrideInBytes = sizeof(VertexElement);
		mVBPlaneView.SizeInBytes = sizeVB;
		mIBPlaneView.BufferLocation = mIBPlane->GetGPUVirtualAddress();
		mIBPlaneView.Format = DXGI_FORMAT_R16_UINT;
		mIBPlaneView.SizeInBytes = sizeIB;

		// DMA

		CHK(mCmdListCopy->Close());
		ID3D12GraphicsCommandList* cmdLists[] = { mCmdListCopy.Get() };
		mCmdQueueCopy->ExecuteCommandLists(1, CommandListCast(cmdLists));
		CHK(mCmdQueueCopy->Signal(mFence.Get(), 10));
		while (mFence->GetCompletedValue() < 10);
		CHK(mCmdAllocCopy->Reset());
	}

	void Draw()
	{
		mFrameCount++;
		auto frameIndex = mSwapChain->GetCurrentBackBufferIndex();

		//-------------------------------

		float* pCB;
		CHK(mConstantBuffer[mFrameCount % BUFFER_COUNT]->Map(0, nullptr, reinterpret_cast<void**>(&pCB)));
		float* pCBSceneMatrix = pCB + 256 * (int)Constants::SceneMatrix / sizeof(*pCB);

		auto rtvScene = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRTV->GetCPUDescriptorHandleForHeapStart());

		auto dsvScene = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDSV->GetCPUDescriptorHandleForHeapStart());
		auto dsvShadow = CD3DX12_CPU_DESCRIPTOR_HANDLE(dsvScene, mDSVStride);

		CD3DX12_GPU_DESCRIPTOR_HANDLE svBase(mShaderView[mFrameCount % BUFFER_COUNT]->GetGPUDescriptorHandleForHeapStart());
		auto svShadow = svBase;
		auto svSceneVS = CD3DX12_GPU_DESCRIPTOR_HANDLE(svBase, (int)ShaderViews::SceneCBVMatrix, mResourceStride);
		auto svScenePS = CD3DX12_GPU_DESCRIPTOR_HANDLE(svBase, (int)ShaderViews::SceneBindlessResource, mResourceStride);

		auto samplerDefault = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSampler->GetGPUDescriptorHandleForHeapStart());

		// Make constant buffer

		auto fov = DirectX::XMConvertToRadians(45.0f);
		auto aspect = 1.0f * WINDOW_WIDTH / WINDOW_HEIGHT;
		auto nearClip = 0.01f;
		auto farClip = 100.0f;

		auto worldMat = DirectX::XMMatrixIdentity();
		auto viewMat = DirectX::XMMatrixLookAtLH(mCameraPos, mCameraTarget, mCameraUp);
		auto projMat = DirectX::XMMatrixPerspectiveFovLH(fov, aspect, nearClip, farClip);

		auto shadowDir = DirectX::XMVectorSet(0.0f, -1.0f, 0.0f, 0);
		auto shadowPos = DirectX::XMVectorSet(0.0f, 5.0f, 0.0f, 0);
		auto shadowUp = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0);
		auto shadowRange = 1.0f;
		auto shadowDistance = 10.0f;

		auto shadowViewMat = DirectX::XMMatrixLookAtLH(shadowPos, DirectX::XMVectorAdd(shadowPos, shadowDir), shadowUp);
		auto shadowProjMat = DirectX::XMMatrixOrthographicLH(shadowRange * 2, shadowRange * 2, 0, shadowDistance);

		*reinterpret_cast<DirectX::XMMATRIX*>(pCBSceneMatrix) = DirectX::XMMatrixTranspose(worldMat * viewMat * projMat);

		// Start recording commands

		CHK(mCmdAlloc[mFrameCount % BUFFER_COUNT]->Reset());
		CHK(mCmdList->Reset(mCmdAlloc[mFrameCount % BUFFER_COUNT].Get(), nullptr));

		ID3D12DescriptorHeap* descHeap[] = { mShaderView[mFrameCount % BUFFER_COUNT].Get(), mSampler.Get() };
		mCmdList->SetDescriptorHeaps(_countof(descHeap), descHeap);

		// Draw scene

		CD3DX12_RESOURCE_BARRIER transitions[10];
		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSceneTex.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ResourceBarrier(1, transitions);

		mCmdList->ClearRenderTargetView(rtvScene, kDefaultRTClearColor, 0, nullptr);
		mCmdList->ClearDepthStencilView(dsvScene, D3D12_CLEAR_FLAG_DEPTH, kDefaultDSClearColor[0], 0, 0, nullptr);

		mCmdList->SetGraphicsRootSignature(mSceneRootSig.Get());
		mCmdList->SetPipelineState(mScenePSO.Get());
		mCmdList->SetGraphicsRootDescriptorTable(0, svSceneVS); // VS, CBV_SRV_UAV
		mCmdList->SetGraphicsRootDescriptorTable(1, svScenePS); // PS, CBV_SRV_UAV
		mCmdList->SetGraphicsRoot32BitConstant(2, static_cast<UINT>(mBindlessTextureIndex), 0); // PS, RootConstant
		//mCmdList->SetGraphicsRootDescriptorTable(2, samplerDefault); // PS, Sampler
		mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		mCmdList->IASetVertexBuffers(0, 1, &mVBView);
		mCmdList->IASetIndexBuffer(&mIBView);
		auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT);
		mCmdList->RSSetViewports(1, &viewport);
		auto scissor = CD3DX12_RECT(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
		mCmdList->RSSetScissorRects(1, &scissor);
		mCmdList->OMSetRenderTargets(1, &rtvScene, TRUE, &dsvScene);
		mCmdList->DrawIndexedInstanced(6 * SphereStacks * SphereSlices, 1, 0, 0, 0);

		mCmdList->IASetVertexBuffers(0, 1, &mVBPlaneView);
		mCmdList->IASetIndexBuffer(&mIBPlaneView);
		mCmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);

		// Copy scene image to swap chain

		transitions[0] = CD3DX12_RESOURCE_BARRIER::Transition(mSceneTex.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
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
		while ((mFence->GetCompletedValue() + 1) < mFrameCount)
		{
			SwitchToThread();
		}
		DXGI_PRESENT_PARAMETERS pp = {};
		CHK(mSwapChain->Present1(0, 0, &pp));

		return;
	}

private:
	DirectX::XMVECTOR mCameraPos = DirectX::XMVectorSet(0.0f, 4.0f, -4.0f, 0);
	//DirectX::XMVECTOR mCameraPos = DirectX::XMVectorSet(0.0f, 0.0f, -4.0f, 0);
	DirectX::XMVECTOR mCameraTarget = DirectX::XMVectorSet(0, 0, 0, 0);
	DirectX::XMVECTOR mCameraUp = DirectX::XMVectorSet(0, 1, 0, 0);
	float mBindlessTextureIndex = 0;

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

	void ChangeTexture(bool forward)
	{
		float newIndex = forward ? (mBindlessTextureIndex + 0.1f) : (mBindlessTextureIndex - 0.1f);
		mBindlessTextureIndex = max(0.0f, min((float)MAX_BINDLESS_RESOURCE + 1.0f, newIndex));
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

#ifdef NDEBUG
	try
#endif
	{
		g_mainWindowHandle = setupWindow(WINDOW_WIDTH, WINDOW_HEIGHT);
		ShowWindow(g_mainWindowHandle, SW_SHOW);
		UpdateWindow(g_mainWindowHandle);

		D3D d3d(WINDOW_WIDTH, WINDOW_HEIGHT, g_mainWindowHandle);

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
					if (keyState[VK_RIGHT] & 0x80) {
						d3d.ChangeTexture(true);
					}
					if (keyState[VK_LEFT] & 0x80) {
						d3d.ChangeTexture(false);
					}
				}
				d3d.Draw();
				d3d.Present();
			}
			else {
				DispatchMessage(&msg);
			}
		}
	}
#ifdef NDEBUG
	catch (std::exception & e) {
		MessageBoxA(g_mainWindowHandle, e.what(), "Exception occuured.", MB_ICONSTOP);
	}
#endif

	return static_cast<int>(msg.wParam);
}
