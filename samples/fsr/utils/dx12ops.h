#pragma once

#include "validation_remap.h"
#include "../common.h"

using namespace cauldron;
using namespace common;

class DX12Ops
{
public:
    DX12Ops() = default;

    ~DX12Ops()
    {
        for (size_t i = 0; i < FSR_REMOTE_SHARED_BUFFER_COUNT; i++)
        {
            ID3D12Resource* pResource = std::get<0>(p_SharedBuffer[i]);
            ID3D12Fence*    pFence    = std::get<1>(p_SharedBuffer[i]);

            if (pResource)
            {
                pResource->Release();
            }

            if (pFence)
            {
                pFence->Release();
            }
        }
    }

    enum class BufferState : UINT64
    {
        IDLE = 0,
        READY,
    };

    bool bufferStateMatches(int bufferIndex, BufferState state)
    {
        ID3D12Fence* pFence = std::get<1>(p_SharedBuffer[bufferIndex]);
        return pFence->GetCompletedValue() == static_cast<UINT64>(state);
    }

    void CreateSharedBuffers(FSRResources pResources, bool shouldCreate = false);

    void TransferToSharedBuffer(FSRResources pResources, int bufferIndex, CommandList* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, true);
    }

    void TransferFromSharedBuffer(FSRResources pResources, int bufferIndex, CommandList* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, false);
    }

private:
    std::tuple<ID3D12Resource*, ID3D12Fence*> p_SharedBuffer[FSR_REMOTE_SHARED_BUFFER_COUNT];

    size_t CalculateTotalSize(FSRResources pResources);

    void PerformTransfer(FSRResources pResources, int bufferIndex, CommandList* pCmdList, bool toSharedBuffer);
};
