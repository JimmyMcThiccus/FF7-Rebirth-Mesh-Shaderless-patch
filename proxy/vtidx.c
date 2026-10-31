#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <stdio.h>
#include <d3d12.h>
#define P(T, M) printf("%-36s %-42s %3zu\n", #T, #M, offsetof(T##Vtbl, M) / sizeof(void*))
int main(void) {
  P(ID3D12Device2, CheckFeatureSupport); P(ID3D12Device2, CreateRootSignature); P(ID3D12Device2, CreateCommandList);
  P(ID3D12Device2, CreateGraphicsPipelineState); P(ID3D12Device2, CreateComputePipelineState); P(ID3D12Device2, CreateCommandAllocator);
  P(ID3D12Device2, CreateCommandSignature); P(ID3D12Device2, CreatePipelineState); P(ID3D12Device1, CreatePipelineLibrary);
  P(ID3D12Device4, CreateCommandList1); P(ID3D12Device2, CreateCommittedResource);
  P(ID3D12GraphicsCommandList6, Close); P(ID3D12GraphicsCommandList6, Reset); P(ID3D12GraphicsCommandList6, DrawInstanced);
  P(ID3D12GraphicsCommandList6, Dispatch); P(ID3D12GraphicsCommandList6, SetPipelineState); P(ID3D12GraphicsCommandList6, ResourceBarrier);
  P(ID3D12GraphicsCommandList6, IASetPrimitiveTopology); P(ID3D12GraphicsCommandList6, SetComputeRootSignature); P(ID3D12GraphicsCommandList6, SetGraphicsRootSignature);
  P(ID3D12GraphicsCommandList6, SetComputeRootDescriptorTable); P(ID3D12GraphicsCommandList6, SetGraphicsRootDescriptorTable);
  P(ID3D12GraphicsCommandList6, SetComputeRoot32BitConstant); P(ID3D12GraphicsCommandList6, SetGraphicsRoot32BitConstant);
  P(ID3D12GraphicsCommandList6, SetComputeRoot32BitConstants); P(ID3D12GraphicsCommandList6, SetGraphicsRoot32BitConstants);
  P(ID3D12GraphicsCommandList6, SetComputeRootConstantBufferView); P(ID3D12GraphicsCommandList6, SetGraphicsRootConstantBufferView);
  P(ID3D12GraphicsCommandList6, SetComputeRootShaderResourceView); P(ID3D12GraphicsCommandList6, SetGraphicsRootShaderResourceView);
  P(ID3D12GraphicsCommandList6, SetComputeRootUnorderedAccessView); P(ID3D12GraphicsCommandList6, SetGraphicsRootUnorderedAccessView);
  P(ID3D12GraphicsCommandList6, ExecuteIndirect); P(ID3D12GraphicsCommandList6, DispatchMesh); P(ID3D12GraphicsCommandList6, ClearState);
  P(ID3D12CommandAllocator, Reset); P(ID3D12PipelineLibrary1, LoadPipeline); P(ID3D12PipelineLibrary1, StorePipeline);
  P(ID3D12PipelineLibrary1, LoadGraphicsPipeline);
  return 0; }
